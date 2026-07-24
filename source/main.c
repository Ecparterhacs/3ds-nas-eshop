#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "game.h"
#include "http_util.h"
#include "installer.h"
#include "ui.h"

static Game games[MAX_GAMES];
static int game_count;
static int selected;
static HttpDebugInfo http_debug;
static GameParseInfo parse_info;
static char status_text[128] = "Starting...";
static InstallProgress install_progress;
static bool install_confirm;
static bool install_notice;
static InstallMode pending_install_mode;
static int pending_cover_game_id = -1;
static u64 pending_cover_at;

typedef struct {
    Ui *ui;
    bool show_debug;
    InstallStage last_stage;
    char last_message[160];
    u64 last_render_at;
} InstallRenderContext;

static void debug_output(const char *message)
{
    if (!message) return;
    printf("%s\n", message);
    svcOutputDebugString(message, (s32)strlen(message));
    svcOutputDebugString("\n", 1);
}

static void load_selected_cover(Ui *ui)
{
    char route[96];
    char *bmp = NULL;
    size_t bmp_size = 0;
    HttpDebugInfo cover_debug;

    ui_clear_cover(ui);
    if (!ui || game_count <= 0 || selected < 0 ||
        selected >= game_count || !games[selected].has_cover) {
        return;
    }

    snprintf(route, sizeof(route), "/api/cover_bmp/%d?w=128",
             games[selected].id);
    if (http_get_alloc(route, &bmp, &bmp_size, &cover_debug) == 0) {
        if (!ui_set_cover_bmp(ui, games[selected].id, bmp, bmp_size)) {
            debug_output("Cover BMP decode failed");
        }
    }
    free(bmp);
}

static void schedule_selected_cover(Ui *ui)
{
    ui_clear_cover(ui);
    pending_cover_game_id = -1;
    pending_cover_at = 0;
    if (game_count > 0 && selected >= 0 && selected < game_count &&
        games[selected].has_cover) {
        pending_cover_game_id = games[selected].id;
        pending_cover_at = osGetTime() + 350;
    }
}

static int reload_games(void)
{
    char *json = NULL;
    size_t json_size = 0;
    int result;

    snprintf(status_text, sizeof(status_text), "HTTP: requesting /api/games");
    debug_output(status_text);
    game_count = 0;
    selected = 0;
    memset(&parse_info, 0, sizeof(parse_info));

    result = http_get_alloc("/api/games", &json, &json_size, &http_debug);
    if (result != 0) {
        snprintf(status_text, sizeof(status_text), "HTTP failed rc=%d %s",
                 result, http_debug.detail);
        debug_output(status_text);
        return result;
    }

    snprintf(status_text, sizeof(status_text), "HTTP: %lu bytes, parsing JSON",
             (unsigned long)json_size);
    debug_output(status_text);

    result = games_parse_json(json, json_size, games, MAX_GAMES, &parse_info);
    free(json);
    if (result != 0) {
        snprintf(status_text, sizeof(status_text),
                 "JSON failed rc=%d at %lu: %s", result,
                 (unsigned long)parse_info.error_offset, parse_info.error);
        debug_output(status_text);
        game_count = parse_info.parsed_count;
        return result;
    }

    game_count = parse_info.parsed_count;
    snprintf(status_text, sizeof(status_text),
             "OK %d/%d | %lu bytes | unicode %d",
             parse_info.parsed_count, parse_info.expected_count,
             (unsigned long)http_debug.downloaded_size,
             parse_info.unicode_escape_count);
    debug_output(status_text);
    return 0;
}

static bool render_install_progress(void *user_data,
                                    const InstallProgress *progress)
{
    InstallRenderContext *context = user_data;
    u32 down;
    u64 now;
    bool message_changed = false;
    bool terminal;
    bool should_render;
    bool can_cancel =
        progress->stage == INSTALL_STAGE_CONNECTING ||
        progress->stage == INSTALL_STAGE_DOWNLOADING ||
        progress->stage == INSTALL_STAGE_VALIDATING ||
        progress->stage == INSTALL_STAGE_INSTALLING;

    hidScanInput();
    down = hidKeysDown();
    now = osGetTime();
    if (context->last_stage != progress->stage ||
        strcmp(context->last_message, progress->message) != 0) {
        message_changed = true;
        snprintf(context->last_message, sizeof(context->last_message), "%s",
                 progress->message);
        context->last_stage = progress->stage;
        debug_output(progress->message);
    }
    terminal = progress->stage == INSTALL_STAGE_SUCCESS ||
               progress->stage == INSTALL_STAGE_ERROR ||
               progress->stage == INSTALL_STAGE_CANCELLED;
    should_render = message_changed || terminal ||
                    now - context->last_render_at >= 120;
    if (should_render) {
        ui_render(context->ui, games, game_count, selected, status_text,
                  &http_debug, &parse_info, context->show_debug, progress);
        context->last_render_at = now;
        if (message_changed) gspWaitForVBlank();
    }
    if (!aptMainLoop()) return false;
    return !(can_cancel && (down & KEY_B));
}

int main(void)
{
    Ui ui;
    bool show_debug = false;
    int http_init_result;

    if (!ui_init(&ui)) return 1;

    http_init_result = (int)httpcInit(0x100000);
    if (R_FAILED((Result)http_init_result)) {
        snprintf(status_text, sizeof(status_text), "httpcInit failed: %08X",
                 (unsigned)http_init_result);
    } else {
        ui_render(&ui, games, 0, 0, "Connecting to NAS...", &http_debug,
                  &parse_info, false, NULL);
        reload_games();
        load_selected_cover(&ui);
    }

    while (aptMainLoop()) {
        u32 down;
        int selected_before = selected;
        hidScanInput();
        down = hidKeysDown();

        if (install_notice) {
            if (down & (KEY_A | KEY_B)) install_notice = false;
        } else if (install_confirm) {
            if (down & KEY_B) {
                install_confirm = false;
            } else if (down & KEY_A) {
                InstallRenderContext context;
                int install_result;
                memset(&context, 0, sizeof(context));
                context.ui = &ui;
                context.show_debug = false;
                context.last_stage = (InstallStage)-1;
                install_confirm = false;
                show_debug = false;
                if (pending_install_mode == INSTALL_MODE_STAGED) {
                    install_result = installer_download_then_install_game(
                        &games[selected], &install_progress,
                        render_install_progress, &context);
                } else {
                    install_result = installer_install_game(
                        &games[selected], &install_progress,
                        render_install_progress, &context);
                }
                if (install_result == 0) {
                    snprintf(status_text, sizeof(status_text),
                             "Installed game %d successfully",
                             games[selected].id);
                } else {
                    snprintf(status_text, sizeof(status_text),
                             "Install failed rc=%d result=%08lX",
                             install_result,
                             (unsigned long)install_progress.service_result);
                }
                debug_output(status_text);
                install_notice = true;
            }
        } else {
            if (down & (KEY_START | KEY_B)) break;
            if (down & KEY_SELECT) show_debug = !show_debug;
            if (down & KEY_X) {
                reload_games();
                schedule_selected_cover(&ui);
            }

            if (game_count > 0) {
                if ((down & KEY_DLEFT) && selected > 0) --selected;
                if ((down & KEY_DRIGHT) && selected + 1 < game_count) ++selected;
                if ((down & KEY_DUP) && selected >= 2) selected -= 2;
                if ((down & KEY_DDOWN) && selected + 2 < game_count) selected += 2;
                if (down & KEY_L) {
                    selected -= 6;
                    if (selected < 0) selected = 0;
                }
                if (down & KEY_R) {
                    selected += 6;
                    if (selected >= game_count) selected = game_count - 1;
                }
                if (down & KEY_A) {
                    memset(&install_progress, 0, sizeof(install_progress));
                    install_progress.stage = INSTALL_STAGE_CONFIRM;
                    install_progress.mode = INSTALL_MODE_DIRECT;
                    pending_install_mode = INSTALL_MODE_DIRECT;
                    install_progress.total =
                        (u64)(games[selected].size_mb * 1024.0f * 1024.0f);
                    snprintf(install_progress.message,
                             sizeof(install_progress.message),
                             "Install %s", games[selected].title);
                    install_confirm = true;
                    show_debug = false;
                }
                if (down & KEY_Y) {
                    memset(&install_progress, 0, sizeof(install_progress));
                    install_progress.stage = INSTALL_STAGE_CONFIRM;
                    install_progress.mode = INSTALL_MODE_STAGED;
                    pending_install_mode = INSTALL_MODE_STAGED;
                    install_progress.total =
                        (u64)(games[selected].size_mb * 1024.0f * 1024.0f);
                    snprintf(install_progress.message,
                             sizeof(install_progress.message),
                             "Download then install %s",
                             games[selected].title);
                    install_confirm = true;
                    show_debug = false;
                }
            }
        }

        if (!install_confirm && !install_notice &&
            selected != selected_before) {
            schedule_selected_cover(&ui);
        }
        if (!install_confirm && !install_notice &&
            pending_cover_game_id >= 0 &&
            pending_cover_game_id == games[selected].id &&
            osGetTime() >= pending_cover_at) {
            pending_cover_game_id = -1;
            pending_cover_at = 0;
            load_selected_cover(&ui);
        }

        ui_render(&ui, games, game_count, selected, status_text, &http_debug,
                  &parse_info, show_debug,
                  (install_confirm || install_notice) ? &install_progress : NULL);
    }

    if (R_SUCCEEDED((Result)http_init_result)) httpcExit();
    ui_exit(&ui);
    return 0;
}
