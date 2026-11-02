/*
 * smsplus_run.c — Entry point for SMS/GG (smsplus) emulator on ESP32-P4
 *
 * Adapted from the original smsplusgx-go main.c.
 * Loads a ROM, runs the SMS/GG emulation loop, returns on MENU press.
 */

#include "smsplus_run.h"
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

#include "shared.h"

#include "odroid_settings.h"
#include "odroid_audio.h"
#include "odroid_input.h"
#include "odroid_system.h"
#include "odroid_display.h"
#include "odroid_sdcard.h"
#include "odroid_menu.h"

#define AUDIO_SAMPLE_RATE 32000

/* Global PSRAM pointer — replaces hardcoded 0x3f800000 */
uint8 *ESP32_PSRAM = NULL;

static const char *SD_BASE_PATH = "/sd";

static uint16 palette[PALETTE_SIZE];
static uint8_t *framebuffer[2];
static int currentFramebuffer = 0;

/* Display copy buffers — decouples emulator from video task (like NES lcdfb) */
static uint8_t *lcdfb[2] = { NULL, NULL };
static int lcdfb_write_idx = 0;

static uint32_t *audioBuffer = NULL;
static int audioBufferCount = 0;

static QueueHandle_t vidQueue;
static TaskHandle_t videoTaskHandle;

static odroid_volume_level Volume;
static odroid_battery_state battery;

static bool scaling_enabled = true;
static bool previous_scaling_enabled = true;

static volatile bool videoTaskIsRunning = false;
static volatile bool smsplus_quit_flag = false;

/* Forward declarations for save/load used by in-game menu */
static bool SaveState(void);
static void LoadState(void);

/* ─── Save-subdirectory from ROM extension (sms / gg / col) ──── */
static const char *sms_get_save_subdir(void)
{
    char *romName = odroid_settings_RomFilePath_get();
    if (!romName) return "sms";

    const char *ext = strrchr(romName, '.');
    const char *subdir = "sms";
    if (ext) {
        if (strcasecmp(ext, ".gg") == 0)  subdir = "gg";
        else if (strcasecmp(ext, ".col") == 0) subdir = "col";
    }
    free(romName);
    return subdir;
}

/* ─── Check / Delete save file for SMS/GG/COL ──────────────────── */
static char *sms_get_save_path(void)
{
    char *romName = odroid_settings_RomFilePath_get();
    if (!romName) return NULL;

    char *fileName = odroid_util_GetFileName(romName);
    free(romName);
    if (!fileName) return NULL;

    const char *subdir = sms_get_save_subdir();

    /* Build path: /sd/odroid/data/<subdir>/<fileName>.sav */
    char pathBuf[512];
    snprintf(pathBuf, sizeof(pathBuf), "%s/odroid/data/%s/%s.sav", SD_BASE_PATH, subdir, fileName);
    free(fileName);

    char *pathName = strdup(pathBuf);
    return pathName;
}

static bool sms_check_save_exists(void)
{
    char *path = sms_get_save_path();
    if (!path) return false;

    struct stat st;
    bool exists = (stat(path, &st) == 0);
    free(path);
    return exists;
}

static bool sms_delete_save(void)
{
    char *path = sms_get_save_path();
    if (!path) return false;

    struct stat st;
    bool ok = false;
    if (stat(path, &st) == 0) {
        ok = (unlink(path) == 0);
    }
    free(path);
    return ok;
}

/* ─── In-Game Menu for SMS/GG (drawn by the launcher) ─────────── */

/* The launcher takes over the display during the menu: wait until the video
 * task has presented the last queued frame (it receives only after presenting) */
static void sms_video_idle(void)
{
    for (int i = 0; i < 500 && uxQueueMessagesWaiting(vidQueue); i++) vTaskDelay(1);
}

/* Returns: true = keep running, false = exit game */
static bool sms_show_ingame_menu(void)
{
    sms_video_idle();
    const char *note = NULL;
    for (;;) {
        switch (odroid_emu_menu(note, sms_check_save_exists() ? EMU_MENU_HAS_SAVE : 0)) {
        case EMU_MENU_SAVE:
            note = SaveState() ? "saved" : "save failed";
            continue;
        case EMU_MENU_DELETE:
            note = sms_delete_save() ? "save deleted" : "delete failed";
            continue;
        case EMU_MENU_LOAD:
            LoadState();
            return true;
        case EMU_MENU_RESTART:
            system_reset();
            return true;
        case EMU_MENU_EXIT:
            printf("SMS InGameMenu: Exit Game\n");
            return false;
        default:
            return true;
        }
    }
}

/* ---- Video Task ---- */
static void videoTask(void *arg)
{
    uint8_t *param;
    videoTaskIsRunning = true;

    const bool isGameGear = (sms.console == CONSOLE_GG) || (sms.console == CONSOLE_GGMS);

    while (1)
    {
        xQueuePeek(vidQueue, &param, portMAX_DELAY);

        if ((uintptr_t)param == 1)
            break;

        if (previous_scaling_enabled != scaling_enabled)
        {
            ili9341_write_frame_sms(NULL, NULL, isGameGear, false);
            previous_scaling_enabled = scaling_enabled;
        }

        render_copy_palette(palette);
        ili9341_write_frame_sms(param, palette, isGameGear, scaling_enabled);

        odroid_input_battery_level_read(&battery);

        xQueueReceive(vidQueue, &param, portMAX_DELAY);
    }

    odroid_display_lock_sms_display();
    odroid_display_show_hourglass();
    odroid_display_unlock_sms_display();

    videoTaskIsRunning = false;
    vTaskDelete(NULL);
    while (1) {}
}

/* ---- Save / Load State ---- */
static bool SaveState(void)
{
    bool ok = false;
    odroid_input_battery_monitor_enabled_set(0);
    odroid_system_led_set(1);

    char *romName = odroid_settings_RomFilePath_get();
    if (romName)
    {
        odroid_display_lock_sms_display();
        odroid_display_drain_spi();

        char *fileName = odroid_util_GetFileName(romName);
        if (!fileName) { free(romName); goto done; }

        const char *subdir = sms_get_save_subdir();

        /* Create save directory tree: /sd/odroid/data/<subdir>/ */
        char dirBuf[512];
        snprintf(dirBuf, sizeof(dirBuf), "%s/odroid", SD_BASE_PATH);
        mkdir(dirBuf, 0775);
        snprintf(dirBuf, sizeof(dirBuf), "%s/odroid/data", SD_BASE_PATH);
        mkdir(dirBuf, 0775);
        snprintf(dirBuf, sizeof(dirBuf), "%s/odroid/data/%s", SD_BASE_PATH, subdir);
        mkdir(dirBuf, 0775);

        /* Save to: /sd/odroid/data/<subdir>/<fileName>.sav */
        char pathBuf[512];
        snprintf(pathBuf, sizeof(pathBuf), "%s/odroid/data/%s/%s.sav", SD_BASE_PATH, subdir, fileName);
        printf("SaveState: saving to '%s'\n", pathBuf);

        FILE *f = fopen(pathBuf, "w");
        if (f)
        {
            system_save_state(f);
            ok = fclose(f) == 0;
            printf("SaveState: OK.\n");
        }
        else
        {
            printf("SaveState: fopen failed\n");
        }

        odroid_display_unlock_sms_display();
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
    if (romName)
    {
        odroid_display_lock_sms_display();
        odroid_display_drain_spi();

        char *fileName = odroid_util_GetFileName(romName);
        if (!fileName) { free(romName); return; }

        const char *subdir = sms_get_save_subdir();

        /* Load from: /sd/odroid/data/<subdir>/<fileName>.sav */
        char pathBuf[512];
        snprintf(pathBuf, sizeof(pathBuf), "%s/odroid/data/%s/%s.sav", SD_BASE_PATH, subdir, fileName);
        printf("LoadState: loading from '%s'\n", pathBuf);

        FILE *f = fopen(pathBuf, "r");
        if (f)
        {
            system_load_state(f);
            fclose(f);
            printf("LoadState: OK.\n");
        }
        else
        {
            printf("LoadState: no save file found.\n");
        }

        odroid_display_unlock_sms_display();
        free(fileName);
        free(romName);
    }

    Volume = odroid_settings_Volume_get();
}

static void DoQuit(void)
{
    printf("DoQuit: stopping audio.\n");
    odroid_audio_terminate();

    printf("DoQuit: stopping video task.\n");
    {
        uint8_t *discard;
        while (xQueueReceive(vidQueue, &discard, 0) == pdTRUE) {}
        uint8_t *param = (uint8_t *)(uintptr_t)1;
        xQueueOverwrite(vidQueue, &param);
        int timeout = 500;
        while (videoTaskIsRunning && --timeout > 0) { vTaskDelay(1); }
    }

    printf("DoQuit: saving state.\n");
    SaveState();

    smsplus_quit_flag = true;
}

/* ---- Callback required by smsplus ---- */
void system_manage_sram(uint8 *sram, int slot, int mode)
{
    printf("system_manage_sram\n");
}

/* ---- Main entry point ---- */
void smsplus_run(const char *rom_path)
{
    printf("smsplus_run: starting SMS/GG emulator, ROM=%s\n", rom_path);

    smsplus_quit_flag = false;

    /* Store ROM path for save/load */
    odroid_settings_RomFilePath_set(rom_path);

    /* Allocate PSRAM buffer (2MB) for ROM + coleco BIOS */
    ESP32_PSRAM = (uint8 *)heap_caps_malloc(0x200000, MALLOC_CAP_SPIRAM);
    if (!ESP32_PSRAM)
    {
        printf("smsplus_run: ERROR — PSRAM alloc failed\n");
        return;
    }

    /* Allocate double-buffered framebuffers */
    framebuffer[0] = heap_caps_malloc(256 * 192, MALLOC_CAP_SPIRAM);
    framebuffer[1] = heap_caps_malloc(256 * 192, MALLOC_CAP_SPIRAM);
    if (!framebuffer[0] || !framebuffer[1])
    {
        printf("smsplus_run: ERROR — framebuffer alloc failed\n");
        goto cleanup;
    }

    /* Display copy buffers for non-blocking video (like NES lcdfb) */
    lcdfb[0] = heap_caps_malloc(256 * 192, MALLOC_CAP_SPIRAM);
    lcdfb[1] = heap_caps_malloc(256 * 192, MALLOC_CAP_SPIRAM);
    if (!lcdfb[0] || !lcdfb[1])
    {
        printf("smsplus_run: ERROR — lcdfb alloc failed\n");
        goto cleanup;
    }
    lcdfb_write_idx = 0;

    /* Open SD card and load ROM */
    esp_err_t r = odroid_sdcard_open("/sd");
    if (r != ESP_OK)
    {
        printf("smsplus_run: ERROR — SD open failed\n");
        goto cleanup;
    }

    load_rom((char *)rom_path);

    /* Set up display */
    const bool isGameGear = (sms.console == CONSOLE_GG) || (sms.console == CONSOLE_GGMS);
    ili9341_write_frame_sms(NULL, NULL, isGameGear, false);

    /* Audio init */
    odroid_audio_init(AUDIO_SAMPLE_RATE);

    /* Video queue and task */
    vidQueue = xQueueCreate(1, sizeof(uint8_t *));
    xTaskCreatePinnedToCore(&videoTask, "sms_video", 1024 * 4, NULL, 5, &videoTaskHandle, 1);

    /* Emulator init */
    sms.use_fm = 0;

    bitmap.width = 256;
    bitmap.height = 192;
    bitmap.pitch = bitmap.width;
    bitmap.data = framebuffer[0];
    currentFramebuffer = 0;

    set_option_defaults();
    option.sndrate = AUDIO_SAMPLE_RATE;
    option.overscan = 0;
    option.extra_gg = 0;

    system_init2();
    system_reset();

    /* Check for resume: if StartAction is RESTART, load saved state */
    if (odroid_settings_StartAction_get() == ODROID_START_ACTION_RESTART)
    {
        odroid_settings_StartAction_set(ODROID_START_ACTION_NORMAL);
        LoadState();
        printf("smsplus_run: resumed from save state\n");
    }

    /* Input state */
    odroid_gamepad_state previousState;
    odroid_input_gamepad_read(&previousState);

    int64_t startTime;
    int64_t stopTime;
    int64_t totalElapsedTime = 0;
    int frame = 0;
    uint16_t muteFrameCount = 0;
    bool ignoreMenuButton = previousState.values[ODROID_INPUT_MENU];

    scaling_enabled = odroid_settings_ScaleDisabled_get(ODROID_SCALE_DISABLE_SMS) ? false : true;

    audioBuffer = NULL;
    audioBufferCount = 0;

    /* ---- Main emulation loop ---- */
    while (!smsplus_quit_flag)
    {
        odroid_gamepad_state joystick;
        odroid_input_gamepad_read(&joystick);

        if (ignoreMenuButton)
        {
            ignoreMenuButton = previousState.values[ODROID_INPUT_MENU];
        }

        /* MENU short-press → in-game menu */
        if (!ignoreMenuButton && previousState.values[ODROID_INPUT_MENU] && !joystick.values[ODROID_INPUT_MENU])
        {
            if (!sms_show_ingame_menu()) {
                DoQuit();
                break;
            }
            /* Re-read gamepad after menu dismissal */
            odroid_input_gamepad_read(&joystick);
        }

        /* VOLUME press → the launcher's volume dialog */
        if (previousState.values[ODROID_INPUT_VOLUME] && !joystick.values[ODROID_INPUT_VOLUME])
        {
            sms_video_idle();
            odroid_emu_volume();
            /* Re-read gamepad after overlay dismissal */
            odroid_input_gamepad_read(&joystick);
        }

        /* Scaling toggle */
        if (joystick.values[ODROID_INPUT_START] && !previousState.values[ODROID_INPUT_RIGHT] && joystick.values[ODROID_INPUT_RIGHT])
        {
            scaling_enabled = !scaling_enabled;
            odroid_settings_ScaleDisabled_set(ODROID_SCALE_DISABLE_SMS, scaling_enabled ? 0 : 1);
        }

        startTime = esp_timer_get_time();

        /* Map gamepad → SMS input */
        int smsButtons = 0;
        if (joystick.values[ODROID_INPUT_UP])    smsButtons |= INPUT_UP;
        if (joystick.values[ODROID_INPUT_DOWN])  smsButtons |= INPUT_DOWN;
        if (joystick.values[ODROID_INPUT_LEFT])  smsButtons |= INPUT_LEFT;
        if (joystick.values[ODROID_INPUT_RIGHT]) smsButtons |= INPUT_RIGHT;
        if (joystick.values[ODROID_INPUT_A])     smsButtons |= INPUT_BUTTON2;
        if (joystick.values[ODROID_INPUT_B])     smsButtons |= INPUT_BUTTON1;

        int smsSystem = 0;
        if (joystick.values[ODROID_INPUT_START])  smsSystem |= INPUT_START;
        if (joystick.values[ODROID_INPUT_SELECT]) smsSystem |= INPUT_PAUSE;

        input.pad[0] = smsButtons;
        input.system = smsSystem;

        /* Colecovision keypad mapping */
        if (sms.console == CONSOLE_COLECO)
        {
            input.system = 0;
            coleco.keypad[0] = 0xff;
            coleco.keypad[1] = 0xff;

            if (joystick.values[ODROID_INPUT_START])
            {
                coleco.keypad[0] = 1;
            }
            if (previousState.values[ODROID_INPUT_SELECT] && !joystick.values[ODROID_INPUT_SELECT])
            {
                system_reset();
            }
        }

        /* Run one frame — render every frame for smooth scrolling */
        system_frame(0);

        /* Copy to display buffer and send non-blocking (drop if busy) */
        {
            int next = lcdfb_write_idx ^ 1;
            memcpy(lcdfb[next], bitmap.data, 256 * 192);
            lcdfb_write_idx = next;
            uint8_t *arg = lcdfb[next];
            xQueueOverwrite(vidQueue, &arg);
        }
        currentFramebuffer = currentFramebuffer ? 0 : 1;
        bitmap.data = framebuffer[currentFramebuffer];

        /* Audio */
        if (!audioBuffer || audioBufferCount < snd.sample_count)
        {
            if (audioBuffer) free(audioBuffer);

            size_t bufferSize = snd.sample_count * 2 * sizeof(int16_t);
            audioBuffer = heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM);
            if (!audioBuffer) abort();

            audioBufferCount = snd.sample_count;
        }

        for (int x = 0; x < snd.sample_count; x++)
        {
            uint32_t sample;
            if (muteFrameCount < 60 * 2)
            {
                sample = 0;
                ++muteFrameCount;
            }
            else
            {
                sample = (snd.output[0][x] << 16) + snd.output[1][x];
            }
            audioBuffer[x] = sample;
        }

        odroid_audio_submit((short *)audioBuffer, snd.sample_count - 1);

        stopTime = esp_timer_get_time();
        previousState = joystick;

        int64_t elapsedTime = stopTime - startTime;
        totalElapsedTime += elapsedTime;
        ++frame;

        if (frame == 60)
        {
            float seconds = totalElapsedTime / 1000000.0f;
            float fps = frame / seconds;

            printf("HEAP:0x%x, FPS:%f, BATTERY:%d [%d]\n",
                   (unsigned)esp_get_free_heap_size(), fps,
                   battery.millivolts, battery.percentage);

            frame = 0;
            totalElapsedTime = 0;
        }
    }

    /* ---- Cleanup ---- */
cleanup:
    system_shutdown();

    if (audioBuffer)
    {
        free(audioBuffer);
        audioBuffer = NULL;
    }

    if (framebuffer[0]) { heap_caps_free(framebuffer[0]); framebuffer[0] = NULL; }
    if (framebuffer[1]) { heap_caps_free(framebuffer[1]); framebuffer[1] = NULL; }

    if (lcdfb[0]) { heap_caps_free(lcdfb[0]); lcdfb[0] = NULL; }
    if (lcdfb[1]) { heap_caps_free(lcdfb[1]); lcdfb[1] = NULL; }

    if (ESP32_PSRAM)
    {
        heap_caps_free(ESP32_PSRAM);
        ESP32_PSRAM = NULL;
    }

    if (vidQueue)
    {
        vQueueDelete(vidQueue);
        vidQueue = NULL;
    }

    smsplus_quit_flag = false;
    printf("smsplus_run: returning to launcher\n");
}
