/*
 *
 * Copyright 2015-present Facebook. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <sys/time.h>
#include <openbmc/ipmi.h>
#include <openbmc/ipmb.h>
#include <openbmc/pal.h>

#define BTN_MAX_SAMPLES   200
#define BTN_POWER_OFF     40
#define MAX_NUM_SLOTS 4
#define HB_TIMESTAMP_COUNT (60 * 60)

// Helper function for msleep
void
msleep(int msec) {
  struct timespec req;

  req.tv_sec = 0;
  req.tv_nsec = msec * 1000 * 1000;

  while(nanosleep(&req, &req) == -1 && errno == EINTR) {
    continue;
  }
}

// Thread for monitoring debug card hotswap
static void *
debug_card_handler() {
  int curr = -1;
  int prev = -1;
  uint8_t prsnt;
  uint8_t pos ;
  uint8_t prev_pos = -1;
  uint8_t lpc;
  int i, ret;

  while (1) {
    // Check if debug card present or not
    ret = pal_is_debug_card_prsnt(&prsnt);
    if (ret) {
      goto debug_card_out;
    }

    curr = prsnt;

    // Check if Debug Card was either inserted or removed
    if (curr != prev) {

      if (!curr) {
      // Debug Card was removed
        syslog(LOG_WARNING, "Debug Card Extraction\n");
        // Switch UART mux to BMC
        ret = pal_switch_uart_mux(HAND_SW_BMC);
        if (ret) {
          goto debug_card_out;
        }
      } else {
        // Debug Card was inserted
        syslog(LOG_WARNING, "Debug Card Insertion\n");

      }
    }

    // If Debug Card is present
    if (curr) {
      ret = pal_get_hand_sw(&pos);
      if (ret) {
        goto debug_card_out;
      }

      if (pos == prev_pos && pos != HAND_SW_BMC & !prev) {
        goto display_post;
      }


      // Switch UART mux based on hand switch
      ret = pal_switch_uart_mux(pos);
      if (ret) {
        goto debug_card_out;
      }

      // Enable POST code based on hand switch
      if (pos == HAND_SW_BMC) {
        // For BMC, there is no need to have POST specific code
        goto debug_card_done;
      }

      // Make sure the server at selected position is present
      ret = pal_is_server_prsnt(pos, &prsnt);
      if (ret || !prsnt) {
        goto debug_card_done;
      }

      // Enable POST codes for all slots
      ret = pal_post_enable(pos);
      if (ret) {
        goto debug_card_out;
      }

display_post:
      // Get last post code and display it
      ret = pal_post_get_last(pos, &lpc);
      if (ret) {
        goto debug_card_out;
      }

      ret = pal_post_handle(pos, lpc);
      if (ret) {
        goto debug_card_out;
      }

    }

debug_card_done:
    prev = curr;
    prev_pos = pos;
debug_card_out:
    if (prsnt)
      msleep(500);
    else
      sleep(1);
  }
}

// Thread to monitor the hand switch
static void *
usb_handler() {
  int curr = -1;
  int prev = -1;
  int ret;
  uint8_t pos;
  uint8_t prsnt;
  uint8_t lpc;

  while (1) {
    // Get the current hand switch position
    ret = pal_get_hand_sw(&pos);
    if (ret) {
      goto hand_sw_out;
    }
    curr = pos;
    if (curr == prev) {
      // No state change, continue;
      goto hand_sw_out;
    }

    // Switch USB Mux to selected server
    ret = pal_switch_usb_mux(pos);
    if (ret) {
      goto hand_sw_out;
    }

    prev = curr;
hand_sw_out:
    sleep(1);
    continue;
  }
}

// Thread to monitor Reset Button and propagate to selected server
static void *
rst_btn_handler() {
  int ret;
  uint8_t pos;
  int i;
  uint8_t btn;

  while (1) {
    // Check the position of hand switch
    ret = pal_get_hand_sw(&pos);
    if (ret || pos == HAND_SW_BMC) {
      // For BMC, no need to handle Reset Button
      sleep (1);
      continue;
    }

    // Check if reset button is pressed
    ret = pal_get_rst_btn(&btn);
    if (ret || !btn) {
      goto rst_btn_out;
    }

    // Pass the reset button to the selected slot
    syslog(LOG_WARNING, "Reset button pressed\n");
    ret = pal_set_rst_btn(pos, 0);
    if (ret) {
      goto rst_btn_out;
    }

    // Wait for the button to be released
    for (i = 0; i < BTN_MAX_SAMPLES; i++) {
      ret = pal_get_rst_btn(&btn);
      if (ret || btn) {
        msleep(100);
        continue;
      }
      syslog(LOG_WARNING, "Reset button released\n");
      syslog(LOG_CRIT, "Reset Button pressed for FRU: %d\n", pos);
      ret = pal_set_rst_btn(pos, 1);
      goto rst_btn_out;
    }

    // handle error case
    if (i == BTN_MAX_SAMPLES) {
      syslog(LOG_WARNING, "Reset button seems to stuck for long time\n");
      goto rst_btn_out;
    }
rst_btn_out:
    msleep(100);
  }
}

// Thread to handle Power Button and power on/off the selected server
static void *
pwr_btn_handler() {
  int ret;
  uint8_t pos, btn, cmd;
  int i;
  uint8_t power;

  while (1) {
    // Check the position of hand switch
    ret = pal_get_hand_sw(&pos);
    if (ret || pos == HAND_SW_BMC) {
      sleep(1);
      continue;
    }

    // Check if power button is pressed
    ret = pal_get_pwr_btn(&btn);
    if (ret || !btn) {
      goto pwr_btn_out;
    }

    syslog(LOG_WARNING, "power button pressed\n");

    // Wait for the button to be released
    for (i = 0; i < BTN_MAX_SAMPLES; i++) {
      ret = pal_get_pwr_btn(&btn);
      if (ret || btn ) {
        msleep(100);
        continue;
      }
      syslog(LOG_WARNING, "power button released\n");
      break;
    }

    // handle error case
    if (i == BTN_MAX_SAMPLES) {
      syslog(LOG_WARNING, "Power button seems to stuck for long time\n");
      goto pwr_btn_out;
    }

    // Get the current power state (power on vs. power off)
    ret = pal_get_server_power(pos, &power);
    if (ret) {
      goto pwr_btn_out;
    }

    // Set power command should reverse of current power state
    cmd = !power;

    // To determine long button press
    if (i >= BTN_POWER_OFF) {
      syslog(LOG_CRIT, "Power Button Long Press for FRU: %d\n", pos);
    } else {

      // If current power state is ON and it is not a long press,
      // the power off should be Graceful Shutdown
      if (power == SERVER_POWER_ON)
        cmd = SERVER_GRACEFUL_SHUTDOWN;

      syslog(LOG_CRIT, "Power Button Press for FRU: %d\n", pos);
    }

    // Reverse the power state of the given server
    ret = pal_set_server_power(pos, cmd);
pwr_btn_out:
    msleep(100);
  }
}

// Thread to handle Heart Beat LED and monitor SLED Cycles
static void *
hb_handler() {
  int count = 0;
  struct timespec ts;
  struct timespec mts;
  char tstr[64] = {0};
  char buf[128] = {0};
  uint8_t por = 0;
  uint8_t time_init = 0;
  long time_sled_on;
  long time_sled_off;

  // Read the last timestamp from KV storage
  pal_get_key_value("timestamp_sled", tstr);
  time_sled_off = (long) strtoul(tstr, NULL, 10);

  // If this reset is due to Power-On-Reset, we detected SLED power OFF event
  if (pal_is_bmc_por()) {
    ctime_r(&time_sled_off, buf);
    syslog(LOG_CRIT, "SLED Powered OFF at %s", buf);
  }


  while (1) {
    // Toggle HB LED
    pal_set_hb_led(1);
    msleep(500);
    pal_set_hb_led(0);
    msleep(500);

    // Make sure the time is initialized properly
    // Since there is no battery backup, the time could be reset to build time
    if (time_init == 0) {
      // Read current time
      clock_gettime(CLOCK_REALTIME, &ts);

      if (ts.tv_sec < time_sled_off) {
        continue;
      }

      // If current time is more than the stored time, the date is correct
      time_init = 1;

      // Need to log SLED ON event, if this is Power-On-Reset
      if (pal_is_bmc_por()) {
        // Get uptime
        clock_gettime(CLOCK_MONOTONIC, &mts);
        // To find out when SLED was on, subtract the uptime from current time
        time_sled_on = ts.tv_sec - mts.tv_sec;

        ctime_r(&time_sled_on, buf);
        // Log an event if this is Power-On-Reset
        syslog(LOG_CRIT, "SLED Powered ON at %s", buf);
      }
    }

    // Store timestamp every one hour to keep track of SLED power
    if (count++ == HB_TIMESTAMP_COUNT) {
      clock_gettime(CLOCK_REALTIME, &ts);
      sprintf(tstr, "%d", ts.tv_sec);
      pal_set_key_value("timestamp_sled", tstr);
      count = 0;
    }
  }
}

// Thread to handle LED state of the server at given slot
static void *
led_handler(void *num) {
  int ret;
  uint8_t prsnt;
  uint8_t power;
  uint8_t pos;
  uint8_t led_blink;
  uint8_t ident = 0;
  int led_on_time, led_off_time;
  char identify[16] = {0};
  char tstr[64] = {0};
  int id_led_on_time = 200;
  int id_led_off_time = 200;
  int power_led_on_time = 500;
  int power_led_off_time = 500;

  uint8_t slot = (*(int*) num) + 1;

#ifdef DEBUG
  syslog(LOG_INFO, "led_handler for slot %d\n", slot);
#endif

  ret = pal_is_server_prsnt(slot, &prsnt);
  if (ret || !prsnt) {
    // Turn off led and exit
    ret = pal_set_led(slot, 0);
    goto led_handler_exit;
  }

  while (1) {

    // Check if this slot needs to be identified
    ident = 0;

    // Check if sled needs to be identified
    memset(identify, 0x0, 16);
    ret = pal_get_key_value("identify_sled", identify);
    if (ret == 0 && !strcmp(identify, "on")) {
       ident = 0x1;
    }

    // Check if slot needs to be identified
    if (!ident) {
      sprintf(tstr, "identify_slot%d", slot);
      memset(identify, 0x0, 16);
      ret = pal_get_key_value(tstr, identify);
      if (ret == 0 && !strcmp(identify, "on")) {
        ident = 0x1;
      }
    }

    if (ident) {
      // Turn OFF Power LED
      pal_set_led(slot, 0);

      // Start blinking the ID LED
      pal_set_id_led(slot, 0);

      msleep(id_led_on_time);

      pal_set_id_led(slot, 1);

      msleep(id_led_off_time);
      continue;
    } else {
      // Turn OFF ID LED
      pal_set_id_led(slot, 1);
    }

    // Get power status for this slot
    ret = pal_get_server_power(slot, &power);
    if (ret) {
      sleep(1);
      continue;
    }

    // Get hand switch position to see if this is selected server
    ret = pal_get_hand_sw(&pos);
    if (ret) {
      sleep(1);
      continue;
    }

    if (pos == HAND_SW_BMC) {
      // Start blinking the ID LED
      pal_set_led(slot, 0);

      msleep(power_led_off_time);

      pal_set_led(slot, 1);

      msleep(power_led_on_time);
      continue;
    }

    if (pos == slot) {
      // This server is selcted one, set led_blink flag
      led_blink = 1;
    } else {
      led_blink = 0;
    }

    if (!led_blink) {
      // Set the led state based on power state
      ret = pal_set_led(slot, power);
      goto led_handler_out;
    }

    // Set blink rate
    if (power) {
      led_on_time = 900;
      led_off_time = 100;
    } else {
      led_on_time = 100;
      led_off_time = 900;
    }

    // Start blinking the LED
    ret = pal_set_led(slot, 1);
    if (ret) {
      goto led_handler_out;
    }

    msleep(led_on_time);

    ret = pal_set_led(slot, 0);
    if (ret) {
      goto led_handler_out;
    }

    msleep(led_off_time);
led_handler_out:
    msleep(100);
  }

led_handler_exit:
  free(num);
}

int
main (int argc, char * const argv[]) {
  pthread_t tid_hand_sw;
  pthread_t tid_debug_card;
  pthread_t tid_rst_btn;
  pthread_t tid_pwr_btn;
  pthread_t tid_hb;
  pthread_t tid_leds[MAX_NUM_SLOTS];
  int i;
  int *ip;
  int rc;
  int pid_file;

  pid_file = open("/var/run/front-paneld.pid", O_CREAT | O_RDWR, 0666);
  rc = flock(pid_file, LOCK_EX | LOCK_NB);
  if(rc) {
    if(EWOULDBLOCK == errno) {
      printf("Another front-paneld instance is running...\n");
      exit(-1);
    }
  } else {
   daemon(0, 1);
   openlog("front-paneld", LOG_CONS, LOG_DAEMON);
  }


  if (pthread_create(&tid_debug_card, NULL, debug_card_handler, NULL) < 0) {
    syslog(LOG_WARNING, "pthread_create for debug card error\n");
    exit(1);
  }
  if (pthread_create(&tid_hand_sw, NULL, usb_handler, NULL) < 0) {
    syslog(LOG_WARNING, "pthread_create for hand switch error\n");
    exit(1);
  }

  if (pthread_create(&tid_rst_btn, NULL, rst_btn_handler, NULL) < 0) {
    syslog(LOG_WARNING, "pthread_create for reset button error\n");
    exit(1);
  }

  if (pthread_create(&tid_pwr_btn, NULL, pwr_btn_handler, NULL) < 0) {
    syslog(LOG_WARNING, "pthread_create for power button error\n");
    exit(1);
  }

  if (pthread_create(&tid_hb, NULL, hb_handler, NULL) < 0) {
    syslog(LOG_WARNING, "pthread_create for heart beat error\n");
    exit(1);
  }

  for (i = 0; i < MAX_NUM_SLOTS; i++) {
    ip = malloc(sizeof(int));
    *ip = i;
    if (pthread_create(&tid_leds[i], NULL, led_handler, (void*)ip) < 0) {
      syslog(LOG_WARNING, "pthread_create for hand switch error\n");
      exit(1);
    }
  }
  pthread_join(tid_debug_card, NULL);
  pthread_join(tid_hand_sw, NULL);
  pthread_join(tid_rst_btn, NULL);
  pthread_join(tid_pwr_btn, NULL);
  pthread_join(tid_hb, NULL);
  for (i = 0;  i < MAX_NUM_SLOTS; i++) {
    pthread_join(tid_leds[i], NULL);
  }

  return 0;
}
