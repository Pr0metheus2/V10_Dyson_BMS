/*
 * bms.c
 *
 *  Author:  David Pye
 *  Contact: davidmpye@gmail.com
 *  Licence: GNU GPL v3 or later
 */ 

#include "bms.h"

//We start off idle.
enum BMS_STATE bms_state = BMS_IDLE;

//If a fault occurs, it'll be lodged here.
enum BMS_ERROR_CODE bms_error = BMS_ERR_NONE;

// Wake-from-sleep needs a short grace window so the first post-sleep BQ reads
// do not trip false fault flashes before discharge is fully settled.
static bool bms_woke_from_sleep = false;
static bool bms_wake_banner_shown = false;
static bool bms_wake_skip_undertemp_once = false;
static bool bms_wake_skip_charge_undertemp_once = false;
static bool bms_wake_skip_charge_cell_fail_once = false;
static bool bms_wake_skip_flat_fault_once = false;
// Retained for testing if post-wake LED refresh suppression is re-enabled.
static uint8_t bms_wake_discharge_led_skip_count = 0;
static uint8_t bms_wake_running_grace_loops = 0;

// #define BMS_WAKE_SKIP_DISCHARGE_LED_DISPLAYS 3

// Re-check undertemp periodically during discharge so real thermistor faults
// still get caught without paying the cost on every 60 ms loop.
#define DISCHARGE_UNDERTEMP_RECHECK_LOOPS 17

// BQ76930 datasheet note: after leaving SHIP/NORMAL wake, allow 400 ms before
// reading initial cell voltages.
#define BQ76930_WAKE_SETTLE_MS 400

#define CELL_BALANCE_TOLERANCE_MV 50
#define CELL_BALANCE_FLASH_STEP_MV 50
#define CELL_BALANCE_INITIAL_DELAY_MS 250
#define CELL_BALANCE_FLASH_ON_MS 250
#define CELL_BALANCE_FLASH_OFF_MS 250

#ifdef SERIAL_DEBUG
char *bms_state_names[] = {
	"BMS_IDLE",
	"BMS_CHARGER_CONNECTED",
	"BMS_CHARGING",
	"BMS_CHARGER_CONNECTED_NOT_CHARGING",
	"BMS_CHARGER_UNPLUGGED",
	"BMS_TRIGGER_PULLED",
	"BMS_DISCHARGING",
	"BMS_FAULT",
	"BMS_SLEEP"
};
#endif

extern volatile struct eeprom_data eeprom_data;

static uint16_t bms_get_lowest_cell_voltage(void) {
	uint16_t *cell_voltages = bq7693_get_cell_voltages();
	uint16_t lowest_cell_voltage = cell_voltages[0];

	for (int i = 1; i < 7; ++i) {
		if (cell_voltages[i] < lowest_cell_voltage) {
			lowest_cell_voltage = cell_voltages[i];
		}
	}

	return lowest_cell_voltage;
}

static bool bms_get_charge_soc_percent(uint8_t *soc_percent) {
	int32_t total_capacity = eeprom_data.total_pack_capacity;
	int32_t charge_level = eeprom_data.current_charge_level;

	if (total_capacity <= 0) {
		return false;
	}
	if (charge_level <= 0) {
		*soc_percent = 0;
		return true;
	}
	if (charge_level >= total_capacity) {
		*soc_percent = 100;
		return true;
	}

	*soc_percent = (uint8_t)((charge_level * 100L) / total_capacity);
	return true;
}

static void bms_show_cell_balance(void) {
	uint16_t *cell_voltages = bq7693_get_cell_voltages();
	uint16_t lowest_cell_voltage = cell_voltages[0];
	uint16_t highest_cell_voltage = cell_voltages[0];

	for (int i = 1; i < 7; ++i) {
		if (cell_voltages[i] < lowest_cell_voltage) {
			lowest_cell_voltage = cell_voltages[i];
		}
		if (cell_voltages[i] > highest_cell_voltage) {
			highest_cell_voltage = cell_voltages[i];
		}
	}

	uint16_t cell_spread_mv = highest_cell_voltage - lowest_cell_voltage;
	if (cell_spread_mv <= CELL_BALANCE_TOLERANCE_MV) {
		return;
	}

	uint8_t flashes = cell_spread_mv / CELL_BALANCE_FLASH_STEP_MV;
	sw_timer_delay_ms(CELL_BALANCE_INITIAL_DELAY_MS);
	for (uint8_t i = 0; i < flashes; ++i) {
		leds_flash_error_led(CELL_BALANCE_FLASH_ON_MS);
		if (i + 1 < flashes) {
			sw_timer_delay_ms(CELL_BALANCE_FLASH_OFF_MS);
		}
	}
}

static void bms_prepare_wake_from_sleep(void) {
	// Wake from sleep should restore output immediately. The BQ76930 is then
	// given settle time in the discharge loop before safety reads are trusted.
	bms_wake_skip_undertemp_once = true;
	bms_wake_skip_charge_undertemp_once = true;
	bms_wake_skip_charge_cell_fail_once = true;
	bms_wake_skip_flat_fault_once = true;
	// bms_wake_discharge_led_skip_count = BMS_WAKE_SKIP_DISCHARGE_LED_DISPLAYS;
	// About 400 ms total settle is handled after discharge is enabled.
	bms_wake_running_grace_loops = 7;
}

static void bms_prepare_wake_for_charge(void) {
	//Wake from sleep can leave the first charge-side readings stale.
	//BQ76930 requires 400 ms before initial cell-voltage reads after wake.
	sw_timer_delay_ms(BQ76930_WAKE_SETTLE_MS);
	(void)bq7693_get_cell_voltages();
	(void)bq7693_read_temperature();
	bms_wake_skip_charge_undertemp_once = true;
}

static bool bms_consume_wake_running_grace(void) {
	if (bms_wake_running_grace_loops == 0) {
		return false;
	}

	--bms_wake_running_grace_loops;
	return true;
}

static bool bms_should_check_undertemp_now(uint8_t *undertemp_check_loops) {
	if (*undertemp_check_loops == 0) {
		*undertemp_check_loops = DISCHARGE_UNDERTEMP_RECHECK_LOOPS;
		return true;
	}

	--(*undertemp_check_loops);
	return false;
}

void pins_init() {
	//Set up the output charge pin
	struct port_config charge_pin_config;
	port_get_config_defaults(&charge_pin_config);
	charge_pin_config.direction = PORT_PIN_DIR_OUTPUT;
	port_pin_set_config(ENABLE_CHARGE_PIN, &charge_pin_config);
	port_pin_set_output_level(ENABLE_CHARGE_PIN, false);
	
	//Two input pins, CHARGER and TRIGGERs
	struct port_config sense_pin_config;
	port_get_config_defaults(&sense_pin_config);
	sense_pin_config.direction = PORT_PIN_DIR_INPUT;
	sense_pin_config.input_pull = PORT_PIN_PULL_NONE;
	port_pin_set_config(CHARGER_CONNECTED_PIN, &sense_pin_config);
	port_pin_set_config(TRIGGER_PRESSED_PIN, &sense_pin_config);
}

volatile int32_t currentmA;

static void bms_sleep_wake_callback(void) {
	// The wake input is handled after system_sleep() returns.
}

static void bms_configure_sleep_wake_source(uint8_t channel, uint32_t gpio_pin,
		uint32_t gpio_pin_mux) {
	struct extint_chan_conf config_extint_chan;
	extint_chan_get_config_defaults(&config_extint_chan);
	config_extint_chan.gpio_pin = gpio_pin;
	config_extint_chan.gpio_pin_mux = gpio_pin_mux;
	config_extint_chan.gpio_pin_pull = EXTINT_PULL_NONE;
	config_extint_chan.detection_criteria = EXTINT_DETECT_RISING;

	extint_chan_set_config(channel, &config_extint_chan);
	extint_chan_clear_detected(channel);
	extint_register_callback(bms_sleep_wake_callback, channel, EXTINT_CALLBACK_TYPE_DETECT);
	extint_chan_enable_callback(channel, EXTINT_CALLBACK_TYPE_DETECT);
}

static void bms_configure_sleep_wake_sources(void) {
	bms_configure_sleep_wake_source(4, PIN_PA04A_EIC_EXTINT4,
		MUX_PA04A_EIC_EXTINT4);
	bms_configure_sleep_wake_source(6, PIN_PA06A_EIC_EXTINT6,
		MUX_PA06A_EIC_EXTINT6);
}
	
void bms_interrupt_callback(void) {
	uint8_t sys_stat;
	bq7693_read_register(SYS_STAT, 1, &sys_stat);
	if (sys_stat & 0x80) {
		//Got a coulomb charger count ready.
		int32_t ccVal = bq7693_read_cc();
		
		//This needs better handling....
		currentmA = ccVal*8.44f;
			
		//Ignore tiny values.
		if ( (ccVal > 0 && ccVal > 2)  || (ccVal < 0 && ccVal < -2) )  {
			ccVal *= 8.44f; //8.44microVolts per LSB.
			//i = V/R
			//sense resistor = 1mOhm
			//microV / milliOhms gives current in mA.   
			//so ccVal has current in mA.
			//Dividing by 14400 would give mAH. (number of 250mS periods in 1 hr.
			//Dividing by 14.4 will give microAH (what we want)
			ccVal /= 14.4f;
		
			eeprom_data.current_charge_level += ccVal;
						
			//We thought the pack was full, but it's still charging, so we need to update its' size.		
			if (eeprom_data.current_charge_level > eeprom_data.total_pack_capacity) {
				eeprom_data.total_pack_capacity = eeprom_data.current_charge_level;
			}
		
			//We thought the pack was empty, but it isn't, so again, we need to update our estimate of what it can hold!
			if (eeprom_data.current_charge_level < 0) {
				//subtracting negative numbers will increment the pack capacity.
				eeprom_data.total_pack_capacity -= eeprom_data.current_charge_level;
				eeprom_data.current_charge_level = 0;
			}
		}
		//Update the CC bit so it'll refire in another 250mS as per datasheet.
		bq7693_write_register(SYS_STAT, 0x80);//Clear CC bit.
	}	
}
	
void interrupts_init() {
	//A single interrupt, focussed on the BQ7693's alert line (PA28), which
	//is on EXTINT 8.
	struct extint_chan_conf config_extint_chan;
	extint_chan_get_config_defaults(&config_extint_chan);	
	config_extint_chan.gpio_pin        = 	PIN_PA28A_EIC_EXTINT8;
	config_extint_chan.gpio_pin_mux =       MUX_PA28A_EIC_EXTINT8;
	//This line is designed to be possible for either device to pull it up or down to indicate a fault condition, so
	//no pullups.
	config_extint_chan.gpio_pin_pull      = EXTINT_PULL_NONE;
	config_extint_chan.detection_criteria = EXTINT_DETECT_RISING;
	config_extint_chan.wake_if_sleeping = false;
	
	extint_chan_set_config(8, &config_extint_chan);
	extint_register_callback(bms_interrupt_callback, 8, EXTINT_CALLBACK_TYPE_DETECT);
	extint_chan_enable_callback(8, EXTINT_CALLBACK_TYPE_DETECT);
	//Enable interrupts.	
	system_interrupt_enable_global();
}

void bms_init() {
	//sets up clocks/IRQ handlers etc.
	system_init();
	//Initialise the delay system
	delay_init();

	//Initialise the timer system
	sw_timer_init();

	//Set up the pins
	pins_init();

	// EEPROM initialization can signal errors through LEDs, so initialize them first.
	leds_init();
	//Init eeprom emulator
	eeprom_init();
	eeprom_read();
	bms_woke_from_sleep = eeprom_consume_sleep_wakeup_flag();
	
	//BQ7693 init
	bq7693_init();

	// A true power-up may check cell voltage immediately on trigger. A sleep wake
	// keeps its existing post-output grace window, so it can enable output now.
	if (!bms_woke_from_sleep) {
		delay_ms(100);
	}
	
#ifdef SERIAL_DEBUG
	serial_debug_init();
#endif
	
	//Initialise the USART we need to talk to the vacuum cleaner
	serial_init();	
	
	//Only show the welcome sequence on true power-up or reset, not on a wake from sleep.
	if (!bms_woke_from_sleep && eeprom_should_show_startup_sequence()) {
		leds_sequence();
	}
	
	//Enable interrupts
	interrupts_init();

	//Enable watchdog
	wdt_init();
}
	
bool bms_is_safe_to_discharge(bool check_undertemp) {
	//Clear error status.
	bms_error = BMS_ERR_NONE;
	bool skip_wake_flat_fault = false;

	if (bms_wake_skip_flat_fault_once) {
		bms_wake_skip_flat_fault_once = false;
		skip_wake_flat_fault = true;
	}
	
	uint16_t *cell_voltages = bq7693_get_cell_voltages();
	//Check any cells undervolt.
	for (int i=0; i<7;++i) {
		if (cell_voltages[i] < CELL_LOWEST_DISCHARGE_VOLTAGE) {
			if (skip_wake_flat_fault) {
				continue;
			}
			bms_error = BMS_ERR_PACK_DISCHARGED;
			
#ifdef SERIAL_DEBUG
			sprintf(debug_msg_buffer, "%s: Cell voltages too low\r\n", __FUNCTION__);
			serial_debug_send_message(debug_msg_buffer);
				
			for (int i=0; i<7; ++i) {
				sprintf(debug_msg_buffer, "Cell %d: %d mV, min %d mV\r\n", i, cell_voltages[i], CELL_LOWEST_DISCHARGE_VOLTAGE);
				serial_debug_send_message(debug_msg_buffer);
			}
#endif		
		}
	}
	//Check pack temperature remains in acceptable range.
	int temp = bq7693_read_temperature();
	if (temp/10  > MAX_PACK_TEMPERATURE) {
		bms_error = BMS_ERR_PACK_OVERTEMP;
		
#ifdef SERIAL_DEBUG
		sprintf(debug_msg_buffer, "%s : Pack overtemp %d 'C, max %d\r\n",__FUNCTION__ ,  temp/10, MAX_PACK_TEMPERATURE);
		serial_debug_send_message(debug_msg_buffer);
#endif

	}
	else if (temp/10 < MIN_PACK_DISCHARGE_TEMP) {
		if (bms_wake_skip_undertemp_once) {
			bms_wake_skip_undertemp_once = false;
		}
		else if (check_undertemp) {
			bms_error = BMS_ERR_PACK_UNDERTEMP;

#ifdef SERIAL_DEBUG
			sprintf(debug_msg_buffer, "%s: Pack undertemp %d 'C, min %d\r\n",__FUNCTION__ , temp/10, MIN_PACK_DISCHARGE_TEMP);
			serial_debug_send_message(debug_msg_buffer);
#endif
		}
	}
	
	//Check sys_stat	
	uint8_t sys_stat;
	bq7693_read_register(SYS_STAT, 1, &sys_stat);

	if (sys_stat & 0x01) 	{
		bms_error = BMS_ERR_OVERCURRENT;
		bq7693_write_register(SYS_STAT, 0x01);

#ifdef SERIAL_DEBUG
		sprintf(debug_msg_buffer, "%s: BMS IC Overcurrent Trip\r\n", __FUNCTION__);
		serial_debug_send_message(debug_msg_buffer);
#endif

	}
	else if (sys_stat & 0x02) {
		bms_error = BMS_ERR_SHORTCIRCUIT;
		bq7693_write_register(SYS_STAT, 0x02);

#ifdef SERIAL_DEBUG
		sprintf(debug_msg_buffer, "%s: Short Circuit Trip\r\n", __FUNCTION__);
		serial_debug_send_message(debug_msg_buffer);
#endif	

	}
	else if (sys_stat & 0x08) {
		if (skip_wake_flat_fault) {
			bq7693_write_register(SYS_STAT, 0x08);
		}
		else {
			bms_error = BMS_ERR_UNDERVOLTAGE;
			bq7693_write_register(SYS_STAT, 0x08);

#ifdef SERIAL_DEBUG
			sprintf(debug_msg_buffer, "%s: Undervoltage Trip\r\n", __FUNCTION__);
			serial_debug_send_message(debug_msg_buffer);
#endif
		}
	}	

	if (bms_error == BMS_ERR_NONE) {
		return true;
	}
	else return false;
	
}

bool bms_is_safe_to_charge() {
	//Clear error status.
	bms_error = BMS_ERR_NONE;
	
	uint16_t *cell_voltages = bq7693_get_cell_voltages();
	
	//Check no cells are so flat they cannot be charged.
	for (int i=0; i<7;++i) {
		if ( cell_voltages[i] < CELL_LOWEST_CHARGE_VOLTAGE ) {
			if (bms_wake_skip_charge_cell_fail_once) {
				bms_wake_skip_charge_cell_fail_once = false;
				continue;
			}
			bms_error = BMS_ERR_CELL_FAIL;	

#ifdef SERIAL_DEBUG
		sprintf(debug_msg_buffer, "%s: Cell %d @ %dV (min %dV)\r\n", __FUNCTION__, i, cell_voltages[i], CELL_LOWEST_CHARGE_VOLTAGE);
		serial_debug_send_message(debug_msg_buffer);
#endif

		}
	}

	//Check pack temperature acceptable (<=60'C)	
	int temp = bq7693_read_temperature();
	if (temp/10  > MAX_PACK_TEMPERATURE) {
		bms_error = BMS_ERR_PACK_OVERTEMP;
	}
	else if (temp/10 < MIN_PACK_CHARGE_TEMP) {
		if (bms_wake_skip_charge_undertemp_once) {
			bms_wake_skip_charge_undertemp_once = false;
		}
		else {
			bms_error = BMS_ERR_PACK_UNDERTEMP;
		}
	}
	
	//Check sys_stat
	uint8_t sys_stat;
	bq7693_read_register(SYS_STAT, 1, &sys_stat);
	if (sys_stat & 0x01) 	{
		bms_error = BMS_ERR_OVERCURRENT;
		bq7693_write_register(SYS_STAT, 0x01);
	}
	else if (sys_stat & 0x04) {
		bms_error = BMS_ERR_OVERVOLTAGE;
		bq7693_write_register(SYS_STAT, 0x04);
	}
	
	if (bms_error == BMS_ERR_NONE) {
		return true;
	}
	else return false;
}

bool bms_is_pack_full() {
	uint16_t *cell_voltages = bq7693_get_cell_voltages();

#ifdef SERIAL_DEBUG
	for (int i=0; i<7; ++i) {
		char message[40];
		sprintf(message, "Cell %d: %d mV, target %d mV\r\n", i, cell_voltages[i], CELL_FULL_CHARGE_VOLTAGE);
	}
#endif

	//If any cells are at their full charge voltage, we are full.
	for (int i=0; i<7;++i) {
		if (cell_voltages[i] >= CELL_FULL_CHARGE_VOLTAGE ) {
			return true;
		}
	}
	return false;
}


void bms_handle_idle() {
	//Three potential ways out of this state - someone pulls the trigger, plugs in a charger, or the IDLE_TIME is exceeded and we go to sleep.
	for (int i=0; i<  (IDLE_TIME * 1000) / 50; ++i) {
		if (port_pin_get_input_level(CHARGER_CONNECTED_PIN) == true) {
			bms_state = BMS_CHARGER_CONNECTED;
			return;
		}
		else if (port_pin_get_input_level(TRIGGER_PRESSED_PIN) == true) {
			bms_state = BMS_TRIGGER_PULLED;
			return;
		}
		sw_timer_delay_ms(50);
	}	
	//Reached the end of our wait loop, with nobody pulling the trigger, or plugging in charger.
	//Transit to sleep state
	bms_state = BMS_SLEEP;
}

void bms_handle_trigger_pulled() {
	if (bms_woke_from_sleep) {
		// Wake-up is handled by the discharge loop so output comes up first.
		// Use the EEPROM-saved coulomb-counter SoC immediately. This matches the
		// later discharge display while BQ readings are still settling.
		uint8_t soc_percent;
		if (bms_get_charge_soc_percent(&soc_percent)) {
			leds_display_battery_soc(soc_percent);
		}
		else if (eeprom_data.lowest_cell_voltage >= CELL_LOWEST_CHARGE_VOLTAGE &&
			eeprom_data.lowest_cell_voltage <= CELL_OVERVOLTAGE_TRIP) {
			leds_display_battery_voltage(eeprom_data.lowest_cell_voltage);
		}
		bms_prepare_wake_from_sleep();
		bms_woke_from_sleep = false;
		bms_state = BMS_DISCHARGING;
		return;
	}

	//Check if it's safe to discharge or not.
	if (bms_is_safe_to_discharge(true)) {
		//All go - unleash the power!
		bms_state = BMS_DISCHARGING;
	}
	else {
		bms_state = BMS_FAULT;
	}
}

void bms_handle_sleep() {
	// Store pack charge data and the last valid LED voltage before sleeping.
#ifdef SERIAL_DEBUG
	serial_debug_send_message("Entering sleep - pack cell voltages:\r\n");
	serial_debug_send_cell_voltages();
#endif
	eeprom_data.lowest_cell_voltage = bms_get_lowest_cell_voltage();
	eeprom_write();
	eeprom_mark_sleep_wakeup();

	// A watchdog reset is not a valid sleep wake-up. Only trigger or charger
	// activity should restart the BMS after standby.
	wdt_deinit();
	bms_configure_sleep_wake_sources();
	bq7693_enter_sleep_mode();
	system_set_sleepmode(SYSTEM_SLEEPMODE_STANDBY);
	system_sleep();

	// Reinitialize all BQ and peripheral state after a valid standby wake-up.
	system_reset();
}

void bms_handle_discharging() {		
	
#ifdef SERIAL_DEBUG
	serial_debug_send_message("Starting discharge\r\n");
#endif
	//Enable the discharge path first so the output is available before we spend
	//time refreshing the indicator LEDs.
	bq7693_enable_discharge();
	//Reset the UART message counter;
	serial_reset_message_counter();
	// Skip the first wake loops so the BQ76930 can settle after output enable
	// before the first discharge safety reads are trusted.
	uint8_t undertemp_check_loops = DISCHARGE_UNDERTEMP_RECHECK_LOOPS;
	
	while (1) {
		
#ifdef SERIAL_DEBUG
		sprintf(debug_msg_buffer,"Discharging at %d mA, %d mAH, capacity %d mAH, Temp %d'C\r\n", currentmA*-1, eeprom_data.current_charge_level/1000, eeprom_data.total_pack_capacity/1000,
		bq7693_read_temperature()/10);
		serial_debug_send_message(debug_msg_buffer);
#endif
		if (port_pin_get_input_level(CHARGER_CONNECTED_PIN)) {
			//Charger insertion must override discharge immediately.
			bq7693_disable_discharge();
			leds_off();
			bms_state = BMS_CHARGER_CONNECTED;
			return;
		}
		if (!port_pin_get_input_level(TRIGGER_PRESSED_PIN)) {
			//Trigger released.
			bq7693_disable_discharge();
			//Clear the battery status etc.
			leds_off();
			bms_state = BMS_IDLE;
			return;
		}
		if (bms_consume_wake_running_grace()) {
			//Deliberately skip the first post-wake safety checks.
		}
		else
		{
		bool check_undertemp_now = bms_should_check_undertemp_now(&undertemp_check_loops);

		if (!bms_is_safe_to_discharge(check_undertemp_now)) {
			//A fault has occurred.
			bq7693_disable_discharge();
			bms_state = BMS_FAULT;
			return;
		}
		}
		
		// Keep the EEPROM-derived indicator visible until BQ wake readings settle.
		if (bms_wake_running_grace_loops == 0) {
			if (bms_wake_discharge_led_skip_count > 0) {
				--bms_wake_discharge_led_skip_count;
			}
			else {
				uint8_t soc_percent;
				if (bms_get_charge_soc_percent(&soc_percent)) {
					leds_display_battery_soc(soc_percent);
				}
				else {
					leds_display_battery_voltage(bms_get_lowest_cell_voltage());
				}
			}
		}
		
		//Send the USART traffic we need to supply to keep the cleaner running
		serial_send_next_message();
		sw_timer_delay_ms(60);
	}
}

void bms_handle_fault() {
	//Turn all the LEDs off.
	leds_off();
	
	//Show the error status and continue to show it, until trigger released and charger unplugged.
	do {
		if (bms_error == BMS_ERR_PACK_DISCHARGED || bms_error == BMS_ERR_UNDERVOLTAGE ) {
			//If the problem is just a flat pack, blink the lowest battery segment three times.
			leds_show_pack_flat();

			//We also need to update the pack capacity as it's flat at this point.
			if (eeprom_data.current_charge_level > 0) {
				eeprom_data.total_pack_capacity -= eeprom_data.current_charge_level;
				eeprom_data.current_charge_level = 0;				
			}
		}
		else {
			//Flash the red error led the number of times indicated by the fault code.
			for (int i=0; i<bms_error; ++i) {
				leds_blink_error_led(500);
			}
			sw_timer_delay_ms(2000);
		}
	} 
	while (port_pin_get_input_level(TRIGGER_PRESSED_PIN) || port_pin_get_input_level(CHARGER_CONNECTED_PIN));
		
	//Return to idle
	bms_state = BMS_IDLE;	
}

void bms_handle_charger_connected() {
	if (bms_woke_from_sleep) {
		bms_prepare_wake_for_charge();
		bms_woke_from_sleep = false;
	}

	if (bms_is_pack_full()) {
		//If the pack is full, transit to idle.
		bms_state = BMS_IDLE;
	}
	else if (bms_is_safe_to_charge()) {
		bms_state = BMS_CHARGING;
	}
	else {
		bms_state = BMS_FAULT;
	}
}

void bms_handle_charger_connected_not_charging() {
	leds_display_battery_voltage(CELL_FULL_CHARGE_VOLTAGE);
	// Wait for a final charger removal while showing the full battery level.
	//If not, to sleep.
	for (int i=0; i<FULL_CHARGE_DISPLAY_TIMEOUT_SECONDS; ++i) {
		if (!port_pin_get_input_level(CHARGER_CONNECTED_PIN)) {
			leds_off();
			bms_state = BMS_CHARGER_UNPLUGGED;
			return;
		}		
		sw_timer_delay_ms(1000);
	}
	//Sleep then!
	bms_state = BMS_SLEEP;
}

void bms_handle_charging() {
	//Sanity check...
	if (!bms_is_safe_to_charge()) {
		bms_state = BMS_FAULT;
		return;
	}
	//Enable charging.
	port_pin_set_output_level(ENABLE_CHARGE_PIN, true);
	 //Enable the charge FET in the BQ7693.
	bq7693_enable_charge();
	
	int charge_pause_counter = 0;
	while (1) {
		//Charging now in progress.		
		//Show the flashing segment selected by the counted state of charge.
		uint8_t soc_percent;
		if (bms_get_charge_soc_percent(&soc_percent)) {
			leds_flash_charging_soc_segment(soc_percent);
		}
		else {
			leds_flash_charging_voltage_segment(bms_get_lowest_cell_voltage());
		}
	
#ifdef SERIAL_DEBUG
		sprintf(debug_msg_buffer,"Charging at %d mA, %d mAH, capacity %d mAH, Temp %d'C\r\n", currentmA, eeprom_data.current_charge_level/1000, eeprom_data.total_pack_capacity/1000, 
		bq7693_read_temperature()/10);
		serial_debug_send_message(debug_msg_buffer);	
#endif
		if (!bms_is_safe_to_charge()) {
			//Safety error.
			port_pin_set_output_level(ENABLE_CHARGE_PIN, false);
			bq7693_disable_charge();

			leds_pwm_disable();
			leds_off();
			bms_state = BMS_FAULT;
			return;
		}
				
		if ( !port_pin_get_input_level(CHARGER_CONNECTED_PIN)) {
			//Charger unplugged.
			//Turn off charging
			port_pin_set_output_level(ENABLE_CHARGE_PIN, false);
			bq7693_disable_charge();

			leds_pwm_disable();
			leds_off();
			bms_state = BMS_CHARGER_UNPLUGGED;
			return;
		}
				
		if (bms_is_pack_full()) {
#ifdef SERIAL_DEBUG
			sprintf(debug_msg_buffer, "Charging paused - cell full, attempt %d of %d\r\n", charge_pause_counter, FULL_CHARGE_PAUSE_COUNT);
			serial_debug_send_message(debug_msg_buffer);			
			serial_debug_send_cell_voltages();
#endif
			//Pause the charging.
			port_pin_set_output_level(ENABLE_CHARGE_PIN, false);
			bq7693_disable_charge();
		
			//Delay for 30 seconds, then go and try again.	
			for (int i=0; i<30; ++i) {
				//This function takes a second.
				if (bms_get_charge_soc_percent(&soc_percent)) {
					leds_flash_charging_soc_segment(soc_percent);
				}
				else {
					leds_flash_charging_voltage_segment(bms_get_lowest_cell_voltage());
				}
				//Check the charger hasn't been unplugged while we're waiting
				//If it has, abandon the charge process and return to main loop
				if (!port_pin_get_input_level(CHARGER_CONNECTED_PIN)) {
					//Charger's been unplugged.
					leds_pwm_disable();
					leds_off();
					bms_state = BMS_CHARGER_UNPLUGGED;
					return;
				}
			}			
			charge_pause_counter++;	
			//Restart charging	
			port_pin_set_output_level(ENABLE_CHARGE_PIN, true);
			bq7693_enable_charge();
		}
		
		if (charge_pause_counter == FULL_CHARGE_PAUSE_COUNT) {
			//After FULL_CHARGE_PAUSE_COUNT pauses, we are full.
			//Disable the charging
			port_pin_set_output_level(ENABLE_CHARGE_PIN, false);
			bq7693_disable_charge();
			
			leds_pwm_disable();
			leds_display_battery_soc(100);

			bms_state = BMS_CHARGER_CONNECTED_NOT_CHARGING;

			//Set charge level to equal capacity.
			eeprom_data.total_pack_capacity = eeprom_data.current_charge_level;

#ifdef SERIAL_DEBUG
			serial_debug_send_message("Charging stopped - cells at capacity\r\n");
			char message[40];
			sprintf(message, "Total pack capacity %dmAh\r\n", eeprom_data.total_pack_capacity/1000);
			serial_debug_send_message(message);
#endif
			return;	
		}			
	}
}

void bms_handle_charger_unplugged() {
	//Show the startup sequence when the charger is removed.
	leds_pwm_disable();
	leds_off();
	leds_sequence();
	// One short red flash per 50 mV of cell-voltage spread above tolerance.
	bms_show_cell_balance();

#ifdef SERIAL_DEBUG
	serial_debug_send_message("Charger unplugged\r\n");
	serial_debug_send_cell_voltages();
#endif

	bms_state = BMS_IDLE;
}


void bms_mainloop() {
	//Handle the state machinery.
	while (1) {
		
#ifdef SERIAL_DEBUG
		if (bms_woke_from_sleep) {
			if (!bms_wake_banner_shown) {
				serial_debug_send_message("\r\nDyson V10 BMS Aftermarket firmware v" FIRMWARE_VERSION_STRING "\r\n");
				bms_wake_banner_shown = true;
			}
		}
		sprintf(debug_msg_buffer, "%s: Entering state %s\r\n", __FUNCTION__, bms_state_names[bms_state]);
		serial_debug_send_message(debug_msg_buffer);
#endif
		
		switch (bms_state) {
			case BMS_IDLE:
				bms_handle_idle();
				break;
			case BMS_SLEEP:
				bms_handle_sleep();
				break;	
			case BMS_TRIGGER_PULLED:
				bms_handle_trigger_pulled();
				break;
			case BMS_CHARGER_CONNECTED:
				bms_handle_charger_connected();
				break;	
			case BMS_CHARGING:
				bms_handle_charging();
				break;
			case BMS_CHARGER_CONNECTED_NOT_CHARGING:
				bms_handle_charger_connected_not_charging();
				break;
			case BMS_CHARGER_UNPLUGGED:
				bms_handle_charger_unplugged();
				break;
			case BMS_DISCHARGING:
				bms_handle_discharging();
				break;
			case BMS_FAULT:
				bms_handle_fault();
				break;
		}
	}
}
