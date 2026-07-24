#include "game.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *fixture =
        "{\"count\":3,\"games\":["
        "{\"id\":1,\"title\":\"\\u5b57\\u5e93\\u5de5\\u5177\","
        "\"filename\":\"folder[a]{b}/one.cia\",\"title_id\":null,"
        "\"cover_url\":null,\"description\":null,\"size_mb\":19.7,"
        "\"has_cover\":false,\"download_url\":\"/download/1\"},"
        "{\"id\":2,\"title\":\"emoji \\ud83c\\udfae\","
        "\"filename\":\"two.cia\",\"size_mb\":0,\"has_cover\":true},"
        "{\"id\":3,\"title\":\"three\",\"filename\":\"x]y}z.cia\","
        "\"size_mb\":3.5,\"has_cover\":null}],\"ok\":true}";
    Game games[512];
    GameParseInfo info;
    int rc;

    if (argc == 2) {
        FILE *file = fopen(argv[1], "rb");
        long size;
        char *json;
        assert(file);
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        rewind(file);
        json = malloc((size_t)size);
        assert(json);
        assert(fread(json, 1, (size_t)size, file) == (size_t)size);
        fclose(file);
        rc = games_parse_json(json, (size_t)size, games, 512, &info);
        free(json);
        assert(rc == 0);
        printf("real JSON: %d/%d games, %d unicode escapes\n",
               info.parsed_count, info.expected_count,
               info.unicode_escape_count);
        assert(info.parsed_count == info.expected_count);
        return 0;
    }

    rc = games_parse_json(fixture, strlen(fixture), games, 512, &info);
    assert(rc == 0);
    assert(info.parsed_count == 3);
    assert(strcmp(games[0].title, "字库工具") == 0);
    assert(strcmp(games[0].filename, "folder[a]{b}/one.cia") == 0);
    assert(games[0].title_id[0] == '\0');
    assert(games[0].description[0] == '\0');
    assert(strcmp(games[1].title, "emoji 🎮") == 0);
    assert(strcmp(games[2].filename, "x]y}z.cia") == 0);
    assert(info.unicode_escape_count == 6);
    puts("fixture JSON: parser + unicode + null tests passed");
    return 0;
}
