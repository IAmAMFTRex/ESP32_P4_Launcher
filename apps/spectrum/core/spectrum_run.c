/*
 * spectrum_run.c — Entry point for ZX Spectrum emulator on ESP32-P4
 *
 * Loads a .z80/.sna snapshot, runs the ZX Spectrum emulation loop,
 * handles the in-game menu (X button), virtual keyboard (Y button),
 * save/load states, and returns on Exit Game.
 */

#include "spectrum_run.h"
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

/* Spectrum core headers */
#include "config.h"
#include "spmain.h"
#include "snapshot.h"
#include "spperif.h"
#include "spscr.h"
#include "spsound.h"
#include "sptape.h"
#include "sptiming.h"
#include "spkey.h"
#include "ay_sound.h"
#include "vgascr.h"
#include "z80.h"
#include "misc.h"

/* Odroid platform */
#include "odroid_settings.h"
#include "odroid_audio.h"
#include "odroid_input.h"
#include "odroid_system.h"
#include "odroid_display.h"
#include "odroid_sdcard.h"
#include "odroid_menu.h"

#define ZX_FB_WIDTH   320
#define ZX_FB_HEIGHT  240
#define AUDIO_SAMPLE_RATE  15600

/* Shared framebuffer: written by spscr.c, read by video task */
uint16_t *sp_framebuffer = NULL;

/* Menu request flag set by vgakey.c */
extern volatile int sp_menu_request;

/* Frame-ready flag set by spmain.c after translate_screen */
extern volatile bool sp_frame_ready;

/* Emulator globals we need access to */
extern int endofsingle;
extern int sp_nosync;
extern int my_lastborder;
extern volatile int screen_visible;
extern int showframe;

static volatile bool zx_quit_flag = false;
static volatile bool videoTaskIsRunning = false;
static volatile bool zx_video_busy = false;   /* the video task is presenting a frame */

static QueueHandle_t zx_vidQueue;
static TaskHandle_t zx_videoTaskHandle;

/* Double-buffer for non-blocking display (RGB565, 320×240) */
static uint16_t *lcdfb[2] = { NULL, NULL };
static int lcdfb_write_idx = 0;

/* ─── 5×7 Bitmap Font ──────────────────────────────────────────── */
static const uint8_t zx_font5x7[][7] = {
    /* ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 'A' */ {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* 'B' */ {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    /* 'C' */ {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    /* 'D' */ {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    /* 'E' */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    /* 'F' */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    /* 'G' */ {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},
    /* 'H' */ {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* 'I' */ {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    /* 'J' */ {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    /* 'K' */ {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    /* 'L' */ {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    /* 'M' */ {0x11,0x1B,0x15,0x11,0x11,0x11,0x11},
    /* 'N' */ {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    /* 'O' */ {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* 'P' */ {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    /* 'Q' */ {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    /* 'R' */ {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    /* 'S' */ {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E},
    /* 'T' */ {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    /* 'U' */ {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* 'V' */ {0x11,0x11,0x11,0x0A,0x0A,0x04,0x04},
    /* 'W' */ {0x11,0x11,0x11,0x11,0x15,0x1B,0x11},
    /* 'X' */ {0x11,0x0A,0x04,0x04,0x04,0x0A,0x11},
    /* 'Y' */ {0x11,0x0A,0x04,0x04,0x04,0x04,0x04},
    /* 'Z' */ {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    /* '0' */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    /* '1' */ {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    /* '2' */ {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F},
    /* '3' */ {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E},
    /* '4' */ {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    /* '5' */ {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    /* '6' */ {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    /* '7' */ {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    /* '8' */ {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    /* '9' */ {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    /* ':' */ {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
    /* '%' */ {0x18,0x19,0x02,0x04,0x08,0x13,0x03},
    /* '/' */ {0x01,0x02,0x02,0x04,0x08,0x08,0x10},
    /* '>' */ {0x10,0x08,0x04,0x02,0x04,0x08,0x10},
};

static int zx_font_index(char c) {
    if (c == ' ') return 0;
    if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 1 + (c - 'a');
    if (c >= '0' && c <= '9') return 27 + (c - '0');
    if (c == ':') return 37;
    if (c == '%') return 38;
    if (c == '/') return 39;
    if (c == '>') return 40;
    return 0;
}

static void zx_draw_char(uint16_t *fb, int x, int y, char c, uint16_t color) {
    int idx = zx_font_index(c);
    for (int row = 0; row < 7; row++) {
        uint8_t bits = zx_font5x7[idx][row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x10 >> col)) {
                int px = x + col, py = y + row;
                if (px >= 0 && px < ZX_FB_WIDTH && py >= 0 && py < ZX_FB_HEIGHT)
                    fb[py * ZX_FB_WIDTH + px] = color;
            }
        }
    }
}

static void zx_draw_string(uint16_t *fb, int x, int y, const char *str, uint16_t color) {
    while (*str) { zx_draw_char(fb, x, y, *str++, color); x += 6; }
}

/* ─── Virtual Keyboard Overlay ─────────────────────────────────── */
extern volatile int zx_vkb_active;
extern volatile int zx_vkb_inject;
extern volatile int zx_vkb_cs;
extern volatile int zx_vkb_ss;

static int vkb_row = 0, vkb_col = 0;
static int vkb_nav_delay = 0;

#define VKB_COLS     10
#define VKB_ROWS     4
#define VKB_KEY_W    32
#define VKB_KEY_H    14
#define VKB_Y_START  (ZX_FB_HEIGHT - VKB_ROWS * VKB_KEY_H - 4)
#define VKB_CS_SCAN  42
#define VKB_SS_SCAN  54

static const char *vkb_labels[VKB_ROWS][VKB_COLS] = {
    { "1","2","3","4","5","6","7","8","9","0" },
    { "Q","W","E","R","T","Y","U","I","O","P" },
    { "A","S","D","F","G","H","J","K","L","EN" },
    { "CS","Z","X","C","V","B","N","M","SS","SP" }
};

/* Scancodes matching vgakey.c map[40] */
static const int vkb_scancodes[VKB_ROWS][VKB_COLS] = {
    {  2,  3,  4,  5,  6,  7,  8,  9, 10, 11 },
    { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25 },
    { 30, 31, 32, 33, 34, 35, 36, 37, 38, 28 },
    { 42, 44, 45, 46, 47, 48, 49, 50, 54, 57 }
};

static void zx_draw_vkb(uint16_t *fb) {
    if (!zx_vkb_active) return;

    int y0 = VKB_Y_START;

    /* Black background strip */
    for (int y = y0 - 1; y < y0 + VKB_ROWS * VKB_KEY_H + 1 && y < ZX_FB_HEIGHT; y++)
        for (int x = 0; x < ZX_FB_WIDTH; x++)
            fb[y * ZX_FB_WIDTH + x] = 0x0000;

    for (int r = 0; r < VKB_ROWS; r++) {
        for (int c = 0; c < VKB_COLS; c++) {
            int kx = c * VKB_KEY_W;
            int ky = y0 + r * VKB_KEY_H;
            bool sel = (r == vkb_row && c == vkb_col);
            int sc = vkb_scancodes[r][c];
            bool shift_on = (sc == VKB_CS_SCAN && zx_vkb_cs) ||
                            (sc == VKB_SS_SCAN && zx_vkb_ss);

            uint16_t bg = sel ? 0xFFE0 : (shift_on ? 0x07E0 : 0x0010);
            uint16_t fg = (sel || shift_on) ? 0x0000 : 0xFFFF;

            /* Fill cell */
            for (int y = ky + 1; y < ky + VKB_KEY_H && y < ZX_FB_HEIGHT; y++)
                for (int x = kx + 1; x < kx + VKB_KEY_W && x < ZX_FB_WIDTH; x++)
                    fb[y * ZX_FB_WIDTH + x] = bg;

            /* Grid border */
            for (int x = kx; x < kx + VKB_KEY_W && x < ZX_FB_WIDTH; x++)
                fb[ky * ZX_FB_WIDTH + x] = 0x4208;
            for (int y = ky; y < ky + VKB_KEY_H && y < ZX_FB_HEIGHT; y++)
                fb[y * ZX_FB_WIDTH + kx] = 0x4208;

            /* Center label */
            const char *lbl = vkb_labels[r][c];
            int len = 0; for (const char *p = lbl; *p; p++) len++;
            int tx = kx + (VKB_KEY_W - len * 6) / 2;
            int ty = ky + (VKB_KEY_H - 7) / 2;
            zx_draw_string(fb, tx, ty, lbl, fg);
        }
    }
}

/* ─── Save Path ────────────────────────────────────────────────── */
static char zx_save_path[300];

static void zx_get_save_path(const char *rom_path) {
    const char *slash = strrchr(rom_path, '/');
    const char *fname = slash ? slash + 1 : rom_path;
    mkdir("/sd/odroid", 0755);
    mkdir("/sd/odroid/data", 0755);
    mkdir("/sd/odroid/data/spectrum", 0755);
    /* Append .sav to full filename (e.g. game.z80.sav) to match launcher convention */
    snprintf(zx_save_path, sizeof(zx_save_path), "/sd/odroid/data/spectrum/%s.sav", fname);
}

static bool zx_save_exists(void) {
    struct stat st;
    return stat(zx_save_path, &st) == 0;
}

/* ─── Save/Load State ──────────────────────────────────────────── */
static bool zx_SaveState(void) {
    FILE *f = fopen(zx_save_path, "wb");
    if (!f) return false;
    snsh_save(f, SN_Z80);
    fclose(f);
    printf("ZX: State saved to %s\n", zx_save_path);
    return true;
}

static bool zx_LoadState(void) {
    if (!zx_save_exists()) return false;
    load_snapshot_file_type(zx_save_path, -1);
    printf("ZX: State loaded from %s\n", zx_save_path);
    return true;
}

/* ─── Video Task ───────────────────────────────────────────────── */
static void zx_video_task(void *arg)
{
    videoTaskIsRunning = true;
    while (1) {
        uint16_t *fb = NULL;
        xQueuePeek(zx_vidQueue, &fb, portMAX_DELAY);
        zx_video_busy = true;   /* before the frame leaves the queue (zx_video_idle) */
        xQueueReceive(zx_vidQueue, &fb, 0);
        if (fb == NULL) { zx_video_busy = false; break; }  /* quit signal */

        /* PPA does 2× scale + 270° rotation in one HW op */
        ili9341_write_frame_rgb565_ex(fb, false);
        zx_video_busy = false;
    }
    videoTaskIsRunning = false;
    vTaskDelete(NULL);
}

/* ─── In-Game Menu (drawn by the launcher) ────────────────────── */
static const char *zx_rom_path_saved;  /* set at start of spectrum_run() */

static bool zx_delete_save(void) {
    if (!zx_save_exists()) return false;
    if (unlink(zx_save_path) == 0) {
        printf("ZX: Deleted save %s\n", zx_save_path);
        return true;
    }
    return false;
}

/* Waits until the video task has shown its last frame: the launcher owns the
 * display while the menu is up, and the main loop is the only frame producer */
static void zx_video_idle(void) {
    for (int t = 0; t < 100 && (uxQueueMessagesWaiting(zx_vidQueue) || zx_video_busy); t++)
        vTaskDelay(pdMS_TO_TICKS(5));
}

static void zx_game_menu(void) {
    zx_video_idle();
    const char *note = NULL;
    for (;;) {
        switch (odroid_emu_menu(note, zx_save_exists() ? EMU_MENU_HAS_SAVE : 0)) {
        case EMU_MENU_SAVE:
            note = zx_SaveState() ? "saved" : "save failed";
            continue;
        case EMU_MENU_DELETE:
            note = zx_delete_save() ? "save deleted" : "delete failed";
            continue;
        case EMU_MENU_LOAD:
            zx_LoadState();
            return;
        case EMU_MENU_RESTART:
            printf("ZX: Restarting from original ROM\n");
            load_snapshot_file_type(zx_rom_path_saved, -1);
            return;
        case EMU_MENU_EXIT:
            /* Exit without auto-saving — start fresh next time */
            zx_quit_flag = true;
            return;
        default:
            return;
        }
    }
}

/* ─── Main Entry Point ─────────────────────────────────────────── */
void spectrum_run(const char *rom_path)
{
    printf("spectrum_run: starting ZX Spectrum emulator, ROM=%s\n", rom_path);
    zx_rom_path_saved = rom_path;

    /* Allocate shared framebuffer in PSRAM (320×240 RGB565) */
    sp_framebuffer = (uint16_t *)heap_caps_malloc(ZX_FB_WIDTH * ZX_FB_HEIGHT * 2, MALLOC_CAP_SPIRAM);
    if (!sp_framebuffer) { printf("ZX: framebuffer alloc failed\n"); return; }
    memset(sp_framebuffer, 0, ZX_FB_WIDTH * ZX_FB_HEIGHT * 2);

    /* Double-buffer for display output (zero-init to avoid white flash) */
    for (int i = 0; i < 2; i++) {
        lcdfb[i] = (uint16_t *)heap_caps_calloc(ZX_FB_WIDTH * ZX_FB_HEIGHT, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!lcdfb[i]) abort();
    }
    lcdfb_write_idx = 0;

    /* Build save path */
    zx_get_save_path(rom_path);

    /* Init audio (beeper at ~15.6kHz) + AY-3-8912 PSG */
    odroid_audio_init(AUDIO_SAMPLE_RATE);
    ay_init(AUDIO_SAMPLE_RATE);

    /* Video queue + task */
    zx_vidQueue = xQueueCreate(1, sizeof(uint16_t *));
    xTaskCreatePinnedToCore(zx_video_task, "zxVideo", 4096, NULL, 5, &zx_videoTaskHandle, 1);

    /* Mount SD if needed */
    odroid_sdcard_open("/sd");

    /* ─── Spectrum emulator init ─────────────────────────────── */
    sp_init();

    /* Load the snapshot file */
    load_snapshot_file_type(rom_path, -1);

    /* Check for saved state and resume */
    if (odroid_settings_StartAction_get() == ODROID_START_ACTION_RESTART && zx_save_exists()) {
        zx_LoadState();
    }
    odroid_settings_StartAction_set(ODROID_START_ACTION_NORMAL);

    /* Force border redraw */
    my_lastborder = 100;

    /* Start emulation */
    spti_init();
    init_spect_scr();
    init_spect_key();

    /* Set up the spectrum video queue (used internally by spmain.c) */
    extern QueueHandle_t sp_vidQueue;
    extern TaskHandle_t sp_videoTaskHandle;
    sp_vidQueue = xQueueCreate(1, sizeof(uint16_t *));
    sp_videoTaskHandle = NULL; /* We handle video in our own task */

    zx_quit_flag = false;
    sp_menu_request = 0;

    /* Push a black frame so the LCD doesn't show stale/uninit data */
    ili9341_write_frame_rgb565(lcdfb[0]);

    printf("ZX: Entering emulation loop\n");

    /* Run the emulation loop inline (adapted from run_singlemode) */
    int t = 0;
    int evenframe, halfsec, updateframe;
    extern qbyte sp_int_ctr;
    sp_int_ctr = 0;
    endofsingle = 0;

    uint64_t startTime, stopTime;
    uint64_t totalElapsedTime = 0;
    uint32_t actualFrameCount = 0;

    spti_reset();

    while (!endofsingle && !zx_quit_flag) {
        startTime = esp_timer_get_time();

        halfsec = !(sp_int_ctr % 25);
        evenframe = !(sp_int_ctr & 1);
        if (screen_visible) {
            updateframe = sp_nosync ? halfsec : !((sp_int_ctr + 1) % showframe);
        } else {
            updateframe = 0;
        }

        if (halfsec) {
            sp_flash_state = ~sp_flash_state;
            flash_change();
        }
        if (evenframe) {
            play_tape();
            sp_scline = 0;
        }

        spkb_process_events(evenframe);

        sp_updating = updateframe;

        t += CHKTICK;
        t = sp_halfframe(t, evenframe ? EVENHF : ODDHF);
        if (SPNM(load_trapped)) {
            SPNM(load_trapped) = 0;
            DANM(haltstate) = 0;
            qload();
        }
        z80_interrupt(0xFF);
        sp_int_ctr++;

        if (!evenframe) rec_tape();

        if (!sp_nosync) {
            if (updateframe) {
                update_screen();
                sp_border_update >>= 1;
                sp_imag_vert = sp_imag_horiz = 0;
                translate_screen();
            }
            if (!sound_avail) spti_wait();
            play_sound(evenframe);
        } else {
            if (updateframe) {
                update_screen();
                sp_border_update >>= 1;
                sp_imag_vert = sp_imag_horiz = 0;
                translate_screen();
            }
        }

        /* After translate_screen, push framebuffer to display */
        if (updateframe) {
            uint16_t *dst = lcdfb[lcdfb_write_idx];
            memcpy(dst, sp_framebuffer, ZX_FB_WIDTH * ZX_FB_HEIGHT * 2);

            /* Virtual keyboard overlay */
            zx_draw_vkb(dst);

            /* No CPU byte-swap needed — PPA does it in hardware */

            xQueueOverwrite(zx_vidQueue, &dst);
            lcdfb_write_idx = 1 - lcdfb_write_idx;
        }

        /* X button / left screen edge = in-game menu */
        if (sp_menu_request) {
            zx_game_menu();
            sp_menu_request = 0;
            my_lastborder = 100; /* Force border redraw */
        }

        /* Y button = toggle virtual keyboard */
        {
            odroid_gamepad_state gs;
            odroid_input_gamepad_read(&gs);

            if (gs.values[ODROID_INPUT_VOLUME]) {
                zx_vkb_active = !zx_vkb_active;
                if (!zx_vkb_active) {
                    zx_vkb_inject = -1;
                    zx_vkb_cs = 0;
                    zx_vkb_ss = 0;
                }
                while (gs.values[ODROID_INPUT_VOLUME]) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    odroid_input_gamepad_read(&gs);
                }
            }

            /* Keyboard navigation when active */
            if (zx_vkb_active) {
                odroid_input_gamepad_read(&gs);

                if (vkb_nav_delay > 0) vkb_nav_delay--;

                if (vkb_nav_delay == 0) {
                    if (gs.values[ODROID_INPUT_UP])    { vkb_row = (vkb_row - 1 + VKB_ROWS) % VKB_ROWS; vkb_nav_delay = 8; }
                    if (gs.values[ODROID_INPUT_DOWN])  { vkb_row = (vkb_row + 1) % VKB_ROWS; vkb_nav_delay = 8; }
                    if (gs.values[ODROID_INPUT_LEFT])  { vkb_col = (vkb_col - 1 + VKB_COLS) % VKB_COLS; vkb_nav_delay = 8; }
                    if (gs.values[ODROID_INPUT_RIGHT]) { vkb_col = (vkb_col + 1) % VKB_COLS; vkb_nav_delay = 8; }
                }
                if (!gs.values[ODROID_INPUT_UP] && !gs.values[ODROID_INPUT_DOWN] &&
                    !gs.values[ODROID_INPUT_LEFT] && !gs.values[ODROID_INPUT_RIGHT])
                    vkb_nav_delay = 0;

                /* A = press selected key */
                int sc = vkb_scancodes[vkb_row][vkb_col];
                if (gs.values[ODROID_INPUT_A]) {
                    if (sc == VKB_CS_SCAN) {
                        zx_vkb_cs = !zx_vkb_cs;
                        while (gs.values[ODROID_INPUT_A]) {
                            vTaskDelay(pdMS_TO_TICKS(20));
                            odroid_input_gamepad_read(&gs);
                        }
                    } else if (sc == VKB_SS_SCAN) {
                        zx_vkb_ss = !zx_vkb_ss;
                        while (gs.values[ODROID_INPUT_A]) {
                            vTaskDelay(pdMS_TO_TICKS(20));
                            odroid_input_gamepad_read(&gs);
                        }
                    } else {
                        zx_vkb_inject = sc;
                    }
                } else {
                    zx_vkb_inject = -1;
                }

                /* B = close keyboard */
                if (gs.values[ODROID_INPUT_B]) {
                    zx_vkb_active = 0;
                    zx_vkb_inject = -1;
                    zx_vkb_cs = 0;
                    zx_vkb_ss = 0;
                    while (gs.values[ODROID_INPUT_B]) {
                        vTaskDelay(pdMS_TO_TICKS(20));
                        odroid_input_gamepad_read(&gs);
                    }
                }
            }
        }

        /* FPS counter */
        stopTime = esp_timer_get_time();
        totalElapsedTime += (stopTime - startTime);
        actualFrameCount++;
        if (actualFrameCount >= 120) {
            float secs = totalElapsedTime / 1000000.0f;
            printf("ZX FPS:%.1f\n", actualFrameCount / secs);
            totalElapsedTime = 0;
            actualFrameCount = 0;
        }
    }

    /* ─── Cleanup ─────────────────────────────────────────────── */
    printf("ZX: Exiting emulation\n");

    /* Stop video task */
    uint16_t *null_ptr = NULL;
    xQueueOverwrite(zx_vidQueue, &null_ptr);
    { int timeout = 500; while (videoTaskIsRunning && --timeout > 0) vTaskDelay(1); }

    /* Free resources */
    if (sp_framebuffer) { heap_caps_free(sp_framebuffer); sp_framebuffer = NULL; }
    for (int i = 0; i < 2; i++) {
        if (lcdfb[i]) { heap_caps_free(lcdfb[i]); lcdfb[i] = NULL; }
    }
    if (zx_vidQueue) { vQueueDelete(zx_vidQueue); zx_vidQueue = NULL; }

    /* Return to caller — app_main() will call app_return_to_launcher() */
}
