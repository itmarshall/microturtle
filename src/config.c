/*
 * config.c: Storage and retrieval of configuration parameters.
 *
 * Author: Ian Marshall
 * Date: 22/03/2018
 */
#include "esp8266.h"
#include "ets_sys.h"
#include "osapi.h"
#include "mem.h"

#include "config.h"

// The flash sector used for storing and retrieving the configuration.
#define CONFIG_SECTOR 0x102

static uint32_t const CONFIG_MAGIC_VALUE = 0x75436667; // 'uCfg'

// The default value to use for the number of steps for each motor to move the turtle 100mm.
static uint32_t const DEFAULT_STRAIGHT_STEPS = 1729;

// The default value to use for the number of steps for each motor to turn the turtle 90 degrees.
static uint32_t const DEFAULT_TURN_STEPS = 2052;

// The default value to use for the angle of the server in the pen up position.
static int8_t const DEFAULT_SERVO_UP_ANGLE = 90;

// The default value to use for the angle of the server in the pen down position.
static int8_t const DEFAULT_SERVO_DOWN_ANGLE = -90;

// The default value to use for the number of steps for a servo moving between the up and down positions.
static uint8_t const DEFAULT_SERVO_MOVE_STEPS = 1;

// The default value to use for the number of ms in the interval of the servo timer.
static uint8_t const DEFAULT_SERVO_TICK_INTERVAL = 1;

// The default value to use for the number of ms in the interval of the stepper motor timer.
static uint8_t const DEFAULT_MOTOR_TICK_INTERVAL = 1;

// The default value to use for the number of ms to pause after a motor movement.
static uint32_t const DEFAULT_MOVE_PAUSE_DURATION = 200;

/*
 * Structure for the physical storage of configuration parameters in the flash. This includes a "magic" value that is
 * also stored in the flash to test if the configuration is stored, or if the flash is simply uninitialised, or random.
 */
typedef struct config_storage_t {
	uint32_t magic;
	config_t config;
} config_storage_t;

LOCAL config_t current_config;

/*
 * Retrieves the values for the number of steps for each motor to move 100mm. The values are written to the supplied
 * pointers.
 */
void get_straight_steps(uint32_t *left, uint32_t *right) {
	*left = current_config.straight_steps_left;
	*right = current_config.straight_steps_right;
}

/*
 * Retrieves the values for the number of steps for each motor to turn the robot by 90 degrees.
 * The values are written to the supplied pointers.
 */
void get_turn_steps(uint32_t *left, uint32_t *right) {
	*left = current_config.turn_steps_left;
	*right = current_config.turn_steps_right;
}

/*
 * Retrieves the value for the servo up angle.
 */
int8_t get_servo_up_angle() {
	return current_config.servo_up_angle;
}

/*
 * Retrieves the value for the servo down angle.
 */
int8_t get_servo_down_angle() {
	return current_config.servo_down_angle;
}

/*
 * Retrieves the value for the servo movement steps.
 */
uint8_t get_servo_move_steps() {
	return current_config.servo_move_steps;
}

/*
 * Retrieves the value for the servo tick interval.
 */
uint32_t get_servo_tick_interval() {
	return current_config.servo_tick_interval;
}

/*
 * Retrieves the value for the motor tick interval.
 */
uint32_t get_motor_tick_interval() {
	return current_config.motor_tick_interval;
}

/*
 * Retrieves the value for the move pause duration.
 */
uint32_t get_move_pause_duration() {
	return current_config.move_pause_duration;
}

/*
 * Retrieves the values for the current configuration.
 */
void ICACHE_FLASH_ATTR get_configuration(config_t *config) {
	os_memcpy(config, &current_config, sizeof(config_t));
}

/*
 * Stores the configuration values in the flash memory, returning true if the values were written 
 * successfully.
 */
bool ICACHE_FLASH_ATTR store_configuration(config_t *config) {
	if (config == NULL) {
		os_printf("NULL configuration passed to store_configuration.\n");
		return false;
	}

	/*
	os_printf("Storing values for straight steps - left: %d, right: %d, ",
			config->straight_steps_left, config->straight_steps_right);
	os_printf("turn steps - left: %d, right: %d.\n",
			config->turn_steps_left, config->turn_steps_right);
	os_printf("sizes: config_t: %d, config_storage_t: %d.\n", sizeof(config_t), sizeof(config_storage_t));
	*/

	// Store the values in flash memory.
	config_storage_t storage;
	os_memcpy(&storage.config, config, sizeof(config_t));
	storage.magic = CONFIG_MAGIC_VALUE;
	bool res = system_param_save_with_protect(CONFIG_SECTOR, &storage, sizeof(config_storage_t));
	if (!res) {
		os_printf("Unable to save configuration to flash memory.\n");
		return false;
	}

	// Update the local values.
	os_memcpy(&current_config, config, sizeof(config_t));
	return true;
}

/*
 * Initialises the configuration management system by loading the configuration into RAM.
 */
void ICACHE_FLASH_ATTR init_config() {
	// Load the configuration values from the flash.
	config_storage_t storage;
	bool res = system_param_load(CONFIG_SECTOR, 0, &storage, sizeof(config_storage_t));
	if ((!res) || (storage.magic != CONFIG_MAGIC_VALUE)) {
		// We don't have a configuration saved in the flash that we can read, use default values instead.
		if (!res) {
			os_printf("Unable to load configuration from flash memory.\n");
		} else {
			os_printf("Flash memory does not hold a configuration.\n");
		}
		current_config.straight_steps_left = DEFAULT_STRAIGHT_STEPS;
		current_config.straight_steps_right = DEFAULT_STRAIGHT_STEPS;
		current_config.turn_steps_left = DEFAULT_TURN_STEPS;
		current_config.turn_steps_right = DEFAULT_TURN_STEPS;
		current_config.servo_up_angle = DEFAULT_SERVO_UP_ANGLE;
		current_config.servo_down_angle = DEFAULT_SERVO_DOWN_ANGLE;
		current_config.servo_move_steps = DEFAULT_SERVO_MOVE_STEPS;
		current_config.servo_tick_interval = DEFAULT_SERVO_TICK_INTERVAL;
		current_config.motor_tick_interval = DEFAULT_MOTOR_TICK_INTERVAL;
		current_config.move_pause_duration = DEFAULT_MOVE_PAUSE_DURATION;
	} else {
		// Store the flash configuration in RAM for fast/easy access.
		os_memcpy(&current_config, &storage.config, sizeof(config_t));
	}

	os_printf("Straight steps - left: %d, right: %d.\n",
			current_config.straight_steps_left, current_config.straight_steps_right);
	os_printf("Turn steps - left: %d, right: %d.\n",
			current_config.turn_steps_left, current_config.turn_steps_right);
}

