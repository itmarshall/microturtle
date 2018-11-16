/*
 * string_builder.h: Lightweight way to create a string via a sequence of concatenations.
 *
 * Author: Ian Marshall
 * Date: 23/07/2016
 */
#ifndef _STRING_BUILDER_H
#define _STRING_BUILDER_H

#include "ets_sys.h"
#include "osapi.h"
#include "os_type.h"    
#include "espmissingincludes.h"

/*
 * Structure for string builders.
 */
typedef struct string_builder {
    char *buf;     // The builder that holds the string. This will always be a valid C string.
    int allocated; // The number of bytes allocated for the builder.
    int len;       // The number of characters used within the builder *NOT* including the NULL terminator.
} string_builder;

/*
 * Creates a string builder, with an initial size. The resulting builder must eventually be freed with 
 * free_string_builder.
 */
string_builder * ICACHE_FLASH_ATTR create_string_builder(int initial_len);

/*
 * De-allocates all memory for a string builder (including its contents).
 */
void ICACHE_FLASH_ATTR free_string_builder(string_builder *buf);

/*
 * Compares a string against the contents of the string builder.
 * Returns a number less than zero if the string builder is less than the string, zero if they match or
 * a number greater than zero if the string builder is greater than the string.
 */
int ICACHE_FLASH_ATTR string_builder_strncmp(string_builder *sb, char *str, size_t len);

/*
 * Appends a string to a pre-existing string builder. The builder is expanded to store the new string if
 * required.
 */
bool ICACHE_FLASH_ATTR append_string_builder(string_builder *buf, const char *str);

/*
 * Appends a string builder to a pre-existing string builder. The builder is expanded to store the new string if 
 * required.
 */
bool ICACHE_FLASH_ATTR append_string_builder_to_string_builder(string_builder *buf, const string_builder *source);

/*
 * Appends a 32-bit signed integer to a pre-existing string builder. The builder is expanded to store
 * the new string if requried.
 */
bool ICACHE_FLASH_ATTR append_int32_string_builder(string_builder *buf, const int32_t val);

/*
 * Appends a character to a pre-existing string builder. The builder is expanded to store the new string if 
 * requried.
 */
bool ICACHE_FLASH_ATTR append_char_string_builder(string_builder *buf, const char c);

/*
 * Dumps the contents of the builder via "os_printf". Designed for debugging purposes.
 */
void ICACHE_FLASH_ATTR printf_string_builder(string_builder *buf);

#endif
