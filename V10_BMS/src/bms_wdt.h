/*
 * wdt.h
 *
 * Created: 14/02/2026 15:06:44
 * Author : Vladislav Gyurov
 * License: GNU GPL v3 or later
 */ 
 
#ifndef WDT_H_
#define WDT_H_
#include "asf.h"

#include "bq7693.h"

extern void wdt_init(void);
extern void wdt_deinit(void);
extern void wdt_mainloop(void);
extern bool wdt_is_initialized(void);

#endif /* BMS_WDT_H_ */
