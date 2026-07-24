#pragma once

#include <3ds.h>
#include <citro2d.h>
#include "game.h"
#include "http_util.h"
#include "installer.h"

typedef struct {
    C3D_RenderTarget *top;
    C3D_RenderTarget *bottom;
    C2D_TextBuf text_buf;
    C2D_Font zh_font;
    bool use_zh_font;
    C3D_Tex cover_texture;
    Tex3DS_SubTexture cover_subtexture;
    bool cover_loaded;
    int cover_game_id;
} Ui;

bool ui_init(Ui *ui);
void ui_exit(Ui *ui);
void ui_clear_cover(Ui *ui);
bool ui_set_cover_bmp(Ui *ui, int game_id, const void *data, size_t size);
void ui_render(Ui *ui, const Game *games, int game_count, int selected,
               const char *status, const HttpDebugInfo *http_debug,
               const GameParseInfo *parse_info, bool show_debug,
               const InstallProgress *install);
