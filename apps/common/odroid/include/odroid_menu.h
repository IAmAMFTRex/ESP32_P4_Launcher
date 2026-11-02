/*
 * The emulators' in-game menu, drawn by the launcher (papp_ui_menu) so it looks
 * like the HOME menu and is the same in every emulator.
 *
 * Items, in this order: Resume game, Save game, [Load saved game], Restart game,
 * [Delete save], Volume, Exit game. Volume opens the launcher's volume dialog and
 * comes back to the menu. The game's sound is paused while the menu is up, and the
 * press that opened or closed it is held back from the game.
 *
 * A core runs it as a loop, like this:
 *
 *   const char *note = NULL;
 *   for (;;) {
 *       switch (odroid_emu_menu(note, has_save() ? EMU_MENU_HAS_SAVE : 0)) {
 *       case EMU_MENU_SAVE:    note = save() ? "saved" : "save failed"; continue;
 *       case EMU_MENU_DELETE:  note = del() ? "save deleted" : "delete failed"; continue;
 *       case EMU_MENU_LOAD:    load(); return;
 *       case EMU_MENU_RESTART: reset(); return;
 *       case EMU_MENU_EXIT:    quit(); return;
 *       default:               return;                 // Resume, B, or a tap beside the menu
 *       }
 *   }
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EMU_MENU_RESUME,
    EMU_MENU_SAVE,
    EMU_MENU_LOAD,
    EMU_MENU_RESTART,
    EMU_MENU_DELETE,
    EMU_MENU_EXIT,
} emu_menu_action_t;

#define EMU_MENU_HAS_SAVE    (1u << 0)   /* a save exists: Load and Delete are offered */
#define EMU_MENU_NO_SAVE     (1u << 1)   /* the core can't save: no Save, Load or Delete */
#define EMU_MENU_NO_RESTART  (1u << 2)

/* note: shown in the title ("Game menu - saved"), or NULL */
emu_menu_action_t odroid_emu_menu(const char *note, unsigned flags);

/* The launcher's volume dialog (the VOLUME button: Y or the right screen edge) */
void odroid_emu_volume(void);

#ifdef __cplusplus
}
#endif
