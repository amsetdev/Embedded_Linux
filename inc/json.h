/**
 * @file json.h
 * @brief Simple JSON parsing utility functions.
 *
 * This module provides lightweight helper functions for reading JSON
 * configuration files and extracting string and integer values.
 * It is intended for simple key-value JSON parsing without requiring
 * an external JSON library.
 */

#ifndef JSON_H
#define JSON_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* -------------------------------------------------------------------------- */
/* File Operations                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Reads an entire file into memory.
 *
 * Allocates a buffer using malloc() and reads the complete contents
 * of the specified file into it.
 *
 * @param filename Path to the file.
 *
 * @return
 * - Pointer to the allocated buffer on success.
 * - NULL if the file cannot be read or memory allocation fails.
 *
 * @note The caller is responsible for freeing the returned buffer.
 */
char *read_file(const char *filename);

/* -------------------------------------------------------------------------- */
/* JSON Parsing Functions                                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Retrieves a string value from a JSON document.
 *
 * Searches the complete JSON text for the specified key and copies
 * the associated string value into the supplied buffer.
 *
 * @param json Pointer to the JSON text.
 * @param key JSON key to search for.
 * @param value Output buffer for the string value.
 * @param value_size Size of the output buffer.
 *
 * @return
 * - 0 on success.
 * - -1 if the key is not found or parsing fails.
 */
int json_get_string(const char *json,
                    const char *key,
                    char *value,
                    size_t value_size);

/**
 * @brief Retrieves an integer value from a JSON document.
 *
 * Searches the complete JSON text for the specified key and returns
 * its integer value.
 *
 * @param json Pointer to the JSON text.
 * @param key JSON key to search for.
 * @param value Pointer to the destination integer.
 *
 * @return
 * - 0 on success.
 * - -1 if the key is not found or parsing fails.
 */
int json_get_int(const char *json,
                 const char *key,
                 int *value);

/**
 * @brief Retrieves a string value from a JSON object.
 *
 * Searches for a string value beginning at the specified JSON object
 * rather than the entire document.
 *
 * @param object Pointer to the JSON object.
 * @param key JSON key to search for.
 * @param value Output buffer for the string value.
 * @param value_size Size of the output buffer.
 *
 * @return
 * - 0 on success.
 * - -1 if the key is not found or parsing fails.
 */
int json_get_string_from(const char *object,
                         const char *key,
                         char *value,
                         size_t value_size);

/**
 * @brief Retrieves an integer value from a JSON object.
 *
 * Searches for an integer value beginning at the specified JSON object
 * rather than the entire document.
 *
 * @param object Pointer to the JSON object.
 * @param key JSON key to search for.
 * @param value Pointer to the destination integer.
 *
 * @return
 * - 0 on success.
 * - -1 if the key is not found or parsing fails.
 */
int json_get_int_from(const char *object,
                      const char *key,
                      int *value);

#ifdef __cplusplus
}
#endif

#endif /* JSON_H */