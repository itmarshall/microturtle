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
	uint32_t straight_steps_left;  // The number of steps for the left motor to move 100mm.
	uint32_t straight_steps_right; // The number of steps for the right motor to move 100mm.
	uint32_t turn_steps_left;      // The number of steps for the left motor to turn 90 degrees.
	uint32_t turn_steps_right;     // The number of steps for the right motor to turn 90 degrees.
} config_t;

/*
 * Retrieves the values for the number of steps for each motor to move 100mm. The values are written
 * to the supplied pointers.
 */
void ICACHE_FLASH_ATTR get_straight_steps(uint32_t *left, uint32_t *right);

/*
 * Retrieves the values for the number of steps for each motor to turn the robot by 90 degrees.
 * The values are written to the supplied pointers.
 */
void ICACHE_FLASH_ATTR get_turn_steps(uint32_t *left, uint32_t *right);

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
