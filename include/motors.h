/*
 * motors.h: Header file for the control routines for the micro-turtle's motors.
 *
 * Author: Ian Marshall
 * Date: 2/01/2018
 */

#ifndef _MOTORS_H
#define _MOTORS_H

// Definition of callback function for motor movement completion events.
typedef void motor_callback_t();

// Type to hold the position of the servo holding the pen.
typedef enum servo_position_t {UP, DOWN} servo_position_t;

/*
 * Sets the servo to the "up" position.
 *
 * Parameters:
 * cb - the call-back function to be invoked when the servo has finished moving.
 */
void servo_up(motor_callback_t *cb);

/*
 * Sets the servo to the "down" position.
 *
 * Parameters:
 * cb - the call-back function to be invoked when the servo has finished moving.
 */
void servo_down(motor_callback_t *cb);

/*
 * Returns the servo's current position.
 */
servo_position_t get_servo();

/*
 * Stops all stepper motors by turning off the current to their coils.
 * This de-enerises the motors, so they will not consume engery, but will also not resist movement.
 */
void stop_motors();

/*
 * Instructs the stepper motors to move a set amount.
 *
 * Parameters:
 * left_steps - the number of steps for the left stepper motor to advance
 * right_steps - the number of steps for the right stepper motor to advance
 * tick_count - the number of ticks over which the left and right steppers are moving.
 * cb - the call-back function to be invoked when the tick_count has been reached.
 */
void drive_motors(
		int16_t left_steps, 
        int16_t right_steps,
        uint16_t tick_count,
        motor_callback_t *cb);

/*
 * (Re)initialises the motor timer.
 */
void ICACHE_FLASH_ATTR init_motor_timer();

/*
 * Initialises the GPIO values for the stepper motors.
 * This requires the gpio_init() function to be called *before* this function.
 */
void init_motors();

#endif
