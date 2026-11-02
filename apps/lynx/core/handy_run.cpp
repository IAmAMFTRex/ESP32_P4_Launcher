/*
 * handy_run.cpp — Atari Lynx (Handy) emulator wrapper for ESP32-P4
 *
 * Provides handy_run(rom_path) callable from C launcher.
 * Bypasses the handy-go.cpp libretro layer and drives CSystem directly.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

extern "C" {
#include "odroid_settings.h"
#include "odroid_audio.h"
#include "odroid_input.h"
#include "odroid_system.h"
#include "odroid_display.h"
#include "odroid_menu.h"
}

/* Handy core — globals are defined in system.cpp (which defines SYSTEM_CPP) */
#include <string.h>      /* C string funcs needed by myadd.h / c65c02.h */
#include "system.h"
#include "lynxdef.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <sys/stat.h>

/* Symbols expected by mikie.cpp / mikie_DisplayRenderLine_raw.h */
ULONG *lynx_mColourMap = NULL;
/* ESP32-P4: never skip frames — always false (no skip needed at 360MHz) */
bool skipNextFrame = false;

#include "handy_run.h"

/* ===================================================================
 * Constants
 * =================================================================== */
#define LYNX_WIDTH        160
#define LYNX_HEIGHT       102
#define AUDIO_SAMPLE_RATE 22050
#define AUDIO_BUFFER_SIZE 2756

/* ===================================================================
 * Globals — prefixed hn_ to avoid collision
 * =================================================================== */
static QueueHandle_t  hn_vidQueue = NULL;
static TaskHandle_t   hn_videoTaskHandle = NULL;
static volatile bool  hn_videoTaskIsRunning = false;
static volatile bool  hn_exitRequested = false;

static CSystem *hn_lynx = NULL;

/* Double-buffered framebuffers in PSRAM */
static uint16_t *hn_framebuffer[2] = { NULL, NULL };
static int       hn_currentFB = 0;

/* Save path */
static char hn_save_path[256] = "";

/* FPS */
static uint32_t fps_frame_count = 0;
static int64_t  fps_last_time = 0;
static float    fps_current = 0.0f;

/* ===================================================================
 * my_special_alloc — required by the Handy core via myadd.h
 * =================================================================== */
extern "C" void *my_special_alloc(unsigned char speed, unsigned char bytes, unsigned long size)
{
    uint32_t caps;
    if (speed) {
        caps = MALLOC_CAP_INTERNAL | (bytes == 1 ? MALLOC_CAP_8BIT : MALLOC_CAP_32BIT);
        /* Fall back to PSRAM if internal is too small */
        uint32_t max_free = heap_caps_get_largest_free_block(caps);
        if (max_free < size) {
            caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        }
    } else {
        caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    }
    void *rc = heap_caps_malloc(size, caps);
    printf("handy ALLOC: size=%lu spi=%d rc=%p\n", size,
           (caps & MALLOC_CAP_SPIRAM) != 0, rc);
    if (!rc) abort();
    return rc;
}

extern "C" void my_special_alloc_free(void *p)
{
    if (p) heap_caps_free(p);
}

/* Frame-done flag — set by display callback, cleared by main loop */
static volatile bool hn_frameDone = false;

/* Timing accumulators (microseconds) */
static int64_t s_audio_time_acc = 0;
static int64_t s_emu_time_acc = 0;
static uint32_t s_audio_bytes_acc = 0;

/* ===================================================================
 * Display callback — called by Mikie when a frame is complete
 * =================================================================== */
static UBYTE *hn_display_callback(ULONG objref)
{
    /* Send the just-rendered framebuffer to the video task (non-blocking) */
    uint16_t *fb = hn_framebuffer[hn_currentFB];
    xQueueOverwrite(hn_vidQueue, &fb);

    /* Swap to the other framebuffer */
    hn_currentFB = hn_currentFB ? 0 : 1;

    /* Process audio — i2s_channel_write blocks when DMA is full,
     * which naturally paces emulation to real-time */
    if (gAudioBufferPointer > 0) {
        int samples = gAudioBufferPointer / 4;  /* 16-bit stereo → sample count */
        s_audio_bytes_acc += gAudioBufferPointer;
        if (samples > 0) {
            int64_t a0 = esp_timer_get_time();
            odroid_audio_submit((short *)gAudioBuffer, samples);
            s_audio_time_acc += esp_timer_get_time() - a0;
        }
        gAudioBufferPointer = 0;
    }

    /* Signal frame done to main loop */
    hn_frameDone = true;

    /* FPS tracking */
    fps_frame_count++;
    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - fps_last_time;
    if (elapsed >= 1000000) {  /* every 1 second */
        fps_current = (float)fps_frame_count * 1000000.0f / (float)elapsed;
        printf("LYNX FPS=%.1f  emu=%.1fms  audio=%.1fms  abytes=%u\n",
               fps_current,
               (float)s_emu_time_acc / (fps_frame_count * 1000.0f),
               (float)s_audio_time_acc / (fps_frame_count * 1000.0f),
               s_audio_bytes_acc);
        fps_frame_count = 0;
        fps_last_time = now;
        s_audio_time_acc = 0;
        s_emu_time_acc = 0;
        s_audio_bytes_acc = 0;
    }

    return (UBYTE *)hn_framebuffer[hn_currentFB];
}

/* ===================================================================
 * Video Task — decodes RAW format and uses PPA hardware scaling
 *
 * The Handy core renders in RAW format (mikie_DisplayRenderLine_raw.h):
 *   Per line (pitch = LYNX_WIDTH*2 = 320 bytes):
 *     Bytes  0–63:  16 TPALETTE entries (per-line palette, 4 bytes each)
 *     Bytes 64–143: 80 bytes of 4-bit packed pixel data (160 pixels)
 *     Bytes 144–319: unused padding
 *
 * Each TPALETTE (little-endian RISC-V) stores:
 *     bits [3:0] = Green (4-bit), bits [7:4] = Red (4-bit),
 *     bits [11:8] = Blue (4-bit)
 *
 * We decode this to 160×102 RGB565, then PPA-scale to 320×240.
 * =================================================================== */
static void hn_videoTask(void *arg)
{
    /* Allocate DMA-aligned decode buffer for 160×102 RGB565 */
    uint16_t *decodeBuf = (uint16_t *)heap_caps_aligned_calloc(
        64, 1, LYNX_WIDTH * LYNX_HEIGHT * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!decodeBuf) {
        printf("handy: decode buffer alloc failed\n");
        hn_videoTaskIsRunning = false;
        vTaskDelete(NULL);
        return;
    }

    hn_videoTaskIsRunning = true;
    while (hn_videoTaskIsRunning)
    {
        uint16_t *srcFB;
        if (xQueueReceive(hn_vidQueue, &srcFB, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            if (srcFB == NULL) {
                break;
            }

            /* Decode RAW framebuffer → 160×102 RGB565 */
            uint8_t *rawBuf = (uint8_t *)srcFB;
            const int pitch = LYNX_WIDTH * 2;  /* 320 bytes per line */

            for (int y = 0; y < LYNX_HEIGHT; y++) {
                uint8_t *lineData = &rawBuf[y * pitch];

                /* Build 16-entry RGB565 LUT from per-line palette */
                uint16_t lut[16];
                uint32_t *palEntries = (uint32_t *)lineData;
                for (int i = 0; i < 16; i++) {
                    uint32_t idx = palEntries[i];
                    int green = idx & 0x0F;
                    int red   = (idx >> 4) & 0x0F;
                    int blue  = (idx >> 8) & 0x0F;
                    lut[i] = (uint16_t)(((red << 12) & 0xF800) |
                                        ((green << 7) & 0x07E0) |
                                        ((blue << 1)  & 0x001F));
                }

                /* Decode 80 bytes of packed 4-bit pixels → 160 RGB565 values */
                uint8_t *pixels = &lineData[64];
                uint16_t *dst = &decodeBuf[y * LYNX_WIDTH];
                for (int x = 0; x < LYNX_WIDTH / 2; x++) {
                    uint8_t byte = pixels[x];
                    dst[x * 2]     = lut[byte >> 4];
                    dst[x * 2 + 1] = lut[byte & 0x0F];
                }
            }

            /* PPA hardware-scale 160×102 → 320×240 and flush to LCD */
            ili9341_write_frame_lynx(decodeBuf);
        }
    }

    heap_caps_free(decodeBuf);
    hn_videoTaskIsRunning = false;
    vTaskDelete(NULL);
}

/* ===================================================================
 * Video task stop/start around launcher dialogs: the launcher owns the
 * display while the menu or the volume dialog is up
 * =================================================================== */
static void hn_video_stop(void)
{
    if (!hn_videoTaskIsRunning) return;
    uint16_t *discard;
    while (xQueueReceive(hn_vidQueue, &discard, 0) == pdTRUE) {}
    uint16_t *stop = NULL;
    xQueueOverwrite(hn_vidQueue, &stop);
    int timeout = 500;
    while (hn_videoTaskIsRunning && --timeout > 0) vTaskDelay(1);
}

static void hn_video_start(void)
{
    if (hn_videoTaskIsRunning) return;
    if (hn_vidQueue) { vQueueDelete(hn_vidQueue); hn_vidQueue = NULL; }
    hn_vidQueue = xQueueCreate(1, sizeof(uint16_t *));
    xTaskCreatePinnedToCore(hn_videoTask, "hn_vidTask", 4096, NULL, 5, &hn_videoTaskHandle, 1);
}

/* ===================================================================
 * Save / Load
 * =================================================================== */
static void ensure_save_dir(void)
{
    struct stat st;
    const char *dirs[] = { "/sd/odroid", "/sd/odroid/data", "/sd/odroid/data/lynx" };
    for (int i = 0; i < 3; i++) {
        if (stat(dirs[i], &st) != 0)
            mkdir(dirs[i], 0777);
    }
}

static void build_save_path(const char *romfile)
{
    const char *name = romfile;
    const char *p = romfile;
    while (*p) {
        if (*p == '/' || *p == '\\') name = p + 1;
        p++;
    }
    snprintf(hn_save_path, sizeof(hn_save_path), "/sd/odroid/data/lynx/%s.sav", name);
}

static bool handy_save_state(void)
{
    if (!hn_lynx || hn_save_path[0] == '\0') return false;
    ensure_save_dir();

    FILE *f = fopen(hn_save_path, "wb");
    if (!f) {
        printf("handy: save failed to open %s\n", hn_save_path);
        return false;
    }
    bool ok = hn_lynx->ContextSave(f);
    fclose(f);
    printf("handy: save %s\n", ok ? "OK" : "FAILED");
    return ok;
}

static bool handy_load_state(void)
{
    if (!hn_lynx || hn_save_path[0] == '\0') return false;

    FILE *f = fopen(hn_save_path, "rb");
    if (!f) {
        printf("handy: no save file %s\n", hn_save_path);
        return false;
    }
    bool ok = hn_lynx->ContextLoad(f);
    fclose(f);
    printf("handy: load %s\n", ok ? "OK" : "FAILED");
    return ok;
}

static bool handy_save_exists(void)
{
    struct stat st;
    return hn_save_path[0] != '\0' && stat(hn_save_path, &st) == 0;
}

static bool handy_delete_save(void)
{
    return hn_save_path[0] != '\0' && remove(hn_save_path) == 0;
}

/* ===================================================================
 * In-game menu (drawn by the launcher)
 * =================================================================== */
static void handy_game_menu(void)
{
    const char *note = NULL;
    for (;;) {
        switch (odroid_emu_menu(note, handy_save_exists() ? EMU_MENU_HAS_SAVE : 0)) {
        case EMU_MENU_SAVE:
            note = handy_save_state() ? "saved" : "save failed";
            continue;
        case EMU_MENU_DELETE:
            note = handy_delete_save() ? "save deleted" : "delete failed";
            continue;
        case EMU_MENU_LOAD:
            handy_load_state();
            return;
        case EMU_MENU_RESTART:
            /* Power cycle: keep the EEPROM (saved, then read back after the reset) */
            hn_lynx->SaveEEPROM();
            hn_lynx->Reset();
            hn_lynx->mEEPROM->Load();
            return;
        case EMU_MENU_EXIT:
            hn_exitRequested = true;
            return;
        default:
            return;
        }
    }
}

/* ===================================================================
 * Input processing
 * =================================================================== */
static void hn_process_input(void)
{
    odroid_gamepad_state state;
    odroid_input_gamepad_read(&state);

    unsigned buttons = 0;
    if (state.values[ODROID_INPUT_A])      buttons |= BUTTON_A;
    if (state.values[ODROID_INPUT_B])      buttons |= BUTTON_B;
    if (state.values[ODROID_INPUT_LEFT])   buttons |= BUTTON_LEFT;
    if (state.values[ODROID_INPUT_RIGHT])  buttons |= BUTTON_RIGHT;
    if (state.values[ODROID_INPUT_UP])     buttons |= BUTTON_UP;
    if (state.values[ODROID_INPUT_DOWN])   buttons |= BUTTON_DOWN;
    if (state.values[ODROID_INPUT_START])  buttons |= BUTTON_PAUSE;
    if (state.values[ODROID_INPUT_SELECT]) buttons |= BUTTON_OPT1;

    hn_lynx->SetButtonData(buttons);

    /* MENU button → in-game menu */
    static bool prev_menu = false;
    if (state.values[ODROID_INPUT_MENU] && !prev_menu)
    {
        hn_video_stop();
        handy_game_menu();
        prev_menu = true;          /* a still-held button doesn't reopen it */
        if (hn_exitRequested) return;
        hn_video_start();
        return;
    }
    prev_menu = state.values[ODROID_INPUT_MENU];

    /* VOLUME button → the launcher's volume dialog */
    static bool prev_vol = false;
    if (state.values[ODROID_INPUT_VOLUME] && !prev_vol)
    {
        hn_video_stop();
        odroid_emu_volume();
        hn_video_start();
        prev_vol = true;
        return;
    }
    prev_vol = state.values[ODROID_INPUT_VOLUME];
}

/* ===================================================================
 * Main entry point
 * =================================================================== */
extern "C" void handy_run(const char *rom_path)
{
    printf("handy_run: Starting Atari Lynx emulator\n");
    printf("handy_run: ROM = %s\n", rom_path);

    hn_exitRequested = false;
    hn_currentFB = 0;
    fps_frame_count = 0;
    fps_last_time = esp_timer_get_time();

    /* Allocate framebuffers in PSRAM */
    for (int i = 0; i < 2; i++) {
        if (!hn_framebuffer[i]) {
            hn_framebuffer[i] = (uint16_t *)heap_caps_malloc(
                LYNX_WIDTH * LYNX_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
            if (!hn_framebuffer[i]) {
                printf("handy_run: ERROR allocating framebuffer %d\n", i);
                return;
            }
        }
        memset(hn_framebuffer[i], 0, LYNX_WIDTH * LYNX_HEIGHT * sizeof(uint16_t));
    }

    /* Build BIOS ROM path — look for lynxboot.img next to ROM or in /sd/roms/lynx/ */
    char rom_dir[256];
    strncpy(rom_dir, rom_path, sizeof(rom_dir) - 1);
    rom_dir[sizeof(rom_dir) - 1] = '\0';
    char *slash = strrchr(rom_dir, '/');
    if (slash) *slash = '\0';

    char bios_path[256];
    snprintf(bios_path, sizeof(bios_path), "%s/lynxboot.img", rom_dir);
    FILE *btest = fopen(bios_path, "rb");
    if (!btest) {
        /* Try common location */
        snprintf(bios_path, sizeof(bios_path), "/sd/roms/lynx/lynxboot.img");
        btest = fopen(bios_path, "rb");
    }
    if (btest) {
        fclose(btest);
        printf("handy_run: BIOS found at %s\n", bios_path);
    } else {
        printf("handy_run: WARNING — no lynxboot.img BIOS found, HLE will be used\n");
        bios_path[0] = '\0';
    }

    /* Create the Lynx system */
    printf("handy_run: Creating CSystem...\n");
    hn_lynx = new CSystem(rom_path, bios_path, true);
    if (!hn_lynx) {
        printf("handy_run: ERROR creating CSystem\n");
        return;
    }

    /* Init audio */
    gAudioEnabled = true;
    gAudioBufferPointer = 0;
    odroid_audio_init(AUDIO_SAMPLE_RATE);

    /* Build save path */
    build_save_path(rom_path);

    /* Create video queue and task BEFORE DisplaySetAttributes — the callback
       fires during SetAttributes and needs the queue to exist already */
    hn_vidQueue = xQueueCreate(1, sizeof(uint16_t *));
    xTaskCreatePinnedToCore(hn_videoTask, "hn_vidTask", 4096, NULL, 5, &hn_videoTaskHandle, 1);

    /* Set up display callback: 16bpp RGB565, pitch=320 bytes per line */
    hn_lynx->DisplaySetAttributes(MIKIE_NO_ROTATE, MIKIE_PIXEL_FORMAT_16BPP_565,
                                  LYNX_WIDTH * 2, hn_display_callback, 0);

    printf("handy_run: Entering main loop\n");

    /* ===== Main emulation loop ===== */
    while (!hn_exitRequested)
    {
        /* Run a full frame: tight Update() loop until display callback fires */
        hn_frameDone = false;
        int64_t t0 = esp_timer_get_time();
        while (!hn_frameDone && !hn_exitRequested) {
            hn_lynx->Update();
        }
        s_emu_time_acc += esp_timer_get_time() - t0;

        /* Poll input once per frame (was per-Update — thousands of times) */
        hn_process_input();
    }

    printf("handy_run: Exiting...\n");

    /* Stop video task */
    if (hn_videoTaskIsRunning) {
        uint16_t *discard;
        while (xQueueReceive(hn_vidQueue, &discard, 0) == pdTRUE) {}
        uint16_t *stop = NULL;
        xQueueOverwrite(hn_vidQueue, &stop);
        int timeout = 500;
        while (hn_videoTaskIsRunning && --timeout > 0) vTaskDelay(1);
    }
    if (hn_vidQueue) {
        vQueueDelete(hn_vidQueue);
        hn_vidQueue = NULL;
    }

    /* Clean up */
    odroid_audio_terminate();

    if (hn_lynx) {
        hn_lynx->SaveEEPROM();
        delete hn_lynx;
        hn_lynx = NULL;
    }

    /* Free buffers */
    for (int i = 0; i < 2; i++) {
        if (hn_framebuffer[i]) {
            free(hn_framebuffer[i]);
            hn_framebuffer[i] = NULL;
        }
    }

    printf("handy_run: Done\n");
}
