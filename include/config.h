/*
 * config.h: Header file for storage and retrieval of configuration parameters.
 *
 * Author: Ian Marshall
 * Date: 25/03/2018
 */

#ifndef __CONFIG_H
#define __CONFIG_H

/*
 * Configuration structure holding the parameters that are stored in flash and returned to users.
 */
typedef struct config_t {
	uint32_t straight_steps_left;   // The number of steps for the left motor to move 100mm.
	uint32_t straight_steps_right;  // The number of steps for the right motor to move 100mm.
	uint32_t turn_steps_left;       // The number of steps for the left motor to turn 90 degrees.
	uint32_t turn_steps_right;      // The number of steps for the right motor to turn 90 degrees.
	int8_t servo_up_angle;          // The angle used for when the servo is holding the pen up.
	int8_t servo_down_angle;        // The angle used for when the servo is holding the pen down.
	uint8_t servo_move_steps;       // The number of steps the servo moves through between up and down.
	uint32_t servo_tick_interval;   // The number of ms in each interval of the servo motor timer.
	uint32_t motor_tick_interval;   // The number of ms in each interval of the stepper motor timer.
	uint32_t acceleration_duration; // The number of ticks taken to ramp up to full speed.
	uint32_t move_pause_duration;   // The number of ms to pause after a motor movement.
} config_t;

/*
 * Retrieves the values for the number of steps for each motor to move 100mm. The values are written
 * to the supplied pointers.
 */
void get_straight_steps(uint32_t *left, uint32_t *right);

/*
 * Retrieves the values for the number of steps for each motor to turn the robot by 90 degrees.
 * The values are written to the supplied pointers.
 */
void get_turn_steps(uint32_t *left, uint32_t *right);

/*
 * Retrieves the value for the servo up angle.
 */
int8_t get_servo_up_angle();

/*
 * Retrieves the value for the servo down angle.
 */
int8_t get_servo_down_angle();

/*
 * Retrieves the value for the servo movement steps.
 */
uint8_t get_servo_move_steps();

/*
 * Retrieves the value for the servo tick interval.
 */
uint32_t get_servo_tick_interval();

/*
 * Retrieves the value for the motor tick interval.
 */
uint32_t get_motor_tick_interval();

/*
 * Retrieves the value for the acceleration duration.
 */
uint32_t get_acceleration_duration();

/*
 * Retrieves the value for the move pause duration.
 */
uint32_t get_move_pause_duration();

/*
 * Retrieves the values for the current configuration.
 */
void ICACHE_FLASH_ATTR get_configuration(config_t *config);

/*
 * Stores the configuration values in the flash memory, returning true if the values were written 
 * successfully.
 */
bool ICACHE_FLASH_ATTR store_configuration(config_t *config);

/*
 * Initialises the configuration management system by loading the configuration into RAM.
 */
void ICACHE_FLASH_ATTR init_config();

#endif
