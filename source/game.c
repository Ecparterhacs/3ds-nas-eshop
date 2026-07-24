#include "game.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *base;
    const char *end;
    GameParseInfo *info;
} Parser;

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && isspace((unsigned char)*p)) ++p;
    return p;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool read_hex4(const char *p, const char *end, unsigned *value)
{
    unsigned v = 0;
    int h;
    if ((size_t)(end - p) < 4) return false;
    for (int i = 0; i < 4; ++i) {
        h = hex_value(p[i]);
        if (h < 0) return false;
        v = (v << 4) | (unsigned)h;
    }
    *value = v;
    return true;
}

static void append_byte(char **out, size_t *left, unsigned byte)
{
    if (*left > 1) {
        **out = (char)byte;
        ++*out;
        --*left;
    }
}

static void append_utf8(char **out, size_t *left, unsigned codepoint)
{
    size_t needed;
    if (codepoint <= 0x7F) needed = 1;
    else if (codepoint <= 0x7FF) needed = 2;
    else if (codepoint <= 0xFFFF) needed = 3;
    else if (codepoint <= 0x10FFFF) needed = 4;
    else return;

    /* Keep truncated destinations valid UTF-8: write all bytes or none. */
    if (*left <= needed) return;

    if (codepoint <= 0x7F) {
        append_byte(out, left, codepoint);
    } else if (codepoint <= 0x7FF) {
        append_byte(out, left, 0xC0u | (codepoint >> 6));
        append_byte(out, left, 0x80u | (codepoint & 0x3Fu));
    } else if (codepoint <= 0xFFFF) {
        append_byte(out, left, 0xE0u | (codepoint >> 12));
        append_byte(out, left, 0x80u | ((codepoint >> 6) & 0x3Fu));
        append_byte(out, left, 0x80u | (codepoint & 0x3Fu));
    } else if (codepoint <= 0x10FFFF) {
        append_byte(out, left, 0xF0u | (codepoint >> 18));
        append_byte(out, left, 0x80u | ((codepoint >> 12) & 0x3Fu));
        append_byte(out, left, 0x80u | ((codepoint >> 6) & 0x3Fu));
        append_byte(out, left, 0x80u | (codepoint & 0x3Fu));
    }
}

/*
 * Decodes one JSON string. p must point at the opening quote.
 * next receives the first character after the closing quote.
 */
static bool decode_string(Parser *parser, const char *p, const char *limit,
                          char *dst, size_t dst_size, const char **next)
{
    char *out = dst;
    size_t left = dst_size;
    if (p >= limit || *p != '"' || dst_size == 0) return false;
    ++p;

    while (p < limit) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"') {
            *out = '\0';
            if (next) *next = p;
            return true;
        }
        if (c < 0x20) return false;
        if (c != '\\') {
            append_byte(&out, &left, c);
            continue;
        }
        if (p >= limit) return false;
        c = (unsigned char)*p++;
        switch (c) {
            case '"': append_byte(&out, &left, '"'); break;
            case '\\': append_byte(&out, &left, '\\'); break;
            case '/': append_byte(&out, &left, '/'); break;
            case 'b': append_byte(&out, &left, '\b'); break;
            case 'f': append_byte(&out, &left, '\f'); break;
            case 'n': append_byte(&out, &left, '\n'); break;
            case 'r': append_byte(&out, &left, '\r'); break;
            case 't': append_byte(&out, &left, '\t'); break;
            case 'u': {
                unsigned cp;
                unsigned low;
                if (!read_hex4(p, limit, &cp)) return false;
                p += 4;
                if (parser->info) ++parser->info->unicode_escape_count;

                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if ((size_t)(limit - p) < 6 || p[0] != '\\' ||
                        p[1] != 'u' || !read_hex4(p + 2, limit, &low) ||
                        low < 0xDC00 || low > 0xDFFF) {
                        return false;
                    }
                    p += 6;
                    if (parser->info) ++parser->info->unicode_escape_count;
                    cp = 0x10000u + ((cp - 0xD800u) << 10) +
                         (low - 0xDC00u);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return false;
                }
                append_utf8(&out, &left, cp);
                break;
            }
            default:
                return false;
        }
    }
    return false;
}

static const char *scan_string_end(const char *p, const char *end)
{
    bool escaped = false;
    if (p >= end || *p != '"') return NULL;
    for (++p; p < end; ++p) {
        if (escaped) {
            escaped = false;
        } else if (*p == '\\') {
            escaped = true;
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

static const char *scan_compound_end(const char *p, const char *end)
{
    char open;
    char close;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;

    if (p >= end || (*p != '{' && *p != '[')) return NULL;
    open = *p;
    close = open == '{' ? '}' : ']';

    for (; p < end; ++p) {
        char c = *p;
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == open) {
            ++depth;
        } else if (c == close) {
            if (--depth == 0) return p + 1;
        }
    }
    return NULL;
}

static const char *skip_value(const char *p, const char *end)
{
    p = skip_ws(p, end);
    if (p >= end) return NULL;
    if (*p == '"') return scan_string_end(p, end);
    if (*p == '{' || *p == '[') return scan_compound_end(p, end);
    while (p < end && *p != ',' && *p != '}' && *p != ']') ++p;
    return p;
}

static bool parse_number_token(const char *p, const char *end, double *out,
                               const char **next)
{
    char token[64];
    size_t n = 0;
    char *token_end;
    p = skip_ws(p, end);
    while (p + n < end && n + 1 < sizeof(token)) {
        char c = p[n];
        if (!(c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E' ||
              (c >= '0' && c <= '9'))) {
            break;
        }
        token[n++] = c;
    }
    if (n == 0) return false;
    token[n] = '\0';
    *out = strtod(token, &token_end);
    if (token_end != token + n) return false;
    if (next) *next = p + n;
    return true;
}

static bool literal_at(const char *p, const char *end, const char *literal)
{
    size_t n = strlen(literal);
    return (size_t)(end - p) >= n && memcmp(p, literal, n) == 0;
}

static bool parse_game_object(Parser *parser, const char *begin,
                              const char *end, Game *game)
{
    const char *p = skip_ws(begin, end);
    char key[40];

    if (p >= end || *p != '{') return false;
    memset(game, 0, sizeof(*game));
    ++p;

    for (;;) {
        const char *after_key;
        const char *after_value;
        char *string_target = NULL;
        size_t string_size = 0;
        double number;

        p = skip_ws(p, end);
        if (p >= end) return false;
        if (*p == '}') return true;
        if (!decode_string(parser, p, end, key, sizeof(key), &after_key)) {
            return false;
        }
        p = skip_ws(after_key, end);
        if (p >= end || *p != ':') return false;
        p = skip_ws(p + 1, end);

        if (strcmp(key, "title") == 0) {
            string_target = game->title; string_size = sizeof(game->title);
        } else if (strcmp(key, "filename") == 0) {
            string_target = game->filename; string_size = sizeof(game->filename);
        } else if (strcmp(key, "title_id") == 0) {
            string_target = game->title_id; string_size = sizeof(game->title_id);
        } else if (strcmp(key, "cover_url") == 0) {
            string_target = game->cover_url; string_size = sizeof(game->cover_url);
        } else if (strcmp(key, "download_url") == 0) {
            string_target = game->download_url; string_size = sizeof(game->download_url);
        } else if (strcmp(key, "description") == 0) {
            string_target = game->description; string_size = sizeof(game->description);
        } else if (strcmp(key, "region") == 0) {
            string_target = game->region; string_size = sizeof(game->region);
        } else if (strcmp(key, "ext") == 0) {
            string_target = game->ext; string_size = sizeof(game->ext);
        }

        if (string_target) {
            if (literal_at(p, end, "null")) {
                string_target[0] = '\0';
                after_value = p + 4;
            } else if (*p == '"') {
                if (!decode_string(parser, p, end, string_target, string_size,
                                   &after_value)) {
                    return false;
                }
            } else {
                return false;
            }
        } else if (strcmp(key, "id") == 0) {
            if (!parse_number_token(p, end, &number, &after_value)) return false;
            game->id = (int)number;
        } else if (strcmp(key, "size_mb") == 0) {
            if (literal_at(p, end, "null")) {
                game->size_mb = 0.0f;
                after_value = p + 4;
            } else {
                if (!parse_number_token(p, end, &number, &after_value)) return false;
                game->size_mb = (float)number;
            }
        } else if (strcmp(key, "has_cover") == 0) {
            if (literal_at(p, end, "true")) {
                game->has_cover = true; after_value = p + 4;
            } else if (literal_at(p, end, "false")) {
                game->has_cover = false; after_value = p + 5;
            } else if (literal_at(p, end, "null")) {
                game->has_cover = false; after_value = p + 4;
            } else {
                return false;
            }
        } else {
            after_value = skip_value(p, end);
            if (!after_value) return false;
        }

        p = skip_ws(after_value, end);
        if (p >= end) return false;
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == '}') return true;
        return false;
    }
}

static bool find_root_fields(Parser *parser, const char **games_array,
                             int *expected)
{
    const char *p = skip_ws(parser->base, parser->end);
    char key[40];
    if (p >= parser->end || *p != '{') return false;
    ++p;

    while (p < parser->end) {
        const char *after_key;
        const char *after_value;
        double number;
        p = skip_ws(p, parser->end);
        if (p < parser->end && *p == '}') return *games_array != NULL;
        if (!decode_string(parser, p, parser->end, key, sizeof(key),
                           &after_key)) {
            return false;
        }
        p = skip_ws(after_key, parser->end);
        if (p >= parser->end || *p != ':') return false;
        p = skip_ws(p + 1, parser->end);

        if (strcmp(key, "games") == 0) {
            if (p >= parser->end || *p != '[') return false;
            *games_array = p;
            after_value = scan_compound_end(p, parser->end);
        } else if (strcmp(key, "count") == 0) {
            if (!parse_number_token(p, parser->end, &number, &after_value)) {
                return false;
            }
            *expected = (int)number;
        } else {
            after_value = skip_value(p, parser->end);
        }
        if (!after_value) return false;
        p = skip_ws(after_value, parser->end);
        if (p < parser->end && *p == ',') ++p;
    }
    return false;
}

static int fail(Parser *parser, const char *at, const char *message, int code)
{
    if (parser->info) {
        parser->info->error_offset =
            at && at >= parser->base ? (size_t)(at - parser->base) : 0;
        snprintf(parser->info->error, sizeof(parser->info->error), "%s", message);
    }
    return code;
}

int games_parse_json(const char *json, size_t json_size, Game *games,
                     size_t games_capacity, GameParseInfo *info)
{
    Parser parser;
    const char *array = NULL;
    const char *array_end;
    const char *p;
    int expected = -1;
    int parsed = 0;

    if (info) memset(info, 0, sizeof(*info));
    if (!json || !games || games_capacity == 0) return -1;
    parser.base = json;
    parser.end = json + json_size;
    parser.info = info;

    if (!find_root_fields(&parser, &array, &expected) || !array) {
        return fail(&parser, json, "Root games array not found", -2);
    }
    if (info) info->expected_count = expected;
    array_end = scan_compound_end(array, parser.end);
    if (!array_end) return fail(&parser, array, "Unclosed games array", -3);

    p = array + 1;
    while (p < array_end - 1) {
        const char *object_end;
        p = skip_ws(p, array_end - 1);
        if (p >= array_end - 1 || *p == ']') break;
        if (*p != '{') {
            return fail(&parser, p, "Expected game object", -4);
        }
        object_end = scan_compound_end(p, array_end);
        if (!object_end) return fail(&parser, p, "Unclosed game object", -5);

        if ((size_t)parsed >= games_capacity) {
            return fail(&parser, p, "Game capacity exceeded", -6);
        }
        if (!parse_game_object(&parser, p, object_end, &games[parsed])) {
            return fail(&parser, p, "Malformed game object", -7);
        }
        ++parsed;
        if (info) info->parsed_count = parsed;
        p = skip_ws(object_end, array_end - 1);
        if (p < array_end - 1 && *p == ',') ++p;
    }

    if (info) {
        info->expected_count = expected >= 0 ? expected : parsed;
        info->parsed_count = parsed;
        info->error[0] = '\0';
    }
    if (expected >= 0 && parsed != expected) {
        return fail(&parser, p, "Parsed count differs from JSON count", -8);
    }
    return 0;
}
