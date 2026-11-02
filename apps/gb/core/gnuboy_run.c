/*
 * gnuboy_run.c — Game Boy emulator entry point for ESP32-P4
 *
 * Based on RetroESP32 gnuboy-go/main/main.c.
 * Adapted to run as a callable function (not app_main) so the
 * launcher can call gnuboy_run() and get control back on MENU press.
 */

#include "gnuboy_run.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* Gnuboy core headers */
#include "loader.h"
#include "hw.h"
#include "lcd.h"
#include "fb.h"
#include "cpu.h"
#include "mem.h"
#include "sound.h"
#include "pcm.h"
#include "regs.h"
#include "rtc.h"
#include "gnuboy.h"
#include "input.h"

/* Odroid compatibility layer */
#include "odroid_settings.h"
#include "odroid_input.h"
#include "odroid_display.h"
#include "odroid_audio.h"
#include "odroid_system.h"
#include "odroid_sdcard.h"
#include "odroid_menu.h"

static const char *TAG = "gnuboy_run";

#define GAMEBOY_WIDTH  (160)
#define GAMEBOY_HEIGHT (144)
#define AUDIO_SAMPLE_RATE (32000)

/* ─── Check / Delete save file for GB/GBC ──────────────────────── */
extern const char* SD_BASE_PATH;

static bool gb_check_save_exists(void)
{
    char *romPath = odroid_settings_RomFilePath_get();
    if (!romPath) return false;

    const char *fname = strrchr(romPath, '/');
    if (fname) fname++; else fname = romPath;

    /* Determine subdirectory from extension */
    const char *ext = strrchr(fname, '.');
    const char *subdir = "gb";
    if (ext && (strcasecmp(ext, ".gbc") == 0)) subdir = "gbc";

    /* Mount SD temporarily */
    esp_err_t r = odroid_sdcard_open(SD_BASE_PATH);
    if (r != ESP_OK) { free(romPath); return false; }

    /* Use full ROM filename (with extension) so path matches main menu:
     * e.g. /sd/odroid/data/gb/Pokemon.gb.sav */
    char pathBuf[512];
    snprintf(pathBuf, sizeof(pathBuf), "%s/odroid/data/%s/%s.sav", SD_BASE_PATH, subdir, fname);

    struct stat st;
    bool exists = (stat(pathBuf, &st) == 0);

    odroid_sdcard_close();
    free(romPath);
    return exists;
}

static bool gb_delete_save(void)
{
    char *romPath = odroid_settings_RomFilePath_get();
    if (!romPath) return false;

    const char *fname = strrchr(romPath, '/');
    if (fname) fname++; else fname = romPath;

    const char *ext = strrchr(fname, '.');
    const char *subdir = "gb";
    if (ext && (strcasecmp(ext, ".gbc") == 0)) subdir = "gbc";

    esp_err_t r = odroid_sdcard_open(SD_BASE_PATH);
    if (r != ESP_OK) { free(romPath); return false; }

    /* Use full ROM filename (with extension) to match main menu convention */
    char pathBuf[512];
    snprintf(pathBuf, sizeof(pathBuf), "%s/odroid/data/%s/%s.sav", SD_BASE_PATH, subdir, fname);

    struct stat st;
    bool ok = false;
    if (stat(pathBuf, &st) == 0) {
        ok = (unlink(pathBuf) == 0);
    }

    odroid_sdcard_close();
    free(romPath);
    return ok;
}

/* --- State --- */
struct fb fb;
struct pcm pcm;

int frame = 0;  /* frame counter used by lcd.c */
uint16_t* displayBuffer[2];
static uint8_t currentBuffer;
static uint16_t* framebuffer_ptr;

static int32_t* audioBuffer[2];
static volatile uint8_t currentAudioBuffer;
static volatile uint16_t currentAudioSampleCount;
static volatile int16_t* currentAudioBufferPtr;

static QueueHandle_t vidQueue;
static QueueHandle_t audioQueue;

static volatile bool videoTaskIsRunning = false;
static volatile bool audioTaskIsRunning = false;
static volatile bool quit_flag = false;

/* --- PCM submit callback (called by gnuboy sound) --- */
int pcm_submit(void)
{
    odroid_audio_submit((short*)currentAudioBufferPtr, currentAudioSampleCount >> 1);
    return 1;
}

/* --- Run one frame --- */
static void run_to_vblank(void)
{
    cpu_emulate(2280);

    while (R_LY > 0 && R_LY < 144)
    {
        emu_step();
    }

    /* VBLANK: send video */
    xQueueSend(vidQueue, &framebuffer_ptr, portMAX_DELAY);

    currentBuffer = currentBuffer ? 0 : 1;
    framebuffer_ptr = displayBuffer[currentBuffer];
    fb.ptr = framebuffer_ptr;

    rtc_tick();

    /* Audio */
    sound_mix();

    currentAudioBufferPtr = (int16_t*)audioBuffer[currentAudioBuffer];
    currentAudioSampleCount = pcm.pos;

    void* tempPtr = (void*)0x1234;
    xQueueSend(audioQueue, &tempPtr, portMAX_DELAY);

    currentAudioBuffer = currentAudioBuffer ? 0 : 1;
    pcm.buf = audioBuffer[currentAudioBuffer];
    pcm.pos = 0;

    if (!(R_LCDC & 0x80)) {
        cpu_emulate(32832);
    }

    while (R_LY > 0) {
        emu_step();
    }
}

/* --- Video task (runs on core 1) --- */
static void videoTask(void *arg)
{
    videoTaskIsRunning = true;
    uint16_t* param;

    while (1)
    {
        xQueuePeek(vidQueue, &param, portMAX_DELAY);

        if (param == (uint16_t*)1)
            break;

        ili9341_write_frame_gb(param, false);

        xQueueReceive(vidQueue, &param, portMAX_DELAY);
    }

    videoTaskIsRunning = false;
    vTaskDelete(NULL);
}

/* --- Audio task --- */
static void audioTask(void *arg)
{
    audioTaskIsRunning = true;
    uint16_t* param;

    while (1)
    {
        xQueuePeek(audioQueue, &param, portMAX_DELAY);

        if (param == (uint16_t*)1)
            break;

        pcm_submit();

        xQueueReceive(audioQueue, &param, portMAX_DELAY);
    }

    odroid_audio_terminate();
    audioTaskIsRunning = false;
    vTaskDelete(NULL);
}

/* ─── In-Game Menu for GB/GBC (drawn by the launcher) ─────────── */
static bool gb_save_state(void)
{
    const char *savPath = sram_get_savefile_path();
    if (!savPath) return false;
    FILE *sf = fopen(savPath, "wb");
    if (!sf) { printf("GB: failed to open %s for writing\n", savPath); return false; }
    savestate(sf);
    fclose(sf);
    printf("GB: full state saved to %s\n", savPath);
    return true;
}

static void gb_load_state(void)
{
    const char *ldPath = sram_get_savefile_path();
    if (!ldPath) return;
    FILE *lf = fopen(ldPath, "rb");
    if (!lf) return;
    loadstate(lf);
    fclose(lf);
    vram_dirty(); pal_dirty(); sound_dirty(); mem_updatemap();
}

/* The launcher takes over the display during the menu: wait until the video
 * task has presented the last queued frame (it receives only after presenting) */
static void gb_video_idle(void)
{
    for (int i = 0; i < 500 && uxQueueMessagesWaiting(vidQueue); i++) vTaskDelay(1);
}

/* Returns: true = keep running, false = exit game */
static bool gb_show_ingame_menu(void)
{
    gb_video_idle();
    const char *note = NULL;
    for (;;) {
        switch (odroid_emu_menu(note, gb_check_save_exists() ? EMU_MENU_HAS_SAVE : 0)) {
        case EMU_MENU_SAVE:
            note = gb_save_state() ? "saved" : "save failed";
            continue;
        case EMU_MENU_DELETE:
            note = gb_delete_save() ? "save deleted" : "delete failed";
            continue;
        case EMU_MENU_LOAD:
            gb_load_state();
            return true;
        case EMU_MENU_RESTART:
            emu_reset();
            lcd_begin();
            sound_reset();
            return true;
        case EMU_MENU_EXIT:
            printf("GB InGameMenu: Exit Game - auto-saving full state\n");
            gb_save_state();
            return false;
        default:
            return true;
        }
    }
}

/* ================================================================ */
void gnuboy_run(const char *rom_path)
{
    ESP_LOGI(TAG, "Starting gnuboy with ROM: %s", rom_path);
    quit_flag = false;

    /* Set ROM path in settings (gnuboy loader reads it) */
    odroid_settings_RomFilePath_set(rom_path);

    /* Prepare display */
    ili9341_prepare();
    ili9341_write_frame_gb(NULL, true);

    /* Audio */
    odroid_audio_init(AUDIO_SAMPLE_RATE);

    /* Allocate display buffers in PSRAM */
    /* DMA-capable + cache-aligned for PPA hardware scaling */
    displayBuffer[0] = heap_caps_aligned_alloc(64, GAMEBOY_WIDTH * GAMEBOY_HEIGHT * 2,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    displayBuffer[1] = heap_caps_aligned_alloc(64, GAMEBOY_WIDTH * GAMEBOY_HEIGHT * 2,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!displayBuffer[0] || !displayBuffer[1]) {
        ESP_LOGE(TAG, "Failed to allocate display buffers!");
        return;
    }
    memset(displayBuffer[0], 0, GAMEBOY_WIDTH * GAMEBOY_HEIGHT * 2);
    memset(displayBuffer[1], 0, GAMEBOY_WIDTH * GAMEBOY_HEIGHT * 2);

    currentBuffer = 0;
    framebuffer_ptr = displayBuffer[0];

    /* Audio buffers */
    const int audioBufferLength = AUDIO_SAMPLE_RATE / 10 + 1;
    const int AUDIO_BUFFER_SIZE = audioBufferLength * sizeof(int16_t) * 2;

    memset(&pcm, 0, sizeof(pcm));
    pcm.hz = AUDIO_SAMPLE_RATE;
    pcm.stereo = 1;
    pcm.len = audioBufferLength;
    pcm.buf = heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    pcm.pos = 0;

    audioBuffer[0] = pcm.buf;
    audioBuffer[1] = heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM);

    if (!audioBuffer[0] || !audioBuffer[1]) {
        ESP_LOGE(TAG, "Failed to allocate audio buffers!");
        goto cleanup;
    }

    currentAudioBuffer = 0;

    /* Set SD base path for save file creation */
    extern const char* SD_BASE_PATH;
    SD_BASE_PATH = "/sd";

    /* Load ROM */
    loader_init(NULL);

    /* Clear display */
    ili9341_write_frame_gb(NULL, true);

    /* Setup gnuboy framebuffer struct */
    memset(&fb, 0, sizeof(fb));
    fb.w = GAMEBOY_WIDTH;
    fb.h = GAMEBOY_HEIGHT;
    fb.pelsize = 2;
    fb.pitch = fb.w * fb.pelsize;
    fb.indexed = 0;
    fb.ptr = framebuffer_ptr;
    fb.enabled = 1;
    fb.dirty = 0;

    /* Create tasks */
    vidQueue = xQueueCreate(1, sizeof(uint16_t*));
    audioQueue = xQueueCreate(1, sizeof(uint16_t*));

    xTaskCreatePinnedToCore(&videoTask, "gb_video", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(&audioTask, "gb_audio", 4096, NULL, 5, NULL, 1);

    /* Reset emulator (baseline init) */
    emu_reset();

    rtc.d = 1;
    rtc.h = 1;
    rtc.m = 1;
    rtc.s = 1;
    rtc.t = 1;

    sound_reset();
    lcd_begin();

    /* Check for resume: if StartAction is RESTART, load full emulator state */
    bool resumed = false;
    if (odroid_settings_StartAction_get() == ODROID_START_ACTION_RESTART)
    {
        odroid_settings_StartAction_set(ODROID_START_ACTION_NORMAL);
        const char *savPath = sram_get_savefile_path();
        if (savPath) {
            FILE *sf = fopen(savPath, "rb");
            if (sf) {
                ESP_LOGI(TAG, "Resuming: loading full state from %s", savPath);
                loadstate(sf);
                fclose(sf);
                vram_dirty();
                pal_dirty();
                sound_dirty();
                mem_updatemap();
                resumed = true;
                ESP_LOGI(TAG, "Resume state loaded successfully");
            } else {
                ESP_LOGW(TAG, "Resume: save file not found: %s", savPath);
            }
        }
    }
    if (!resumed) {
        ESP_LOGI(TAG, "Fresh start (no resume)");
    }

    /* Main emulation loop */
    odroid_gamepad_state lastJoystickState;
    odroid_input_gamepad_read(&lastJoystickState);
    bool ignoreMenuButton = lastJoystickState.values[ODROID_INPUT_MENU];
    uint16_t menuButtonFrameCount = 0;
    frame = 0;  /* reset global frame counter (used by lcd.c) — do NOT redeclare */
    uint64_t totalElapsedUs = 0;
    int actualFrameCount = 0;

    while (!quit_flag)
    {
        odroid_gamepad_state joystick;
        odroid_input_gamepad_read(&joystick);

        if (ignoreMenuButton)
            ignoreMenuButton = lastJoystickState.values[ODROID_INPUT_MENU];

        if (!ignoreMenuButton && lastJoystickState.values[ODROID_INPUT_MENU] && joystick.values[ODROID_INPUT_MENU])
            ++menuButtonFrameCount;
        else
            menuButtonFrameCount = 0;

        /* Short MENU press: show in-game menu */
        if (!ignoreMenuButton && lastJoystickState.values[ODROID_INPUT_MENU] && !joystick.values[ODROID_INPUT_MENU])
        {
            ESP_LOGI(TAG, "MENU pressed — opening in-game menu");
            if (!gb_show_ingame_menu()) {
                ESP_LOGI(TAG, "User chose Exit — returning to launcher");
                quit_flag = true;
                break;
            }
            /* Re-read gamepad state after menu closes */
            odroid_input_gamepad_read(&joystick);
        }

        /* Volume button: the launcher's volume dialog */
        if (!lastJoystickState.values[ODROID_INPUT_VOLUME] && joystick.values[ODROID_INPUT_VOLUME])
        {
            gb_video_idle();
            odroid_emu_volume();
            /* Re-read gamepad state after overlay closes */
            odroid_input_gamepad_read(&joystick);
        }

        /* Gamepad to emulator input */
        pad_set(PAD_UP, joystick.values[ODROID_INPUT_UP]);
        pad_set(PAD_RIGHT, joystick.values[ODROID_INPUT_RIGHT]);
        pad_set(PAD_DOWN, joystick.values[ODROID_INPUT_DOWN]);
        pad_set(PAD_LEFT, joystick.values[ODROID_INPUT_LEFT]);
        pad_set(PAD_SELECT, joystick.values[ODROID_INPUT_SELECT]);
        pad_set(PAD_START, joystick.values[ODROID_INPUT_START]);
        pad_set(PAD_A, joystick.values[ODROID_INPUT_A]);
        pad_set(PAD_B, joystick.values[ODROID_INPUT_B]);

        /* Run one frame */
        int64_t startUs = esp_timer_get_time();
        run_to_vblank();
        int64_t endUs = esp_timer_get_time();

        lastJoystickState = joystick;

        totalElapsedUs += (endUs - startUs);
        ++frame;
        ++actualFrameCount;

        if (actualFrameCount == 60)
        {
            float seconds = totalElapsedUs / 1000000.0f;
            float fps = actualFrameCount / seconds;
            ESP_LOGI(TAG, "HEAP:0x%x, FPS:%.1f", (unsigned)esp_get_free_heap_size(), fps);
            actualFrameCount = 0;
            totalElapsedUs = 0;
        }
    }

    /* --- Shutdown --- */
    ESP_LOGI(TAG, "Shutting down gnuboy...");

    /* Stop audio */
    {
        uint16_t* discard;
        while (xQueueReceive(audioQueue, &discard, 0) == pdTRUE) {}
        uint16_t* param = (uint16_t*)1;
        xQueueOverwrite(audioQueue, &param);
        int timeout = 500;
        while (audioTaskIsRunning && --timeout > 0) vTaskDelay(1);
    }

    /* Stop video */
    {
        uint16_t* discard;
        while (xQueueReceive(vidQueue, &discard, 0) == pdTRUE) {}
        uint16_t* param = (uint16_t*)1;
        xQueueOverwrite(vidQueue, &param);
        int timeout = 500;
        while (videoTaskIsRunning && --timeout > 0) vTaskDelay(1);
    }

cleanup:
    /* Unload ROM, free SRAM, save battery-backed RAM */
    loader_unload();

    /* Close ROM file handle kept open for on-demand bank loading */
    {
        extern FILE* RomFile;
        extern uint8_t BankCache[];
        if (RomFile) { fclose(RomFile); RomFile = NULL; }
        memset(BankCache, 0, sizeof(uint8_t) * (512 / 8));
    }

    /* Free resources */
    if (vidQueue) { vQueueDelete(vidQueue); vidQueue = NULL; }
    if (audioQueue) { vQueueDelete(audioQueue); audioQueue = NULL; }

    if (displayBuffer[0]) { heap_caps_free(displayBuffer[0]); displayBuffer[0] = NULL; }
    if (displayBuffer[1]) { heap_caps_free(displayBuffer[1]); displayBuffer[1] = NULL; }
    if (audioBuffer[0]) { heap_caps_free(audioBuffer[0]); audioBuffer[0] = NULL; }
    if (audioBuffer[1]) { heap_caps_free(audioBuffer[1]); audioBuffer[1] = NULL; }
    pcm.buf = NULL;

    /* Clear display */
    ili9341_clear(0x0000);
    display_flush_force();

    ESP_LOGI(TAG, "gnuboy exited cleanly");
}
