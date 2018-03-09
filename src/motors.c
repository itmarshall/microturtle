/*
 * motors.c: Control routines for the micro-turtle's motors.
 *
 * Author: Ian Marshall
 * Date: 2/01/2018
 */
#include "esp8266.h"
#include "ets_sys.h"
#include "osapi.h"
#include "mem.h"
#include "gpio.h"
#include "pwm.h"

#include "motors.h"

#define PWM_PERIOD 20000 // 20ms
#define PWM_MIN 22222    // 1ms
#define PWM_MAX 44444    // 2ms
#define SERVO_UP_ANGLE -90
#define SERVO_DOWN_ANGLE -90

// Absolute value macro.
#define ABS(x) (((x) < 0) ? -(x) : (x));

// The number of stepper motors that this program is using.
#define STEPPER_MOTOR_COUNT 2

// The number of milliseconds per tick of the motor timer.
#define MOTOR_TICK_INTERVAL 1

// The maximum number of timer ticks that must pass without movement before the motors are turned 
// off to save energy. This value is equal to 5 seconds.
#define MAX_IDLE_COUNT 5000

// Structure used to hold information for the internal workigs of the motor movement.
// This data is per-motor.
typedef struct {
	int32_t steps;
	int32_t d;
	int32_t step;
	int32_t last_step;
	uint8_t direction;
} tick_data_t;

// Stepper data used in the control of motor movement.
LOCAL tick_data_t stepper_data[STEPPER_MOTOR_COUNT];

// The current tick that the motor control is up to.
LOCAL uint32_t current_tick = 0;

// The total number of ticks that the motor control's current sequence.
LOCAL uint32_t total_ticks = 0;

// The callback function to call when the motor control's sequence is complete.
LOCAL motor_callback_t *motor_cb = NULL;

// The total number of ticks for the next motor control sequence.
LOCAL int32_t next_total_ticks = 0;

// The number of steps of each stepper motor for the next motor control sequence.
LOCAL int32_t next_steps[STEPPER_MOTOR_COUNT] = {0, 0};

// The timer used for moving the stepper motors.
LOCAL os_timer_t motor_timer;

// S1 - GPIO 0,  2,  4,  5
// S2 - GPIO 3, 12, 13, 14
#define STEP_SEQUENCE_COUNT 8
#define STEPPER_1_MASK (BIT0 | BIT2  | BIT4  | BIT5)
#define STEPPER_2_MASK (BIT3 | BIT12 | BIT13 | BIT14)
LOCAL uint32_t step_values[STEPPER_MOTOR_COUNT][STEP_SEQUENCE_COUNT] = {
	{
	 // Stepper 1.
	 BIT0,
	 BIT0 | BIT2,
	 BIT2,
	 BIT2 | BIT4,
	 BIT4,
	 BIT4 | BIT5,
	 BIT5,
	 BIT5 | BIT0
	}, {
	 // Stepper 2.
	 BIT3,
	 BIT3 | BIT12,
	 BIT12,
	 BIT12 | BIT13,
	 BIT13,
	 BIT13 | BIT14,
	 BIT14,
	 BIT14 | BIT3
	}
};

// The current step in the step sequence for each motor.
LOCAL int8_t current_step[STEPPER_MOTOR_COUNT] = {0, 0};

/*
 * Sets the servo's position, in the range of [-90 90]. Angle is in degrees.
 */
LOCAL void ICACHE_FLASH_ATTR set_servo(int8_t position) {
	// Ensure the position is in the range of [-90 90] degrees.
	if (position < -90) {
		position = -90;
	} else if (position > 90) {
		position = 90;
	}

	// Calculate the duty cycle to keep it between 1ms (-90 degs) and 2ms (+90 degs).
	uint32_t pwm_duty = ((uint32_t)(position + 90) * (PWM_MAX - PWM_MIN) / 180) + PWM_MIN;

	// Set the new PWM duty cycle.
	pwm_set_duty(pwm_duty, 0);
	pwm_start();
}

/*
 * Sets the servo to the "up" position.
 */
void ICACHE_FLASH_ATTR servo_up() {
	// TODO: Calibration of up angle.
	set_servo(SERVO_UP_ANGLE);
}

/*
 * Sets the servo to the "down" position.
 */
void ICACHE_FLASH_ATTR servo_down() {
	// TODO: Calibration of down angle.
	set_servo(SERVO_DOWN_ANGLE);
}


/*
 * Performs the actual step of one or both stepper motors.
 * The values passed for each stepper motor has a value that fall into one of three categories:
 * - Positive value - step forwards
 * - Negative value - step backwards
 * - Zero - no step.
 *
 * The magnitude of the number has no bearing on the steps of the motor.
 *
 * Parameters:
 * stepper1 - number to control the first stepper's movements.
 * stepper2 - number to control the second stepper's movements.
 */
LOCAL void ICACHE_FLASH_ATTR step_motors(int8_t stepper1, int8_t stepper2) {
	uint32_t set_mask = 0;
	uint32_t clear_mask = 0;
	uint32_t enable_mask = 0;

	if ((stepper1 == 0) && (stepper2 == 0)) {
		// Nothing to do, no motor has been asked to move.
		return;
	}

	// Calculate the values for the left stepper motor.
	if (stepper1 != 0) {
		int8_t step = (stepper1 < 0) ? -1 : 1;
		current_step[0] = (current_step[0] + step + STEP_SEQUENCE_COUNT) % STEP_SEQUENCE_COUNT;
		set_mask |= step_values[0][current_step[0]];
		clear_mask |= STEPPER_1_MASK & ~step_values[0][current_step[0]];
		enable_mask |= STEPPER_1_MASK;
	}

	// Calculate the values for the right stepper motor.
	if (stepper2 != 0) {
		int8_t step = (stepper2 < 0) ? -1 : 1;
		current_step[1] = (current_step[1] + step + STEP_SEQUENCE_COUNT) % STEP_SEQUENCE_COUNT;
		set_mask |= step_values[1][current_step[1]];
		clear_mask |= STEPPER_2_MASK & ~step_values[1][current_step[1]];
		enable_mask |= STEPPER_2_MASK;
	}

	// Set the GPIO outputs to move the selected stepper motors.
	gpio_output_set(set_mask, clear_mask, enable_mask, 0);
}

/*
 * Stops all stepper motors by turning off the current to their coils.
 * This de-enerises the motors, so they will not consume engery, but will also not resist movement.
 */
void ICACHE_FLASH_ATTR stop_motors() {
	gpio_output_set(0, STEPPER_1_MASK | STEPPER_2_MASK, STEPPER_1_MASK | STEPPER_2_MASK, 0);
}

/*
 * Instructs the stepper motors to move a set amount.
 *
 * Parameters:
 * left_steps - the number of steps for the left stepper motor to advance
 * right_steps - the number of steps for the right stepper motor to advance
 * tick_count - the number of ticks over which the left and right steppers are moving.
 * cb - the call-back function to be invoked when the tick_count has been reached.
 */
void ICACHE_FLASH_ATTR drive_motors(
	int16_t left_steps, 
	int16_t right_steps,
	uint16_t tick_count,
	motor_callback_t *cb) {
	// Set the global variables.
	total_ticks = 0;
	current_tick = 0;
	motor_cb = cb;

	if ((tick_count == 0) || ((left_steps == 0) && (right_steps == 0))) {
		// There's nothing to do.
		return;
	}

	// Set the per-stepper values, first for the left stepper.
	stepper_data[0].steps = ABS(left_steps);
	stepper_data[0].d = (2 * left_steps) - tick_count;
	stepper_data[0].step = 0;
	stepper_data[0].last_step = -1;
	stepper_data[0].direction = (left_steps > 0) ? 1 : -1;

	// Now for the right stepper
	stepper_data[1].steps = ABS(right_steps);
	stepper_data[1].d = (2 * right_steps) - tick_count;
	stepper_data[1].step = 0;
	stepper_data[1].last_step = -1;
	stepper_data[1].direction = (right_steps > 0) ? 1 : -1;

	// Finally, set the total ticks, and we're good to go.
	total_ticks = tick_count;
}

/*
 * Timer callback used to determine if any stepper needs to move to the next step.
 * The algorithm to choose whether to initiate a step is based on Bresenham's line 
 * algorithm.
 */
LOCAL void ICACHE_FLASH_ATTR motor_timer_cb(void *arg) {
	// Count of the number of idle cycles for the motors.
	// This value is stored between calls to this method.
	static uint32_t idle_count = 0;

	// Make sure we have something to do.
	if (total_ticks <= 0) {
		if (++idle_count > MAX_IDLE_COUNT) {
			// The motors have been idle for too long, turn them off to save electricity.
			stop_motors();
		}
		return;
	} else {
		// We have something to do, so we're not idle.
		idle_count = 0;
	}

	// Determine whether either/both of the motors should be stepping this tick.
	bool steps[] = {false, false};
	for (int ii = 0; ii <= 1; ii ++) {
		if (stepper_data[ii].steps <= 0) {
			// There's nothing to do for this motor.
			continue;
		}
		if (stepper_data[ii].step != stepper_data[ii].last_step) {
			steps[ii] = true;
			stepper_data[ii].last_step = stepper_data[ii].step;
		}

		if (stepper_data[ii].d > 0) {
			stepper_data[ii].step++;
			stepper_data[ii].d -= 2*total_ticks;
		}
		stepper_data[ii].d += 2*stepper_data[ii].steps;
	}

	// Step the appropriate motor(s).
	if ((steps[0]) || (steps[1])) {
		step_motors((steps[0]) ? stepper_data[0].direction : 0,
				    (steps[1]) ? stepper_data[1].direction : 0);
	}

	// Move the values ready for the next tick.
	current_tick++;
	if (current_tick >= total_ticks) {
		// We have now finished our ticks.
		total_ticks = 0;
		if (motor_cb != NULL) {
			// Invoke the callback function.
			motor_cb();
		}
	}
}

/*
 * Initialises the motor control system.
 */
void ICACHE_FLASH_ATTR init_motors() {
	// Set up the GPIO function selections for stepper 1.
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO4_U, FUNC_GPIO4);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO5_U, FUNC_GPIO5);

	// Set up the GPIO function selections for stepper 2.
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_GPIO3);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDI_U, FUNC_GPIO12);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, FUNC_GPIO13);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTMS_U, FUNC_GPIO14);

	// Run through each step once, so that the motor is now synchronised with our state.
	total_ticks = 0;
	next_total_ticks = 0;
	drive_motors(STEP_SEQUENCE_COUNT, STEP_SEQUENCE_COUNT, STEP_SEQUENCE_COUNT, NULL);

	// Start the timer.
    os_timer_disarm(&motor_timer);
    os_timer_setfn(&motor_timer, (os_timer_func_t *)motor_timer_cb, (void *)0);
    os_timer_arm(&motor_timer, MOTOR_TICK_INTERVAL, 1);

	// Start the servo motor pulse width modulation on GPIO 15.
	uint32_t pwm_info[][3] = {{PERIPHS_IO_MUX_MTDO_U, FUNC_GPIO15, 15}};
	uint32_t servo_duty[1] = {0};
	pwm_init(PWM_PERIOD, servo_duty, 1, pwm_info);
	set_servo(SERVO_UP_ANGLE);
}

