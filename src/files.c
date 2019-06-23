/*
 * config.c: Storage and retrieval of configuration parameters.
 *
 * Author: Ian Marshall
 * Date: 22/03/2018
 */
#include "esp8266.h"
#include "ets_sys.h"
#include "osapi.h"
#include "spi_flash.h"
#include "mem.h"

#include "udp_debug.h"

#include "files.h"

// The flash sector used for storing and retrieving the file directory.
#define DIRECTORY_SECTOR 0x110

// The flash sector used for storing and retrieving the file contents.
#define FILE_BASE_SECTOR 0x120

// "Magic" number used to identify a directory block as having been written by the program.
static uint32_t const DIRECTORY_MAGIC_VALUE = 0x7546696C; // 'uFil'

/*
 * Structure for the physical storage of directory in the flash. This includes a "magic" value that is also stored in 
 * the flash to test if the directory is stored, or if the flash is simply uninitialised, or random.
 */
typedef struct directory_storage_t {
	uint32_t magic;
	file_t directory[FILE_COUNT + 1];
} directory_storage_t;

/*
 * Retrieves the status for all files within the micro-turtle.
 * Returns the number of files stored in the supplied array.
 */
int ICACHE_FLASH_ATTR list_files(file_t *files, uint8_t file_count) {
	directory_storage_t storage;
	bool res = system_param_load(DIRECTORY_SECTOR, 0, &storage, sizeof(directory_storage_t));
	if ((!res) || (storage.magic != DIRECTORY_MAGIC_VALUE)) {
		// We don't have a configuration saved in the flash that we can read, create a default directory.
		if (!res) {
			os_printf("Unable to load configuration from flash memory.\n");
		} else {
			os_printf("Flash memory does not hold a configuration.\n");
		}
		storage.magic = DIRECTORY_MAGIC_VALUE;
		for (uint8_t ii = 0; ii < FILE_COUNT + 1; ii++) {
			storage.directory[ii].slot = ii;
			storage.directory[ii].in_use = false;
			storage.directory[ii].size = 0;
			storage.directory[ii].timestamp = 0;
			for (uint8_t jj = 0; jj <= MAX_FILENAME_LEN; jj++) {
				storage.directory[ii].name[jj] = '\0';
			}
		}

		// Store the directory to the flash memory.
		res = system_param_save_with_protect(DIRECTORY_SECTOR, &storage, sizeof(directory_storage_t));
		if (!res) {
			os_printf("Unable to save bare directory to flash memory.\n");
			return 0;
		}
	} 

	// Copy the retrieved or initialised data to the supplied files array.
	uint8_t count = (file_count > (FILE_COUNT + 1)) ? FILE_COUNT + 1 : file_count;
	os_memcpy(files, storage.directory, count * sizeof(file_t));
	return count;
}

/*
 * Loads the contents of a file from flash memory into the supplied buffer.
 */
bool ICACHE_FLASH_ATTR load_file(uint8_t file_number, char *contents, uint32_t offset, uint32_t max_size) {
	// Verify the parameters.
	if (file_number >= FILE_COUNT) {
		debug_print("Bad file number received: %d.\n", file_number);
		return false;
	}
	if (contents == (char *)NULL) {
		debug_print("NULL file contents supplied.\n");
		return false;
	}
	if ((offset % 4) != 0) {
		debug_print("Offset must be on a 4-byte boundary: %d.\n", offset);
		return false;
	}
	if (max_size > (MAX_FILE_SECTORS * SPI_FLASH_SEC_SIZE)) {
		debug_print("File size is too big: %d.\n", max_size);
		return false;
	}
	if ((max_size % 4) != 0) {
		debug_print("Max_size must be on a 4-byte boundary: %d.\n", max_size);
		return false;
	}

	// Get the file information.
	file_t directory[FILE_COUNT + 1];
	if (list_files(directory, FILE_COUNT + 1) != (FILE_COUNT + 1)) {
		debug_print("Unable to load directory to load file %d.\n", file_number);
		return false;
	}

	// Ensure we have a file to return.
	if (!directory[file_number].in_use) {
		debug_print("Request to read from file %d, which is not in use.\n", file_number);
		return false;
	}

	// Read the file's contents.
	uint16_t start_sector = FILE_BASE_SECTOR + (directory[file_number].slot * MAX_FILE_SECTORS);
	uint32_t read_size = directory[file_number].size - offset;
	read_size += ((read_size % 4) == 0) ? 0 : (4 - (read_size % 4));
	if (read_size > max_size) {
		read_size = max_size;
	}
	//debug_print("Reading from flash address %x to %x of length %d.\n",
	//		(start_sector * SPI_FLASH_SEC_SIZE) + offset,
	//		(uint32_t *)contents,
	//		read_size);
	SpiFlashOpResult res = spi_flash_read(
			(start_sector * SPI_FLASH_SEC_SIZE) + offset,
			(uint32_t *)contents,
			read_size);
	if (res != SPI_FLASH_RESULT_OK) {
		debug_print("Unable to read flash sector %d for file %d, size %d, offset %d: %d.\n",
				start_sector, file_number, read_size, offset, res);
		return false;
	}

	// Read completed successfully.
	return true;
}

/*
 * Prepares the storage to hold a new file.
 */
uint8_t ICACHE_FLASH_ATTR prepare_file_save(uint8_t file_number, uint32_t file_size) {
	// Verify the parameters.
	if (file_number >= FILE_COUNT) {
		debug_print("Bad file number received: %d.\n", file_number);
		return 255;
	}
	if (file_size > (MAX_FILE_SECTORS * SPI_FLASH_SEC_SIZE)) {
		debug_print("File size is too big: %d.\n", file_size);
		return 255;
	}

	// Get the file information.
	file_t directory[FILE_COUNT + 1];
	if (list_files(directory, FILE_COUNT + 1) != (FILE_COUNT + 1)) {
		debug_print("Unable to load directory to save file %d.\n", file_number);
		return 255;
	}
	
	// Prepare the flash memory for the writing.
	uint8_t save_slot;
	if (!directory[file_number].in_use) {
		// The file isn't currently in use, so save to that location.
		save_slot = directory[file_number].slot;
	} else {
		// The file is in use, save to an empty slot.
		save_slot = directory[FILE_COUNT].slot;
	}

	// Erase the flash memory, ready for writing.
	SpiFlashOpResult res;
	uint16_t start_sector = FILE_BASE_SECTOR + (save_slot * MAX_FILE_SECTORS);
	uint8_t sector_count = file_size / SPI_FLASH_SEC_SIZE;
	if ((sector_count * SPI_FLASH_SEC_SIZE) < file_size) {
		sector_count++;
	}
	for (int ii = 0; ii < sector_count; ii++) {
		res = spi_flash_erase_sector(start_sector + ii);
		if (res != SPI_FLASH_RESULT_OK) {
			debug_print("Unable to erase flash sector %d for file %d: %d.\n", start_sector + ii, file_number, res);
			return 255;
		}
	}

	// Return the save slot for future writing.
	return save_slot;
}

/*
 * Stores the file data in flash memory.
 */
bool ICACHE_FLASH_ATTR store_file_data(uint8_t save_slot, uint32_t length, uint32_t offset, char *contents) {
	// Verify the parameters.
	if (save_slot > FILE_COUNT) {
		debug_print("Bad save slot received: %d.\n", save_slot);
		return false;
	}
	if (contents == (char *)NULL) {
		debug_print("NULL file contents supplied.\n");
		return false;
	}

	// Write the file's contents.
	SpiFlashOpResult res;
	uint16_t start_sector = FILE_BASE_SECTOR + (save_slot * MAX_FILE_SECTORS);
	res = spi_flash_write((start_sector * SPI_FLASH_SEC_SIZE) + offset, (uint32_t *)contents, length);
	if (res != SPI_FLASH_RESULT_OK) {
		debug_print("Unable to write flash contents to addres %ld for save slot %d: %d.\n",
				(start_sector * SPI_FLASH_SEC_SIZE) + offset, save_slot, res);
		return false;
	}

	// Write complete.
	return true;
}

/*
 * Completes the saving of a file by updating the directory structure.
 */
bool ICACHE_FLASH_ATTR complete_file_save(
		uint8_t file_number, 
		uint32_t file_size,
		uint32_t timestamp,
		char *file_name,
		uint8_t save_slot) {
	// Get the file information.
	file_t directory[FILE_COUNT + 1];
	if (list_files(directory, FILE_COUNT + 1) != (FILE_COUNT + 1)) {
		debug_print("Unable to load directory to save file %d.\n", file_number);
		return false;
	}

	// Update the directory.
	if (save_slot != directory[file_number].slot) {
		// The slot that was used by this file is now the free slot.
		directory[FILE_COUNT].slot = directory[file_number].slot;
	}
	directory[file_number].slot = save_slot;
	directory[file_number].in_use = true;
	directory[file_number].size = file_size;
	directory[file_number].timestamp = timestamp;
	strncpy(directory[file_number].name, file_name, MAX_FILENAME_LEN);
	directory[file_number].name[MAX_FILENAME_LEN] = '\0';
	directory_storage_t storage;
	storage.magic = DIRECTORY_MAGIC_VALUE;
	os_memcpy(&storage.directory, directory, (FILE_COUNT + 1) * sizeof(file_t));
	bool save_res = system_param_save_with_protect(DIRECTORY_SECTOR, &storage, sizeof(directory_storage_t));
	if (!save_res) {
		os_printf("Unable to save directory to flash memory.\n");
		return false;
	}

	// Write succeeded.
	return true;
}

/*
 * Saves the contents of a file to the flash memory.
 */
bool ICACHE_FLASH_ATTR save_file(uint8_t file_number, file_t file, char *contents) {
	// Verify the parameters.
	if (file_number >= FILE_COUNT) {
		debug_print("Bad file number received: %d.\n", file_number);
		return false;
	}
	if (contents == (char *)NULL) {
		debug_print("NULL file contents supplied.\n");
		return false;
	}
	if (file.size > (MAX_FILE_SECTORS * SPI_FLASH_SEC_SIZE)) {
		debug_print("File size is too big: %d.\n", file.size);
		return false;
	}

	// Get the file information.
	file_t directory[FILE_COUNT + 1];
	if (list_files(directory, FILE_COUNT + 1) != (FILE_COUNT + 1)) {
		debug_print("Unable to load directory to save file %d.\n", file_number);
		return false;
	}

	// Prepare the flash memory for the writing.
	uint8_t save_slot;
	if (!directory[file_number].in_use) {
		// The file isn't currently in use, so save to that location.
		save_slot = directory[file_number].slot;
	} else {
		// The file is in use, save to an empty slot.
		save_slot = directory[FILE_COUNT].slot;
	}
	SpiFlashOpResult res;
	uint16_t start_sector = FILE_BASE_SECTOR + (save_slot * MAX_FILE_SECTORS);
	uint8_t sector_count = file.size / SPI_FLASH_SEC_SIZE;
	if ((sector_count * SPI_FLASH_SEC_SIZE) < file.size) {
		sector_count++;
	}
	for (int ii = 0; ii < sector_count; ii++) {
		res = spi_flash_erase_sector(start_sector + ii);
		if (res != SPI_FLASH_RESULT_OK) {
			debug_print("Unable to erase flash sector %d for file %d: %d.\n", start_sector + ii, file_number, res);
			return false;
		}
	}

	// Write the file's contents.
	res = spi_flash_write(start_sector * SPI_FLASH_SEC_SIZE, (uint32_t *)contents, file.size);
	if (res != SPI_FLASH_RESULT_OK) {
		debug_print("Unable to write flash contents to addres %ld for file %d: %d.\n",
				start_sector * SPI_FLASH_SEC_SIZE, file_number, res);
		return false;
	}
	
	// Update the directory.
	if (save_slot != directory[file_number].slot) {
		// The slot that was used by this file is now the free slot.
		directory[FILE_COUNT].slot = directory[file_number].slot;
	}
	directory[file_number].slot = save_slot;
	directory[file_number].in_use = true;
	directory[file_number].size = file.size;
	directory[file_number].timestamp = file.timestamp;
	strncpy(directory[file_number].name, file.name, MAX_FILENAME_LEN);
	directory[file_number].name[MAX_FILENAME_LEN] = '\0';
	directory_storage_t storage;
	storage.magic = DIRECTORY_MAGIC_VALUE;
	os_memcpy(&storage.directory, directory, (FILE_COUNT + 1) * sizeof(file_t));
	bool save_res = system_param_save_with_protect(DIRECTORY_SECTOR, &storage, sizeof(directory_storage_t));
	if (!save_res) {
		os_printf("Unable to save directory to flash memory.\n");
		return false;
	}

	// Write succeeded.
	return true;
}
