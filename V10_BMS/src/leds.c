/*
 * leds.c
 *
 *  Author:  David Pye
 *  Contact: davidmpye@gmail.com
 *  Licence: GNU GPL v3 or later
 */
#include "leds.h"
#include "sw_timer.h"

// Speed of LED sequence.
#define LED_SEQ_TIME 20

// Hardware PWM breathing cadence.
#define LED_BREATHE_STEP_MS 12
#define LED_BREATHE_STEPS 64
#define LED_BREATHE_MIN_DUTY 0
#define LED_BREATHE_MAX_DUTY 255
#define LED_PWM_PERIOD 255

// Battery indicator thresholds are based on the lowest measured cell voltage.
#define LED_BATTERY_SEGMENT_1_MV 3400
#define LED_BATTERY_SEGMENT_2_MV 3800

// Three segments represent thirds of the counted state of charge.
#define LED_BATTERY_SEGMENT_1_SOC_PERCENT 34
#define LED_BATTERY_SEGMENT_2_SOC_PERCENT 67

#define NUM_LEDS 6
uint16_t leds[] = { LED_FILTER, LED_BLOCKED, LED_ERR, LED_BAT_LO, LED_BAT_MED, LED_BAT_HI };

enum led_pwm_mode {
	LED_PWM_NONE = 0,
	LED_PWM_LO,
	LED_PWM_MED,
	LED_PWM_HI,
};

static struct tc_module led_pwm_tc5;
static struct tc_module led_pwm_tc3;
static enum led_pwm_mode led_pwm_mode = LED_PWM_NONE;
static uint8_t led_pwm_phase = 0;
static bool led_pwm_rising = true;

static void leds_gpio_init(void);
static void leds_battery_gpio_set(bool lo, bool med, bool hi);
static void leds_sequence_with_step_time(int step_time_ms);
static void leds_pwm_disable_hw(void);
static void leds_pwm_init(enum led_pwm_mode mode);
static void leds_pwm_set_duty(uint8_t duty);
static uint8_t leds_pwm_duty_from_phase(uint8_t phase);
static void leds_breathe_tick(uint16_t led_pin);
static uint8_t leds_battery_segments_from_voltage(int cell_mv);
static uint8_t leds_battery_segments_from_soc(uint8_t soc_percent);

void leds_init(void) {
	leds_gpio_init();
	leds_off();
}

static void leds_gpio_init(void) {
	struct port_config led_port_config;
	port_get_config_defaults(&led_port_config);
	led_port_config.direction = PORT_PIN_DIR_OUTPUT;
	for (int i = 0; i < NUM_LEDS; ++i) {
		port_pin_set_config(leds[i], &led_port_config);
	}
}

static void leds_battery_gpio_set(bool lo, bool med, bool hi) {
	port_pin_set_output_level(LED_BAT_LO, lo);
	port_pin_set_output_level(LED_BAT_MED, med);
	port_pin_set_output_level(LED_BAT_HI, hi);
}

static uint8_t leds_battery_segments_from_voltage(int cell_mv) {
	if (cell_mv < LED_BATTERY_SEGMENT_1_MV) {
		return 1;
	}
	if (cell_mv < LED_BATTERY_SEGMENT_2_MV) {
		return 2;
	}
	return 3;
}

static uint8_t leds_battery_segments_from_soc(uint8_t soc_percent) {
	if (soc_percent < LED_BATTERY_SEGMENT_1_SOC_PERCENT) {
		return 1;
	}
	if (soc_percent < LED_BATTERY_SEGMENT_2_SOC_PERCENT) {
		return 2;
	}
	return 3;
}

static void leds_pwm_disable_hw(void) {
	if (led_pwm_mode == LED_PWM_NONE) {
		return;
	}

	if (led_pwm_mode == LED_PWM_HI) {
		tc_disable(&led_pwm_tc3);
	}
	else {
		tc_disable(&led_pwm_tc5);
	}

	led_pwm_mode = LED_PWM_NONE;
	led_pwm_phase = 0;
	led_pwm_rising = true;
}

static void leds_pwm_init(enum led_pwm_mode mode) {
	if (led_pwm_mode == mode) {
		return;
	}

	leds_pwm_disable_hw();
	leds_gpio_init();

	struct tc_config config;
	tc_get_config_defaults(&config);
	config.clock_source = GCLK_GENERATOR_0;
	config.counter_size = TC_COUNTER_SIZE_8BIT;
	config.clock_prescaler = TC_CLOCK_PRESCALER_DIV64;
	config.wave_generation = TC_WAVE_GENERATION_NORMAL_PWM;
	config.reload_action = TC_RELOAD_ACTION_GCLK;
	config.waveform_invert_output = 0;
	config.counter_8_bit.value = 0;
	config.counter_8_bit.period = LED_PWM_PERIOD;
	config.counter_8_bit.compare_capture_channel[TC_COMPARE_CAPTURE_CHANNEL_0] = 0;
	config.counter_8_bit.compare_capture_channel[TC_COMPARE_CAPTURE_CHANNEL_1] = 0;
	config.pwm_channel[TC_COMPARE_CAPTURE_CHANNEL_0].enabled = false;
	config.pwm_channel[TC_COMPARE_CAPTURE_CHANNEL_1].enabled = false;

	if (mode == LED_PWM_HI) {
		config.pwm_channel[TC_COMPARE_CAPTURE_CHANNEL_0].enabled = true;
		config.pwm_channel[TC_COMPARE_CAPTURE_CHANNEL_0].pin_out = LED_BAT_HI;
		config.pwm_channel[TC_COMPARE_CAPTURE_CHANNEL_0].pin_mux = PINMUX_PA18F_TC3_WO0;
		tc_init(&led_pwm_tc3, TC3, &config);
		tc_enable(&led_pwm_tc3);
	}
	else {
		if (mode == LED_PWM_LO) {
			config.pwm_channel[TC_COMPARE_CAPTURE_CHANNEL_1].enabled = true;
			config.pwm_channel[TC_COMPARE_CAPTURE_CHANNEL_1].pin_out = LED_BAT_LO;
			config.pwm_channel[TC_COMPARE_CAPTURE_CHANNEL_1].pin_mux = PINMUX_PA25F_TC5_WO1;
		}
		else {
			config.pwm_channel[TC_COMPARE_CAPTURE_CHANNEL_0].enabled = true;
			config.pwm_channel[TC_COMPARE_CAPTURE_CHANNEL_0].pin_out = LED_BAT_MED;
			config.pwm_channel[TC_COMPARE_CAPTURE_CHANNEL_0].pin_mux = PINMUX_PA24F_TC5_WO0;
		}
		tc_init(&led_pwm_tc5, TC5, &config);
		tc_enable(&led_pwm_tc5);
	}

	led_pwm_mode = mode;
	led_pwm_phase = 0;
	led_pwm_rising = true;
}

void leds_pwm_disable(void) {
	leds_pwm_disable_hw();
	leds_gpio_init();
	leds_off();
}

static uint8_t leds_pwm_duty_from_phase(uint8_t phase) {
	uint32_t progress = ((uint32_t)phase * 100U) / (LED_BREATHE_STEPS - 1U);
	// Remap the phase so the first part of the fade is steeper, which shortens
	// the low-end hold while keeping the top end quick.
	if (progress < 35U) {
		progress = (progress * 2U);
	}
	else if (progress < 70U) {
		progress = 70U + (((progress - 35U) * 25U) / 35U);
	}
	else {
		progress = 95U + (((progress - 70U) * 5U) / 30U);
	}
	uint32_t eased = (progress * progress * progress) / 10000U;
	uint32_t duty = LED_BREATHE_MIN_DUTY +
		((eased * (LED_BREATHE_MAX_DUTY - LED_BREATHE_MIN_DUTY)) / 100U);
	return (uint8_t)duty;
}

static void leds_pwm_set_duty(uint8_t duty) {
	if (led_pwm_mode == LED_PWM_HI) {
		tc_set_compare_value(&led_pwm_tc3, TC_COMPARE_CAPTURE_CHANNEL_0, duty);
	}
	else if (led_pwm_mode == LED_PWM_LO) {
		tc_set_compare_value(&led_pwm_tc5, TC_COMPARE_CAPTURE_CHANNEL_1, duty);
	}
	else if (led_pwm_mode == LED_PWM_MED) {
		tc_set_compare_value(&led_pwm_tc5, TC_COMPARE_CAPTURE_CHANNEL_0, duty);
	}
}

static void leds_breathe_tick(uint16_t led_pin) {
	enum led_pwm_mode desired_mode;

	if (led_pin == LED_BAT_HI) {
		desired_mode = LED_PWM_HI;
	}
	else if (led_pin == LED_BAT_MED) {
		desired_mode = LED_PWM_MED;
	}
	else {
		desired_mode = LED_PWM_LO;
	}

	if (led_pwm_mode != desired_mode) {
		leds_pwm_init(desired_mode);
	}

	// Keep non-breathing battery segments on as simple GPIO outputs.
	if (desired_mode == LED_PWM_LO) {
		leds_battery_gpio_set(false, false, false);
	}
	else if (desired_mode == LED_PWM_MED) {
		leds_battery_gpio_set(true, false, false);
	}
	else {
		leds_battery_gpio_set(true, true, false);
	}

	leds_pwm_set_duty(leds_pwm_duty_from_phase(led_pwm_phase));
	sw_timer_delay_ms(LED_BREATHE_STEP_MS);

	if (led_pwm_rising) {
		if (led_pwm_phase < (LED_BREATHE_STEPS - 1U)) {
			++led_pwm_phase;
		}
		else {
			led_pwm_rising = false;
			--led_pwm_phase;
		}
	}
	else {
		if (led_pwm_phase > 0U) {
			--led_pwm_phase;
		}
		else {
			led_pwm_rising = true;
			++led_pwm_phase;
		}
	}
}

static void leds_sequence_with_step_time(int step_time_ms) {
	for (int i = 0; i < NUM_LEDS; ++i) {
		port_pin_set_output_level(leds[i], true);
		sw_timer_delay_ms(step_time_ms);
	}
	for (int i = NUM_LEDS - 1; i >= 0; --i) {
		port_pin_set_output_level(leds[i], false);
		sw_timer_delay_ms(step_time_ms);
	}
}

void leds_sequence(void) {
	leds_sequence_with_step_time(LED_SEQ_TIME);
}

void leds_off(void) {
	for (int i = 0; i < NUM_LEDS; ++i) {
		port_pin_set_output_level(leds[i], false);
	}
}

void leds_on(void) {
	for (int i = 0; i < NUM_LEDS; ++i) {
		port_pin_set_output_level(leds[i], true);
	}
}

void leds_display_battery_voltage(int cell_mv) {
	leds_pwm_disable();
	uint8_t segments = leds_battery_segments_from_voltage(cell_mv);

	leds_battery_gpio_set(segments >= 1, segments >= 2, segments >= 3);
}

void leds_display_battery_soc(uint8_t soc_percent) {
	leds_pwm_disable();
	uint8_t segments = leds_battery_segments_from_soc(soc_percent);

	leds_battery_gpio_set(segments >= 1, segments >= 2, segments >= 3);
}

void leds_flash_charging_voltage_segment(int cell_mv) {
	uint8_t segments = leds_battery_segments_from_voltage(cell_mv);

	if (segments <= 1) {
		leds_breathe_tick(LED_BAT_LO);
	}
	else if (segments == 2) {
		leds_breathe_tick(LED_BAT_MED);
	}
	else {
		leds_breathe_tick(LED_BAT_HI);
	}
}

void leds_flash_charging_soc_segment(uint8_t soc_percent) {
	uint8_t segments = leds_battery_segments_from_soc(soc_percent);

	if (segments <= 1) {
		leds_breathe_tick(LED_BAT_LO);
	}
	else if (segments == 2) {
		leds_breathe_tick(LED_BAT_MED);
	}
	else {
		leds_breathe_tick(LED_BAT_HI);
	}
}

void leds_blink_error_led(int ms) {
	port_pin_set_output_level(LED_ERR, true);
	sw_timer_delay_ms(ms / 2);
	port_pin_set_output_level(LED_ERR, false);
	sw_timer_delay_ms(ms / 2);
}

void leds_flash_error_led(int on_ms) {
	port_pin_set_output_level(LED_ERR, true);
	sw_timer_delay_ms(on_ms);
	port_pin_set_output_level(LED_ERR, false);
}

void leds_show_pack_flat(void) {
	for (int i = 0; i < 5; ++i) {
		port_pin_set_output_level(LED_BAT_LO, true);
		sw_timer_delay_ms(100);
		port_pin_set_output_level(LED_BAT_LO, false);
		sw_timer_delay_ms(100);
	}
}

void leds_show_filter_err_status(bool status) {
	port_pin_set_output_level(LED_FILTER, status);
}

void leds_show_blocked_err_status(bool status) {
	port_pin_set_output_level(LED_BLOCKED, status);
}
