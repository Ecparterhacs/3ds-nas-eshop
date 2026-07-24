#pragma once

#include <3ds.h>
#include <stdbool.h>

#include "game.h"

typedef enum {
    INSTALL_STAGE_CONFIRM,
    INSTALL_STAGE_CONNECTING,
    INSTALL_STAGE_DOWNLOADING,
    INSTALL_STAGE_VALIDATING,
    INSTALL_STAGE_INSTALLING,
    INSTALL_STAGE_FINISHING,
    INSTALL_STAGE_SUCCESS,
    INSTALL_STAGE_ERROR,
    INSTALL_STAGE_CANCELLED
} InstallStage;

typedef enum {
    INSTALL_MODE_DIRECT,
    INSTALL_MODE_STAGED
} InstallMode;

typedef struct {
    InstallStage stage;
    InstallMode mode;
    u64 downloaded;
    u64 total;
    u64 free_bytes;
    u64 title_id;
    double bytes_per_second;
    int percent;
    bool replacing;
    Result service_result;
    char message[160];
} InstallProgress;

/* Return false from the callback to cancel the operation. */
typedef bool (*InstallProgressCallback)(void *user_data,
                                        const InstallProgress *progress);

/*
 * Streams one CIA from the configured NAS directly into AM's SD installer.
 * Returns 0 on success, -12 on user cancellation, or another negative code.
 */
int installer_install_game(const Game *game, InstallProgress *progress,
                           InstallProgressCallback callback, void *user_data);

/*
 * Downloads into SD:/3ds/nas-eshop/cache with resume support, installs from
 * that file, and deletes only the completed cache file after AM succeeds.
 */
int installer_download_then_install_game(
    const Game *game, InstallProgress *progress,
    InstallProgressCallback callback, void *user_data);
