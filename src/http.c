/*
 * http.c: Processes inputs and outputs for the HTTP server.
 *
 * Author: Ian Marshall
 * Date: 11/11/2018
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
#include "cgiwifi.h"

#include "config.h"
#include "files.h"
#include "string_builder.h"
#include "udp_debug.h"
#include "vm.h"

// The base SSID name for the soft AP network.
#define SSID "MICROTURTLE_"

// The maximum number of bytes that will be sent in a single transfer.
#define MAX_TRANSFER_SIZE 1024

// The buffer size used when saving files to flash.
#define UPLOAD_BUFLEN 1024

LOCAL const uint16_t TICK_COUNT = 100;
LOCAL const uint16_t CODE_LEN = 1024;
LOCAL const uint16_t CONFIG_LEN = 512;

// Current values used for remote control of the turtle.
LOCAL int16_t rc_left = 0;
LOCAL int16_t rc_right = 0;

// Forward definitions.
LOCAL int cgiRunBytecode(HttpdConnData *connData);
LOCAL int cgiListFiles(HttpdConnData *connData);
LOCAL int cgiLoadFile(HttpdConnData *connData);
LOCAL int cgiSaveFile(HttpdConnData *connData);
LOCAL int cgiCalibrateLine(HttpdConnData *connData);
LOCAL int cgiCalibrateTurn(HttpdConnData *connData);
LOCAL int tpl_get_configuration(HttpdConnData *connData, char *token, void **arg);
LOCAL int cgiSetConfiguration(HttpdConnData *connData);
LOCAL int cgiWifiStatus(HttpdConnData *connData);
LOCAL int cgiConnectNetwork(HttpdConnData *connData);
LOCAL void drive(Websock *ws, char *data, int len, int index);
LOCAL void get_pen();
LOCAL void move_pen(Websock *ws, char *data, int len, int index);
LOCAL void steps_complete();
LOCAL void ws_connected(Websock *ws);
LOCAL void wifi_event_cb(System_Event_t *event);
LOCAL void httpCodeReturn(HttpdConnData *connData, uint16_t code, char *title, char *message);
LOCAL inline void store_int_32(uint8_t *array, uint8_t index, int32_t value);
LOCAL int json_parse_functions(
		int *index, char *data, int max_index, program_t *program, HttpdConnData *connData);
LOCAL int json_check_key(int *index, char *data, int max_index, int count, ...);
LOCAL int json_skip_whitespace(int index, char *data, int max_index);
LOCAL int32_t json_read_int_32(int *index, char *data, int max_index);
LOCAL string_builder * json_read_string(int *index, char *data, int max_index);

// The URLs that the HTTP server can handle.
HttpdBuiltInUrl builtInUrls[]={
	{"/", cgiRedirect, "/welcome.html"},
	{"/runBytecode.cgi", cgiRunBytecode, NULL},
	{"/ws.cgi", cgiWebsocket, ws_connected},
	{"/file/ls.cgi", cgiListFiles, NULL},
	{"/file/load.cgi", cgiLoadFile, NULL},
	{"/file/save.cgi", cgiSaveFile, NULL},
	{"/configuration", cgiRedirect, "/configuration/configure.tpl"},
	{"/configuration/", cgiRedirect, "/configuration/configure.tpl"},
	{"/configuration/calibrate.tpl", cgiEspFsTemplate, tpl_get_configuration},
	{"/configuration/configure.tpl", cgiEspFsTemplate, tpl_get_configuration},
	{"/configuration/drawLine.cgi", cgiCalibrateLine, NULL},
	{"/configuration/drawTurn.cgi", cgiCalibrateTurn, NULL},
	{"/configuration/setConfiguration.cgi", cgiSetConfiguration, NULL},
	{"/configuration/scan.cgi", cgiWiFiScan, NULL},
	{"/configuration/status.cgi", cgiWifiStatus, NULL},
	{"/configuration/connect.cgi", cgiConnectNetwork, NULL},
	{"*", cgiEspFsHook, NULL}, //Catch-all cgi function for the filesystem
	{NULL, NULL, NULL}
};

// Type used for long file transfers.
typedef struct {
	uint32_t file_number;
	uint32_t offset;
	uint32_t size;
} file_tracker_t;

typedef enum {
	INITIALISE,
	IN_PROGRESS,
	COMPLETE,
	UPLOAD_ERROR
} upload_state_t;

typedef struct {
	upload_state_t state;
	uint32_t file_number;
	uint32_t save_slot;
	uint64_t timestamp;
	char name[MAX_FILENAME_LEN + 1];
	uint32_t offset;
	uint32_t length;
	uint32_t remaining;
	uint32_t buffered;
	char buf[UPLOAD_BUFLEN];
} file_upload_t;

//------------------
// Public functions.
//------------------

/*
 * Notifies any listeners via web socket connections the current program's execution status.
 */
void ICACHE_FLASH_ATTR notify_program_status(prog_status_t status, uint32_t function, uint32_t index) {
	string_builder *sb = create_string_builder(48);
	if (sb == NULL) {
		os_printf("Unable to create string builder for program status notification.\n");
		return;
	}
	append_string_builder(sb, "{\"program\":{\"status\":\"");
	switch (status) {
		case IDLE:
			append_string_builder(sb, "idle\"}}");
			break;
		case RUNNING:
			append_string_builder(sb, "running\",\"function\":");
			append_int32_string_builder(sb, function);
			append_string_builder(sb, ", \"index\": ");
			append_int32_string_builder(sb, index);
			append_string_builder(sb, "}}");
			break;
		case ERROR:
			append_string_builder(sb, "error\"}}");
			break;
		default:
			append_string_builder(sb, "unknown\"}}");
			break;
	}
	cgiWebsockBroadcast("/ws.cgi", sb->buf, sb->len, WEBSOCK_FLAG_NONE);
	free_string_builder(sb);
}

/*
 * Notifies any listeners via web socket connections the current servo position (up/down).
 */
void ICACHE_FLASH_ATTR notify_servo_position(servo_position_t pos) {
	string_builder *sb = create_string_builder(32);
	if (sb == NULL) {
		os_printf("Unable to create string builder for servo position notification.\n");
		return;
	}
	append_string_builder(sb, "{\"servo\":{\"position\":\"");
	switch (pos) {
		case UP:
			append_string_builder(sb, "up\"}}");
			break;
		case DOWN:
			append_string_builder(sb, "down\"}}");
			break;
		default:
			append_string_builder(sb, "unknown\"}}");
			break;
	}
	cgiWebsockBroadcast("/ws.cgi", sb->buf, sb->len, WEBSOCK_FLAG_NONE);
	free_string_builder(sb);
}

/*
 * Sets up the WiFi interface on the ESP8266.
 */
void ICACHE_FLASH_ATTR wifi_init() {
	// Enter station + soft AP mode.
	wifi_set_opmode_current(STATIONAP_MODE);

	// Get the name of the soft AP network.
	uint8_t mac[6];
	wifi_get_macaddr(SOFTAP_IF, mac);
	char ssid[32] = {'\0'};
	uint8_t ssid_len = strlen(SSID);
	os_memcpy(&ssid[0], SSID, ssid_len);
	os_sprintf(&ssid[ssid_len], "%x_%x_%x", mac[3], mac[4], mac[5]);
	ssid_len = strlen(ssid);

	// Configure the soft AP network.
	struct softap_config config;
	wifi_softap_get_config(&config);
	os_memcpy(&config.ssid, ssid, 32);
	config.ssid_len = ssid_len;
	config.authmode = AUTH_OPEN;
	config.ssid_hidden = 0;
	wifi_softap_set_config(&config);

	// Try and get the DHCP information from the access point.
	wifi_station_dhcpc_start();

    // Set up the call back for the status of the WiFi.
    wifi_set_event_handler_cb(wifi_event_cb);
}

/*
 * Initialises the HTTP server.
 */
void ICACHE_FLASH_ATTR http_init() {
	// Initialise the HTTP server.
	espFsInit((void*)(webpages_espfs_start));
	httpdInit(builtInUrls, 80);
}

//------------------------------------------------------------------------------
// HTTP invocation handlers.
//------------------------------------------------------------------------------

/*
 * Runs a program using the supplied bytecode instructions.
 */
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
	//   {"globals": <globals>, "functions": [
	//     {"args": <arg_count>,
	//      "locals": <local_var_count>, 
	//      "stack": <stack_size>,
	//      "codes": [<function_bytecode>]
	//     }, ...]
	//   }
	// }}
	// First, check we are an object.
	int index = 0;
	index = json_skip_whitespace(0, code, CODE_LEN);
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
	if (json_check_key(&index, code, CODE_LEN, 1, "program") == -1) {
		// Currently, only the program command is supported, and this is not it.
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
		match_index = json_check_key(&index, code, CODE_LEN, 2, "globals", "functions");
		switch (match_index) {
			case 0: {
				// This is the global information.
				int32_t count = json_read_int_32(&index, code, CODE_LEN);
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
				int ret = json_parse_functions(&index, code, CODE_LEN, program, connData);
				if (ret == -1) {
					// An error occurred (which is reported inside json_parse_functions).
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

/*
 * Lists all of the files that have been defined in the flash memory.
 */
LOCAL int ICACHE_FLASH_ATTR cgiListFiles(HttpdConnData *connData) {
	// First, get the file list.
	file_t files[FILE_COUNT];
	int count = list_files(files, FILE_COUNT);
	if (count == 0) {
		debug_print("Unable to load any files for file list.\n");
		httpCodeReturn(connData, 500, "Internal Error", "Unable to load any files for file list.");
		return HTTPD_CGI_DONE;
	} else if (count != FILE_COUNT) {
		debug_print("Only %d of %d files were returned in a file list.\n", count, FILE_COUNT);
	}

	string_builder *sb = create_string_builder(128);
	if (sb == NULL) {
		debug_print("Unable to create string builder for file list.\n");
		httpCodeReturn(connData, 500, "Internal Error", "Unable to perepare response for file list.");
		return HTTPD_CGI_DONE;
	} else {
		// Build the JSON string, like:
		// {"files":[{"number":1, "inUse":true, "size":1024, "timestamp":2048, "name":"file.logo"}, ...]}
		append_string_builder(sb, "{\"files\":[");
		for (int ii = 0; ii < count; ii++) {
			append_string_builder(sb, "{\"number\":");
			append_int32_string_builder(sb, ii);
			append_string_builder(sb, ", \"inUse\":");
			if (files[ii].in_use) {
				append_string_builder(sb, "true");
			} else {
				append_string_builder(sb, "false");
			}
			append_string_builder(sb, ", \"size\":");
			append_int32_string_builder(sb, files[ii].size);
			append_string_builder(sb, ", \"timestamp\":");
			append_int32_string_builder(sb, files[ii].timestamp);
			append_string_builder(sb, ", \"name\":\"");
			append_string_builder(sb, files[ii].name);
			if (ii < (count - 1)) {
				append_string_builder(sb, "\"}, ");
			} else {
				append_string_builder(sb, "\"}");
			}
		}
		append_string_builder(sb, "]}");
	}

	// Write the response header.
	httpdStartResponse(connData, 200);
	httpdHeader(connData, "Content-Type", "application/json");
	httpdEndHeaders(connData);

	// Write the response JSON message.
	httpdSend(connData, sb->buf, sb->len);

	// Free the string builder.
	free_string_builder(sb);

	return HTTPD_CGI_DONE;
}

/*
 * Loads a file from the flash memory.
 */
LOCAL int ICACHE_FLASH_ATTR cgiLoadFile(HttpdConnData *connData) {
	file_tracker_t *track = connData->cgiData;
	if (connData->conn == NULL) {
		if (track != NULL) {
			free(track);
		}
		return HTTPD_CGI_DONE;
	}

	if (track == NULL) {
		// Get the parameters.
		char num_buf[12];
		if (httpdFindArg(connData->getArgs, "file_number", num_buf, 12) == -1) {
			httpCodeReturn(connData, 400, "Missing parameter", "Missing the \"file_number\" parameter.");
			free(track);
			return HTTPD_CGI_DONE;
		}
		uint32_t file_number = atoi(num_buf);
		if (file_number >= FILE_COUNT) {
			httpCodeReturn(connData, 400, "Invalid parameter", "The selected file number is invalid.");
			free(track);
			return HTTPD_CGI_DONE;
		}

		// Get the file list.
		file_t files[FILE_COUNT];
		int count = list_files(files, FILE_COUNT);
		if (count < (file_number + 1)) {
			debug_print("Unable to load file list for file %d.\n", file_number);
			httpCodeReturn(connData, 500, "Internal Error", "Unable to load file list for file.");
			free(track);
			return HTTPD_CGI_DONE;
		}

		// Ensure there is a file to load.
		if ((!files[file_number].in_use) || (files[file_number].size == 0)) {
			debug_print("Unable to load file %d that is not in use.\n", file_number);
			httpCodeReturn(connData, 400, "File is not in use", "Unable to load file that has not been saved.");
			free(track);
			return HTTPD_CGI_DONE;
		}
		
		// Prepare the state structure.
		track = malloc(sizeof(file_tracker_t));
		if (track == NULL) {
			debug_print("Unable to allocate memory for tracker for file %d.\n", file_number);
			httpCodeReturn(connData, 400, "Memory overflow", "Unable to allocate memory to fulfil load request.");
			free(track);
			return HTTPD_CGI_DONE;
		}
		track->file_number = file_number;
		track->offset = 0;
		track->size = files[file_number].size;
		connData->cgiData = track;
	}

	// Get the file's contents.
	char buf[MAX_TRANSFER_SIZE];
	int32_t remaining = track->size - track->offset;
	uint32_t size = (remaining > MAX_TRANSFER_SIZE) ? MAX_TRANSFER_SIZE : remaining;
	size += ((size % 4) == 0) ? 0 : (4 - (size % 4));
	debug_print("Loading file %d to %x from %d of size %d.\n",
			(uint8_t)track->file_number, buf, track->offset, size);
	bool res = load_file((uint8_t)track->file_number, buf, track->offset, size);
	if (!res) {
		debug_print("Unable to load file %d.\n", track->file_number);
		httpCodeReturn(connData, 500, "Internal Error", "Unable to load file.");
		return HTTPD_CGI_DONE;
	}
	if (track->offset == 0) {
		// Write the response header.
		httpdStartResponse(connData, 200);
		httpdHeader(connData, "Content-Type", "text/plain");
		httpdEndHeaders(connData);
	}

	// Send the file's contents.
	httpdSend(connData, buf, (remaining < size) ? remaining : size);

	remaining -= size;
	if (remaining <= 0) {
		// Transfer complete.
		free(track);
		return HTTPD_CGI_DONE;
	} else {
		// There is still more to transfer.
		track->offset += size;
		return HTTPD_CGI_MORE;
	}
}

/*
 * Saves a file to the flash memory.
 */
LOCAL int ICACHE_FLASH_ATTR cgiSaveFile(HttpdConnData *connData) {
	file_upload_t *upl = connData->cgiData;
	if (connData->conn == NULL) {
		if (upl != NULL) {
			free(upl);
		}
		return HTTPD_CGI_DONE;
	}

	if (upl == NULL) {
		// Set up the upload structure.
		upl = (file_upload_t *)malloc(sizeof(file_upload_t));
		if (upl == NULL) {
			debug_print("Unable to allocate memory for file upload.\n");
			return HTTPD_CGI_DONE;
		}
		memset(upl, 0, sizeof(file_upload_t));
		upl->state = INITIALISE;
		connData->cgiData = upl;
	}

	char *data = connData->post->buff;
	int dataLen = connData->post->buffLen;

	while (dataLen > 0) {
		if (upl->state == INITIALISE) {
			// We're initialising the upload. Get the required information.
			char l_buf[12];
			if (httpdGetHeader(connData, "Content-Length", l_buf, 12)) {
				upl->length = atoi(l_buf);
				if ((upl->length <= 0) || (upl->length > MAX_FILE_SIZE)) {
					debug_print("Bad file size: %d.\n", upl->length);
					httpCodeReturn(connData, 400, "Invalid file size", 
							"Bad file size.");
					free(upl);
					return HTTPD_CGI_DONE;
				}
			}
			char *p = data;
			char *e;
			if (strncmp(p, "number: ", 8) == 0) {
				p += 8;
				e = strstr(p, "\r\n");
				if (e == NULL) {
					debug_print("Missing end to number argument.\n");
					httpCodeReturn(connData, 400, "Invalid parameter", 
							"Bad \"number\" argument.");
					free(upl);
					return HTTPD_CGI_DONE;
				}
				*e = '\0';
				upl->file_number = atoi(p);
				if ((upl->file_number < 0) || (upl->file_number >= FILE_COUNT)) {
					debug_print("Bad file number: %d.\n", upl->file_number);
					httpCodeReturn(connData, 400, "Invalid parameter", 
							"Bad \"number\" argument.");
					free(upl);
					return HTTPD_CGI_DONE;
				}
				p = e + 2;
			} else {
				debug_print("Missing number.\n");
				httpCodeReturn(connData, 400, "Invalid parameter", 
						"Missing \"number\" argument.");
				return HTTPD_CGI_DONE;
			}
			if (strncmp(p, "name: ", 6) == 0) {
				p += 6;
				e = strstr(p, "\r\n");
				if (e == NULL) {
					debug_print("Missing end to name argument.\n");
					httpCodeReturn(connData, 400, "Invalid parameter", 
							"Bad \"name\" argument.");
					free(upl);
					return HTTPD_CGI_DONE;
				}
				*e = '\0';
				strncpy(upl->name, p, MAX_FILENAME_LEN);
				p = e + 2;
			} else {
				debug_print("Missing name.\n");
				httpCodeReturn(connData, 400, "Invalid parameter", 
						"Missing \"name\" argument.");
				return HTTPD_CGI_DONE;
			}
			if (strncmp(p, "timestamp: ", 11) == 0) {
				p += 11;
				e = strstr(p, "\r\n");
				if (e == NULL) {
					debug_print("Missing end to timestamp argument.\n");
					httpCodeReturn(connData, 400, "Invalid parameter", 
							"Bad \"timestamp\" argument.");
					free(upl);
					return HTTPD_CGI_DONE;
				}
				*e = '\0';
				upl->timestamp = strtoll(p, NULL, 10);
				if (upl->timestamp == 0) {
					debug_print("Bad timestamp: %lld.\n", upl->timestamp);
					httpCodeReturn(connData, 400, "Invalid parameter", 
							"Bad \"timestamp\" argument.");
					free(upl);
					return HTTPD_CGI_DONE;
				}
				p = e + 2;
			} else {
				debug_print("Missing timestamp.\n");
				httpCodeReturn(connData, 400, "Invalid parameter", 
						"Missing \"timestamp\" argument.");
				return HTTPD_CGI_DONE;
			}

			// Update the values.
			upl->length -= (p - data);
			upl->remaining = upl->length;
			dataLen -= (p - data);
			data = p;
			upl->state = IN_PROGRESS;

			// Prepare to write the file.
			debug_print("Preparing file save for num=%d, size=%d.\n",
					upl->file_number, upl->length);
			upl->save_slot = prepare_file_save(upl->file_number, upl->length);
			if (upl->save_slot == 255) {
				httpCodeReturn(connData, 500, "Internal error",
						"Unable to prepare for file save.");
				free(upl);
				return HTTPD_CGI_DONE;
			}
		} else if (upl->state == IN_PROGRESS) {
			int left_in_block = UPLOAD_BUFLEN - upl->buffered;
			if (left_in_block > upl->remaining) {
				left_in_block = upl->remaining;
			}

			if (dataLen < left_in_block) {
				// We're still not finished with this block.
				memcpy(&upl->buf[upl->buffered], data, dataLen);
				upl->buffered += dataLen;
				upl->remaining -= dataLen;
				dataLen = 0;
			} else {
				// This block (or the rest of the file) is finished.
				memcpy(&upl->buf[upl->buffered], data, left_in_block);
				upl->buffered += left_in_block;
				upl->remaining -= left_in_block;
				store_file_data(upl->save_slot, upl->buffered, upl->offset, upl->buf);
				upl->offset += upl->buffered;
				upl->buffered = 0;
				dataLen -= left_in_block;
				data += left_in_block;
				if (upl->remaining == 0) {
					// The file upload is complete.
					debug_print("Completing file save.\n");
					bool ret = complete_file_save(
							upl->file_number,
							upl->length,
							upl->timestamp,
							upl->name,
							upl->save_slot);
					upl->state = ret ? COMPLETE : UPLOAD_ERROR;
				}
			}
		} else if ((upl->state == COMPLETE) || (upl->state == UPLOAD_ERROR)) {
			// Consume all data without processing it.
			dataLen = 0;
		}
	}

	if ((upl->state == COMPLETE) || (upl->state == UPLOAD_ERROR)) {
		debug_print("Upload finished with state %d.\n", upl->state);
		// The file is finished - either good or bad.
		if (upl->state == COMPLETE) {
			httpCodeReturn(connData, 200, "Success", "File was saved successfully.");
		} else {
			httpCodeReturn(connData, 500, "Unable to save file", 
					"An error occurred while saving the file.");
		}
		free (upl);
		return HTTPD_CGI_DONE;
	}

	return HTTPD_CGI_MORE;
}

/*
 * Draws a line for calibration purposes.
 */
LOCAL int ICACHE_FLASH_ATTR cgiCalibrateLine(HttpdConnData *connData) {
	if (connData->conn == NULL) {
		return HTTPD_CGI_DONE;
	}

	// Get the parameters.
	char l_buf[12];
	char r_buf[12];
	if (httpdFindArg(connData->post->buff, "left", l_buf, 12) == -1) {
		httpCodeReturn(connData, 400, "Missing parameter", "Missing the \"left\" parameter.");
		return HTTPD_CGI_DONE;
	}
	if (httpdFindArg(connData->post->buff, "right", r_buf, 12) == -1) {
		httpCodeReturn(connData, 400, "Missing parameter", "Missing the \"right\" parameter.");
		return HTTPD_CGI_DONE;
	}
	uint32_t left = atoi(l_buf);
	uint32_t right = atoi(r_buf);

	// Create the program to draw the line.
	program_t *program;
	program = (program_t *)os_malloc(sizeof(program_t));
	if (program == NULL) {
		httpCodeReturn(connData, 500, "Internal error", 
				"Unable to allocate memory to process calibration request.");
		return HTTPD_CGI_DONE;
	}
	program->global_count = 0;
	program->function_count = 1;
	program->functions = (function_t *)os_malloc(sizeof(function_t));
	if (program->functions == NULL) {
		os_free(program);
		httpCodeReturn(connData, 500, "Internal error", 
				"Unable to allocate memory to process calibration request.");
		return HTTPD_CGI_DONE;
	}
	program->functions[0].id = 0;
	program->functions[0].argument_count = 0;
	program->functions[0].local_count = 0;
	program->functions[0].stack_size = 2;
	program->functions[0].length = 14;
	program->functions[0].code = (uint8_t *)os_malloc(13);
	if (program->functions[0].code == NULL) {
		os_free(program->functions);
		os_free(program);
		httpCodeReturn(connData, 500, "Internal error", 
				"Unable to allocate memory to process calibration request.");
		return HTTPD_CGI_DONE;
	}
	program->functions[0].code[0]  = 6;   // PD
	program->functions[0].code[1]  = 15;  // IConst (left)
	store_int_32(program->functions[0].code, 2, left);
	program->functions[0].code[6]  = 15;  // IConst (right)
	store_int_32(program->functions[0].code, 7, right);
	program->functions[0].code[11] = 44; // FDRAW
	program->functions[0].code[12] = 5;  // PU
	program->functions[0].code[13] = 40; // STOP

	// Begin the line sequence.
	run_program(program);
	httpCodeReturn(connData, 200, "OK", "OK");
	return HTTPD_CGI_DONE;
}

/*
 * Draws two lines at a 180° angle for calibration purposes.
 */
LOCAL int ICACHE_FLASH_ATTR cgiCalibrateTurn(HttpdConnData *connData) {
	if (connData->conn == NULL) {
		return HTTPD_CGI_DONE;
	}

	// Get the parameters.
	char l_buf[12];
	char r_buf[12];
	if (httpdFindArg(connData->post->buff, "left", l_buf, 12) == -1) {
		httpCodeReturn(connData, 400, "Missing parameter", "Missing the \"left\" parameter.");
		return HTTPD_CGI_DONE;
	}
	if (httpdFindArg(connData->post->buff, "right", r_buf, 12) == -1) {
		httpCodeReturn(connData, 400, "Missing parameter", "Missing the \"right\" parameter.");
		return HTTPD_CGI_DONE;
	}
	uint32_t left = atoi(l_buf);
	uint32_t right = atoi(r_buf);
	if (httpdFindArg(connData->post->buff, "leftStraight", l_buf, 12) == -1) {
		httpCodeReturn(connData, 400, "Missing parameter", "Missing the \"leftStraight\" parameter.");
		return HTTPD_CGI_DONE;
	}
	if (httpdFindArg(connData->post->buff, "rightStraight", r_buf, 12) == -1) {
		httpCodeReturn(connData, 400, "Missing parameter", "Missing the \"rightStraight\" parameter.");
		return HTTPD_CGI_DONE;
	}
	uint32_t leftStraight = atoi(l_buf);
	uint32_t rightStraight = atoi(r_buf);

	// Create the program to draw the lines.
	program_t *program;
	program = (program_t *)os_malloc(sizeof(program_t));
	if (program == NULL) {
		httpCodeReturn(connData, 500, "Internal error", 
				"Unable to allocate memory to process calibration request.");
		return HTTPD_CGI_DONE;
	}
	program->global_count = 0;
	program->function_count = 1;
	program->functions = (function_t *)os_malloc(sizeof(function_t));
	if (program->functions == NULL) {
		os_free(program);
		httpCodeReturn(connData, 500, "Internal error", 
				"Unable to allocate memory to process calibration request.");
		return HTTPD_CGI_DONE;
	}
	program->functions[0].id = 0;
	program->functions[0].argument_count = 0;
	program->functions[0].local_count = 0;
	program->functions[0].stack_size = 2;
	program->functions[0].length = 36;
	program->functions[0].code = (uint8_t *)os_malloc(33);
	if (program->functions[0].code == NULL) {
		os_free(program->functions);
		os_free(program);
		httpCodeReturn(connData, 500, "Internal error", 
				"Unable to allocate memory to process calibration request.");
		return HTTPD_CGI_DONE;
	}
	program->functions[0].code[0] = 6;   // PD

	program->functions[0].code[1] = 15;  // IConst (left straight)
	store_int_32(program->functions[0].code, 2, leftStraight);
	program->functions[0].code[6] = 15;  // IConst (right straight)
	store_int_32(program->functions[0].code, 7, rightStraight);
	program->functions[0].code[11] = 44; // FDRAW

	program->functions[0].code[12] = 15;  // IConst (left)
	store_int_32(program->functions[0].code, 13, left);
	program->functions[0].code[17] = 15;  // IConst (right)
	store_int_32(program->functions[0].code, 18, right);
	program->functions[0].code[22] = 47; // RTRAW

	program->functions[0].code[23] = 15;  // IConst (left straight)
	store_int_32(program->functions[0].code, 24, leftStraight/2);
	program->functions[0].code[28] = 15;  // IConst (right straight)
	store_int_32(program->functions[0].code, 29, rightStraight/2);
	program->functions[0].code[33] = 44; // FDRAW

	program->functions[0].code[34] = 5;  // PU
	program->functions[0].code[35] = 40; // STOP

	// Begin the line sequence.
	run_program(program);
	httpCodeReturn(connData, 200, "OK", "OK");
	return HTTPD_CGI_DONE;
}

/*
 * Retrieves the current configuration for the turtle.
 */
LOCAL int ICACHE_FLASH_ATTR tpl_get_configuration(HttpdConnData *connData, char *token, void **arg) {
	if (token == NULL) {
		// The call has been cancelled.
		return HTTPD_CGI_DONE;
	}

	// Get the configuration.
	config_t config;
	get_configuration(&config);

	// Store the configuration value in the buffer.
	char buf[12];
	if (os_strcmp(token, "straightStepsLeft") == 0) {
		os_sprintf(buf, "%d", config.straight_steps_left);
	} else if (os_strcmp(token, "straightStepsRight") == 0) {
		os_sprintf(buf, "%d", config.straight_steps_right);
	} else if (os_strcmp(token, "turnStepsLeft") == 0) {
		os_sprintf(buf, "%d", config.turn_steps_left);
	} else if (os_strcmp(token, "turnStepsRight") == 0) {
		os_sprintf(buf, "%d", config.turn_steps_right);
	} else if (os_strcmp(token, "servoUpAngle") == 0) {
		os_sprintf(buf, "%d", config.servo_up_angle);
	} else if (os_strcmp(token, "servoDownAngle") == 0) {
		os_sprintf(buf, "%d", config.servo_down_angle);
	} else if (os_strcmp(token, "servoMoveSteps") == 0) {
		os_sprintf(buf, "%d", config.servo_move_steps);
	} else if (os_strcmp(token, "servoTickInterval") == 0) {
		os_sprintf(buf, "%d", config.servo_tick_interval);
	} else if (os_strcmp(token, "motorTickInterval") == 0) {
		os_sprintf(buf, "%d", config.motor_tick_interval);
	} else if (os_strcmp(token, "movementPause") == 0) {
		os_sprintf(buf, "%d", config.move_pause_duration);
	} else {
		return HTTPD_CGI_DONE;
	}

	// Send the buffer's contents to fill in the token.
	httpdSend(connData, buf, -1);
	return HTTPD_CGI_DONE;
}

/*
 * Sets the configuration data for the calibration in the ESP's flash memory.
 */
LOCAL int ICACHE_FLASH_ATTR cgiSetConfiguration(HttpdConnData *connData) {
	if (connData->conn == NULL) {
		return HTTPD_CGI_DONE;
	}

	// Get the parameters.
	char configuration[CONFIG_LEN];
	if (httpdFindArg(connData->post->buff, "configuration", configuration, CONFIG_LEN) == -1) {
		httpCodeReturn(connData, 400, "Missing parameter", "Missing the \"left\" parameter.");
		return HTTPD_CGI_DONE;
	}

	// Convert the structure into the configuration entries.
	// We expect the following JSON format:
	// {"configuration":{
	//   {"straightStepsLeft": <straight_steps_left>,
	//    "straightStepsRight": <straight_steps_right>,
	//    "turnStepsLeft": <turn_steps_left>,
	//    "turnStepsRight": <turn_steps_right>,
	//    "servoUpAngle": <servo_up_angle>,           (optional)
	//    "servoDownAngle": <servo_down_angle>,       (optional)
	//    "servoMoveSteps": <servo_move_steps>,       (optional)
	//    "servoTickInterval": <servo_tick_interval>, (optional)
	//    "motorTickInterval": <motor_tick_interval>, (optional)
	//    "movementPause": <movement_pause>           (optional)
	//   }
	// }}
	// First, check we are an object.
	int index = 0;
	index = json_skip_whitespace(0, configuration, CONFIG_LEN);
	if (index == -1) {
		httpCodeReturn(connData, 400, "Bad parameter", "Invalid \"configuration\" parameter preamble.");
		return HTTPD_CGI_DONE;
	}
	if (configuration[index++] != '{') {
		// We must start with an object.
		httpCodeReturn(connData, 400, "Bad parameter", "Invalid \"configuration\" parameter opening.");
		return HTTPD_CGI_DONE;
	}

	// See if this is the configuration command.
	if (json_check_key(&index, configuration, CODE_LEN, 1, "configuration") == -1) {
		// Currently, only the drive command is supported, and this is not it.
		httpCodeReturn(connData, 400, "Bad parameter", "Invalid \"configuration\" parameter - not a configuration.");
		return HTTPD_CGI_DONE;
	}

	if (configuration[index++] != '{') {
		// All programs must be objects.
		httpCodeReturn(connData, 400, "Bad parameter", 
				"Invalid \"configuration\" parameter - configuration command must be an object.");
		return HTTPD_CGI_DONE;
	}

	// Get the current configuration, so unused optional parameters keep their value.
	config_t config;
	get_configuration(&config);

	int match_index;
	bool have_ssl = false;
	bool have_ssr = false;
	bool have_tsl = false;
	bool have_tsr = false;
	while (true) {
		match_index = json_check_key(&index, configuration, CONFIG_LEN, 10,
				"straightStepsLeft", "straightStepsRight", "turnStepsLeft", "turnStepsRight",
				"servoUpAngle", "servoDownAngle", "servoMoveSteps", "servoTickInterval",
				"motorTickInterval", "movementPause");

		if ((match_index >= 0) && (match_index < 10)) {
			int32_t value = json_read_int_32(&index, configuration, CONFIG_LEN);
			if ((value < 100) && (match_index < 4)) {
				// The step counts must be > 100 to make any kind of sense.
				char *paramName;
				switch(match_index) {
					case 0:
						paramName = "straightStepsLeft";
						break;
					case 1:
						paramName = "straightStepsRight";
						break;
					case 2:
						paramName = "turnStepsLeft";
						break;
					case 3:
						paramName = "turnStepsRight";
						break;
				}
				string_builder *sb = create_string_builder(64);
				if (sb == NULL) {
					os_printf("Unable to create string builder for set configuration reply.");
					httpCodeReturn(connData, 400, "Bad parameter",
							"Invalid value for configuration parameter in \"configuration\" parameter.");
				} else {
					append_string_builder(sb, "Invalid value for \"");
					append_string_builder(sb, paramName);
					append_string_builder(sb, "\" parameter in \"configuration\" parameter: ");
					append_int32_string_builder(sb, value);
					httpCodeReturn(connData, 400, "Bad parameter", sb->buf);
					free_string_builder(sb);

				}
				return HTTPD_CGI_DONE;
			}
			switch (match_index) {
				case 0:
					// Straight steps left.
					config.straight_steps_left = value;
					have_ssl = true;
					break;
				case 1:
					// Straight steps right.
					config.straight_steps_right = value;
					have_ssr = true;
					break;
				case 2:
					// Turn steps left.
					config.turn_steps_left = value;
					have_tsl = true;
					break;
				case 3:
					// Turn steps right.
					config.turn_steps_right = value;
					have_tsr = true;
					break;
				case 4:
					// Servo up angle.
					config.servo_up_angle = value;
					break;
				case 5:
					// Servo down angle.
					config.servo_down_angle = value;
					break;
				case 6:
					// Servo move steps.
					config.servo_move_steps = value;
					break;
				case 7:
					// Servo tick interval.
					config.servo_tick_interval = value;
					break;
				case 8:
					// Motor tick interval.
					config.motor_tick_interval = value;
					break;
				case 9:
					// Servo step pause.
					config.move_pause_duration = value;
					break;
			}
		} else {
			httpCodeReturn(connData, 400, "Bad parameter",
					"Invalid \"configuration\" parameter field - unknown field.");
			return HTTPD_CGI_DONE;
		}

		if (configuration[index] == ',') {
			// There are more key/value pairs to check.
			index++;
		} else {
			// There are no more key/value pairs to check.
			break;
		}
	}

	if (!have_ssl || !have_ssr || !have_tsl || !have_tsr) {
		httpCodeReturn(connData, 400, "Bad parameter",
				"Missing \"configuration\" parameter field.");
		return HTTPD_CGI_DONE;
	}

	// Store the configuration.
	if (store_configuration(&config)) {
		init_motor_timer();
		httpCodeReturn(connData, 200, "OK", "OK");
	} else {
		httpCodeReturn(connData, 500, "Internal error", "Unable to store configuration in flash memory.");
	}
	return HTTPD_CGI_DONE;
}

/*
 * CGI function to return the current status of the WiFi connection as JSON data.
 */
LOCAL int cgiWifiStatus(HttpdConnData *connData) {
	string_builder *sb = create_string_builder(128);
	if (sb == NULL) {
		httpCodeReturn(connData, 500, "Resource error", "Unable to allocate internal memory for request.");
		return HTTPD_CGI_DONE;
	}

	// Get the operating mode status.
	uint8_t mode = wifi_get_opmode_default();
	append_string_builder(sb, "{\"opmode\": \"");
	switch (mode) {
		case STATION_MODE:
			append_string_builder(sb, "Station");
			break;
		case SOFTAP_MODE:
			append_string_builder(sb, "Access Point");
			break;
		case STATIONAP_MODE:
			append_string_builder(sb, "Station and Access Point");
			break;
		default:
			append_string_builder(sb, "Unknown");
			break;
	}

	// Get the access point information.
	append_string_builder(sb, "\", \"ap\": { ");
	struct softap_config apConfig;
	bool res = wifi_softap_get_config(&apConfig);
	if (res) {
		append_string_builder(sb, "\"ssid\": \"");
		append_string_builder(sb, apConfig.ssid);
		append_string_builder(sb, "\", \"ssidHidden\": \"");
		append_string_builder(sb, (apConfig.ssid_hidden == 0) ? "No" : "Yes");
		append_string_builder(sb, "\", \"password\": \"");
		append_string_builder(sb, apConfig.password);
		append_string_builder(sb, "\", \"channel\": ");
		append_int32_string_builder(sb, (int32_t)apConfig.channel);
		append_string_builder(sb, ", \"auth\": \"");
		switch (apConfig.authmode) {
			case AUTH_OPEN:
				append_string_builder(sb, "Open");
				break;
			case AUTH_WEP:
				append_string_builder(sb, "WEP");
				break;
			case AUTH_WPA_PSK:
				append_string_builder(sb, "WPA PSK");
				break;
			case AUTH_WPA2_PSK:
				append_string_builder(sb, "WPA2 PSK");
				break;
			case AUTH_WPA_WPA2_PSK:
				append_string_builder(sb, "WPA/WPA2 PSK");
				break;
			default:
				append_string_builder(sb, "Unknown");
				break;
		}
		append_string_builder(sb, "\", ");
	}
    struct ip_info info;
	res = wifi_get_ip_info(SOFTAP_IF, &info);
	if (!res) {
		append_string_builder(sb, "\"ip\": \"Unknown\"");
	} else {
		char buf[20];
		os_sprintf(buf, IPSTR, IP2STR(&info.ip));
		append_string_builder(sb, "\"ip\": \"");
		append_string_builder(sb, buf);
		append_string_builder(sb, "\", ");
	}
	uint8_t mac[6];
	char mac_str[18];
	wifi_get_macaddr(SOFTAP_IF, mac);
	append_string_builder(sb, "\"mac\": \"");
	os_sprintf(mac_str, MACSTR, MAC2STR(mac));
	append_string_builder(sb, mac_str);
	int32_t clients = (int32_t)wifi_softap_get_station_num();
	append_string_builder(sb, "\", \"clientCount\": ");
	append_int32_string_builder(sb, clients);
	append_string_builder(sb, "}");

	// Get the station information.
	append_string_builder(sb, ", \"station\": { \"status\": ");
	int stnStatus = wifi_station_get_connect_status();
	switch (stnStatus) {
		case STATION_IDLE:
			append_string_builder(sb, "\"Idle\"");
			break;
		case STATION_CONNECTING:
			append_string_builder(sb, "\"Connecting\"");
			break;
		case STATION_WRONG_PASSWORD:
			append_string_builder(sb, "\"Incorrect password\"");
			break;
		case STATION_NO_AP_FOUND:
			append_string_builder(sb, "\"Access point not found\"");
			break;
		case STATION_CONNECT_FAIL:
			append_string_builder(sb, "\"Connection failed\"");
			break;
		case STATION_GOT_IP:
			append_string_builder(sb, "\"Connected\", \"ip\": \"");
			res = wifi_get_ip_info(STATION_IF, &info);
			if (!res) {
				append_string_builder(sb, "Unknown\"");
			} else {
				char buf[20];
				os_sprintf(buf, IPSTR, IP2STR(&info.ip));
				append_string_builder(sb, buf);
				append_string_builder(sb, "\"");
			}
			break;
	}
	struct station_config config;
	res = wifi_station_get_config(&config);
	if (res) {
		append_string_builder(sb, ", \"ssid\": \"");
		append_string_builder(sb, config.ssid);
		append_string_builder(sb, "\"");
	}
	wifi_get_macaddr(STATION_IF, mac);
	append_string_builder(sb, ", \"mac\": \"");
	os_sprintf(mac_str, MACSTR, MAC2STR(mac));
	append_string_builder(sb, mac_str);
	append_string_builder(sb, "\", \"rssi\": ");
	int8_t rssi = wifi_station_get_rssi();
	if (rssi == 31) {
		append_string_builder(sb, "\"Unknown\" }");
	} else {
		append_int32_string_builder(sb, (int32_t)rssi);
		append_string_builder(sb, " }");
	}

	// Send the JSON response.
	append_string_builder(sb, "}");
	httpdStartResponse(connData, 200);
	httpdHeader(connData, "Content-Type", "text/json");
	httpdEndHeaders(connData);
	httpdSend(connData, sb->buf, sb->len);
	free_string_builder(sb);
	return HTTPD_CGI_DONE;
}

/*
 * CGI function to configure the network, connecting to a station if required.
 * All required parameters are sourced from the HTML connection.
 */
LOCAL int ICACHE_FLASH_ATTR cgiConnectNetwork(HttpdConnData *connData) {
	// Get the settings.
	char essid[33];
	char passwd[65];
	char tmp_str[8];
	bool is_save;

	if (connData->conn==NULL) {
		//Connection aborted. Clean up.
		return HTTPD_CGI_DONE;
	}
	
	// Get the parameters.
	if (httpdFindArg(connData->post->buff, "stnEssid", essid, sizeof(essid)) == -1) {
		// No ESSID has been set.
		httpCodeReturn(connData, 400, "Missing parameter", "Missing the \"stnEssid\" parameter.");
		return HTTPD_CGI_DONE;
	}
	if (httpdFindArg(connData->post->buff, "stnPasswd", passwd, sizeof(passwd)) == -1) {
		// No ESSID has been set.
		httpCodeReturn(connData, 400, "Missing parameter", "Missing the \"stnPasswd\" parameter.");
		return HTTPD_CGI_DONE;
	}
	if (httpdFindArg(connData->post->buff, "auth", tmp_str, sizeof(tmp_str)) == -1) {
		// No auth mode has been set.
		httpCodeReturn(connData, 400, "Missing parameter", "Missing the \"auth\" parameter.");
		return HTTPD_CGI_DONE;
	}
	int auth = atoi(tmp_str);
	if ((auth < AUTH_OPEN) || (auth > AUTH_WPA_WPA2_PSK)) {
		httpCodeReturn(connData, 400, "Invalid parameter", "Bad valule for the \"auth\" parameter.");
		return HTTPD_CGI_DONE;
	}
	if ((auth != AUTH_OPEN) && (httpdFindArg(connData->post->buff, "apPasswd", passwd, sizeof(passwd)) == -1)) {
		// No ESSID has been set.
		httpCodeReturn(connData, 400, "Missing parameter", "Missing the \"apPasswd\" parameter.");
		return HTTPD_CGI_DONE;
	}
	if (httpdFindArg(connData->post->buff, "doSave", tmp_str, sizeof(tmp_str)) == -1) {
		// No save flag has been set, don't save.
		is_save = false;
	} else {
		if ((tmp_str[0] == 't') || (tmp_str[1] == 'T')) {
			is_save = true;
		} else {
			is_save = false;
		}
	}

	// Set the station configuration.
	struct station_config stnConfig;
	stnConfig.bssid_set = 0;
	os_memcpy(&stnConfig.ssid, essid, 32);
	os_memcpy(&stnConfig.password, passwd, 64);
	wifi_station_dhcpc_stop();
	wifi_station_disconnect();
	if (is_save) {
		wifi_station_set_config(&stnConfig);
	} else {
		wifi_station_set_config_current(&stnConfig);
	}
	wifi_station_connect();
	wifi_station_dhcpc_start();

	// Set the soft AP configuration.
	struct softap_config apConfig;
	wifi_softap_get_config(&apConfig);
	os_memcpy(&apConfig.ssid, essid, 32);
	apConfig.ssid_len = strlen(apConfig.ssid);
	if (auth != AUTH_OPEN) {
		os_memcpy(&apConfig.password, passwd, 64);
	}
	apConfig.channel = 11;
	apConfig.ssid_hidden = 0;
	if (is_save) {
		wifi_softap_set_config(&apConfig);
	} else {
		wifi_softap_set_config_current(&apConfig);
	}

	httpdRedirect(connData, "/net/networks.html");
	return HTTPD_CGI_DONE;
}

/*
 * Processes the reception of a message from a web socket.
 */
LOCAL void ICACHE_FLASH_ATTR ws_recv(Websock *ws, char *data, int len, int flags) {
	// First, check we are an object.
	int index = 0;
	index = json_skip_whitespace(0, data, len);
	if (index == -1) {
		return;
	}
	if (data[index++] != '{') {
		// We must start with an object.
		return;
	}

	// Check the command.
	int match_index = json_check_key(&index, data, len, 3, "drive", "getPen", "movePen");
	switch (match_index) {
		case 0:
			// This is a drive command.
			drive(ws, data, len, index);
			break;
		case 1:
			// This is a get pen command.
			get_pen();
			break;
		case 2:
			// This is a move pen command.
			move_pen(ws, data, len, index);
			break;
	}
}

/*
 * Processes the reception of a message from a web socket containing remote control drive instructions.
 * We expect the following JSON command:
 *     {"drive": {"left": <left>, "right": <right>}}
 */
LOCAL void ICACHE_FLASH_ATTR drive(Websock *ws, char *data, int len, int index) {
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
		match_index = json_check_key(&index, data, len, 2, "left", "right");
		switch (match_index) {
			case 0:
				// Read the left value.
				left = (int16_t)json_read_int_32(&index, data, len);
				has_left = true;
				break;
			case 1:
				// Read the right value.
				right = (int16_t)json_read_int_32(&index, data, len);
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
 * Processes the reception of a message from a web socket containing a request for the pen position.
 * We expect the following JSON command:
 *     {"getPen": 1} - the actualvalue of "getPen" is ignored, so it's not processed here.
 */
LOCAL void ICACHE_FLASH_ATTR get_pen() {
	servo_position_t pos = get_servo();
	notify_servo_position(pos);
}

/*
 * Processes the reception of a message from a web socket containing a request to move the pen.
 * We expect the following JSON command:
 *     {"movePen": "<up|down>"}
 */
LOCAL void ICACHE_FLASH_ATTR move_pen(Websock *ws, char *data, int len, int index) {
	// Get the new position.
	string_builder *sb = json_read_string(&index, data, len);
	if (sb == NULL) {
		os_printf("Unable to read string value in move_pen command.\n");
		return;
	}
	if (string_builder_strncmp(sb, "up", 3) == 0) {
		servo_up(NULL);
	} else if (string_builder_strncmp(sb, "down", 5) == 0) {
		servo_down(NULL);
	}

	free_string_builder(sb);
}

//---------------------
// Call-back functions.
//---------------------

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
LOCAL void ws_connected(Websock *ws) {
	ws->recvCb=ws_recv;
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

//------------------------------------------------------------------------------
// Helper functions.
//------------------------------------------------------------------------------

/*
 * Creates a return web page with the specified return code and text.
 */
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
 * Stores a 32-bit signed integer into an array as four 8-bit unsigned integers.
 */
LOCAL inline void store_int_32(uint8_t *array, uint8_t index, int32_t value) {
	array[index]     = (value & 0xFF000000) >> 24;
	array[index + 1] = (value & 0x00FF0000) >> 16;
	array[index + 2] = (value & 0x0000FF00) >> 8;
	array[index + 3] = (value & 0x000000FF);
}

//------------------------------------------------------------------------------
// JSON parsing functions.
//------------------------------------------------------------------------------
LOCAL int ICACHE_FLASH_ATTR json_parse_functions(
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
			match_index = json_check_key(index, data, max_index, 4, "args", "locals", "stack", "codes");
			switch (match_index) {
				case 0: {
					// Argument count.
					program->functions[ii].argument_count = json_read_int_32(index, data, max_index);
					have_args = true;
					} break;
				case 1: {
					// Locals count.
					program->functions[ii].local_count = json_read_int_32(index, data, max_index);
					have_locals = true;
					} break;
				case 2: {
					// Maximum stack size.
					program->functions[ii].stack_size = json_read_int_32(index, data, max_index);
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
						program->functions[ii].code[jj] = (uint8_t)json_read_int_32(
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
		*index = json_skip_whitespace(*index, data, max_index);
		if (data[*index] != '}') {
			httpCodeReturn(connData, 400, "Bad parameter",
					"Invalid \"code\" parameter: missing end to function object.");
			return -1;
		}
		(*index)++;

		// Ensure we have a comma at the end of the object, if there are more functions to be read.
		*index = json_skip_whitespace(*index, data, max_index);
		if ((ii < (function_count - 1)) && (data[*index] != ',')) {
			httpCodeReturn(connData, 400, "Bad parameter",
					"Invalid \"code\" parameter: missing end to function object.");
			return -1;
		}
		(*index)++;

		// Skip any trailing whitespace.
		*index = json_skip_whitespace(*index, data, max_index);
	}

	// If we get here, all is good.
	return (int)function_count;
}

LOCAL int ICACHE_FLASH_ATTR json_check_key(int *index, char *data, int max_index, int count, ...) {
	// Ignore any leading whitespace.
	int new_index = *index;
	new_index = json_skip_whitespace(new_index, data, max_index);
	if (new_index == -1) {
		debug_print("Check whitespace 1 failed.\n");
		return -1;
	}

	// Keys must start with a double quotation mark.
	if (data[new_index++] != '"') {
		debug_print("Key must start with double quotes (idx = %d).\n", (new_index - 1));
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
		debug_print("Key \"%s\"does not match any option.\n", data[new_index]);
		return -1;
	}

	// Keys must end with a double quotation mark.
	new_index += str_len;
	if (data[new_index++] != '"') {
		debug_print("Key must end with double quotes.\n");
		return -1;
	}

	// Ignore any trailing whitespace.
	new_index = json_skip_whitespace(new_index, data, max_index);
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
	new_index = json_skip_whitespace(new_index, data, max_index);
	if (new_index == -1) {
		debug_print("Check whitespace 3 failed.\n");
		return -1;
	}

	// Update the index value and return the index of the match.
	*index = new_index;
	return match_index;
}

LOCAL int ICACHE_FLASH_ATTR json_skip_whitespace(int index, char *data, int max_index) {
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

/*
 * Reads a number from the current position in a JSON structure and returns it as a 32-bit signed integer.
 */
LOCAL int32_t ICACHE_FLASH_ATTR json_read_int_32(int *index, char *data, int max_index) {
	// Ignore any leading whitespace.
	*index = json_skip_whitespace(*index, data, max_index);

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
	*index = json_skip_whitespace(*index, data, max_index);

	// Return the decoded number.
	return multiplier * number;
}

/*
 * Reads a string from the current position in a JSON structure and returns it in a string builder.
 * This string builder must be freed by the caller.
 */
LOCAL string_builder * ICACHE_FLASH_ATTR json_read_string(int *index, char *data, int max_index) {
	// Ignore any leading whitespace.
	*index = json_skip_whitespace(*index, data, max_index);

	string_builder *sb = create_string_builder(16);
	if (sb == NULL) {
		os_printf("Unable to create string builder for JSON parsing.\n");
		return NULL;
	}

	// Decode the string - make sure it starts with ".
	if (data[*index] != '"') {
		debug_print("JRS: no starting quote.\n");
		free_string_builder(sb);
		return NULL;
	}
	(*index)++;
	bool in_escape = false;
	uint16_t hex_char = 0;
	int hex_count = -1;
	while (*index < max_index) {
		if (in_escape) {
			switch (data[*index]) {
				case '"':  // Fall-through
				case '\\': // Fall-through
				case '/':  // Fall-through
				case 'b':  // Fall-through
				case 'n':  // Fall-through
				case 'r':  // Fall-through
				case 't':
					append_char_string_builder(sb, data[*index]);
					break;
				case 'u':
					hex_char = 0;
					hex_count = 0;
				default:
					// Invalid escaped character.
					debug_print("JRS: invalid escape character: %c.\n", data[*index]);
					free_string_builder(sb);
					return NULL;
			}
			in_escape = false;
		} else if (hex_count >= 0) {
			if ((data[*index] >= '0') && (data[*index] <= '9')) {
				// This is a hex digit - 0-9.
				hex_char *= 16;
				hex_char += data[*index] - '0';
			} else if ((data[*index] >= 'A') && (data[*index] <= 'F')) {
				// This is a hex digit - A-F.
				hex_char *= 16;
				hex_char += data[*index] - 'A';
			} else if ((data[*index] >= 'a') && (data[*index] <= 'f')) {
				// This is a hex digit - a-f.
				hex_char *= 16;
				hex_char += data[*index] - 'a';
			} else {
				// Not a valid hex digit.
				debug_print("JRS: not a valid hex digit: %c.\n", data[*index]);
				free_string_builder(sb);
				return NULL;
			}
			if (hex_count >= 3) {
				// We're finished with this hex digit.
				hex_count = -1;
				append_char_string_builder(sb, hex_char & 0xFF);
			} else {
				hex_count++;
			}
		} else if (data[*index] == '"') {
			// This is the end of the string. Skip any trailing whitespace.
			*index = json_skip_whitespace(*index, data, max_index);

			// Return the string builder containing the string.
			return sb;
		} else if (data[*index] == '\\') {
			// We are starting an escape sequence.
			in_escape = true;
		} else if (data[*index] >= 0x20) {
			append_char_string_builder(sb, data[*index]);
		} else {
			// This is an invalid character.
			debug_print("JRS: Invalid character: %c.\n", data[*index]);
			free_string_builder(sb);
			return NULL;
		}

		(*index)++;
	}

	// If we get here, we ran out of space before finding the end of the string.
	debug_print("JRS: Ran out of buffer space before reaching string end.\n");
	free_string_builder(sb);
	return NULL;
}
