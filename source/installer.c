#include "installer.h"

#include "cia_util.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INSTALL_CHUNK_SIZE (256u * 1024u)
#define INSTALL_SPACE_MARGIN (32ull * 1024ull * 1024ull)
#define DOWNLOAD_PIPE_SLOTS 2
#define DOWNLOAD_THREAD_STACK (32u * 1024u)

typedef struct {
    httpcContext *context;
    unsigned char *buffers[DOWNLOAD_PIPE_SLOTS];
    u32 sizes[DOWNLOAD_PIPE_SLOTS];
    Result results[DOWNLOAD_PIPE_SLOTS];
    LightSemaphore empty_slots;
    LightSemaphore full_slots;
    volatile bool stop;
} DownloadPipe;

static void download_pipe_main(void *argument)
{
    DownloadPipe *pipe = argument;
    int index = 0;

    while (!pipe->stop) {
        u32 received = 0;
        Result result;

        LightSemaphore_Acquire(&pipe->empty_slots, 1);
        if (pipe->stop) break;

        result = httpcDownloadData(pipe->context, pipe->buffers[index],
                                   INSTALL_CHUNK_SIZE, &received);
        pipe->sizes[index] = received;
        pipe->results[index] = result;
        LightSemaphore_Release(&pipe->full_slots, 1);

        if (result != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) break;
        index = (index + 1) % DOWNLOAD_PIPE_SLOTS;
    }
}

static void stop_download_pipe(DownloadPipe *pipe, Thread thread,
                               httpcContext *context, bool cancel_http_read)
{
    if (!thread) return;
    pipe->stop = true;
    if (cancel_http_read) httpcCancelConnection(context);
    LightSemaphore_Release(&pipe->empty_slots, 1);
    threadJoin(thread, U64_MAX);
    threadFree(thread);
}

static void set_message(InstallProgress *progress, InstallStage stage,
                        const char *message, Result result)
{
    progress->stage = stage;
    progress->service_result = result;
    snprintf(progress->message, sizeof(progress->message), "%s", message);
}

static bool publish(InstallProgress *progress,
                    InstallProgressCallback callback, void *user_data)
{
    if (!callback) return true;
    return callback(user_data, progress);
}

static void update_transfer_stats(InstallProgress *progress, u64 started_at)
{
    u64 elapsed_ms = osGetTime() - started_at;
    if (progress->total > 0) {
        progress->percent = (int)((progress->downloaded * 100u) /
                                  progress->total);
        if (progress->percent > 100) progress->percent = 100;
    }
    if (elapsed_ms > 0) {
        progress->bytes_per_second =
            (double)progress->downloaded * 1000.0 / (double)elapsed_ms;
    }
}

static void update_transfer_stats_from(InstallProgress *progress,
                                       u64 started_at, u64 starting_bytes)
{
    u64 elapsed_ms = osGetTime() - started_at;
    if (progress->total > 0) {
        progress->percent = (int)((progress->downloaded * 100u) /
                                  progress->total);
        if (progress->percent > 100) progress->percent = 100;
    }
    if (elapsed_ms > 0 && progress->downloaded >= starting_bytes) {
        progress->bytes_per_second =
            (double)(progress->downloaded - starting_bytes) * 1000.0 /
            (double)elapsed_ms;
    }
}

static void cancel_http(httpcContext *context)
{
    httpcCancelConnection(context);
    httpcCloseContext(context);
}

static int fail_install(InstallProgress *progress, InstallStage stage,
                        const char *message, Result result, int code,
                        InstallProgressCallback callback, void *user_data)
{
    set_message(progress, stage, message, result);
    publish(progress, callback, user_data);
    return code;
}

static bool get_sd_free_bytes(u64 *free_bytes)
{
    FS_ArchiveResource resource;
    memset(&resource, 0, sizeof(resource));
    if (R_FAILED(FSUSER_GetArchiveResource(&resource, SYSTEM_MEDIATYPE_SD))) {
        return false;
    }
    *free_bytes = (u64)resource.freeClusters * (u64)resource.clusterSize;
    return true;
}

int installer_install_game(const Game *game, InstallProgress *progress,
                           InstallProgressCallback callback, void *user_data)
{
    httpcContext context;
    char url[512];
    unsigned char *buffer = NULL;
    DownloadPipe pipe;
    Thread download_thread = NULL;
    Handle cia_handle = 0;
    bool context_open = false;
    bool am_open = false;
    bool cia_open = false;
    bool http_finished = false;
    u32 status = 0;
    u32 current = 0;
    u32 total = 0;
    u32 received = 0;
    u32 written = 0;
    u64 title_id = 0;
    u64 started_at = 0;
    AM_TitleInfo title_info;
    Result result = 0;
    int return_code = -1;
    int consume_index = 0;

    memset(&pipe, 0, sizeof(pipe));

    if (!game || !progress || game->download_url[0] == '\0') return -1;
    memset(progress, 0, sizeof(*progress));
    progress->mode = INSTALL_MODE_DIRECT;
    set_message(progress, INSTALL_STAGE_CONNECTING,
                "Connecting to NAS...", 0);
    if (!publish(progress, callback, user_data)) {
        return fail_install(progress, INSTALL_STAGE_CANCELLED,
                            "Installation cancelled.", 0, -12,
                            callback, user_data);
    }

    if (strcmp(game->ext, ".cia") != 0) {
        return fail_install(progress, INSTALL_STAGE_ERROR,
                            "Selected file is not a CIA.", 0, -2,
                            callback, user_data);
    }
    snprintf(url, sizeof(url), "http://%s:%u%s", NAS_HOST,
             (unsigned)NAS_PORT, game->download_url);
    set_message(progress, INSTALL_STAGE_CONNECTING,
                "Step 1/5: Opening download URL...", 0);
    publish(progress, callback, user_data);
    result = httpcOpenContext(&context, HTTPC_METHOD_GET, url, 0);
    if (R_FAILED(result)) {
        return fail_install(progress, INSTALL_STAGE_ERROR,
                            "Could not open the download URL.", result, -4,
                            callback, user_data);
    }
    context_open = true;
    httpcAddRequestHeaderField(&context, "User-Agent", "3DS-eShop/2.1");
    httpcAddRequestHeaderField(&context, "Connection", "close");

    set_message(progress, INSTALL_STAGE_CONNECTING,
                "Step 2/5: Sending request to NAS...", 0);
    publish(progress, callback, user_data);
    result = httpcBeginRequest(&context);
    if (R_FAILED(result)) {
        return_code = fail_install(progress, INSTALL_STAGE_ERROR,
                                   "NAS request failed.", result, -5,
                                   callback, user_data);
        goto cleanup;
    }
    set_message(progress, INSTALL_STAGE_CONNECTING,
                "Step 3/5: Reading NAS response...", 0);
    publish(progress, callback, user_data);
    result = httpcGetResponseStatusCode(&context, &status);
    if (R_FAILED(result) || status < 200 || status >= 300) {
        return_code = fail_install(progress, INSTALL_STAGE_ERROR,
                                   "NAS returned a non-2xx status.", result, -6,
                                   callback, user_data);
        goto cleanup;
    }
    result = httpcGetDownloadSizeState(&context, &current, &total);
    if (R_FAILED(result) || total == 0) {
        return_code = fail_install(progress, INSTALL_STAGE_ERROR,
                                   "NAS did not provide a file size.", result, -7,
                                   callback, user_data);
        goto cleanup;
    }
    progress->total = total;
    if (get_sd_free_bytes(&progress->free_bytes) &&
        progress->free_bytes < (u64)total + INSTALL_SPACE_MARGIN) {
        return_code = fail_install(progress, INSTALL_STAGE_ERROR,
                                   "Not enough free space on the SD card.", 0, -8,
                                   callback, user_data);
        goto cleanup;
    }

    buffer = malloc(INSTALL_CHUNK_SIZE);
    if (!buffer) {
        return_code = fail_install(progress, INSTALL_STAGE_ERROR,
                                   "Not enough memory for download buffer.", 0, -9,
                                   callback, user_data);
        goto cleanup;
    }

    set_message(progress, INSTALL_STAGE_VALIDATING,
                "Step 4/5: Reading and validating CIA...", 0);
    publish(progress, callback, user_data);
    started_at = osGetTime();
    result = httpcDownloadData(&context, buffer, INSTALL_CHUNK_SIZE, &received);
    if (R_FAILED(result) &&
        result != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) {
        return_code = fail_install(progress, INSTALL_STAGE_ERROR,
                                   "Failed to read CIA header.", result, -10,
                                   callback, user_data);
        goto cleanup;
    }
    http_finished = R_SUCCEEDED(result);
    if (!cia_extract_title_id(buffer, received, &title_id) ||
        !cia_title_is_safe_sd(title_id)) {
        return_code = fail_install(progress, INSTALL_STAGE_ERROR,
                                   "CIA is invalid or is not safe SD content.",
                                   0, -11, callback, user_data);
        goto cleanup;
    }
    progress->title_id = title_id;

    if (cia_title_requires_new3ds(title_id)) {
        bool is_new3ds = false;
        if (R_SUCCEEDED(APT_CheckNew3DS(&is_new3ds)) && !is_new3ds) {
            return_code = fail_install(progress, INSTALL_STAGE_ERROR,
                                       "This title requires a New 3DS.", 0, -13,
                                       callback, user_data);
            goto cleanup;
        }
    }

    set_message(progress, INSTALL_STAGE_VALIDATING,
                "Step 5/5: Opening the SD installer...", 0);
    publish(progress, callback, user_data);
    result = amInit();
    if (R_FAILED(result)) {
        return_code = fail_install(progress, INSTALL_STAGE_ERROR,
                                   "AM service is unavailable. Check Luma.",
                                   result, -14, callback, user_data);
        goto cleanup;
    }
    am_open = true;

    memset(&title_info, 0, sizeof(title_info));
    progress->replacing =
        R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &title_id, &title_info));
    if (progress->replacing) {
        result = AM_StartCiaInstallOverwrite(&cia_handle, MEDIATYPE_SD);
    } else {
        result = AM_StartCiaInstall(MEDIATYPE_SD, &cia_handle);
    }
    if (R_FAILED(result)) {
        return_code = fail_install(progress, INSTALL_STAGE_ERROR,
                                   "Could not start SD installation.", result,
                                   -15, callback, user_data);
        goto cleanup;
    }
    cia_open = true;

    set_message(progress, INSTALL_STAGE_INSTALLING,
                progress->replacing ? "Installing (replacing existing title)..."
                                    : "Downloading and installing...", 0);

    if (!http_finished) {
        for (int index = 0; index < DOWNLOAD_PIPE_SLOTS; ++index) {
            pipe.buffers[index] = malloc(INSTALL_CHUNK_SIZE);
            if (!pipe.buffers[index]) {
                return_code = fail_install(
                    progress, INSTALL_STAGE_ERROR,
                    "Not enough memory for download pipeline.", 0, -20,
                    callback, user_data);
                goto cleanup;
            }
        }
        pipe.context = &context;
        LightSemaphore_Init(&pipe.empty_slots, DOWNLOAD_PIPE_SLOTS,
                            DOWNLOAD_PIPE_SLOTS);
        LightSemaphore_Init(&pipe.full_slots, 0, DOWNLOAD_PIPE_SLOTS);
        download_thread = threadCreate(
            download_pipe_main, &pipe, DOWNLOAD_THREAD_STACK, 0x30, -2, false);
        if (!download_thread) {
            return_code = fail_install(
                progress, INSTALL_STAGE_ERROR,
                "Could not start the download pipeline.", 0, -21,
                callback, user_data);
            goto cleanup;
        }
    }

    if (received > 0) {
        written = 0;
        result = FSFILE_Write(cia_handle, &written, progress->downloaded,
                              buffer, received, 0);
        if (R_FAILED(result) || written != received) {
            return_code = fail_install(progress, INSTALL_STAGE_ERROR,
                                       "Failed to write CIA into AM.",
                                       result, -16, callback, user_data);
            goto cleanup;
        }
        progress->downloaded += written;
        update_transfer_stats(progress, started_at);
        if (!publish(progress, callback, user_data)) {
            return_code = fail_install(progress, INSTALL_STAGE_CANCELLED,
                                       "Installation cancelled.", 0, -12,
                                       callback, user_data);
            goto cleanup;
        }
    }

    while (!http_finished) {
        while (LightSemaphore_TryAcquire(&pipe.full_slots, 1) != 0) {
            if (!publish(progress, callback, user_data)) {
                return_code = fail_install(
                    progress, INSTALL_STAGE_CANCELLED,
                    "Installation cancelled.", 0, -12, callback, user_data);
                goto cleanup;
            }
            svcSleepThread(10 * 1000 * 1000);
        }

        received = pipe.sizes[consume_index];
        result = pipe.results[consume_index];
        if (received > 0) {
            written = 0;
            Result write_result =
                FSFILE_Write(cia_handle, &written, progress->downloaded,
                             pipe.buffers[consume_index], received, 0);
            if (R_FAILED(write_result) || written != received) {
                return_code = fail_install(progress, INSTALL_STAGE_ERROR,
                                           "Failed to write CIA into AM.",
                                           write_result, -16,
                                           callback, user_data);
                goto cleanup;
            }
            progress->downloaded += written;
            update_transfer_stats(progress, started_at);
        }
        LightSemaphore_Release(&pipe.empty_slots, 1);
        consume_index = (consume_index + 1) % DOWNLOAD_PIPE_SLOTS;

        if (result == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) {
            http_finished = false;
        } else if (R_FAILED(result)) {
            return_code = fail_install(progress, INSTALL_STAGE_ERROR,
                                       "Download interrupted.", result, -17,
                                       callback, user_data);
            goto cleanup;
        } else {
            http_finished = true;
        }

        if (!publish(progress, callback, user_data)) {
            return_code = fail_install(progress, INSTALL_STAGE_CANCELLED,
                                       "Installation cancelled.", 0, -12,
                                       callback, user_data);
            goto cleanup;
        }
    }

    stop_download_pipe(&pipe, download_thread, &context, false);
    download_thread = NULL;

    if (progress->downloaded != progress->total) {
        return_code = fail_install(progress, INSTALL_STAGE_ERROR,
                                   "Downloaded size does not match NAS size.",
                                   0, -18, callback, user_data);
        goto cleanup;
    }

    httpcCloseContext(&context);
    context_open = false;
    set_message(progress, INSTALL_STAGE_FINISHING,
                "Finalizing title installation...", 0);
    publish(progress, callback, user_data);

    result = AM_FinishCiaInstall(cia_handle);
    if (R_FAILED(result)) {
        return_code = fail_install(progress, INSTALL_STAGE_ERROR,
                                   "AM could not finalize this title.", result,
                                   -19, callback, user_data);
        goto cleanup;
    }
    cia_open = false;

    progress->percent = 100;
    set_message(progress, INSTALL_STAGE_SUCCESS,
                "Installed successfully. Return HOME to unwrap.", 0);
    publish(progress, callback, user_data);
    return_code = 0;

cleanup:
    if (download_thread) {
        stop_download_pipe(&pipe, download_thread, &context, true);
        download_thread = NULL;
    }
    if (context_open) cancel_http(&context);
    if (cia_open) AM_CancelCIAInstall(cia_handle);
    if (am_open) amExit();
    free(buffer);
    for (int index = 0; index < DOWNLOAD_PIPE_SLOTS; ++index) {
        free(pipe.buffers[index]);
    }
    return return_code;
}

int installer_download_then_install_game(
    const Game *game, InstallProgress *progress,
    InstallProgressCallback callback, void *user_data)
{
    static const char *cache_dir = "/3ds/nas-eshop/cache";
    httpcContext context;
    FS_Archive sd_archive = 0;
    Handle cache_file = 0;
    Handle cia_handle = 0;
    char part_path[128];
    char final_path[128];
    char url[512];
    char range_header[80];
    unsigned char *buffer = NULL;
    bool archive_open = false;
    bool file_open = false;
    bool context_open = false;
    bool am_open = false;
    bool cia_open = false;
    bool final_ready = false;
    u32 status = 0;
    u32 current = 0;
    u32 response_size = 0;
    u32 received = 0;
    u32 written = 0;
    u32 bytes_read = 0;
    u64 cached_size = 0;
    u64 total_size = 0;
    u64 title_id = 0;
    u64 started_at = 0;
    Result result = 0;
    int return_code = -1;
    AM_TitleInfo title_info;

    if (!game || !progress || game->download_url[0] == '\0') return -1;
    memset(progress, 0, sizeof(*progress));
    progress->mode = INSTALL_MODE_STAGED;
    if (strcmp(game->ext, ".cia") != 0) {
        return fail_install(progress, INSTALL_STAGE_ERROR,
                            "Selected file is not a CIA.", 0, -2,
                            callback, user_data);
    }

    snprintf(part_path, sizeof(part_path),
             "%s/game_%d.cia.part", cache_dir, game->id);
    snprintf(final_path, sizeof(final_path),
             "%s/game_%d.cia", cache_dir, game->id);
    snprintf(url, sizeof(url), "http://%s:%u%s", NAS_HOST,
             (unsigned)NAS_PORT, game->download_url);

    set_message(progress, INSTALL_STAGE_CONNECTING,
                "Preparing the SD cache...", 0);
    publish(progress, callback, user_data);

    result = FSUSER_OpenArchive(
        &sd_archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if (R_FAILED(result)) {
        return fail_install(progress, INSTALL_STAGE_ERROR,
                            "Could not open the SD card.", result, -30,
                            callback, user_data);
    }
    archive_open = true;
    FSUSER_CreateDirectory(
        sd_archive, fsMakePath(PATH_ASCII, "/3ds/nas-eshop"), 0);
    FSUSER_CreateDirectory(
        sd_archive, fsMakePath(PATH_ASCII, cache_dir), 0);

    result = FSUSER_OpenFile(
        &cache_file, sd_archive, fsMakePath(PATH_ASCII, final_path),
        FS_OPEN_READ, 0);
    if (R_SUCCEEDED(result)) {
        file_open = true;
        if (R_SUCCEEDED(FSFILE_GetSize(cache_file, &total_size)) &&
            total_size > 0) {
            final_ready = true;
        } else {
            FSFILE_Close(cache_file);
            file_open = false;
            FSUSER_DeleteFile(
                sd_archive, fsMakePath(PATH_ASCII, final_path));
        }
    }

    if (!final_ready) {
        result = FSUSER_OpenFile(
            &cache_file, sd_archive, fsMakePath(PATH_ASCII, part_path),
            FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
        if (R_FAILED(result)) {
            return_code = fail_install(
                progress, INSTALL_STAGE_ERROR,
                "Could not create the SD cache file.", result, -31,
                callback, user_data);
            goto cleanup_staged;
        }
        file_open = true;
        if (R_FAILED(FSFILE_GetSize(cache_file, &cached_size))) {
            cached_size = 0;
        }

        result = httpcOpenContext(&context, HTTPC_METHOD_GET, url, 0);
        if (R_FAILED(result)) {
            return_code = fail_install(
                progress, INSTALL_STAGE_ERROR,
                "Could not open the download URL.", result, -4,
                callback, user_data);
            goto cleanup_staged;
        }
        context_open = true;
        httpcAddRequestHeaderField(
            &context, "User-Agent", "3DS-eShop/3.0");
        httpcAddRequestHeaderField(&context, "Connection", "close");
        if (cached_size > 0) {
            snprintf(range_header, sizeof(range_header), "bytes=%llu-",
                     (unsigned long long)cached_size);
            httpcAddRequestHeaderField(&context, "Range", range_header);
        }

        set_message(progress, INSTALL_STAGE_CONNECTING,
                    cached_size > 0 ? "Resuming cached download..."
                                    : "Connecting to NAS...", 0);
        publish(progress, callback, user_data);
        result = httpcBeginRequest(&context);
        if (R_FAILED(result)) {
            return_code = fail_install(
                progress, INSTALL_STAGE_ERROR,
                "NAS request failed.", result, -5,
                callback, user_data);
            goto cleanup_staged;
        }
        result = httpcGetResponseStatusCode(&context, &status);
        if (R_FAILED(result) ||
            !((cached_size > 0 && status == 206) ||
              status == 200)) {
            return_code = fail_install(
                progress, INSTALL_STAGE_ERROR,
                "NAS rejected the cached download.", result, -32,
                callback, user_data);
            goto cleanup_staged;
        }
        if (cached_size > 0 && status == 200) {
            result = FSFILE_SetSize(cache_file, 0);
            if (R_FAILED(result)) {
                return_code = fail_install(
                    progress, INSTALL_STAGE_ERROR,
                    "Could not reset the partial cache.", result, -33,
                    callback, user_data);
                goto cleanup_staged;
            }
            cached_size = 0;
        }
        result = httpcGetDownloadSizeState(
            &context, &current, &response_size);
        if (R_FAILED(result) || response_size == 0) {
            return_code = fail_install(
                progress, INSTALL_STAGE_ERROR,
                "NAS did not provide a file size.", result, -7,
                callback, user_data);
            goto cleanup_staged;
        }
        total_size = cached_size + response_size;

        if (get_sd_free_bytes(&progress->free_bytes) &&
            progress->free_bytes <
                (u64)response_size + total_size + INSTALL_SPACE_MARGIN) {
            return_code = fail_install(
                progress, INSTALL_STAGE_ERROR,
                "Staged install needs about twice the CIA size.", 0, -34,
                callback, user_data);
            goto cleanup_staged;
        }

        buffer = malloc(INSTALL_CHUNK_SIZE);
        if (!buffer) {
            return_code = fail_install(
                progress, INSTALL_STAGE_ERROR,
                "Not enough memory for download buffer.", 0, -9,
                callback, user_data);
            goto cleanup_staged;
        }

        progress->total = total_size;
        progress->downloaded = cached_size;
        set_message(progress, INSTALL_STAGE_DOWNLOADING,
                    cached_size > 0 ? "Resuming download to SD cache..."
                                    : "Downloading CIA to SD cache...", 0);
        started_at = osGetTime();
        publish(progress, callback, user_data);

        for (;;) {
            received = 0;
            result = httpcDownloadData(
                &context, buffer, INSTALL_CHUNK_SIZE, &received);
            if (received > 0) {
                written = 0;
                Result write_result = FSFILE_Write(
                    cache_file, &written, progress->downloaded,
                    buffer, received, 0);
                if (R_FAILED(write_result) || written != received) {
                    return_code = fail_install(
                        progress, INSTALL_STAGE_ERROR,
                        "Failed to write the SD cache.", write_result, -35,
                        callback, user_data);
                    goto cleanup_staged;
                }
                progress->downloaded += written;
                update_transfer_stats_from(
                    progress, started_at, cached_size);
                if (!publish(progress, callback, user_data)) {
                    return_code = fail_install(
                        progress, INSTALL_STAGE_CANCELLED,
                        "Download paused. Partial cache was kept.", 0, -12,
                        callback, user_data);
                    goto cleanup_staged;
                }
            }
            if (result == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) {
                continue;
            }
            if (R_FAILED(result)) {
                return_code = fail_install(
                    progress, INSTALL_STAGE_ERROR,
                    "Download interrupted. Partial cache was kept.",
                    result, -17, callback, user_data);
                goto cleanup_staged;
            }
            break;
        }
        if (progress->downloaded != total_size) {
            return_code = fail_install(
                progress, INSTALL_STAGE_ERROR,
                "Cached size does not match the NAS size.", 0, -18,
                callback, user_data);
            goto cleanup_staged;
        }

        httpcCloseContext(&context);
        context_open = false;
        FSFILE_Close(cache_file);
        file_open = false;
        FSUSER_DeleteFile(
            sd_archive, fsMakePath(PATH_ASCII, final_path));
        result = FSUSER_RenameFile(
            sd_archive, fsMakePath(PATH_ASCII, part_path),
            sd_archive, fsMakePath(PATH_ASCII, final_path));
        if (R_FAILED(result)) {
            return_code = fail_install(
                progress, INSTALL_STAGE_ERROR,
                "Could not finalize the downloaded cache.", result, -36,
                callback, user_data);
            goto cleanup_staged;
        }
        final_ready = true;
        result = FSUSER_OpenFile(
            &cache_file, sd_archive, fsMakePath(PATH_ASCII, final_path),
            FS_OPEN_READ, 0);
        if (R_FAILED(result)) {
            return_code = fail_install(
                progress, INSTALL_STAGE_ERROR,
                "Could not reopen the downloaded CIA.", result, -37,
                callback, user_data);
            goto cleanup_staged;
        }
        file_open = true;
    }

    if (!buffer) {
        buffer = malloc(INSTALL_CHUNK_SIZE);
        if (!buffer) {
            return_code = fail_install(
                progress, INSTALL_STAGE_ERROR,
                "Not enough memory for install buffer.", 0, -9,
                callback, user_data);
            goto cleanup_staged;
        }
    }
    if (R_FAILED(FSFILE_GetSize(cache_file, &total_size)) ||
        total_size == 0) {
        return_code = fail_install(
            progress, INSTALL_STAGE_ERROR,
            "Cached CIA is empty.", 0, -38, callback, user_data);
        goto cleanup_staged;
    }
    if (get_sd_free_bytes(&progress->free_bytes) &&
        progress->free_bytes < total_size + INSTALL_SPACE_MARGIN) {
        return_code = fail_install(
            progress, INSTALL_STAGE_ERROR,
            "Not enough room to install the cached CIA.", 0, -8,
            callback, user_data);
        goto cleanup_staged;
    }

    set_message(progress, INSTALL_STAGE_VALIDATING,
                "Validating the cached CIA...", 0);
    publish(progress, callback, user_data);
    result = FSFILE_Read(
        cache_file, &bytes_read, 0, buffer,
        total_size < INSTALL_CHUNK_SIZE ? (u32)total_size
                                        : INSTALL_CHUNK_SIZE);
    if (R_FAILED(result) ||
        !cia_extract_title_id(buffer, bytes_read, &title_id) ||
        !cia_title_is_safe_sd(title_id)) {
        return_code = fail_install(
            progress, INSTALL_STAGE_ERROR,
            "Cached CIA is invalid or unsafe. Cache was kept.",
            result, -11, callback, user_data);
        goto cleanup_staged;
    }
    progress->title_id = title_id;
    if (cia_title_requires_new3ds(title_id)) {
        bool is_new3ds = false;
        if (R_SUCCEEDED(APT_CheckNew3DS(&is_new3ds)) && !is_new3ds) {
            return_code = fail_install(
                progress, INSTALL_STAGE_ERROR,
                "This title requires a New 3DS. Cache was kept.",
                0, -13, callback, user_data);
            goto cleanup_staged;
        }
    }

    result = amInit();
    if (R_FAILED(result)) {
        return_code = fail_install(
            progress, INSTALL_STAGE_ERROR,
            "AM service is unavailable. Cache was kept.",
            result, -14, callback, user_data);
        goto cleanup_staged;
    }
    am_open = true;
    memset(&title_info, 0, sizeof(title_info));
    progress->replacing =
        R_SUCCEEDED(AM_GetTitleInfo(
            MEDIATYPE_SD, 1, &title_id, &title_info));
    result = progress->replacing
        ? AM_StartCiaInstallOverwrite(&cia_handle, MEDIATYPE_SD)
        : AM_StartCiaInstall(MEDIATYPE_SD, &cia_handle);
    if (R_FAILED(result)) {
        return_code = fail_install(
            progress, INSTALL_STAGE_ERROR,
            "Could not start installation. Cache was kept.",
            result, -15, callback, user_data);
        goto cleanup_staged;
    }
    cia_open = true;

    progress->downloaded = 0;
    progress->total = total_size;
    progress->percent = 0;
    progress->bytes_per_second = 0;
    set_message(progress, INSTALL_STAGE_INSTALLING,
                "Installing from the completed SD cache...", 0);
    started_at = osGetTime();
    publish(progress, callback, user_data);

    while (progress->downloaded < total_size) {
        u32 wanted =
            total_size - progress->downloaded < INSTALL_CHUNK_SIZE
                ? (u32)(total_size - progress->downloaded)
                : INSTALL_CHUNK_SIZE;
        bytes_read = 0;
        result = FSFILE_Read(
            cache_file, &bytes_read, progress->downloaded, buffer, wanted);
        if (R_FAILED(result) || bytes_read == 0) {
            return_code = fail_install(
                progress, INSTALL_STAGE_ERROR,
                "Failed to read cached CIA. Cache was kept.",
                result, -39, callback, user_data);
            goto cleanup_staged;
        }
        written = 0;
        result = FSFILE_Write(
            cia_handle, &written, progress->downloaded,
            buffer, bytes_read, 0);
        if (R_FAILED(result) || written != bytes_read) {
            return_code = fail_install(
                progress, INSTALL_STAGE_ERROR,
                "AM write failed. Cache was kept.",
                result, -16, callback, user_data);
            goto cleanup_staged;
        }
        progress->downloaded += written;
        update_transfer_stats(progress, started_at);
        if (!publish(progress, callback, user_data)) {
            return_code = fail_install(
                progress, INSTALL_STAGE_CANCELLED,
                "Install cancelled. Completed cache was kept.",
                0, -12, callback, user_data);
            goto cleanup_staged;
        }
    }

    set_message(progress, INSTALL_STAGE_FINISHING,
                "Finalizing installation...", 0);
    publish(progress, callback, user_data);
    result = AM_FinishCiaInstall(cia_handle);
    if (R_FAILED(result)) {
        return_code = fail_install(
            progress, INSTALL_STAGE_ERROR,
            "Finalize failed. Completed cache was kept.",
            result, -19, callback, user_data);
        goto cleanup_staged;
    }
    cia_open = false;

    FSFILE_Close(cache_file);
    file_open = false;
    result = FSUSER_DeleteFile(
        sd_archive, fsMakePath(PATH_ASCII, final_path));
    progress->percent = 100;
    if (R_SUCCEEDED(result)) {
        set_message(progress, INSTALL_STAGE_SUCCESS,
                    "Installed. Downloaded CIA was deleted.", 0);
    } else {
        set_message(progress, INSTALL_STAGE_SUCCESS,
                    "Installed, but the cache could not be deleted.", result);
    }
    publish(progress, callback, user_data);
    return_code = 0;

cleanup_staged:
    if (context_open) cancel_http(&context);
    if (cia_open) AM_CancelCIAInstall(cia_handle);
    if (am_open) amExit();
    if (file_open) FSFILE_Close(cache_file);
    if (archive_open) FSUSER_CloseArchive(sd_archive);
    free(buffer);
    return return_code;
}
