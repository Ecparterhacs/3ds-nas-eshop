#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int id;
    float size_mb;
    bool has_cover;
    char title[256];
    char filename[512];
    char title_id[32];
    char cover_url[256];
    char download_url[256];
    char description[512];
    char region[32];
    char ext[16];
} Game;

typedef struct {
    int expected_count;
    int parsed_count;
    int unicode_escape_count;
    size_t error_offset;
    char error[96];
} GameParseInfo;

/*
 * Parses the root {"count": N, "games": [...]} response.
 * JSON strings, including \uXXXX and surrogate pairs, are decoded to UTF-8.
 * Returns 0 on success or a negative value on malformed/truncated JSON.
 */
int games_parse_json(const char *json, size_t json_size, Game *games,
                     size_t games_capacity, GameParseInfo *info);

