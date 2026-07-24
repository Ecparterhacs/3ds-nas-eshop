#include "http_util.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_detail(HttpDebugInfo *debug, const char *message)
{
    if (!debug) return;
    snprintf(debug->detail, sizeof(debug->detail), "%s", message);
}

static void cancel_then_close(httpcContext *context)
{
    httpcCancelConnection(context);
    httpcCloseContext(context);
}

int http_get_alloc(const char *route, char **out_data, size_t *out_size,
                   HttpDebugInfo *debug)
{
    httpcContext context;
    char url[512];
    char *buffer = NULL;
    size_t used = 0;
    size_t capacity = HTTP_INITIAL_CAPACITY;
    u32 downloaded = 0;
    u32 current = 0;
    u32 total = 0;
    u32 status = 0;
    Result result;

    if (!route || !out_data || !out_size) return -1;
    *out_data = NULL;
    *out_size = 0;
    if (debug) memset(debug, 0, sizeof(*debug));

    snprintf(url, sizeof(url), "http://%s:%u%s", NAS_HOST,
             (unsigned)NAS_PORT, route);

    result = httpcOpenContext(&context, HTTPC_METHOD_GET, url, 0);
    if (R_FAILED(result)) {
        if (debug) debug->result = result;
        set_detail(debug, "httpcOpenContext failed");
        return -2;
    }

    httpcAddRequestHeaderField(&context, "User-Agent", "3DS-eShop/2.0");
    httpcAddRequestHeaderField(&context, "Connection", "close");

    result = httpcBeginRequest(&context);
    if (R_FAILED(result)) {
        if (debug) debug->result = result;
        set_detail(debug, "httpcBeginRequest failed");
        cancel_then_close(&context);
        return -3;
    }

    result = httpcGetResponseStatusCode(&context, &status);
    if (debug) {
        debug->result = result;
        debug->status_code = status;
    }
    if (R_FAILED(result) || status < 200 || status >= 300) {
        set_detail(debug, "HTTP status is not 2xx");
        cancel_then_close(&context);
        return -4;
    }

    if (R_SUCCEEDED(httpcGetDownloadSizeState(&context, &current, &total))) {
        if (debug) debug->declared_size = total;
        if (total > 0 && total < HTTP_MAX_CAPACITY) {
            capacity = (size_t)total + 1;
            if (capacity < 4096) capacity = 4096;
        }
    }

    buffer = malloc(capacity);
    if (!buffer) {
        set_detail(debug, "Out of memory");
        cancel_then_close(&context);
        return -5;
    }

    for (;;) {
        if (used + 1 >= capacity) {
            size_t next = capacity * 2;
            char *grown;
            if (next > HTTP_MAX_CAPACITY) next = HTTP_MAX_CAPACITY;
            if (next <= capacity) {
                free(buffer);
                set_detail(debug, "Response exceeds 2 MiB");
                cancel_then_close(&context);
                return -6;
            }
            grown = realloc(buffer, next);
            if (!grown) {
                free(buffer);
                set_detail(debug, "Could not grow HTTP buffer");
                cancel_then_close(&context);
                return -7;
            }
            buffer = grown;
            capacity = next;
        }

        downloaded = 0;
        result = httpcDownloadData(&context, (u8 *)buffer + used,
                                   (u32)(capacity - used - 1), &downloaded);
        used += downloaded;

        if (result == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) {
            continue;
        }
        if (R_FAILED(result)) {
            free(buffer);
            if (debug) debug->result = result;
            set_detail(debug, "httpcDownloadData failed");
            cancel_then_close(&context);
            return -8;
        }
        break;
    }

    buffer[used] = '\0';
    httpcCloseContext(&context);

    if (debug) {
        debug->result = 0;
        debug->downloaded_size = (u32)used;
        debug->capacity = (u32)capacity;
        snprintf(debug->detail, sizeof(debug->detail), "Complete response");
    }
    *out_data = buffer;
    *out_size = used;
    return 0;
}
