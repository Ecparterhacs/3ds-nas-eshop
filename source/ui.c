#include "ui.h"
#include "bmp_util.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

#define COLOR_BG       C2D_Color32(239, 247, 247, 255)
#define COLOR_PANEL    C2D_Color32(255, 255, 255, 255)
#define COLOR_PANEL_2  C2D_Color32(225, 238, 239, 255)
#define COLOR_HEADER   C2D_Color32(241, 126, 36, 255)
#define COLOR_ORANGE_2 C2D_Color32(255, 177, 58, 255)
#define COLOR_ACCENT   C2D_Color32(29, 169, 181, 255)
#define COLOR_GOLD     C2D_Color32(241, 126, 36, 255)
#define COLOR_TEXT     C2D_Color32(52, 65, 70, 255)
#define COLOR_MUTED    C2D_Color32(111, 132, 137, 255)
#define COLOR_LINE     C2D_Color32(200, 217, 219, 255)
#define COLOR_SHADOW   C2D_Color32(177, 197, 199, 150)
#define COLOR_OK       C2D_Color32(72, 184, 104, 255)
#define COLOR_BAD      C2D_Color32(218, 76, 76, 255)
#define COLOR_WHITE    C2D_Color32(255, 255, 255, 255)

static void draw_text(Ui *ui, const char *value, float x, float y, float scale,
                      u32 color, float wrap_width)
{
    C2D_Text text;
    float measured_width = 0.0f;
    float measured_height = 0.0f;
    float fit = 1.0f;
    float fitted_scale;
    unsigned line_count = 1;
    const char *input = value && value[0] ? value : "-";
    const char *cursor;

    if (ui->use_zh_font) {
        C2D_TextFontParse(&text, ui->zh_font, ui->text_buf, input);
    } else {
        C2D_TextParse(&text, ui->text_buf, input);
    }
    C2D_TextOptimize(&text);

    /*
     * The Chinese system font reports a substantially taller glyph box than
     * citro2d's default font. Fixed scales and WordWrap therefore produce
     * extra lines that overlap the next UI row on real hardware. Measure the
     * parsed font, fit it into one nominal 32 px line per explicit newline,
     * and treat wrap_width as a single-line width limit. Only explicit '\n'
     * characters are allowed to create another row.
     */
    for (cursor = input; *cursor; ++cursor) {
        if (*cursor == '\n') ++line_count;
    }
    C2D_TextGetDimensions(
        &text, scale, scale, &measured_width, &measured_height);
    if (measured_height > 0.0f) {
        float maximum_height = 32.0f * scale * (float)line_count;
        if (measured_height > maximum_height) {
            fit = maximum_height / measured_height;
        }
    }
    if (wrap_width > 0.0f && measured_width > 0.0f &&
        measured_width * fit > wrap_width) {
        float width_fit = wrap_width / measured_width;
        if (width_fit < fit) fit = width_fit;
    }
    fitted_scale = scale * fit;
    C2D_DrawText(&text, C2D_WithColor, x, y, 0.5f,
                 fitted_scale, fitted_scale, color);
}

static void clip_text_units(const char *source, char *output, size_t output_size,
                            int max_units)
{
    const unsigned char *cursor = (const unsigned char *)source;
    size_t used = 0;
    int units = 0;
    bool truncated = false;

    if (!source || output_size == 0) return;
    while (*cursor) {
        size_t length = 1;
        int width = *cursor < 0x80 ? 1 : 2;
        if ((*cursor & 0xE0) == 0xC0) length = 2;
        else if ((*cursor & 0xF0) == 0xE0) length = 3;
        else if ((*cursor & 0xF8) == 0xF0) length = 4;

        if (units + width > max_units - 3 ||
            used + length + 4 > output_size) {
            truncated = true;
            break;
        }
        memcpy(output + used, cursor, length);
        used += length;
        units += width;
        cursor += length;
    }
    if (truncated && used + 3 < output_size) {
        memcpy(output + used, "...", 3);
        used += 3;
    }
    output[used] = '\0';
}

static void draw_badge(Ui *ui, const char *label, float x, float y, float width,
                       u32 background)
{
    C2D_DrawRectSolid(x, y, 0.2f, width, 18.0f, background);
    draw_text(ui, label, x + 7.0f, y + 2.0f, 0.40f, COLOR_WHITE, 0);
}

static void draw_card(float x, float y, float width, float height, bool active)
{
    C2D_DrawRectSolid(x + 2, y + 3, 0.10f, width, height, COLOR_SHADOW);
    if (active) {
        C2D_DrawRectSolid(x - 2, y - 2, 0.11f, width + 4, height + 4,
                          COLOR_HEADER);
    }
    C2D_DrawRectSolid(x, y, 0.12f, width, height, COLOR_PANEL);
}

static void draw_loaded_cover(Ui *ui, float x, float y, float width,
                              float height, float depth)
{
    C2D_Image image;
    C2D_DrawParams parameters;
    Tex3DS_SubTexture cropped;
    float source_aspect;
    float target_aspect;

    if (!ui->cover_loaded) return;
    cropped = ui->cover_subtexture;
    source_aspect = (float)cropped.width / cropped.height;
    target_aspect = width / height;
    if (source_aspect > target_aspect) {
        float keep = target_aspect / source_aspect;
        float span = (cropped.right - cropped.left) * keep;
        float center = (cropped.left + cropped.right) * 0.5f;
        cropped.left = center - span * 0.5f;
        cropped.right = center + span * 0.5f;
    } else if (source_aspect < target_aspect) {
        float keep = source_aspect / target_aspect;
        float span = (cropped.top - cropped.bottom) * keep;
        float center = (cropped.top + cropped.bottom) * 0.5f;
        cropped.top = center + span * 0.5f;
        cropped.bottom = center - span * 0.5f;
    }
    image.tex = &ui->cover_texture;
    image.subtex = &cropped;
    parameters.pos.x = x;
    parameters.pos.y = y;
    parameters.pos.w = width;
    parameters.pos.h = height;
    parameters.center.x = 0.0f;
    parameters.center.y = 0.0f;
    parameters.depth = depth;
    parameters.angle = 0.0f;
    C2D_DrawImage(image, &parameters, NULL);
}

bool ui_init(Ui *ui)
{
    memset(ui, 0, sizeof(*ui));
    gfxInitDefault();
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        gfxExit();
        return false;
    }
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) {
        C3D_Fini();
        gfxExit();
        return false;
    }
    C2D_Prepare();
    ui->top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    ui->bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    ui->text_buf = C2D_TextBufNew(8192);
    ui->zh_font = C2D_FontLoadSystem(CFG_REGION_CHN);
    ui->use_zh_font = ui->zh_font != NULL;
    return ui->top && ui->bottom && ui->text_buf;
}

void ui_exit(Ui *ui)
{
    ui_clear_cover(ui);
    if (ui->zh_font) C2D_FontFree(ui->zh_font);
    if (ui->text_buf) C2D_TextBufDelete(ui->text_buf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

void ui_clear_cover(Ui *ui)
{
    if (ui->cover_loaded) C3D_TexDelete(&ui->cover_texture);
    ui->cover_loaded = false;
    ui->cover_game_id = -1;
    memset(&ui->cover_texture, 0, sizeof(ui->cover_texture));
    memset(&ui->cover_subtexture, 0, sizeof(ui->cover_subtexture));
}

bool ui_set_cover_bmp(Ui *ui, int game_id, const void *data, size_t size)
{
    ui_clear_cover(ui);
    if (!bmp_to_texture(data, size, &ui->cover_texture,
                        &ui->cover_subtexture)) {
        return false;
    }
    ui->cover_loaded = true;
    ui->cover_game_id = game_id;
    return true;
}

static void render_top(Ui *ui, const Game *games, int game_count, int selected)
{
    const int page_size = 6;
    int page_start = selected >= 0 ? (selected / page_size) * page_size : 0;
    char text[128];
    char title_text[128];

    C2D_TargetClear(ui->top, COLOR_BG);
    C2D_SceneBegin(ui->top);
    C2D_DrawCircleSolid(34, 74, 0.02f, 28, C2D_Color32(224, 240, 240, 255));
    C2D_DrawCircleSolid(364, 185, 0.02f, 41, C2D_Color32(224, 240, 240, 255));
    C2D_DrawRectSolid(0, 0, 0.1f, 400, 40, COLOR_HEADER);
    C2D_DrawRectSolid(0, 37, 0.2f, 400, 3, COLOR_ORANGE_2);
    draw_text(ui, "NAS eShop", 17, 7, 0.72f, COLOR_WHITE, 0);
    draw_text(ui, "Browse & install from your NAS", 145, 14, 0.34f,
              C2D_Color32(255, 235, 213, 255), 0);
    snprintf(text, sizeof(text), "%d games", game_count);
    draw_badge(ui, text, 313, 11, 73, COLOR_ACCENT);

    if (game_count <= 0) {
        draw_text(ui, "正在连接 NAS…", 112, 105, 0.62f, COLOR_MUTED, 0);
        return;
    }

    for (int slot = 0; slot < page_size && page_start + slot < game_count;
         ++slot) {
        int index = page_start + slot;
        const Game *game = &games[index];
        int column = slot % 2;
        int row = slot / 2;
        float x = 10.0f + column * 195.0f;
        float y = 48.0f + row * 55.0f;
        bool active = index == selected;
        u32 tile_color = (game->id % 3 == 0) ? COLOR_ACCENT :
                         (game->id % 3 == 1) ? COLOR_HEADER : COLOR_OK;

        draw_card(x, y, 185, 47, active);
        C2D_DrawRectSolid(x + 5, y + 5, 0.2f, 37, 37, tile_color);
        if (active && ui->cover_loaded && ui->cover_game_id == game->id) {
            draw_loaded_cover(ui, x + 5, y + 5, 37, 37, 0.25f);
        } else {
            snprintf(text, sizeof(text), "%03d", index + 1);
            draw_text(ui, text, x + 10, y + 14, 0.39f, COLOR_WHITE, 0);
        }
        clip_text_units(game->title, title_text, sizeof(title_text), 20);
        draw_text(ui, title_text, x + 49, y + 7, 0.37f,
                  active ? COLOR_HEADER : COLOR_TEXT, 128);
        snprintf(text, sizeof(text), "%.0f MB  %s", game->size_mb,
                 game->ext[0] ? game->ext + 1 : "CIA");
        draw_text(ui, text, x + 49, y + 28, 0.31f, COLOR_MUTED, 128);
    }

    snprintf(text, sizeof(text), "Page %d / %d",
             page_start / page_size + 1,
             (game_count + page_size - 1) / page_size);
    draw_text(ui, "← → Select    ↑ ↓ Row    L/R Page", 13, 218, 0.34f,
              COLOR_MUTED, 0);
    draw_text(ui, text, 321, 218, 0.32f, COLOR_HEADER, 0);
}

static void render_bottom(Ui *ui, const Game *games, int game_count, int selected,
                          const char *status, const HttpDebugInfo *http_debug,
                          const GameParseInfo *parse_info, bool show_debug)
{
    char text[384];
    char title_text[192];
    const Game *game = game_count > 0 && selected >= 0 ? &games[selected] : NULL;
    u32 status_color = game_count > 0 ? COLOR_OK : COLOR_BAD;

    C2D_TargetClear(ui->bottom, COLOR_BG);
    C2D_SceneBegin(ui->bottom);
    C2D_DrawRectSolid(0, 0, 0.1f, 320, 35, COLOR_HEADER);
    C2D_DrawRectSolid(0, 32, 0.2f, 320, 3, COLOR_ORANGE_2);
    draw_text(ui, "商品详情 / DETAILS", 14, 6, 0.54f, COLOR_WHITE, 0);
    C2D_DrawCircleSolid(297, 16, 0.3f, 6, status_color);

    if (game) {
        draw_card(10, 43, 300, 101, false);
        C2D_DrawRectSolid(16, 49, 0.2f, 66, 83,
                          game->has_cover ? COLOR_ACCENT : COLOR_PANEL_2);
        if (ui->cover_loaded && ui->cover_game_id == game->id) {
            draw_loaded_cover(ui, 16, 49, 66, 83, 0.25f);
        } else {
            draw_text(ui, game->has_cover ? "LOAD" : "NO\nCOVER",
                      27, 81, 0.40f,
                      game->has_cover ? COLOR_WHITE : COLOR_MUTED, 48);
        }
        clip_text_units(game->title, title_text, sizeof(title_text), 31);
        draw_text(ui, title_text, 93, 51, 0.46f, COLOR_HEADER, 207);
        snprintf(text, sizeof(text), "%.1f MB   %s", game->size_mb,
                 game->region[0] ? game->region : "CIA");
        draw_text(ui, text, 93, 96, 0.38f, COLOR_MUTED, 207);
        snprintf(text, sizeof(text), "ID %d  %s", game->id,
                 game->title_id[0] ? game->title_id : "no title id");
        draw_text(ui, text, 93, 117, 0.32f, COLOR_MUTED, 207);

        C2D_DrawRectSolid(10, 150, 0.1f, 300, 32, COLOR_PANEL_2);
        snprintf(text, sizeof(text), "http://%s:%u%s", NAS_HOST,
                 (unsigned)NAS_PORT, game->download_url);
        draw_text(ui, text, 16, 156, 0.30f, COLOR_ACCENT, 288);
    }

    C2D_DrawRectSolid(0, 190, 0.1f, 320, 50, COLOR_PANEL);
    C2D_DrawRectSolid(7, 197, 0.2f, 96, 34, COLOR_ACCENT);
    draw_text(ui, "A 极速直装", 18, 204, 0.39f, COLOR_WHITE, 76);
    C2D_DrawRectSolid(109, 197, 0.2f, 117, 34, COLOR_OK);
    draw_text(ui, "Y 下载后安装", 119, 204, 0.37f, COLOR_WHITE, 97);
    C2D_DrawRectSolid(232, 197, 0.2f, 78, 34, COLOR_PANEL_2);
    draw_text(ui, "X 刷新", 247, 204, 0.37f, COLOR_TEXT, 54);

    if (show_debug) {
        C2D_DrawRectSolid(7, 38, 0.30f, 306, 148,
                          C2D_Color32(35, 45, 49, 246));
        snprintf(text, sizeof(text), "DEBUG / 真机诊断  %s", CLIENT_BUILD);
        draw_text(ui, text, 16, 47, 0.50f, COLOR_GOLD, 288);
        snprintf(text, sizeof(text), "Status: %s", status ? status : "-");
        draw_text(ui, text, 16, 72, 0.36f, COLOR_TEXT, 288);
        if (http_debug) {
            snprintf(text, sizeof(text),
                     "HTTP %lu | bytes %lu / %lu\nbuffer %lu | result %08lX\n%s",
                     (unsigned long)http_debug->status_code,
                     (unsigned long)http_debug->downloaded_size,
                     (unsigned long)http_debug->declared_size,
                     (unsigned long)http_debug->capacity,
                     (unsigned long)http_debug->result,
                     http_debug->detail);
            draw_text(ui, text, 16, 94, 0.34f, COLOR_MUTED, 288);
        }
        if (parse_info) {
            snprintf(text, sizeof(text),
                     "JSON %d / %d | unicode %d\noffset %lu  %s",
                     parse_info->parsed_count, parse_info->expected_count,
                     parse_info->unicode_escape_count,
                     (unsigned long)parse_info->error_offset,
                     parse_info->error[0] ? parse_info->error : "parse complete");
            draw_text(ui, text, 16, 144, 0.34f,
                      parse_info->error[0] ? COLOR_BAD : COLOR_OK, 288);
        }
    }
}

static void render_install_overlay(Ui *ui, const Game *games, int game_count,
                                   int selected, const InstallProgress *install)
{
    char text[256];
    char title_text[192];
    char message_text[192];
    char speed_text[64];
    char eta_text[64];
    const Game *game =
        game_count > 0 && selected >= 0 ? &games[selected] : NULL;
    float ratio = install->total > 0
        ? (float)((double)install->downloaded / (double)install->total) : 0.0f;

    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;

    C2D_DrawRectSolid(8, 41, 0.30f, 306, 194, COLOR_SHADOW);
    C2D_DrawRectSolid(6, 38, 0.31f, 306, 194, COLOR_PANEL);
    C2D_DrawRectSolid(6, 38, 0.32f, 306, 32, COLOR_HEADER);

    if (install->stage == INSTALL_STAGE_CONFIRM) {
        draw_text(ui,
                  install->mode == INSTALL_MODE_STAGED
                      ? "下载到 SD 卡后安装"
                      : "确认极速直装",
                  17, 45, 0.38f, COLOR_WHITE, 0);
        if (game) {
            clip_text_units(game->title, title_text, sizeof(title_text), 38);
            draw_text(ui, title_text, 18, 82, 0.44f, COLOR_TEXT, 280);
            snprintf(text, sizeof(text), "文件大小：%.1f MB", game->size_mb);
            draw_text(ui, text, 18, 124, 0.36f, COLOR_MUTED, 0);
            draw_text(ui,
                      install->mode == INSTALL_MODE_STAGED
                          ? "成功后自动删除缓存；失败时保留。"
                          : "安装期间请勿关机或拔出 SD 卡。",
                      18, 148, 0.34f, COLOR_BAD, 0);
        }
        C2D_DrawRectSolid(18, 190, 0.40f, 125, 31, COLOR_PANEL_2);
        draw_text(ui, "B 取消", 57, 197, 0.39f, COLOR_MUTED, 0);
        C2D_DrawRectSolid(163, 190, 0.40f, 130, 31, COLOR_ACCENT);
        draw_text(ui, "A 开始安装", 190, 197, 0.39f, COLOR_WHITE, 0);
        return;
    }

    if (install->stage == INSTALL_STAGE_SUCCESS) {
        draw_text(ui, "安装完成", 17, 45, 0.38f, COLOR_WHITE, 0);
    } else if (install->stage == INSTALL_STAGE_ERROR) {
        draw_text(ui, "安装失败", 17, 45, 0.38f, COLOR_WHITE, 0);
    } else if (install->stage == INSTALL_STAGE_CANCELLED) {
        draw_text(ui, "安装已取消", 17, 45, 0.38f, COLOR_WHITE, 0);
    } else if (install->stage == INSTALL_STAGE_DOWNLOADING) {
        draw_text(ui, "正在下载到 SD 卡", 17, 45, 0.38f, COLOR_WHITE, 0);
    } else if (install->mode == INSTALL_MODE_STAGED &&
               install->stage == INSTALL_STAGE_INSTALLING) {
        draw_text(ui, "正在从 SD 卡安装", 17, 45, 0.38f, COLOR_WHITE, 0);
    } else {
        draw_text(ui, "正在下载并安装", 17, 45, 0.38f, COLOR_WHITE, 0);
    }

    /*
     * The CHN system font has much taller vertical metrics than the default
     * font. Keep the stage title and status in two fixed, single-line slots
     * so their glyph boxes cannot overlap on real hardware.
     */
    clip_text_units(install->message, message_text, sizeof(message_text), 42);
    draw_text(ui, message_text, 18, 82, 0.27f, COLOR_TEXT, 274);

    if (install->stage == INSTALL_STAGE_CONNECTING ||
        install->stage == INSTALL_STAGE_DOWNLOADING ||
        install->stage == INSTALL_STAGE_VALIDATING ||
        install->stage == INSTALL_STAGE_INSTALLING ||
        install->stage == INSTALL_STAGE_FINISHING) {
        C2D_DrawRectSolid(18, 107, 0.40f, 274, 17, COLOR_PANEL_2);
        C2D_DrawRectSolid(18, 107, 0.41f, 274.0f * ratio, 17, COLOR_OK);
        if (install->total > 0) {
            snprintf(text, sizeof(text),
                     "%d%%     %.1f / %.1f MB",
                     install->percent,
                     (double)install->downloaded / (1024.0 * 1024.0),
                     (double)install->total / (1024.0 * 1024.0));
        } else {
            snprintf(text, sizeof(text), "0%%   Preparing connection...");
        }
        draw_text(ui, text, 18, 130, 0.36f, COLOR_MUTED, 274);

        if (install->bytes_per_second >= 1024.0 * 1024.0) {
            snprintf(speed_text, sizeof(speed_text), "SPEED  %.2f MB/s",
                     install->bytes_per_second / (1024.0 * 1024.0));
        } else if (install->bytes_per_second > 0) {
            snprintf(speed_text, sizeof(speed_text), "SPEED  %.0f KB/s",
                     install->bytes_per_second / 1024.0);
        } else {
            snprintf(speed_text, sizeof(speed_text), "SPEED  -- KB/s");
        }
        if (install->bytes_per_second > 0 &&
            install->total > install->downloaded) {
            unsigned eta = (unsigned)((install->total - install->downloaded) /
                                      install->bytes_per_second);
            snprintf(eta_text, sizeof(eta_text), "ETA  %02u:%02u",
                     eta / 60, eta % 60);
        } else {
            snprintf(eta_text, sizeof(eta_text), "ETA  --:--");
        }
        draw_text(ui, speed_text, 18, 155, 0.46f, COLOR_ACCENT, 174);
        draw_text(ui, eta_text, 207, 158, 0.36f, COLOR_MUTED, 85);

        if (install->title_id != 0) {
            snprintf(text, sizeof(text), "Title ID %016llX%s",
                     (unsigned long long)install->title_id,
                     install->replacing ? "  [overwrite]" : "");
            draw_text(ui, text, 18, 182, 0.28f, COLOR_MUTED, 274);
        } else {
            draw_text(ui, "正在准备安全的 SD 卡安装…", 18, 182, 0.28f,
                      COLOR_MUTED, 0);
        }
        C2D_DrawRectSolid(198, 198, 0.40f, 94, 26, COLOR_PANEL_2);
        draw_text(ui, "B 取消", 225, 203, 0.34f, COLOR_BAD, 0);
    } else {
        if (install->title_id != 0) {
            snprintf(text, sizeof(text), "Title ID %016llX",
                     (unsigned long long)install->title_id);
            draw_text(ui, text, 18, 128, 0.34f, COLOR_MUTED, 0);
        }
        if (R_FAILED(install->service_result)) {
            snprintf(text, sizeof(text), "Result %08lX",
                     (unsigned long)install->service_result);
            draw_text(ui, text, 18, 156, 0.34f, COLOR_BAD, 0);
        }
        C2D_DrawRectSolid(178, 190, 0.40f, 114, 31, COLOR_ACCENT);
        draw_text(ui, "A / B 返回", 201, 197, 0.36f, COLOR_WHITE, 0);
    }
}

void ui_render(Ui *ui, const Game *games, int game_count, int selected,
               const char *status, const HttpDebugInfo *http_debug,
               const GameParseInfo *parse_info, bool show_debug,
               const InstallProgress *install)
{
    C2D_TextBufClear(ui->text_buf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    render_top(ui, games, game_count, selected);
    render_bottom(ui, games, game_count, selected, status, http_debug,
                  parse_info, show_debug);
    if (install) {
        render_install_overlay(ui, games, game_count, selected, install);
    }
    C3D_FrameEnd(0);
}
