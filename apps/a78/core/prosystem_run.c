/*
 * prosystem_run.c — Entry point for Atari 7800 (prosystem) emulator on ESP32-P4
 *
 * Adapted from the original prosystem-odroid-go main.c.
 * Loads a ROM, runs the 7800 emulation loop, returns on MENU press.
 */

#include "prosystem_run.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

/* ProSystem core headers */
#include "Bios.h"
#include "Cartridge.h"
#include "Database.h"
#include "Maria.h"
#include "Palette.h"
#include "Pokey.h"
#include "Region.h"
#include "ProSystem.h"
#include "Tia.h"

/* Odroid platform */
#include "odroid_settings.h"
#include "odroid_audio.h"
#include "odroid_input.h"
#include "odroid_system.h"
#include "odroid_display.h"
#include "odroid_sdcard.h"
#include "odroid_menu.h"

#define AUDIO_SAMPLE_RATE  32000
#define PRO_FB_WIDTH       320
#define PRO_FB_HEIGHT      240

static const char *SD_BASE_PATH = "/sd";

static QueueHandle_t vidQueue;
static TaskHandle_t videoTaskHandle;

static uint8_t *framebuffer;
static uint16_t *display_palette16;
static uint8_t keyboard_data[17];

/* Double-buffer for non-blocking video (like SMS lcdfb pattern) */
static uint8_t *lcdfb[2] = { NULL, NULL };
static int lcdfb_write_idx = 0;

static int16_t *sampleBuffer;
static size_t sampleBufferLength;

static volatile bool videoTaskIsRunning = false;
static volatile bool prosystem_quit_flag = false;

/* The RenderFlag is referenced by Maria.c (extern bool RenderFlag) */
bool RenderFlag;

/* Forward declarations for save/load */
static bool SaveState(void);
static void LoadState(void);

/* ─── Save path helpers ──────────────────────────────────────── */
static char *pro_get_save_path(void)
{
    char *romName = odroid_settings_RomFilePath_get();
    if (!romName) return NULL;

    char *fileName = odroid_util_GetFileName(romName);
    free(romName);
    if (!fileName) return NULL;

    char pathBuf[512];
    snprintf(pathBuf, sizeof(pathBuf), "%s/odroid/data/a78/%s.sav", SD_BASE_PATH, fileName);
    free(fileName);

    return strdup(pathBuf);
}

static bool pro_check_save_exists(void)
{
    char *path = pro_get_save_path();
    if (!path) return false;
    struct stat st;
    bool exists = (stat(path, &st) == 0);
    free(path);
    return exists;
}

static bool pro_delete_save(void)
{
    char *path = pro_get_save_path();
    if (!path) return false;
    struct stat st;
    bool ok = false;
    if (stat(path, &st) == 0) {
        ok = (unlink(path) == 0);
    }
    free(path);
    return ok;
}

/* ─── In-Game Menu (drawn by the launcher) ─────────────────────── */

/* The launcher takes over the display during the menu: wait until the video
 * task has presented the last queued frame (it receives only after presenting) */
static void pro_video_idle(void)
{
    for (int i = 0; i < 500 && uxQueueMessagesWaiting(vidQueue); i++) vTaskDelay(1);
}

/* Returns: true = keep running, false = exit game */
static bool pro_show_ingame_menu(void)
{
    pro_video_idle();
    const char *note = NULL;
    for (;;) {
        switch (odroid_emu_menu(note, pro_check_save_exists() ? EMU_MENU_HAS_SAVE : 0)) {
        case EMU_MENU_SAVE:
            note = SaveState() ? "saved" : "save failed";
            continue;
        case EMU_MENU_DELETE:
            note = pro_delete_save() ? "save deleted" : "delete failed";
            continue;
        case EMU_MENU_LOAD:
            LoadState();
            return true;
        case EMU_MENU_RESTART:
            prosystem_Reset();
            return true;
        case EMU_MENU_EXIT:
            printf("A7800 InGameMenu: Exit Game\n");
            return false;
        default:
            return true;
        }
    }
}

/* ─── Video Task ───────────────────────────────────────────────── */
static void videoTask(void *arg)
{
    uint8_t *param;
    videoTaskIsRunning = true;

    while (1) {
        xQueuePeek(vidQueue, &param, portMAX_DELAY);

        if ((uintptr_t)param == 1)
            break;

        ili9341_write_frame_prosystem(param, display_palette16);

        xQueueReceive(vidQueue, &param, portMAX_DELAY);
    }

    odroid_display_lock();
    odroid_display_show_hourglass();
    odroid_display_unlock();

    videoTaskIsRunning = false;
    vTaskDelete(NULL);
    while (1) {}
}

/* ─── Palette Setup ────────────────────────────────────────────── */
static void display_ResetPalette16(void)
{
    display_palette16 = (uint16_t *)malloc(256 * sizeof(uint16_t));
    if (!display_palette16) abort();

    uint8_t *paldata = palette_data;

    for (unsigned index = 0; index < 256; index++) {
        uint16_t r = paldata[(index * 3) + 0];
        uint16_t g = paldata[(index * 3) + 1];
        uint16_t b = paldata[(index * 3) + 2];

        /* RGB888 → RGB565 */
        uint16_t rgb565 = ((r << 8) & 0xf800) | ((g << 3) & 0x07e0) | (b >> 3);
        display_palette16[index] = rgb565;
    }
}

/* ─── Audio Resample ───────────────────────────────────────────── */
static uint8_t tiaSamples[1024];
static uint8_t pokeySamples[1024];

static void sound_Resample(const uint8_t *source, uint8_t *target, int length)
{
    int measurement = AUDIO_SAMPLE_RATE;
    int sourceIndex = 0;
    int targetIndex = 0;
    int max = ((prosystem_frequency * prosystem_scanlines) << 1);

    while (targetIndex < length) {
        if (measurement >= max) {
            target[targetIndex++] = source[sourceIndex];
            measurement -= max;
        } else {
            sourceIndex++;
            measurement += AUDIO_SAMPLE_RATE;
        }
    }
}

/* ─── Save / Load State ───────────────────────────────────────── */
#define SAVE_STATE_SIZE 32829  /* prosystem max state size */

static bool SaveState(void)
{
    bool ok = false;
    odroid_input_battery_monitor_enabled_set(0);
    odroid_system_led_set(1);

    char *romName = odroid_settings_RomFilePath_get();
    if (romName) {
        odroid_display_lock();
        odroid_display_drain_spi();

        char *fileName = odroid_util_GetFileName(romName);
        if (!fileName) { free(romName); goto done; }

        /* Create save directory tree: /sd/odroid/data/a78/ */
        char dirBuf[512];
        snprintf(dirBuf, sizeof(dirBuf), "%s/odroid", SD_BASE_PATH);
        mkdir(dirBuf, 0775);
        snprintf(dirBuf, sizeof(dirBuf), "%s/odroid/data", SD_BASE_PATH);
        mkdir(dirBuf, 0775);
        snprintf(dirBuf, sizeof(dirBuf), "%s/odroid/data/a78", SD_BASE_PATH);
        mkdir(dirBuf, 0775);

        char pathBuf[512];
        snprintf(pathBuf, sizeof(pathBuf), "%s/odroid/data/a78/%s.sav", SD_BASE_PATH, fileName);
        printf("A7800 SaveState: saving to '%s'\n", pathBuf);

        /* Allocate state buffer and save */
        char *stateBuffer = (char *)heap_caps_malloc(SAVE_STATE_SIZE, MALLOC_CAP_SPIRAM);
        if (stateBuffer) {
            memset(stateBuffer, 0, SAVE_STATE_SIZE);
            if (prosystem_Save(stateBuffer, false)) {
                FILE *f = fopen(pathBuf, "wb");
                if (f) {
                    ok = fwrite(stateBuffer, 1, SAVE_STATE_SIZE, f) == SAVE_STATE_SIZE;
                    ok = fclose(f) == 0 && ok;
                    printf("A7800 SaveState: OK.\n");
                } else {
                    printf("A7800 SaveState: fopen failed\n");
                }
            }
            heap_caps_free(stateBuffer);
        }

        odroid_display_unlock();
        free(fileName);
        free(romName);
    }

done:
    odroid_system_led_set(0);
    odroid_input_battery_monitor_enabled_set(1);
    return ok;
}

static void LoadState(void)
{
    char *romName = odroid_settings_RomFilePath_get();
    if (romName) {
        odroid_display_lock();
        odroid_display_drain_spi();

        char *fileName = odroid_util_GetFileName(romName);
        if (!fileName) { free(romName); return; }

        char pathBuf[512];
        snprintf(pathBuf, sizeof(pathBuf), "%s/odroid/data/a78/%s.sav", SD_BASE_PATH, fileName);
        printf("A7800 LoadState: loading from '%s'\n", pathBuf);

        FILE *f = fopen(pathBuf, "rb");
        if (f) {
            char *stateBuffer = (char *)heap_caps_malloc(SAVE_STATE_SIZE, MALLOC_CAP_SPIRAM);
            if (stateBuffer) {
                size_t read = fread(stateBuffer, 1, SAVE_STATE_SIZE, f);
                if (read > 0) {
                    prosystem_Load(stateBuffer);
                    printf("A7800 LoadState: OK (%d bytes).\n", (int)read);
                }
                heap_caps_free(stateBuffer);
            }
            fclose(f);
        } else {
            printf("A7800 LoadState: no save file found.\n");
        }

        odroid_display_unlock();
        free(fileName);
        free(romName);
    }
}

static void DoQuit(void)
{
    printf("A7800 DoQuit: stopping audio.\n");
    odroid_audio_terminate();

    printf("A7800 DoQuit: stopping video task.\n");
    {
        uint8_t *discard;
        while (xQueueReceive(vidQueue, &discard, 0) == pdTRUE) {}
        uint8_t *param = (uint8_t *)(uintptr_t)1;
        xQueueOverwrite(vidQueue, &param);
        int timeout = 500;
        while (videoTaskIsRunning && --timeout > 0) { vTaskDelay(1); }
    }

    printf("A7800 DoQuit: saving state.\n");
    SaveState();

    prosystem_quit_flag = true;
}

/* ─── Emulator Init ────────────────────────────────────────────── */
static void emu_init(const char *filename)
{
    framebuffer = (uint8_t *)heap_caps_malloc(PRO_FB_WIDTH * PRO_FB_HEIGHT, MALLOC_CAP_SPIRAM);
    if (!framebuffer) abort();

    memset(keyboard_data, 0, sizeof(keyboard_data));

    /* Difficulty switches: left=(B)eginner, right=(A)dvanced */
    keyboard_data[15] = 1;  /* Left diff: beginner */
    keyboard_data[16] = 0;  /* Right diff: advanced (fixes Tower Toppler) */

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("A7800: Failed to open ROM: %s\n", filename);
        return;
    }

    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    void *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!data) abort();

    size_t count = fread(data, 1, size, fp);
    if (count != size) abort();
    fclose(fp);

    if (!cartridge_Load((const uint8_t *)data, size)) {
        printf("A7800: cartridge_Load failed\n");
        heap_caps_free(data);
        return;
    }

    /* Allocate maria_surface in PSRAM (93KB) */
    extern uint8_t *maria_surface;
    if (!maria_surface) {
        maria_surface = heap_caps_malloc(320 * 292, MALLOC_CAP_SPIRAM);
        if (!maria_surface) abort();
        memset(maria_surface, 0, 320 * 292);
    }

    /* BIOS is optional — skip for now */
    database_Load(cartridge_digest);
    prosystem_Reset();
    display_ResetPalette16();
}

/* ─── Main Entry Point ─────────────────────────────────────────── */
void prosystem_run(const char *rom_path)
{
    printf("prosystem_run: starting Atari 7800 emulator, ROM=%s\n", rom_path);

    prosystem_quit_flag = false;

    /* Store ROM path for save/load */
    odroid_settings_RomFilePath_set(rom_path);

    /* Double-buffer for non-blocking video */
    lcdfb[0] = (uint8_t *)heap_caps_malloc(PRO_FB_WIDTH * PRO_FB_HEIGHT, MALLOC_CAP_SPIRAM);
    lcdfb[1] = (uint8_t *)heap_caps_malloc(PRO_FB_WIDTH * PRO_FB_HEIGHT, MALLOC_CAP_SPIRAM);
    if (!lcdfb[0] || !lcdfb[1]) {
        printf("prosystem_run: ERROR — lcdfb alloc failed\n");
        goto cleanup;
    }
    lcdfb_write_idx = 0;

    /* Open SD card and load ROM */
    esp_err_t r = odroid_sdcard_open("/sd");
    if (r != ESP_OK) {
        printf("prosystem_run: ERROR — SD open failed\n");
        goto cleanup;
    }

    emu_init(rom_path);

    /* Audio init */
    odroid_audio_init(AUDIO_SAMPLE_RATE);

    /* Allocate audio sample buffer */
    int audioLength = AUDIO_SAMPLE_RATE / prosystem_frequency;
    sampleBufferLength = audioLength * sizeof(int16_t) * 2;
    sampleBuffer = (int16_t *)malloc(sampleBufferLength);
    if (!sampleBuffer) abort();

    /* Video queue and task */
    vidQueue = xQueueCreate(1, sizeof(uint8_t *));
    xTaskCreatePinnedToCore(&videoTask, "a78_video", 1024 * 4, NULL, 5, &videoTaskHandle, 1);

    /* Check for resume: if StartAction is RESTART, load saved state */
    if (odroid_settings_StartAction_get() == ODROID_START_ACTION_RESTART) {
        odroid_settings_StartAction_set(ODROID_START_ACTION_NORMAL);
        LoadState();
        printf("prosystem_run: resumed from save state\n");
    }

    /* Input state */
    odroid_gamepad_state previousState;
    odroid_input_gamepad_read(&previousState);

    int64_t startTime;
    int64_t stopTime;
    int64_t totalElapsedTime = 0;
    int frame = 0;
    int renderFrames = 0;
    bool ignoreMenuButton = previousState.values[ODROID_INPUT_MENU];

    /* ---- Main emulation loop ---- */
    while (!prosystem_quit_flag) {
        odroid_gamepad_state joystick;
        odroid_input_gamepad_read(&joystick);

        if (ignoreMenuButton) {
            ignoreMenuButton = previousState.values[ODROID_INPUT_MENU];
        }

        /* MENU button → in-game menu */
        if (!ignoreMenuButton && previousState.values[ODROID_INPUT_MENU] &&
            !joystick.values[ODROID_INPUT_MENU]) {
            if (!pro_show_ingame_menu()) {
                DoQuit();
                break;
            }
            odroid_input_gamepad_read(&joystick);
        }

        /* VOLUME button → the launcher's volume dialog */
        if (previousState.values[ODROID_INPUT_VOLUME] &&
            !joystick.values[ODROID_INPUT_VOLUME]) {
            pro_video_idle();
            odroid_emu_volume();
            odroid_input_gamepad_read(&joystick);
        }

        startTime = esp_timer_get_time();

        /* Map gamepad → prosystem keyboard_data */
        keyboard_data[0] = joystick.values[ODROID_INPUT_RIGHT];
        keyboard_data[1] = joystick.values[ODROID_INPUT_LEFT];
        keyboard_data[2] = joystick.values[ODROID_INPUT_DOWN];
        keyboard_data[3] = joystick.values[ODROID_INPUT_UP];
        keyboard_data[4] = joystick.values[ODROID_INPUT_B];
        keyboard_data[5] = joystick.values[ODROID_INPUT_A];
        keyboard_data[13] = joystick.values[ODROID_INPUT_SELECT];
        keyboard_data[14] = joystick.values[ODROID_INPUT_START];

        /* Run one frame */
        RenderFlag = frame & 1;  /* render every other frame */
        prosystem_ExecuteFrame(keyboard_data);

        if (RenderFlag) {
            /* Extract visible 320×240 from maria_surface */
            const uint8_t *buffer = maria_surface + (maria_visibleArea.top * 320);
            int vert = maria_visibleArea.bottom - maria_visibleArea.top;
            int offset = (vert < 271) ? ((240 / 2) - (192 / 2)) : 0;
            buffer -= offset * 320;

            /* Copy to display buffer and send to video task */
            int next = lcdfb_write_idx ^ 1;
            memcpy(lcdfb[next], buffer, PRO_FB_WIDTH * PRO_FB_HEIGHT);
            lcdfb_write_idx = next;
            uint8_t *arg = lcdfb[next];
            xQueueOverwrite(vidQueue, &arg);

            ++renderFrames;
        }

        /* Audio */
        int length = AUDIO_SAMPLE_RATE / prosystem_frequency;

        memset(tiaSamples, 0, sizeof(tiaSamples));
        sound_Resample(tia_buffer, tiaSamples, length);

        if (cartridge_pokey) {
            memset(pokeySamples, 0, sizeof(pokeySamples));
            sound_Resample(pokey_buffer, pokeySamples, length);
        }

        /* Convert 8u to 16s stereo */
        uint32_t *framePtr = (uint32_t *)sampleBuffer;
        for (int i = 0; i < length; i++) {
            int16_t sample16 = (tiaSamples[i] - 128);
            if (cartridge_pokey) {
                sample16 += (pokeySamples[i] - 128);
                sample16 >>= 1;
            }
            sample16 <<= 8;
            framePtr[i] = ((uint32_t)(uint16_t)sample16 << 16) | (uint16_t)sample16;
        }

        odroid_audio_submit((int16_t *)sampleBuffer, length);

        previousState = joystick;

        stopTime = esp_timer_get_time();
        int64_t elapsedTime = stopTime - startTime;
        totalElapsedTime += elapsedTime;
        ++frame;

        if (frame == 60) {
            float seconds = totalElapsedTime / 1000000.0f;
            float fps = frame / seconds;
            float renderFps = renderFrames / seconds;

            printf("A7800 HEAP:0x%x, SIM:%f, REN:%f\n",
                   (unsigned)esp_get_free_heap_size(), fps, renderFps);

            frame = 0;
            renderFrames = 0;
            totalElapsedTime = 0;
        }
    }

    /* ---- Cleanup ---- */
cleanup:
    prosystem_Close();

    if (sampleBuffer) { free(sampleBuffer); sampleBuffer = NULL; }
    if (framebuffer) { heap_caps_free(framebuffer); framebuffer = NULL; }
    if (display_palette16) { free(display_palette16); display_palette16 = NULL; }

    if (lcdfb[0]) { heap_caps_free(lcdfb[0]); lcdfb[0] = NULL; }
    if (lcdfb[1]) { heap_caps_free(lcdfb[1]); lcdfb[1] = NULL; }

    if (vidQueue) {
        vQueueDelete(vidQueue);
        vidQueue = NULL;
    }

    prosystem_quit_flag = false;
    printf("prosystem_run: returning to launcher\n");
}
