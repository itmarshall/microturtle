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
#include "motors.h"
#include "vm.h"

// Stores the address to which the results from the inverter are sent via HTTP in an ip_addr structure.
#define REMOTE_ADDR(ip) (ip)[0] = 10; (ip)[1] = 0; (ip)[2] = 1; (ip)[3] = 253;

// Debug control.
#define DEBUG 1
#define debug_print(fmt, ...) \
            do { if (DEBUG) os_printf("%s:%d:%s(): " fmt, __FILE__, \
                                __LINE__, __func__, ##__VA_ARGS__); } while (0)

LOCAL const uint16_t TICK_COUNT = 100;
LOCAL const uint16_t CODE_LEN = 1024;

LOCAL int16_t rc_left = 0;
LOCAL int16_t rc_right = 0;

// Forward definitions.
LOCAL void steps_complete();
LOCAL void httpCodeReturn(HttpdConnData *connData, uint16_t code, char *title, char *message);
LOCAL int parse_functions(
	int *index, char *data, int max_index, program_t *program, HttpdConnData *connData);

LOCAL int skip_whitespace(int index, char *data, int max_index) {
	for (int ii = index; ii < max_index; ii++) {
		if ((data[ii] != ' ') && (data[ii] != '\t') && (data[ii] != '\r') && (data[ii] != '\n')) {
			// This is not whitespace.
			return ii;
		}

		if (data[ii] == '\0') {
			break;
		}
	}

	// If we get here, there is nothing left in the string.
	return -1;
}

LOCAL int check_key(int *index, char *data, int max_index, int count, ...) {
	// Ignore any leading whitespace.
	int new_index = *index;
	new_index = skip_whitespace(new_index, data, max_index);
	if (new_index == -1) {
		debug_print("Check whitespace 1 failed.\n");
		return -1;
	}

	// Keys must start with a double quotation mark.
	if (data[new_index++] != '"') {
		debug_print("Key must start with double quotes.\n");
		return -1;
	}

	// Check the key names against the supplied list.
	va_list ap;
	va_start(ap, count);
	char *str;
	int str_len;
	int match_index = -1;
	for (int ii = 0; ii < count; ii++) {
		str = (char *)va_arg(ap, char *);
		str_len = os_strlen(str);
		if (os_strncmp(&data[new_index], str, str_len) == 0) {
			// This is the right key.
			match_index = ii;
			break;
		}
	}
	va_end(ap);
	if (match_index == -1) {
		// This key doesn't match any of the supplied options.
		debug_print("Key does not match any option.\n");
		return -1;
	}

	// Keys must end with a double quotation mark.
	new_index += str_len;
	if (data[new_index++] != '"') {
		debug_print("Key must end with double quotes.\n");
		return -1;
	}

	// Ignore any trailing whitespace.
	new_index = skip_whitespace(new_index, data, max_index);
	if (new_index == -1) {
		debug_print("Check whitespace 2 failed.\n");
		return -1;
	}

	// Keys must be separated by a colon.
	if (data[new_index++] != ':') {
		debug_print("Keys must have a colon.\n");
		return -1;
	}
	
	// Ignore any whitespace after the colon.
	new_index = skip_whitespace(new_index, data, max_index);
	if (new_index == -1) {
		debug_print("Check whitespace 3 failed.\n");
		return -1;
	}

	// Update the index value and return the index of the match.
	*index = new_index;
	return match_index;
}

LOCAL int32_t read_int_32(int *index, char *data, int max_index) {
	// Ignore any leading whitespace.
	*index = skip_whitespace(*index, data, max_index);

	// Decode the number.
	int32_t number = 0;
	int32_t multiplier = 1;
	bool number_start = true;
	while (*index < max_index) {
		if ((number_start) && (data[*index] == '-')) {
			multiplier = -1;
			number_start = false;
		} else if ((data[*index] >= '0') && (data[*index] <= '9')) {
			// We have a numeric digit.
			number *= 10;
			number += data[*index] - '0';
		} else {
			// We're done with the number.
			break;
		}
		(*index)++;
	}

	// Ignore any trailing whitespace.
	*index = skip_whitespace(*index, data, max_index);

	// Return the decoded number.
	return number;
}

/*
 * Processes the reception of a message from a web socket.
 */
void ws_recv(Websock *ws, char *data, int len, int flags) {
	// For remote control, expect the following JSON command:
	// {"drive": {"left": <left>, "right": <right>}}
	// First, check we are an object.
	int index = 0;
	index = skip_whitespace(0, data, len);
	if (index == -1) {
		return;
	}
	if (data[index++] != '{') {
		// We must start with an object.
		return;
	}

	// See if this is the drive command.
	if (check_key(&index, data, len, 1, "drive") == -1) {
		// Currently, only the drive command is supported, and this is not it.
		return;
	}

	// Read the drive data
	bool has_left = false;
	bool has_right = false;
	int16_t left = 0;
	int16_t right = 0;
	int match_index;
	if (data[index++] != '{') {
		return;
	}
	while (true) {
		match_index = check_key(&index, data, len, 2, "left", "right");
		switch (match_index) {
			case 0:
				// Read the left value.
				left = (int16_t)read_int_32(&index, data, len);
				has_left = true;
				break;
			case 1:
				// Read the right value.
				right = (int16_t)read_int_32(&index, data, len);
				has_right = true;
				break;
			default:
				// This key is neither left nor right.
				return;
		}
		if (data[index] == ',') {
			// There are more key/value pairs to check.
			index++;
		} else {
			// There are no more key/value pairs to check.
			break;
		}
	}
	if ((!has_left) || (!has_right)) {
		// We didn't get both left and right values.
		return;
	}

	// Drive the stepper motors for the new values.
	rc_left = left;
	rc_right = right;
	drive_motors(left, right, TICK_COUNT, steps_complete);
}

/*
 * Callback function for when the motor's steps have completed in remote control driving mode.
 */
LOCAL void ICACHE_FLASH_ATTR steps_complete() {
	if ((rc_left == 0) && (rc_right == 0)) {
		// Don't start a new cycle, as we don't want to move.
		return;
	}

	// Start up a new cycle.
	drive_motors(rc_left, rc_right, TICK_COUNT, steps_complete);
}

/*
 * Processes the connection for a web socket.
 */
void ws_connected(Websock *ws) {
	ws->recvCb=ws_recv;
}

LOCAL int ICACHE_FLASH_ATTR cgiRunBytecode(HttpdConnData *connData) {
	// Get the bytecode.
	char code[CODE_LEN];
	if (httpdFindArg(connData->post->buff, "code", code, CODE_LEN) == -1) {
		httpCodeReturn(connData, 400, "Missing parameter", "Missing the \"code\" parameter.");
		return HTTPD_CGI_DONE;
	}

	// Convert the bytecode into a program_t structure.
	// We expect the following JSON format:
	// {"program":{
	//   {"globals":<globals>, "functions": [
	//     {"args":<arg_count>,
	//      "locals":<local_var_count>, 
	//      "stack": <stack_size>,
	//      "codes": [<function_bytecode>]
	//     }, ...]
	//   }
	// }}
	// First, check we are an object.
	int index = 0;
	index = skip_whitespace(0, code, CODE_LEN);
	if (index == -1) {
		httpCodeReturn(connData, 400, "Bad parameter", "Invalid \"code\" parameter preamble.");
		return HTTPD_CGI_DONE;
	}
	if (code[index++] != '{') {
		// We must start with an object.
		httpCodeReturn(connData, 400, "Bad parameter", "Invalid \"code\" parameter opening.");
		return HTTPD_CGI_DONE;
	}

	// See if this is the program command.
	if (check_key(&index, code, CODE_LEN, 1, "program") == -1) {
		// Currently, only the drive command is supported, and this is not it.
		httpCodeReturn(connData, 400, "Bad parameter", "Invalid \"code\" parameter - not a program.");
		return HTTPD_CGI_DONE;
	}

	if (code[index++] != '{') {
		// All programs must be objects.
		httpCodeReturn(connData, 400, "Bad parameter", 
				"Invalid \"code\" parameter - program command must be an object.");
		return HTTPD_CGI_DONE;
	}
	program_t *program;
	program = (program_t *)os_malloc(sizeof(program_t));
	if (program == NULL) {
		httpCodeReturn(connData, 500, "Internal error", 
				"Unable to allocate memory to process program.");
		return HTTPD_CGI_DONE;
	}

	// Handle the top-level properties.
	int match_index;
	bool have_globals = false;
	bool have_functions = false;
	while (true) {
		match_index = check_key(&index, code, CODE_LEN, 2, "globals", "functions");
		switch (match_index) {
			case 0: {
				// This is the global information.
				int32_t count = read_int_32(&index, code, CODE_LEN);
				if (count < 0) {
					httpCodeReturn(connData, 400, "Bad parameter", 
							"Invalid global count in \"code\" parameter.");
					return HTTPD_CGI_DONE;
				}
				program->global_count = (uint32_t)count;
				have_globals = true;
				} break;
			case 1: {
				// This is the function definition.
				int ret = parse_functions(&index, code, CODE_LEN, program, connData);
				if (ret == -1) {
					// An error occurred (which is reported inside parse_functions).
					return HTTPD_CGI_DONE;
				}
				have_functions = true;
				} break;
			default:
				// Unknown property.
				httpCodeReturn(connData, 400, "Bad parameter", 
						"Invalid \"code\" parameter - unknown program field.");
				return HTTPD_CGI_DONE;
		}
		if (code[index] == ',') {
			// There are more key/value pairs to check.
			index++;
		} else {
			// There are no more key/value pairs to check.
			break;
		}
	}

	if ((!have_globals) || (!have_functions)) {
		httpCodeReturn(connData, 400, "Bad parameter", 
				"Invalid \"code\" parameter, missing globals or functions.");
		return HTTPD_CGI_DONE;
	}

	// We now have a valid program structure, start execution of the program.
	run_program(program);
	httpCodeReturn(connData, 200, "OK", "OK");
	return HTTPD_CGI_DONE;
}

LOCAL int ICACHE_FLASH_ATTR parse_functions(
	int *index, char *data, int max_index, program_t *program, HttpdConnData *connData) {
	// Functions must reside in an array, see how many there are.
	if (data[*index] != '[') {
		httpCodeReturn(connData, 400, "Bad parameter", 
				"Non-array for functions in \"code\" parameter.");
		return -1;
	}
	(*index)++;
	int16_t bracket_depth = 1;
	int16_t brace_depth = 0;
	uint16_t function_count = 0;
	for (int ii = *index; ii < max_index; ii++) {
		if (data[ii] == ']') {
			bracket_depth--;
			if (bracket_depth == 0) {
				break;
			}
		} else if (data[ii] == '[') {
			bracket_depth++;
		} else if (data[ii] == '{') {
			if (brace_depth == 0) {
				function_count++;
			}
			brace_depth++;
		} else if (data[ii] == '}') {
			brace_depth--;
		}
	}
	if (function_count == 0) {
		httpCodeReturn(connData, 400, "Bad parameter", 
				"No functions found in \"code\" parameter.");
		return -1;
	}
	program->function_count = function_count;
	program->functions = (function_t *)os_malloc(function_count * sizeof(function_t));
	if (program->functions == NULL) {
		httpCodeReturn(connData, 500, "Internal error", 
				"Unable to allocate memory to process program function.");
		return -1;
	}

	int match_index;
	for (int ii = 0; ii < function_count; ii++) {
		// Store the function's ID, which is simply the index.
		program->functions[ii].id = ii;

		if (data[*index] != '{') {
			httpCodeReturn(connData, 400, "Bad parameter",
					"Invalid \"code\" parameter - function object.");
			return -1;
		}
		(*index)++;

		// Read the values for this function.
		bool have_args, have_locals, have_stack, have_code = false;
		while (true) {
			match_index = check_key(index, data, max_index, 4, "args", "locals", "stack", "codes");
			switch (match_index) {
				case 0: {
					// Argument count.
					program->functions[ii].argument_count = read_int_32(index, data, max_index);
					have_args = true;
					} break;
				case 1: {
					// Locals count.
					program->functions[ii].local_count = read_int_32(index, data, max_index);
					have_locals = true;
					} break;
				case 2: {
					// Maximum stack size.
					program->functions[ii].stack_size = read_int_32(index, data, max_index);
					have_stack = true;
					} break;
				case 3: {
					// The bytecode for this function.
					if (data[*index] != '[') {
						httpCodeReturn(connData, 400, "Bad parameter",
								"Bytecode for functions must be in an array.");
						return -1;
					}
					(*index)++;

					// Find out how many bytes are in the bytecode.
					int tmp_index = *index;
					int len = 0;
					bool in_number = false;
					for (int jj = *index; jj < max_index; jj++) {
						if ((data[jj] >= '0') && (data[jj] <= '9')) {
							// This is a numeric digit.
							if (!in_number) {
								in_number = true;
								len++;
							}
						} else if (data[jj] == ',') {
							if (!in_number) {
								// This is an invalid array.
								httpCodeReturn(connData, 400, "Bad parameter",
										"Bytecode for functions must not hold empty numbers.");
								return -1;
							}
							in_number = false;
						} else if (data[jj] == ']') {
							// We've reached the end of the array.
							break;
						} else if ((data[jj] == ' ') || (data[jj] == '\n') || 
								(data[jj] == '\r') || (data[jj] == '\t')) {
							// Ignore whitespace.
							continue;
						} else {
							// This is an invalid array.
							httpCodeReturn(connData, 400, "Bad parameter",
									"Bytecode for functions must be in a valid array.");
							return -1;
						}
					}

					// Copy the bytecode into the function's code.
					program->functions[ii].length = len;
					program->functions[ii].code = (uint8_t *)os_malloc(len);
					if (program->functions[ii].code == NULL) {
						httpCodeReturn(connData, 500, "Internal error", 
								"Unable to allocate memory to process program funtion's code.");
						return -1;
					}
					for (int jj = 0; jj < len; jj++) {
						program->functions[ii].code[jj] = (uint8_t)read_int_32(
								index, data, max_index);
						// Skip the comma and end of array characters.
						(*index)++;
					}

					have_code = true;
					} break;
				default:
					httpCodeReturn(connData, 400, "Bad parameter",
							"Invalid \"code\" parameter - unknown function field.");
					return -1;
			}
			if (data[*index] == ',') {
				// There are more key/value pairs to check.
				(*index)++;
			} else {
				// There are no more key/value pairs to check.
				break;
			}
		}

		// Ensure we got everything we needed.
		if ((!have_args) || (!have_locals) || (!have_stack) || (!have_code)) {
			httpCodeReturn(connData, 400, "Bad parameter",
					"Invalid \"code\" parameter: missing required function parameter.");
			return -1;
		}

		// Read through to the end of the object.
		*index = skip_whitespace(*index, data, max_index);
		if (data[*index] != '}') {
			httpCodeReturn(connData, 400, "Bad parameter",
					"Invalid \"code\" parameter: missing end to function object.");
			return -1;
		}
		(*index)++;

		// Ensure we have a comma at the end of the object, if there are more functions to be read.
		*index = skip_whitespace(*index, data, max_index);
		if ((ii < (function_count - 1)) && (data[*index] != ',')) {
			httpCodeReturn(connData, 400, "Bad parameter",
					"Invalid \"code\" parameter: missing end to function object.");
			return -1;
		}
		(*index)++;

		// Skip any trailing whitespace.
		*index = skip_whitespace(*index, data, max_index);

		// TODO: Debug code.
		/*
		debug_print("Function %d: argc=%d, locals=%d, stack=%d.\n",
				ii, program->functions[ii].argument_count, program->functions[ii].local_count, program->functions[ii].stack_size); 
		os_delay_us(2000);
		*/
	}

	// If we get here, all is good.
	return (int)function_count;
}

LOCAL void ICACHE_FLASH_ATTR httpCodeReturn(HttpdConnData *connData, uint16_t code, char *title, char *message) {
	// Write the header.
	httpdStartResponse(connData, code);
	httpdHeader(connData, "Content-Type", "text/html");
	httpdEndHeaders(connData);

	// Write the body message.
	httpdSend(connData, "<html><head><title>", -1);
	httpdSend(connData, title, -1);
	httpdSend(connData, "</title></head><body><p>", -1);
	httpdSend(connData, message, -1);
	httpdSend(connData, "</p></body></html>", -1);
}

/*
 * Call-back for when we have an event from the wireless internet connection.
 */
LOCAL void ICACHE_FLASH_ATTR wifi_event_cb(System_Event_t *event) {
    struct ip_info info;

    // To determine what actually happened, we need to look at the event.
    switch (event->event) {
        case EVENT_STAMODE_CONNECTED: {
            // We are connected as a station, but we don't have an IP address yet.
            char ssid[33];
            uint8_t len = event->event_info.connected.ssid_len;
            if (len > 32) {
                len = 32;
            }
            strncpy(ssid, event->event_info.connected.ssid, len + 1);
            os_printf("Received EVENT_STAMODE_CONNECTED. "
                      "SSID = %s, BSSID = "MACSTR", channel = %d.\n",
                      ssid, MAC2STR(event->event_info.connected.bssid), event->event_info.connected.channel);
            break;
        }
        case EVENT_STAMODE_DISCONNECTED: {
            // We have been disconnected as a station.
            char ssid[33];
            uint8_t len = event->event_info.connected.ssid_len;
            if (len > 32) {
                len = 32;
            }
            strncpy(ssid, event->event_info.connected.ssid, len + 1);
            os_printf("Received EVENT_STAMODE_DISCONNECTED. "
                      "SSID = %s, BSSID = "MACSTR", channel = %d.\n",
                      ssid, MAC2STR(event->event_info.disconnected.bssid), event->event_info.disconnected.reason);
            break;
        }
        case EVENT_STAMODE_GOT_IP:
            // We have an IP address, ready to run. Return the IP address, too.
            os_printf("Received EVENT_STAMODE_GOT_IP. IP = "IPSTR", mask = "IPSTR", gateway = "IPSTR"\n", 
                      IP2STR(&event->event_info.got_ip.ip.addr), 
                      IP2STR(&event->event_info.got_ip.mask.addr),
                      IP2STR(&event->event_info.got_ip.gw));
            break;
        case EVENT_STAMODE_DHCP_TIMEOUT:
            // We couldn't get an IP address via DHCP, so we'll have to try re-connecting.
            os_printf("Received EVENT_STAMODE_DHCP_TIMEOUT.\n");
            wifi_station_disconnect();
            wifi_station_connect();
            break;
    }
}

/*
 * Sets up the WiFi interface on the ESP8266.
 */
LOCAL void ICACHE_FLASH_ATTR wifi_init() {
    // Set up the call back for the status of the WiFi.
    wifi_set_event_handler_cb(wifi_event_cb);
}

// The URLs that the HTTP server can handle.
HttpdBuiltInUrl builtInUrls[]={
	{"/", cgiRedirect, "/welcome.html"},
	{"/runBytecode.cgi", cgiRunBytecode, NULL},
	{"/ws.cgi", cgiWebsocket, ws_connected},
	{"*", cgiEspFsHook, NULL}, //Catch-all cgi function for the filesystem
	{NULL, NULL, NULL}
};

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

	// Initialise the virtual machine.
	init_vm();

	// Initialise the HTTP server.
	espFsInit((void*)(webpages_espfs_start));
	httpdInit(builtInUrls, 80);
}
