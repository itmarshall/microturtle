/*
 * user_main.c: Main entry-point for the web bootstrap code.
 *
 * Author: Ian Marshall
 * Date: 12/12/2017
 */
#include <stdarg.h>
#include "esp8266.h"
#include "httpd.h"
#include "httpdespfs.h"
#include "ets_sys.h"
#include "osapi.h"
#include "mem.h"
#include "gpio.h"
#include "os_type.h"
#include "ip_addr.h"
#include "espconn.h"
#include "user_interface.h"
#include "espmissingincludes.h"

#include "espfs.h"
#include "captdns.h"
#include "webpages-espfs.h"
#include "cgiwebsocket.h"

#include "tcp_ota.h"
#include "udp_debug.h"
#include "string_builder.h"
#include "config.h"
#include "motors.h"
#include "vm.h"
#include "http.h"

// Stores the address to which the results from the inverter are sent via HTTP in an ip_addr structure.
#define REMOTE_ADDR(ip) (ip)[0] = 10; (ip)[1] = 0; (ip)[2] = 1; (ip)[3] = 253;

/*
 * Entry point for the program. Sets up the microcontroller for use.
 */
void user_init(void) {
	// Initialise the wifi.
	wifi_init();

    // Initialise the OTA flash system.
    ota_init();

    // Initialise the network debugging.
    dbg_init();

	// Initialise the stepper and servo motors.
	gpio_init();
	init_motors();

	// Initialise the configuration.
	init_config();

	// Initialise the virtual machine.
	init_vm();

	// Initialise the HTTP server.
	http_init();
}
