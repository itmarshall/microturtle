/*
 * http.h: Header file for serving HTTP pages.
 *
 * Author: Ian Marshall
 * Date: 11/11/2018
 */

#ifndef __HTTP_H
#define __HTTP_H

#include "motors.h"
#include "vm.h"

/*
 * Notifies any listeners via web socket connections the current program's execution status.
 */
void ICACHE_FLASH_ATTR notify_program_status(prog_status_t status, uint32_t function, uint32_t index);

/*
 * Notifies any listeners via web socket connections the current servo position (up/down).
 */
void ICACHE_FLASH_ATTR notify_servo_position(servo_position_t pos);

/*
 * Sets up the WiFi interface on the ESP8266.
 */
void ICACHE_FLASH_ATTR wifi_init();

/*
 * Initialises the HTTP server.
 */
void ICACHE_FLASH_ATTR http_init();

#endif
