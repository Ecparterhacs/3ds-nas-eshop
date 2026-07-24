#pragma once

#include <3ds.h>
#include <stddef.h>

typedef struct {
    u32 status_code;
    u32 declared_size;
    u32 downloaded_size;
    u32 capacity;
    Result result;
    char detail[96];
} HttpDebugInfo;

/*
 * Downloads a complete NAS response into a NUL-terminated malloc buffer.
 * The function grows past 256 KiB if HTTPC reports DOWNLOADPENDING.
 */
int http_get_alloc(const char *route, char **out_data, size_t *out_size,
                   HttpDebugInfo *debug);

