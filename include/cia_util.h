#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Extracts the big-endian title ID from the TMD embedded in an initial CIA
 * block. The block normally only needs to contain the first 64-256 KiB.
 */
bool cia_extract_title_id(const void *data, size_t size, uint64_t *title_id);

/*
 * Limits direct installation to SD-based user content:
 * applications, demos, updates and DLC.
 */
bool cia_title_is_safe_sd(uint64_t title_id);
bool cia_title_requires_new3ds(uint64_t title_id);

