/*
 * vm.c: Virtual Machine implementation for the micro-turtle.
 *
 * Author: Ian Marshall
 * Date: 2/02/2018
 */
#include "esp8266.h"
#include "ets_sys.h"
#include "osapi.h"
#include "mem.h"
#include "cgiwebsocket.h"

#include "udp_debug.h"
#include "string_builder.h"
#include "vm.h"
#include "config.h"
#include "motors.h"

// Helper macro to convert 4 bytes from an array into a 32-bit integer.
#define BYTES_TO_INT32(arr, idx) (((arr)[(idx)]     << 24) + \
                                  ((arr)[(idx) + 1] << 16) + \
								  ((arr)[(idx) + 2] << 8) + \
								  ((arr)[(idx) + 3]))

// Helper macro to choose the maximum of two numbers.
#define MAX(x, y) ((x) > (y)) ? (x) : (y)

// The maximum number of variables in a function or globals.
#define MAX_VAR_COUNT 32

// The maximum number of variables in the operand stack for a function.
#define MAX_STACK_SIZE 32

// The maximum number of functions allowed in the program (including main).
#define MAX_FUNC_COUNT 64

// The maximum number of bytes allowed in a function's code.
#define MAX_FUNC_LEN 2048

// The priority for the task used to execute the next program instruction.
// This is performed in a task to ensure long running programs don't overload the ESP8266.
#define EXEC_INSTR_PRI 1

// The queue length for the task used to execute the next program instruction.
#define EXEC_INSTR_QUEUE_LEN 2

// The number of milliseconds that the microturtle will wait after a motor movement before executing
// the next instruction.
#define MOVE_PAUSE_DURATION 200

// Byte code instruction definitions
#define INSTR_FD         1
#define INSTR_BK         2
#define INSTR_LT         3
#define INSTR_RT         4
#define INSTR_PU         5
#define INSTR_PD         6
#define INSTR_IADD       7
#define INSTR_ISUB       8
#define INSTR_IMUL       9
#define INSTR_IDIV      10
#define INSTR_ICONST_0  11
#define INSTR_ICONST_1  12
#define INSTR_ICONST_45 13
#define INSTR_ICONST_90 14
#define INSTR_ICONST    15
#define INSTR_ILOAD_0   16
#define INSTR_ILOAD_1   17
#define INSTR_ILOAD_2   18
#define INSTR_ILOAD     19
#define INSTR_ISTORE_0  20
#define INSTR_ISTORE_1  21
#define INSTR_ISTORE_2  22
#define INSTR_ISTORE    23
#define INSTR_GLOAD_0   24
#define INSTR_GLOAD_1   25
#define INSTR_GLOAD_2   26
#define INSTR_GLOAD     27
#define INSTR_GSTORE_0  28
#define INSTR_GSTORE_1  29
#define INSTR_GSTORE_2  30
#define INSTR_GSTORE    31
#define INSTR_ILT       32
#define INSTR_ILE       33
#define INSTR_IGT       34
#define INSTR_IGE       35
#define INSTR_IEQ       36
#define INSTR_INE       37
#define INSTR_CALL      38
#define INSTR_RET       39
#define INSTR_STOP      40
#define INSTR_BR        41
#define INSTR_BRT       42
#define INSTR_BRF       43
#define INSTR_FDRAW     44
#define INSTR_BKRAW     45
#define INSTR_LTRAW     46
#define INSTR_RTRAW     47

// The lengths of each instruction in bytes, including the instruction itself.
const uint8_t INSTR_LEN[] = {
	0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 5,
	1, 1, 1, 5, 1, 1, 1, 5, 1, 1, 1, 5, 1, 1, 1, 5,
	1, 1, 1, 1, 1, 1, 5, 1, 1, 5, 5, 5};

/*
 * The type for the program counter.
 */
typedef struct pc_t {
	uint32_t func;  // Function ID.
	uint32_t idx;   // Index in function's code.
} pc_t;

/*
 * The stack frame type for holding stack information.
 */
typedef struct stack_frame_t {
	pc_t pc;                    // The program counter for this frame's execution.
	uint32_t local_count;       // The number of local variables in this frame.
	int32_t *locals;            // The array of local variables.
	uint32_t stack_size;        // The number of entries currently in this frame's operand stack.
	uint32_t max_stack_size;    // The maximum number of stack entries for this frame.
	int32_t *stack;             // The operand stack for this frame.
	struct stack_frame_t *prev; // Pointer to the previous frame (if any).
	struct stack_frame_t *next; // Pointer to the next frame (if any).
} stack_frame_t;

/*
 * Type for storing the global variables.
 */
typedef struct global_t {
	uint32_t global_count; // The number of global variables.
	int32_t *values;       // The global variable values.
} global_t;

// Forward definitions.
LOCAL void ICACHE_FLASH_ATTR free_program();
LOCAL stack_frame_t * ICACHE_FLASH_ATTR create_stack_frame(function_t *function);
LOCAL void ICACHE_FLASH_ATTR execute_instruction();
void ICACHE_FLASH_ATTR program_error(char *message);
void ICACHE_FLASH_ATTR end_move_pause();
LOCAL inline bool stack_push(int32_t val);
LOCAL inline int32_t stack_pop();

// The status of the program execution.
LOCAL prog_status_t program_status = IDLE;

// The program being executed.
LOCAL program_t *program;

// The head of the stack.
LOCAL stack_frame_t *stack = NULL;

// The stack pointer, pointing to the current stack frame.
LOCAL stack_frame_t *sp = NULL;

// The global variable storage for this program.
LOCAL global_t globals;

// The queue used for posting events to the instruction execution queue.
LOCAL os_event_t vm_exec_queue[EXEC_INSTR_QUEUE_LEN];

// Timer for post movement pauses.
LOCAL os_timer_t move_pause_timer;

/*
 * Runs a program on the micro-turtle in the background. The supplied program information is used
 * directoy, so the memory cannot be modified. The memory will automatically be freed when the
 * program's execution has halted.
 */
bool ICACHE_FLASH_ATTR run_program(program_t *prog) {
	// Stop any currently running program.
	if (program_status == RUNNING) {
		stop_program();
	}
	program_status = IDLE;

	// Check the program's validity - basic checks only.
	if (prog == NULL) {
		os_printf("NULL program received.\n");
		return false;
	}
	if (prog->global_count > MAX_VAR_COUNT) {
		os_printf("Too many global variables - %d.\n", prog->global_count);
		return false;
	}
	if (prog->function_count > MAX_FUNC_COUNT) {
		os_printf("Too many functions - %d.\n", prog->function_count);
		return false;
	}
	if (prog->function_count == 0) {
		os_printf("No functions defined.\n");
		return false;
	}
	for (uint32_t ii = 0; ii < prog->function_count; ii++) {
		if (prog->functions[ii].argument_count > MAX_VAR_COUNT) {
			os_printf("Too many arguments for function %d - %d.\n",
					ii, prog->functions[ii].argument_count);
			return false;
		}
		if (prog->functions[ii].local_count > MAX_VAR_COUNT) {
			os_printf("Too many local variables for function %d - %d.\n",
					ii, prog->functions[ii].local_count);
			return false;
		}
		if (prog->functions[ii].stack_size > MAX_STACK_SIZE) {
			os_printf("Stack size too large for function %d - %d.\n",
					ii, prog->functions[ii].stack_size);
			return false;
		}
		if (prog->functions[ii].length > MAX_FUNC_LEN) {
			os_printf("Function %d is too long - %d bytes.\n",
					ii, prog->functions[ii].length);
			return false;
		}
		if (prog->functions[ii].length == 0) {
			os_printf("Function %d has no contents.\n", ii);
			return false;
		}
	}

	// Delete any old program information.
	free_program();

	// Copy the program pointer locally.
	program = prog;

	// Initialise the stack.
	stack = create_stack_frame(&program->functions[0]);
	if (stack == NULL) {
		free_program();
		return false;
	}
	sp = stack;

	// Initialise the globals.
	globals.global_count = prog->global_count;
	if (globals.global_count > 0) {
		globals.values = (int32_t *)os_malloc(globals.global_count * sizeof(int32_t));
		if (globals.values == NULL) {
			os_printf("Unable to allocate global memory.\n");
			free_program();
			return false;
		}
	} else {
		globals.values = NULL;
	}

	// Start the program by executing the first instuction.
	program_status = RUNNING;
	execute_instruction();
}

/*
 * Deallocates the storage for the program and all its' functions, stacks and global variables.
 */
LOCAL void ICACHE_FLASH_ATTR free_program() {
	if (program != NULL) {
		// Free the functions within the program.
		if (program->functions != NULL) {
			for (uint32_t ii = 0; ii < program->function_count; ii++) {
				if (program->functions[ii].code != NULL) {
					os_free(program->functions[ii].code);
				}
			}
			os_free(program->functions);
		}

		// Free the program itself.
		os_free(program);
		program = NULL;
	}

	// Free the stack memory.
	while (stack != NULL) {
		stack_frame_t *s = stack;
		stack = s->next;
		os_free(s->locals);
		os_free(s->stack);
		os_free(s);
		stack = NULL;
	}
	sp = NULL;

	// Free the global memory.
	if (globals.values != NULL) {
		os_free(globals.values);
	}
	globals.global_count = 0;
	globals.values = NULL;
}

/*
 * Stops the execution of a program and frees the space for it.
 */
void ICACHE_FLASH_ATTR stop_program() {
	// Signal the program to stop running.
	os_printf("Stopping program.\n");
	program_status = IDLE;

	// Stop the motors.
	drive_motors(0, 0, 1, NULL);

	// Call to execute the next instruction, which will safely free the memory.
	execute_instruction();

	// Send a message to the web socket.
	char str[] = "{\"program\": {\"status\": \"stopped\"}}";
	cgiWebsockBroadcast("/ws.cgi", str, os_strlen(str), WEBSOCK_FLAG_NONE);
}

/*
 * Schedules the execution of the next program instruction. This is performed as a task invocation 
 * to ensure that long running programs (especially those that do not use the motors) do not cause 
 * a watchdog timeout in the ESP8266.
 */
LOCAL void ICACHE_FLASH_ATTR execute_instruction() {
	// Post the request to the execution task queue to execute the next instruction.
	system_os_post(EXEC_INSTR_PRI, 0, 0);
}

/*
 * Task that executes the next instruction pointed to by the program counter.
 */
LOCAL void ICACHE_FLASH_ATTR vm_execute_task(os_event_t *event) {
	// Ensure we're still running the program.
	if ((program_status != RUNNING) || (program == NULL)) {
		if (program != NULL) {
			free_program();
		}
		program_status = IDLE;
		os_printf("Not executing instruction as program status is not running.\n");
		return;
	}

	// Check we have enough space for this instruction.
	if ((sp->pc.idx + INSTR_LEN[sp->pc.func]) > program->functions[sp->pc.func].length) {
		os_printf("End of function reached without RET/STOP instruction.");
		stop_program();
	}

	// Get the code at the current program counter.
	uint8_t *code = &program->functions[sp->pc.func].code[sp->pc.idx];
	debug_print("Executing instruction at function %d, index %d: %d.\n",
			sp->pc.func, sp->pc.idx, code[0]);

	// Send a message to the web socket.
	string_builder *sb = create_string_builder(128);
	if (sb == NULL) {
		os_printf("Unable to create string builder for web socket reply.");
	} else {
		append_string_builder(sb, "{\"program\": {\"status\": \"running\", \"function\": ");
		append_int32_string_builder(sb, sp->pc.func);
		append_string_builder(sb, ", \"index\": ");
		append_int32_string_builder(sb, sp->pc.idx);
		append_string_builder(sb, "}}");
		cgiWebsockBroadcast("/ws.cgi", sb->buf, sb->len, WEBSOCK_FLAG_NONE);
		free_string_builder(sb);
	}

	// Define variables for use within the below switch block.
	int32_t operand1;
	int32_t operand2;
	uint32_t left_scale;
	uint32_t right_scale;
	uint32_t addr;
	stack_frame_t *sf;
	int32_t id;
	int32_t ii;

	// Execute the code instruction.
	bool auto_update_pc = true;
	bool defer_next_instr = false;
	switch (code[0]) {
		case INSTR_FD:
			// Move forward by the number of mm at the end of the stack.
			operand1 = stack_pop();
			get_straight_steps(&left_scale, &right_scale);
			operand2 = operand1 * right_scale / 100; 
			operand1 = operand1 * left_scale  / 100;
			os_printf("Moving forward by %d, %d steps.\n", operand1, operand2);
			drive_motors(operand1, operand2, MAX(operand1, operand2), end_move_pause);
			defer_next_instr = true;
			break;
		case INSTR_BK:
			// Move backwards by the number of mm at the end of the stack.
			operand1 = stack_pop();
			get_straight_steps(&left_scale, &right_scale);
			operand2 = operand1 * right_scale / 100; 
			operand1 = operand1 * left_scale  / 100;
			os_printf("Moving backward by %d, %d steps.\n", operand1, operand2);
			drive_motors(-1 * operand1, -1 * operand2, MAX(operand1, operand2), end_move_pause);
			defer_next_instr = true;
			break;
		case INSTR_LT:
			// Turn left by the number of degrees at the end of the stack.
			operand1 = stack_pop();
			get_turn_steps(&left_scale, &right_scale);
			operand2 = operand1 * right_scale / 180; 
			operand1 = operand1 * left_scale  / 180;
			os_printf("Turning left by %d, %d steps.\n", operand1, operand2);
			drive_motors(-1 * operand1, operand2, MAX(operand1, operand2), end_move_pause);
			defer_next_instr = true;
			break;
		case INSTR_RT:
			// Turn right by the number of degrees at the end of the stack.
			operand1 = stack_pop();
			get_turn_steps(&left_scale, &right_scale);
			operand2 = operand1 * right_scale / 180; 
			operand1 = operand1 * left_scale  / 180;
			os_printf("Turning right by %d, %d steps.\n", operand1, operand2);
			drive_motors(operand1, -1 * operand2, MAX(operand1, operand2), end_move_pause);
			defer_next_instr = true;
			break;
		case INSTR_FDRAW:
			// Move forward by the number of steps at the end of the stack, right then left.
			operand2 = stack_pop();
			operand1 = stack_pop();
			os_printf("Moving forward by %d, %d steps.\n", operand1, operand2);
			drive_motors(operand1, operand2, MAX(operand1, operand2), end_move_pause);
			defer_next_instr = true;
			break;
		case INSTR_BKRAW:
			// Move backwards by the number of steps at the end of the stack, right then left.
			operand2 = stack_pop();
			operand1 = stack_pop();
			os_printf("Moving backward by %d, %d steps.\n", operand1, operand2);
			drive_motors(-1 * operand1, -1 * operand2, MAX(operand1, operand2), end_move_pause);
			defer_next_instr = true;
			break;
		case INSTR_LTRAW:
			// Turn left by the number of steps at the end of the stack, right then left.
			operand2 = stack_pop();
			operand1 = stack_pop();
			os_printf("Turning left by %d, %d steps.\n", operand1, operand2);
			drive_motors(-1 * operand1, operand2, MAX(operand1, operand2), end_move_pause);
			defer_next_instr = true;
			break;
		case INSTR_RTRAW:
			// Turn right by the number of steps at the end of the stack, right then left.
			operand2 = stack_pop();
			operand1 = stack_pop();
			os_printf("Turning right by %d, %d steps.\n", operand1, operand2);
			drive_motors(operand1, -1 * operand2, MAX(operand1, operand2), end_move_pause);
			defer_next_instr = true;
			break;
		case INSTR_PU:
			// Raise the pen, and wait for it to finish moving.
			servo_up();
			defer_next_instr = true;
			end_move_pause();
			break;
		case INSTR_PD:
			// Lower the pen, and wait for it to finish moving.
			servo_down();
			defer_next_instr = true;
			end_move_pause();
			break;
		case INSTR_IADD:
			// Add the topmost two values on the stack and add it to the stack.
			operand2 = stack_pop();
			operand1 = stack_pop();
			stack_push(operand1 + operand2);
			break;
		case INSTR_ISUB:
			// Subtract the topmost two values on the stack from each other and add it to the stack.
			operand2 = stack_pop();
			operand1 = stack_pop();
			stack_push(operand1 - operand2);
			break;
		case INSTR_IMUL:
			// Multiply the topmost two values on the stack and add it to the stack.
			operand2 = stack_pop();
			operand1 = stack_pop();
			stack_push(operand1 * operand2);
			break;
		case INSTR_IDIV:
			// Divide the topmost two values on the stack from each other and add it to the stack.
			operand2 = stack_pop();
			operand1 = stack_pop();
			stack_push(operand1 / operand2);
			break;
		case INSTR_ICONST_0:
			// Store the constant zero on the stack.
			stack_push(0);
			break;
		case INSTR_ICONST_1:
			// Store the constant one on the stack.
			stack_push(1);
			break;
		case INSTR_ICONST_45:
			// Store the constant 45 on the stack.
			stack_push(45);
			break;
		case INSTR_ICONST_90:
			// Store the constant 90 on the stack.
			stack_push(90);
			break;
		case INSTR_ICONST:
			// Store the constant from the byte code on the stack.
			stack_push(BYTES_TO_INT32(code, 1));
			break;
		case INSTR_ILOAD_0:
			// Load the value from the first variable on to the stack.
			if (sp->locals != NULL) {
				stack_push(sp->locals[0]);
			} else {
				program_error("Invalid local variable - 0");
			}
			break;
		case INSTR_ILOAD_1:
			// Load the value from the second variable on to the stack.
			if ((sp->locals != NULL) && (sp->local_count >= 1)) {
				stack_push(sp->locals[1]);
			} else {
				program_error("Invalid local variable - 1");
			}
			break;
		case INSTR_ILOAD_2:
			// Load the value from the third variable on to the stack.
			if ((sp->locals != NULL) && (sp->local_count >= 2)) {
				stack_push(sp->locals[2]);
			} else {
				program_error("Invalid local variable - 2");
			}
			break;
		case INSTR_ILOAD:
			// Load the value from the variable in the byte code on to the stack.
			operand1 = BYTES_TO_INT32(code, 1);
			if ((sp->locals != NULL) && (sp->local_count >= operand1)) {
				stack_push(sp->locals[operand1]);
			} else {
				program_error("Invalid local variable");
			}
			break;
		case INSTR_ISTORE_0:
			// Store the value from the stack in the first variable.
			if (sp->locals != NULL) {
				sp->locals[0] = stack_pop();
			} else {
				program_error("Invalid local variable - 0");
			}
			break;
		case INSTR_ISTORE_1:
			// Store the value from the stack in the second variable.
			if ((sp->locals != NULL) && (sp->local_count >= 1)) {
				sp->locals[1] = stack_pop();
			} else {
				program_error("Invalid local variable - 1");
			}
			break;
		case INSTR_ISTORE_2:
			// Store the value from the stack in the third variable.
			if ((sp->locals != NULL) && (sp->local_count >= 2)) {
				sp->locals[2] = stack_pop();
			} else {
				program_error("Invalid local variable - 2");
			}
			break;
		case INSTR_ISTORE:
			// Store the value from the stack in the variable in the byte code.
			operand1 = BYTES_TO_INT32(code, 1);
			if ((sp->locals != NULL) && (sp->local_count >= operand1)) {
				sp->locals[operand1] = stack_pop();
			} else {
				program_error("Invalid local variable");
			}
			break;
		case INSTR_GLOAD_0:
			// Load the value from the first global variable on to the stack.
			if (globals.values != NULL) {
				stack_push(globals.values[0]);
			} else {
				program_error("Invalid global variable - 0");
			}
			break;
		case INSTR_GLOAD_1:
			// Load the value from the second global variable on to the stack.
			if ((globals.values != NULL) && (globals.global_count >= 1)) {
				stack_push(globals.values[1]);
			} else {
				program_error("Invalid global variable - 1");
			}
			break;
		case INSTR_GLOAD_2:
			// Load the value from the third global variable on to the stack.
			if ((globals.values != NULL) && (globals.global_count >= 2)) {
				stack_push(globals.values[2]);
			} else {
				program_error("Invalid global variable - 2");
			}
			break;
		case INSTR_GLOAD:
			// Load the value from the global variable in the byte code on to the stack.
			operand1 = BYTES_TO_INT32(code, 1);
			if ((globals.values != NULL) && (globals.global_count >= operand1)) {
				stack_push(globals.values[operand1]);
			} else {
				program_error("Invalid global variable");
			}
			break;
		case INSTR_GSTORE_0:
			// Store the value from the stack in the first global variable.
			if (globals.values != NULL) {
				globals.values[0] = stack_pop();
			} else {
				program_error("Invalid global variable - 0");
			}
			break;
		case INSTR_GSTORE_1:
			// Store the value from the stack in the second global variable.
			if ((globals.values != NULL) && (globals.global_count >= 1)) {
				globals.values[1] = stack_pop();
			} else {
				program_error("Invalid global variable - 1");
			}
			break;
		case INSTR_GSTORE_2:
			// Store the value from the stack in the third global variable.
			if ((globals.values != NULL) && (globals.global_count >= 2)) {
				globals.values[2] = stack_pop();
			} else {
				program_error("Invalid global variable - 2");
			}
			break;
		case INSTR_GSTORE:
			// Store the value from the stack in the global variable in the byte code.
			operand1 = BYTES_TO_INT32(code, 1);
			if ((globals.values != NULL) && (globals.global_count >= operand1)) {
				globals.values[operand1] = stack_pop();
			} else {
				program_error("Invalid global variable");
			}
			break;
		case INSTR_ILT:
			// Performs a < comparison on the topmost two values from the stack.
			operand2 = stack_pop();
			operand1 = stack_pop();
			stack_push((operand1 < operand2) ? 1 : 0);
			break;
		case INSTR_ILE:
			// Performs a <= comparison on the topmost two values from the stack.
			operand2 = stack_pop();
			operand1 = stack_pop();
			stack_push((operand1 <= operand2) ? 1 : 0);
			break;
		case INSTR_IGT:
			// Performs a > comparison on the topmost two values from the stack.
			operand2 = stack_pop();
			operand1 = stack_pop();
			stack_push((operand1 > operand2) ? 1 : 0);
			break;
		case INSTR_IGE:
			// Performs a >= comparison on the topmost two values from the stack.
			operand2 = stack_pop();
			operand1 = stack_pop();
			stack_push((operand1 >= operand2) ? 1 : 0);
			break;
		case INSTR_IEQ:
			// Performs an equality comparison on the topmost two values from the stack.
			operand2 = stack_pop();
			operand1 = stack_pop();
			stack_push((operand1 == operand2) ? 1 : 0);
			break;
		case INSTR_INE:
			// Performs an inequality comparison on the topmost two values from the stack.
			operand2 = stack_pop();
			operand1 = stack_pop();
			stack_push((operand1 != operand2) ? 1 : 0);
			break;
		case INSTR_CALL:
			// Calls a function. First, check the function ID is valid.
			id = BYTES_TO_INT32(code, 1);
			if ((id <= 0) || (id >= program->function_count)) {
				program_error("Invalid function ID for CALL instruction.\n");
				return;
			}
			os_printf("Calling to function %d with %d arguments and %d stack.\n",
					id, program->functions[id].argument_count, program->functions[id].stack_size);

			// Update this stack frame's program counter to the instruction to be called upon 
			// returning from this call.
			sp->pc.idx += INSTR_LEN[code[0]];

			// Create a new stack frame.
			sf = create_stack_frame(&program->functions[id]);
			if (sf == NULL) {
				// Could not create the stack frame.
				program_error("Unable to create stack frame for CALL instruction.");
				return;
			}

			// Copy any parameters to the new stack frame's local variables.
			for (ii = program->functions[id].argument_count - 1; ii >= 0; ii--) {
				sf->locals[ii] = stack_pop();
			}

			// Add the stack frame to the end of the stack and point to it.
			sf->prev = sp;
			sp->next = sf;
			sp = sf;
			auto_update_pc = false;
			break;
		case INSTR_RET:
			// Ends the execution of a function.
			// Move to the previous stack frame.
			sf = sp;
			if (sp->prev == NULL) {
				program_error("Attempt to RETurn from <main> function.");
				return;
			}
			sp = sp->prev;
			sp->next = NULL;

			// Deallocate the memory for the old stack frame.
			if (sf->locals != NULL) {
				os_free(sf->locals);
			}
			if (sf->stack != NULL) {
				os_free(sf->stack);
			}
			os_free(sf);
			auto_update_pc = false;
			break;
		case INSTR_STOP:
			// Stops the execution of this program.
			stop_program();
			auto_update_pc = false;
			defer_next_instr = true;
			break;
		case INSTR_BR:
			// Performs an unconditional branch.
			addr = BYTES_TO_INT32(code, 1);
			if (addr > program->functions[sp->pc.func].length) {
				program_error("Cannot branch beyond function boundary.");
				return;
			}
			sp->pc.idx = addr;
			auto_update_pc = false;
			break;
		case INSTR_BRT:
			// Performs a conditional branch, if the stack value is true.
			if (stack_pop() != 0) {
				addr = BYTES_TO_INT32(code, 1);
				if (addr > program->functions[sp->pc.func].length) {
					program_error("Cannot branch beyond function boundary.");
					return;
				}
				sp->pc.idx = addr;
				auto_update_pc = false;
			}
			break;
		case INSTR_BRF:
			// Performs a conditional branch, if the stack value is false.
			if (stack_pop() == 0) {
				addr = BYTES_TO_INT32(code, 1);
				if (addr > program->functions[sp->pc.func].length) {
					program_error("Cannot branch beyond function boundary.");
					return;
				}
				sp->pc.idx = addr;
				auto_update_pc = false;
			}
			break;
		default:
			// Unknown instruction.
			program_error("Unknown instruction in program.");
			return;
	}

	if (auto_update_pc) {
		// Update the program counter.
		sp->pc.idx += INSTR_LEN[code[0]];
	}
	if (defer_next_instr == false) {
		// Run the next instruction.
		execute_instruction();
	}
}

/*
 * Handles an error in the program. This will stop the program's execution, and free its' memory.
 */
void ICACHE_FLASH_ATTR program_error(char *message) {
	os_printf(message);
	program_status = ERROR;
	free_program();
}

/*
 * Waits for a period of time before invoking the next program instruction. This is designed to 
 * ensure that one movement command doesn't impact the next movement.
 */
void ICACHE_FLASH_ATTR end_move_pause() {
	// Trigger the post movement pause timer.
    os_timer_arm(&move_pause_timer, MOVE_PAUSE_DURATION, false);
}

/*
 * Invokes the next instruction after a movement pause.
 */
void ICACHE_FLASH_ATTR move_pause_timer_cb(void *arg) {
	execute_instruction();
}

/*
 * Adds a value to the end of the stack.
 * Returns true if the value could be stored on the stack.
 */
LOCAL inline bool ICACHE_FLASH_ATTR stack_push(int32_t val) {
	// Ensure we have a stack.
	if (sp == NULL) {
		os_printf("ERROR: No current stack to push to.\n");
		return false;
	}

	// Ensure we're not going to exceed the stack's size.
	if (sp->stack_size >= sp->max_stack_size) {
		os_printf("ERROR: Stack overflow (%d of %d).\n", sp->stack_size, sp->max_stack_size);
		return false;
	}

	// Store the value at the end of the stack.
	sp->stack[sp->stack_size++] = val;
}

/*
 * Removes and retrieves the newest value from the end of the stack.
 * If there are no values on the stack, a zero is returned instead.
 */
LOCAL inline int32_t ICACHE_FLASH_ATTR stack_pop() {
	// Ensure we have a stack.
	if (sp == NULL) {
		os_printf("ERROR: No current stack to pull from.\n");
		return 0;
	}

	// Ensure there is something on the stack to be popped.
	if (sp->stack_size == 0) {
		os_printf("ERROR: Stack underflow.\n");
		return 0;
	}

	// Retrieve the item from the stack.
	return sp->stack[--sp->stack_size];
}

/*
 * Creates a stack frame ready to hold information for a given function.
 * The created stack frame is isolated, as both the prev and next pointers are set to NULL.
 * Callers to this function should set these as appropriate.
 */
LOCAL stack_frame_t * ICACHE_FLASH_ATTR create_stack_frame(function_t *function) {
	// Allocate the memory for the stack frame.
	os_printf("Allocating stack frame with %d arguments, %d locals, %d stack.\n", 
			function->argument_count, function->local_count, function->stack_size);
	stack_frame_t *sf = (stack_frame_t *)os_malloc(sizeof(stack_frame_t));
	if (sf == NULL) {
		os_printf("Unable to allocate stack frame.\n");
		return NULL;
	}

	// Set the non-array values.
	sf->pc.func = function->id;
	sf->pc.idx = 0;
	sf->local_count = function->argument_count + function->local_count;
	sf->max_stack_size = function->stack_size;
	sf->stack_size = 0;
	sf->prev = NULL;
	sf->next = NULL;

	// Allocate memory for the locals, if any.
	if (sf->local_count > 0) {
		sf->locals = os_malloc(sf->local_count * sizeof(int32_t));
		if (sf->locals == NULL) {
			os_printf("Unable to allocate stack frame locals.\n");
			os_free(sf);
			return NULL;
		}
	} else {
		sf->locals = NULL;
	}

	// Allocate memory for the stack, if any.
	if (sf->max_stack_size > 0) {
		sf->stack = os_malloc(sf->max_stack_size * sizeof(int32_t));
		if (sf->stack == NULL) {
			os_printf("Unable to allocate stack frame stack.\n");
			os_free(sf->locals);
			os_free(sf);
			return NULL;
		}
	} else {
		sf->stack = NULL;
	}

	return sf;
}

/*
 * Initialise the Virtual Machine at system start-up.
 */
void ICACHE_FLASH_ATTR init_vm() {
	// Initialise the program status.
	program_status = IDLE;

	// Initialise the program to be empty.
	program = NULL;

	// Initialise the stack to be empty.
	stack = NULL;
	sp = NULL;

	// Initialise the globals to be empty.
	globals.global_count = 0;
	globals.values = NULL;

	// Set up the task for executing the next program instruction.
	system_os_task(vm_execute_task, EXEC_INSTR_PRI, vm_exec_queue, EXEC_INSTR_QUEUE_LEN);

	// Set up the timer for post-movement delays.
    os_timer_disarm(&move_pause_timer);
    os_timer_setfn(&move_pause_timer, (os_timer_func_t *)move_pause_timer_cb, (void *)0);
}
