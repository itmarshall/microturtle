/*
 * files.h: Header file for storage and retrieval of program files.
 *
 * Author: Ian Marshall
 * Date: 17/11/2018
 */

#ifndef __FILES_H
#define __FILES_H

// The number of files that can be stored in the system.
#define FILE_COUNT 10

// The maximum number of characters that may be in a file name.
#define MAX_FILENAME_LEN 32

// The maximum number of sectors that a file may consume.
#define MAX_FILE_SECTORS 3

// The maximum number of bytes that a file may consume.
#define MAX_FILE_SIZE (MAX_FILE_SECTORS * SPI_FLASH_SEC_SIZE)

/*
 * Structure for the handling of files within the micro-turtle.
 */
typedef struct file_t {
	int8_t slot;                     // The slot, or index of the file's storage area.
	bool in_use;                     // Flag indicating if the file exists.
	uint32_t size;                   // # of bytes within the file (if any).
	uint64_t timestamp;              // # of milliseconds since the UNIX epoch.
	char name[MAX_FILENAME_LEN + 1]; // File name, including terminating '\0'.
} file_t;

/*
 * Structure for handling in-progress writes where there is insufficient
 * memory to store the file in a single operation.
 */
typedef struct filesave_t {
	int8_t file_number;              // The file number that is being written to.
	int8_t save_slot;                // The slot, or index of the file's storage area.
	uint32_t file_size;              // # of bytes within the total file.
	uint32_t saved_size;             // # of bytes currently written to the file.
	uint64_t timestamp;              // File timestamp.
	char name[MAX_FILENAME_LEN + 1]; // File name, including terminating '\0'.
	char unaligned_bytes[3];         // Bytes that have not been written as they didn't
	                                 // align to a 4 byte boundary.
} filesave_t;

/*
 * Retrieves the status for all files within the micro-turtle.
 * Returns the number of files stored in the supplied array.
 */
int ICACHE_FLASH_ATTR list_files(file_t *files, uint8_t file_count);

/*
 * Loads the contents of a file from flash memory into the supplied buffer.
 */
bool ICACHE_FLASH_ATTR load_file(uint8_t file_number, char *contents, uint32_t offset, uint32_t max_size);

/*
 * Prepares the storage to hold a new file.
 */
uint8_t ICACHE_FLASH_ATTR prepare_file_save(uint8_t file_number, uint32_t file_size);

/*
 * Stores the file data in flash memory.
 */
bool ICACHE_FLASH_ATTR store_file_data(uint8_t save_slot, uint32_t length, uint32_t offset, char *contents);

/*
 * Completes the saving of a file by updating the directory structure.
 */
bool ICACHE_FLASH_ATTR complete_file_save(
		uint8_t file_number, 
		uint32_t file_size,
		uint32_t timestamp,
		char *file_name,
		uint8_t save_slot);

/*
 * Saves the contents of a file to the flash memory.
 */
bool ICACHE_FLASH_ATTR save_file(uint8_t file_number, file_t file, char *contents);

#endif
