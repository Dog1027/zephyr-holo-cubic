#pragma once

#include <stddef.h>
#include "file.h"

/*
 * Settings the user can also edit on a PC: /config.txt on the SD card, one "key=value" per line.
 * Spaces around the key and the value are ignored. Blank lines, '#' comments and keys this
 * firmware does not know are kept as they are when a value is changed.
 */
#define SDCONF_PATH FILE_ROOT "/config.txt"

// Whole file limit, the file is read into a fixed buffer
#define SDCONF_MAX_SIZE 1024

// Copy the value of key into buf (NUL terminated). Returns the value length,
// -ENOENT if the key (or the file) is missing, -ENOSPC if buf is too small
int sdconf_get(const char *key, char *buf, size_t len);

// Replace the key's line, or append one if it is missing. value NULL removes the key
int sdconf_set(const char *key, const char *value);
