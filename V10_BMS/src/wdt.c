/*
 * bms_wdt.c
 *
 * Created: 14/02/2026 15:06:58
 * Author : Vladislav Gyurov
 * License: GNU GPL v3 or later
 */ 

#include "wdt.h"
#include "sw_timer.h"

#define WDT_TIMER_MS                  100

static sw_timer wdt_timer = 0;

static void wdt_early_warning_callback(void);

void wdt_init(void) {
  struct wdt_conf config_wdt;

	wdt_get_config_defaults(&config_wdt);
  config_wdt.clock_source         = GCLK_GENERATOR_3;
	config_wdt.timeout_period       = WDT_PERIOD_16384CLK;
	config_wdt.early_warning_period = WDT_PERIOD_16384CLK;
	wdt_set_config(&config_wdt);
  wdt_register_callback(wdt_early_warning_callback, WDT_CALLBACK_EARLY_WARNING);
  wdt_enable_callback(WDT_CALLBACK_EARLY_WARNING);

  sw_timer_start(&wdt_timer);
}


void wdt_deinit(void) {
    struct wdt_conf config_wdt;
    wdt_disable_callback(WDT_CALLBACK_EARLY_WARNING);
    wdt_get_config_defaults(&config_wdt);
    config_wdt.enable = false;
    wdt_set_config(&config_wdt);
}

void wdt_mainloop(void) {
  if(sw_timer_is_elapsed(&wdt_timer, WDT_TIMER_MS)) {
    sw_timer_start(&wdt_timer);
    wdt_reset_count();
  }
}

static void wdt_early_warning_callback(void) {
  bq7693_disable_charge();
  bq7693_disable_discharge();

  leds_blink_error_led(10);
}

