/*
 * vm.h: Header file for the Virtual Machine implementation for the micro-turtle.
 *
 * Author: Ian Marshall
 * Date: 2/02/2018
 */
#ifndef _VM_H
#define _VM_H

#include "esp8266.h"
#include "ets_sys.h"

#include "motors.h"

/*
 * Type for defining a function.
 */
typedef struct function_t {
	uint32_t id;             // The ID of this function.
	uint32_t argument_count; // Number of arguments
	uint32_t local_count;    // Number of local variables
	uint32_t stack_size;     // Required stack size for operands
	uint32_t length;         // Length of code, in bytes
	uint8_t *code;           // Function byte-code.
} function_t;

/*
 * Type for defining a program.
 */
typedef struct program_t {
	uint32_t global_count;
	uint32_t function_count;
	function_t *functions;
} program_t;

/*
 * Type for the program's execution status.
 */
typedef enum prog_status_t {
	IDLE,
	RUNNING,
	ERROR
} prog_status_t;

/*
 * Runs a program on the micro-turtle in the background. The supplied program information can be
 * freed after this invocation, as a local copy is made prior to running the program.
 */
bool run_program(program_t *prog);

/*
 * Stops the execution of a program and frees the space for it.
 */
void stop_program();

/*
 * Initialise the Virtual Machine at system start-up.
 */
void init_vm();

#endif
