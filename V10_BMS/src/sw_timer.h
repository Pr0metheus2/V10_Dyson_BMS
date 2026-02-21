/*
 * sw_timer.h
 *
 * Created: 21-Jan-26 16:31:23
 * Author : Vladislav Gyurov
 * License: GNU GPL v3 or later
 */ 
#ifndef SW_TIMER_H_
#define SW_TIMER_H_

#include "asf.h"
#include "bms.h"
#include "wdt.h"

typedef uint32_t sw_timer;

#define SW_TIMER_TICK_MS      1
#define SW_TIMER_SERVICES()   { \
  wdt_mainloop();\
}

extern void sw_timer_init(void);
extern void sw_timer_start(sw_timer * sw_timer_ptr);
extern void sw_timer_stop(sw_timer * sw_timer_ptr);
extern bool sw_timer_is_started(sw_timer * sw_timer_ptr);
extern bool sw_timer_is_elapsed(sw_timer * sw_timer_ptr, uint32_t timeout);
extern sw_timer sw_timer_get_elapsed_time(sw_timer * sw_timer_ptr);
extern void sw_timer_delay_ms(uint32_t sw_timer_delay_ms);

#endif /* SW_TIMER_H_ */