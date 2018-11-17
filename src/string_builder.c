/*
 * string_builder.c: Lightweight way to create a string via a sequence of concatenations.
 *
 * Author: Ian Marshall
 * Date: 23/07/2016
 */
#include "ets_sys.h"
#include "osapi.h"
#include "mem.h"
#include "os_type.h"
#include "espmissingincludes.h"
#include "user_interface.h"

#include "string_builder.h"

LOCAL bool ICACHE_FLASH_ATTR resize_string_builder(string_builder *sb, unsigned int additional_required);

/*
 * Creates a string builder, with an initial size. The resulting builder must eventually be freed with 
 * free_string_builder.
 */
string_builder * ICACHE_FLASH_ATTR create_string_builder(int initial_len) {
    // Create the builder structure.
    string_builder *sb = (string_builder *)os_malloc(sizeof(string_builder));
    if (sb == NULL) {
        return NULL;
    }

    // Fill in the builder length fields.
    sb->allocated = (initial_len < 16) ? 16 : initial_len;
    sb->len = 0;

    // Allocate the requires space within the structure.
    sb->buf = (char *)os_malloc(sb->allocated * sizeof(char));
    if (sb->buf == NULL) {
        os_free(sb);
        return NULL;
    }

    // Ensure the string is empty.
    sb->buf[0] = '\0';

    // Buffer created, return it.
    return sb;
}

/*
 * De-allocates all memory for a string builder (including its contents).
 */
void ICACHE_FLASH_ATTR free_string_builder(string_builder *sb) {
    if (sb != NULL) {
        if (sb->buf != NULL) {
            os_free(sb->buf);
        }
        os_free(sb);
    }
}

/*
 * Compares a string against the contents of the string builder.
 * Returns a number less than zero if the string builder is less than the string, zero if they match or
 * a number greater than zero if the string builder is greater than the string.
 */
int ICACHE_FLASH_ATTR string_builder_strncmp(string_builder *sb, char *str, size_t len) {
	char *p1 = sb->buf;
	char *p2 = str;
	int count = 0;

	while ((count < sb->len) && (count < len)) {
		if (*p1 != *p2) {
			// There is a difference return -1 or 1 depending on which is lesser.
			return ((*(unsigned char *)p1 < *(unsigned char *)p2) ? -1 : 1);
		} else if (*p1 == '\0') {
			// Both strings match to their ends.
			return 0;
		}

		// Move to the next character in both strings.
		p1++;
		p2++;
		count++;
	}

	// If we get to here, the strings match up until the supplied length.
	return 0;
}

/*
 * Appends a string to a pre-existing string builder. The builder is expanded to store the new string if required.
 */
bool ICACHE_FLASH_ATTR append_string_builder(string_builder *sb, const char *str) {
	// Ensure we have space to add the string to the builder.
	int len = os_strlen(str) + 1;
	int free = sb->allocated - sb->len - 1;
	if (free < len) {
		// We need to increase the size of the builder to fit the string in.
		if (!resize_string_builder(sb, len - free)) {
			// We were unable to resize the builder.
			os_printf("Unable to resize builder for string \"%s\".", str);
			return false;
		}
	}

	// Add the string.
	os_memmove(&sb->buf[sb->len], str, len - 1);
	sb->buf[sb->len + len] = '\0';
	sb->len += len - 1;
	return true;
}

/*
 * Appends a string builder to a pre-existing string builder. The builder is expanded to store the new string if 
 * required.
 */
bool ICACHE_FLASH_ATTR append_string_builder_to_string_builder(string_builder *sb, const string_builder *source) {
    // Ensure we have space to add the string to the builder.
    int free = sb->allocated - sb->len - 1;
    if (free < (source->len + 1)) {
        // We need to increase the size of the builder to fit the string in.
        if (!resize_string_builder(sb, source->len - free + 1)) {
            // We were unable to resize the builder.
            os_printf("Unable to resize builder for string builder appending\n.");
            return false;
        }
    }

    // Add the string builder's contents, including the trailing '\0'.
    os_memmove(&sb->buf[sb->len], source->buf, source->len + 1);
    sb->len += source->len;
    return true;
}

/*
 * Appends a 32-bit signed integer to a pre-existing string builder. The builder is expanded to store the new string if 
 * requried.
 */
bool ICACHE_FLASH_ATTR append_int32_string_builder(string_builder *sb, const int32_t val) {
    // Convert the integer to a string.
    char str[12];
    os_sprintf(str, "%d", val);

    // Store the string in the builder.
    return append_string_builder(sb, str);
}

/*
 * Appends a character to a pre-existing string builder. The builder is expanded to store the new string if 
 * requried.
 */
bool ICACHE_FLASH_ATTR append_char_string_builder(string_builder *sb, const char c) {
    // Ensure we have space to add the character to the builder.
    int free = sb->allocated - sb->len - 1;
    if (free < 1) {
        // We need to increase the size of the builder to fit the string in.
        if (!resize_string_builder(sb, 1)) {
            // We were unable to resize the builder.
            os_printf("Unable to resize builder for character \"%c\".", c);
            return false;
        }
    }

    // Add the character.
	sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
    return true;
}

/*
 * Dumps the contents of the builder via "os_printf". Designed for debugging purposes.
 */
void ICACHE_FLASH_ATTR printf_string_builder(string_builder *sb) {
    if (sb == NULL) {
        os_printf("NULL builder.\n");
    } else {
        for (int ii = 0; ii < sb->len; ii++) {
            os_printf("%c", sb->buf[ii]);
        }
    }
}

/*
 * Alters the size of a string builder by increasing the amount of storage available.
 */
LOCAL bool ICACHE_FLASH_ATTR resize_string_builder(string_builder *sb, 
                                                   unsigned int additional_required) {
    // Find the new size of the builder.
    int new_size;
    if ((sb->allocated + sb->allocated - sb->len) < additional_required) {
        // Merely doubling the builder won't help, create the additional requried.
        new_size = sb->len + additional_required;
    } else {
        new_size = sb->allocated + sb->allocated;
    }

    char *new_string;
    new_string = (char *)os_malloc(new_size * sizeof(char));
    if (new_string == NULL) {
        // We couldn't create the new builder to hold the expanded string.
        return false;
    }

    // Copy the old string to the new one.
    char *old_string = sb->buf;
    os_memmove(new_string, old_string, sb->len);
    os_free(old_string);

    // Update the builder structure.
    sb->buf = new_string;
    sb->allocated = new_size;
    return true;
}
