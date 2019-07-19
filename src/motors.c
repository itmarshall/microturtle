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

#include "http.h"
#include "motors.h"
#include "config.h"

#define PWM_PERIOD 20000 // 20ms
#define PWM_MIN 22222    // 1ms
#define PWM_MAX 44444    // 2ms

// Absolute value macro.
#define ABS(x) (((x) < 0) ? -(x) : (x))

// The number of stepper motors that this program is using.
#define STEPPER_MOTOR_COUNT 2

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

// Structure used to hold phase information for acceleration.
// This data is per-movement sequence.
typedef struct {
	uint32_t accel_limit;     // The actual acceleration duration for short runs.
	uint32_t accel_duration;  // The duration the acceleration curve is over.
	uint32_t cruise_duration; // The duration the cruise runs for.
	uint32_t phase_tick;      // The current tick in this phase.
	float last_position;     // The position of the last step for this phase.
} phase_data_t;

// The various phases that a motor movement can go through.
typedef enum {
	STATIONARY,
	ACCEL_1,
	ACCEL_2,
	CRUISING,
	DECEL_1,
	DECEL_2
} phases_t;

// Stepper data used in the control of motor movement.
LOCAL tick_data_t stepper_data[STEPPER_MOTOR_COUNT];

// Phase information used in the control of accelerated motor movement.
LOCAL phase_data_t phase_data;

// The current phase of the accelerated movement.
LOCAL phases_t current_phase = STATIONARY;

// The current tick that the motor control is up to.
LOCAL uint32_t current_tick = 0;

// The total number of ticks in the motor control's current sequence.
LOCAL uint32_t total_ticks = 0;

// The total number of steps in the motor control's current sequence.
LOCAL uint32_t total_steps = 0;

// Flag used to enable/disable motor acceleration for this sequence.
LOCAL bool acceleration_active = false;

// The callback function to call when the motor control's sequence is complete.
LOCAL motor_callback_t *motor_cb = NULL;

// The total number of ticks for the next motor control sequence.
LOCAL int32_t next_total_ticks = 0;

// The number of steps of each stepper motor for the next motor control sequence.
LOCAL int32_t next_steps[STEPPER_MOTOR_COUNT] = {0, 0};

// The callback function to call when the servo's movement sequence is complete.
LOCAL motor_callback_t *servo_cb = NULL;

// The timer used for moving the stepper motors.
LOCAL os_timer_t motor_timer;

// The timer used for moving the servo motor.
LOCAL os_timer_t servo_timer;

// The current angle of the servo.
LOCAL int8_t servo_angle;

// The angle the servo is moving towards.
LOCAL int8_t destination_angle;

// The current step for the servo movement.
LOCAL uint8_t servo_step;

// The size of each step of the servo movement.
LOCAL int8_t servo_step_size;

// The current position of the servo.
LOCAL servo_position_t servo_pos; 

// S1 - GPIO 2, 15, 12, 14
// S2 - GPIO 3,  5,  4,  0
#define STEP_SEQUENCE_COUNT 8
#define STEPPER_1_MASK (BIT2 | BIT15 | BIT12 | BIT14)
#define STEPPER_2_MASK (BIT3 | BIT5  | BIT4  | BIT0)

LOCAL uint32_t step_values[STEPPER_MOTOR_COUNT][STEP_SEQUENCE_COUNT] = {
	{
	 // Stepper 1.
	 BIT2,
	 BIT2 | BIT15,
	 BIT15,
	 BIT15 | BIT12,
	 BIT12,
	 BIT12 | BIT14,
	 BIT14,
	 BIT14 | BIT2
	}, {
	 // Stepper 2.
	 BIT3,
	 BIT3 | BIT5,
	 BIT5,
	 BIT5 | BIT4,
	 BIT4,
	 BIT4 | BIT0,
	 BIT0,
	 BIT0 | BIT3
	}
};

// The current step in the step sequence for each motor.
LOCAL int8_t current_step[STEPPER_MOTOR_COUNT] = {0, 0};

// Calculates the maximum value of two 16-bit integers.
LOCAL int16_t ICACHE_FLASH_ATTR max16(int16_t a, int16_t b) {
	return (a < b) ? b : a;
}

/*
 * Sets the servo's position, in the range of [-90 90]. Angle is in degrees.
 */
LOCAL void ICACHE_FLASH_ATTR set_servo(int8_t position, motor_callback_t *cb) {
	// Store the callback.
	servo_cb = cb;

	// Ensure the position is in the range of [-90 90] degrees.
	if (position < -90) {
		position = -90;
	} else if (position > 90) {
		position = 90;
	}

	os_printf("Setting servo angle to %d.\n", position);
	destination_angle = position;

	// Calculate the size of each step.
	uint8_t steps = get_servo_move_steps();
	os_printf("steps = %d\n", steps);
	if (steps <= 0) {
		steps = 1;
	}
	servo_step_size = (destination_angle - servo_angle) / steps;
	servo_step = 0;

	// Start the servo timer.
	uint32_t interval = get_servo_tick_interval();
	if (get_servo_move_steps() == 1) {
		// Minimal step time as there is only one step.
		interval = 1;
	}
	//os_printf("Servo tick interval = %ld.\n", interval);
	os_timer_disarm(&servo_timer);
    os_timer_arm(&servo_timer, interval, 1);
}

LOCAL void ICACHE_FLASH_ATTR servo_timer_cb(void *arg) {
	// Update the angle.
	servo_angle += servo_step_size;
	servo_step++;

	if (servo_step >= get_servo_move_steps()) {
		// Ensure the finishing angle is the destination angle to remove any rounding errors.
		servo_angle = destination_angle;
	}
	os_printf("In servo timer callback for step %d, angle=%d.\n", servo_step, servo_angle);

	// Calculate the duty cycle to keep it between 1ms (-90 degs) and 2ms (+90 degs).
	uint32_t pwm_duty = ((uint32_t)(servo_angle + 90) * (PWM_MAX - PWM_MIN) / 180) + PWM_MIN;

	// Set the new PWM duty cycle.
	pwm_set_duty(pwm_duty, 0);
	pwm_start();

	if (servo_step >= get_servo_move_steps()) {
		// We're finished with the servo movement steps.
		os_timer_disarm(&servo_timer);
		if (servo_cb != NULL) {
			// Invoke the callback function.
			servo_cb();
		}
	}
}

/*
 * Sets the servo to the "up" position.
 */
void ICACHE_FLASH_ATTR servo_up(motor_callback_t *cb) {
	set_servo(get_servo_up_angle(), cb);
	servo_pos = UP;
	notify_servo_position(UP);
}

/*
 * Sets the servo to the "down" position.
 */
void ICACHE_FLASH_ATTR servo_down(motor_callback_t *cb) {
	set_servo(get_servo_down_angle(), cb);
	servo_pos = DOWN;
	notify_servo_position(DOWN);
}

/*
 * Returns the servo's current position.
 */
servo_position_t get_servo(){
	return servo_pos;
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
		int8_t step = (stepper1 < 0) ? 1 : -1;
		current_step[0] = (current_step[0] + step + STEP_SEQUENCE_COUNT) % STEP_SEQUENCE_COUNT;
		set_mask |= step_values[0][current_step[0]];
		clear_mask |= STEPPER_1_MASK & ~step_values[0][current_step[0]];
		enable_mask |= STEPPER_1_MASK;
	}

	// Calculate the values for the right stepper motor.
	if (stepper2 != 0) {
		int8_t step = (stepper2 < 0) ? 1 : -1;
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
 * left_steps  - the number of steps for the left stepper motor to advance
 * right_steps - the number of steps for the right stepper motor to advance
 * tick_count  - the number of ticks over which the left and right steppers are moving
                 This is not used when acceleration is enabled
 * accelerate  - flag set when acceleration is required
 * cb          - the call-back function to be invoked when the steps have been completed.
 */
void ICACHE_FLASH_ATTR drive_motors(
	int16_t left_steps, 
	int16_t right_steps,
	uint16_t tick_count,
	bool accelerate,
	motor_callback_t *cb) {

	// Set the global variables.
	total_ticks = 0;
	total_steps = 0;
	current_tick = 0;
	current_phase = accelerate ? ACCEL_1 : CRUISING;
	acceleration_active = accelerate;
	motor_cb = cb;

	//os_printf("drive_motors, l=%d, r=%d, tc=%d, a=%d.\n", left_steps, right_steps, tick_count, accelerate);
	if ((left_steps == 0) && (right_steps == 0)) {
		// There's nothing to do.
		return;
	}

	// Set the phases.
	uint32_t steps = max16(ABS(left_steps), ABS(right_steps));
	if (accelerate) {
		phase_data.accel_limit = get_acceleration_duration();
		phase_data.accel_duration = phase_data.accel_limit / 2;
		phase_data.cruise_duration = steps - (2 * phase_data.accel_duration);
		phase_data.phase_tick = 0;
		os_printf("al=%d, ad=%d, cd=%d.\n", phase_data.accel_limit, phase_data.accel_duration, phase_data.cruise_duration);
		if (phase_data.accel_duration > (steps/2)) {
			// We don't finish accelerating before it's time to decelerate.
			uint32_t target = steps / 2;
			uint32_t left = 0;
			uint32_t right = get_acceleration_duration();
			uint32_t mid;
			float crossover = phase_data.accel_duration;
			crossover = (crossover * crossover * crossover) /
				(6 * phase_data.accel_duration * phase_data.accel_duration);
			float m_value;
			os_printf("a_d: %d, st: %d, t: %d.\n",
					phase_data.accel_duration, steps, target);
			while (left < right) {
				mid = (left + right) / 2;
				m_value = (float)mid;
				if (mid <= phase_data.accel_duration) {
					m_value = (m_value * m_value * m_value) /
						(6 * phase_data.accel_duration * phase_data.accel_duration);
				} else {
					m_value -= phase_data.accel_duration;
					m_value = -((m_value * m_value * m_value) / 
						(6 * phase_data.accel_duration * phase_data.accel_duration)) +
						((m_value * m_value) / (2 * phase_data.accel_duration)) +
						(m_value / 2) +
						crossover;
				}
				os_printf("left=%d, right=%d, mid=%d, m_v=%.4lf.\n", 
						left, right, mid, m_value);
				if (((uint32_t)m_value) < target) {
					left = mid + 1;
				} else {
					right = mid;
				}
			}
			mid = left;
			phase_data.accel_limit = mid;
			phase_data.cruise_duration = steps - (2*target);
			os_printf("mid=%d, a_l=%d, c_d=%d.\n",
					mid, phase_data.accel_limit, phase_data.cruise_duration);
		}
	} else {
		phase_data.accel_limit = 0;
		phase_data.accel_duration = 0;
		phase_data.cruise_duration = tick_count;
		phase_data.phase_tick = 0;
	}

	// Set the per-stepper values, first for the left stepper.
	stepper_data[0].steps = ABS(left_steps);
	stepper_data[0].d = (2 * ABS(left_steps)) - (accelerate ? steps : tick_count);
	stepper_data[0].step = 0;
	stepper_data[0].last_step = -1;
	stepper_data[0].direction = (left_steps > 0) ? 1 : -1;

	// Now for the right stepper
	stepper_data[1].steps = ABS(right_steps);
	stepper_data[1].d = (2 * ABS(right_steps)) - (accelerate ? steps : tick_count);
	stepper_data[1].step = 0;
	stepper_data[1].last_step = -1;
	stepper_data[1].direction = (right_steps > 0) ? 1 : -1;

	// Finally, set the totals, and we're good to go.
	total_steps = steps;
	if (accelerate) {
		total_ticks = 2*phase_data.accel_limit + phase_data.cruise_duration;
	} else {
		total_ticks = tick_count;
	}
}

/*
 * Timer callback used to determine if any stepper needs to move to the next step.
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
			idle_count = 0;
		}
		return;
	} else {
		// We have something to do, so we're not idle.
		idle_count = 0;
	}

	float position;
	float tick = ++phase_data.phase_tick;
	current_tick++;
	bool reset = false;
	bool complete = false;
	switch (current_phase) {
		case ACCEL_1:
			position = (tick * tick * tick) /
				(6 * phase_data.accel_duration * phase_data.accel_duration);
			if (tick >= phase_data.accel_limit) {
				// We've passed the acceleration limit, skip ahead.
				if (phase_data.cruise_duration > 0) {
					current_phase = CRUISING;
				} else {
					current_phase = DECEL_1;
				}
				reset = true;
			} else if (tick >= phase_data.accel_duration) {
				// Move to the next phase of the movement.
				current_phase = ACCEL_2;
				reset = true;
			}
			break;
		case ACCEL_2:
			position = -((tick * tick * tick) / 
					(6 * phase_data.accel_duration * phase_data.accel_duration)) +
				((tick * tick) / (2 * phase_data.accel_duration)) +
				(tick / 2);
			//os_printf("p=%.4lf, t=%.2lf, a_d=%d, a_l=%d.\n",
			//		position, tick, phase_data.accel_duration, phase_data.accel_limit);
			if ((tick + phase_data.accel_duration) >= phase_data.accel_limit) {
				// We've passed the acceleration limit, skip ahead.
				if (phase_data.cruise_duration > 0) {
					current_phase = CRUISING;
				} else {
					current_phase = DECEL_1;
				}
				reset = true;
			} else if (tick >= phase_data.accel_limit) {
				// Move to the next phase of the movement.
				current_phase = CRUISING;
				reset = true;
			}
			break;
		case CRUISING:
			position = phase_data.last_position + 1;
			if (tick >= phase_data.cruise_duration) {
				// Move to the next phase of the movement.
				if (!acceleration_active) {
					// There is no next phase without acceleration.
					complete = true;
					current_phase = STATIONARY;
				} else {
					current_phase = DECEL_1;
				}
				reset = true;
			}
			break;
		case DECEL_1:
			position = -((tick * tick * tick) /
					(6 * phase_data.accel_duration * phase_data.accel_duration)) +
				tick;
			if (tick >= phase_data.accel_duration) {
				// Move to the next phase of the movement.
				current_phase = DECEL_2;
				reset = true;
			}
			break;
		case DECEL_2:
			position = (tick * tick * tick) /
				(6 * phase_data.accel_duration * phase_data.accel_duration) -
				((tick * tick) / (2 * phase_data.accel_duration)) + 
				(tick / 2);
			if (tick >= phase_data.accel_duration) {
				// We are now done.
				complete = true;
				current_phase = STATIONARY;
				reset = true;
			}
			break;
		defaule:
			// We shouldn't get here.
			current_phase = STATIONARY;
			reset = true;
	}

	// See if we need to step.
	if ((position - phase_data.last_position) >= 1.0) {
		phase_data.last_position += 1.0;

		// Determine whether which of the motors should be stepping this tick.
		bool steps[] = {false, false};
		for (int ii = 0; ii <= 1; ii ++) {
			/*
			if (ii == 0) {
				printf("%d: steps=%d, d=%d, step=%d, l_s=%d, dir=%d.\n",
						ii,
						stepper_data[ii].steps,
						stepper_data[ii].d,
						stepper_data[ii].step,
						stepper_data[ii].last_step,
						stepper_data[ii].direction);
			}
			*/
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
				stepper_data[ii].d -= 2*total_steps;
			}
			stepper_data[ii].d += 2*stepper_data[ii].steps;
		}

		// Step the appropriate motor(s).
		if ((steps[0]) || (steps[1])) {
			step_motors((steps[0]) ? stepper_data[0].direction : 0,
						(steps[1]) ? stepper_data[1].direction : 0);
		}
	}

	// If we're at the end of a phase, we reset the counters.
	if (reset) {
		phase_data.phase_tick = 0;
		if (phase_data.accel_limit != (2*phase_data.accel_duration)) {
			if (current_phase == DECEL_1) {
				float correction;
				if (phase_data.accel_limit <= phase_data.accel_duration) {
					// We need to skip the deceleration 1 phase.
					current_phase = DECEL_2;
					phase_data.phase_tick = phase_data.accel_duration - phase_data.accel_limit;
					correction = phase_data.phase_tick;
					correction = (correction * correction * correction) /
						(6 * phase_data.accel_duration * phase_data.accel_duration) -
						((correction * correction) / (2 * phase_data.accel_duration)) + 
						(correction / 2);
				} else {
					// Count the ticks remaining from the end of the phase.
					phase_data.phase_tick = (2*phase_data.accel_duration) - phase_data.accel_limit;
					correction = phase_data.phase_tick;
					correction = -((correction * correction * correction) /
							(6 * phase_data.accel_duration * phase_data.accel_duration)) +
							correction;
				}
				phase_data.last_position += correction;
			} else if ((current_phase == DECEL_2) &&
					(phase_data.accel_limit < phase_data.accel_duration)) {
				// Count the ticks remaining from the end of the phase.
				phase_data.phase_tick = phase_data.accel_duration - phase_data.accel_limit;
			}
		}
		phase_data.last_position -= position;
	}

	if (complete) {
		total_ticks = 0;
		total_steps = 0;
		if (motor_cb != NULL) {
			// Invoke the callback function.
			motor_cb();
		}
	}
}

/*
 * (Re)initialises the motor timer.
 */
void ICACHE_FLASH_ATTR init_motor_timer() {
	uint32_t interval = get_motor_tick_interval();
	if (interval <= 0) {
		interval = 1;
	}
    os_timer_disarm(&motor_timer);
    os_timer_setfn(&motor_timer, (os_timer_func_t *)motor_timer_cb, (void *)0);
    os_timer_arm(&motor_timer, interval, 1);
}

/*
 * Initialises the motor control system.
 */
void ICACHE_FLASH_ATTR init_motors() {
	// Set up the GPIO function selections for stepper 1.
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, FUNC_GPIO15);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDI_U, FUNC_GPIO12);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTMS_U, FUNC_GPIO14);

	// Set up the GPIO function selections for stepper 2.
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_GPIO3);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO5_U, FUNC_GPIO5);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO4_U, FUNC_GPIO4);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0);

	// Run through each step once, so that the motor is now synchronised with our state.
	total_ticks = 0;
	next_total_ticks = 0;
	drive_motors(STEP_SEQUENCE_COUNT, STEP_SEQUENCE_COUNT, STEP_SEQUENCE_COUNT, false, NULL);

	// Start the motor timer.
	init_motor_timer();

	// Prepare, but do not start the servo timer.
	os_timer_disarm(&servo_timer);
	os_timer_setfn(&servo_timer, (os_timer_func_t *)servo_timer_cb, (void *)0);

	// Start the servo motor pulse width modulation on GPIO 13.
	uint32_t pwm_info[][3] = {{PERIPHS_IO_MUX_MTCK_U, FUNC_GPIO13, 13}};
	uint32_t servo_duty[1] = {0};
	pwm_init(PWM_PERIOD, servo_duty, 1, pwm_info);
	servo_up(NULL);
}

