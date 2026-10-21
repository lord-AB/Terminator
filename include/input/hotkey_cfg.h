/*
 * hotkey_cfg.h — ~/.terminatorrc parser and KeySym-to-Action mapper
 */
#ifndef INPUT_HOTKEY_CFG_H
#define INPUT_HOTKEY_CFG_H

#include "common.h"
#include <X11/Xlib.h>
#include <X11/keysym.h>

typedef enum {
    HOTKEY_ACTION_NONE = 0,
    HOTKEY_ACTION_MOVE_LEFT,
    HOTKEY_ACTION_MOVE_RIGHT,
    HOTKEY_ACTION_MOVE_HOME,
    HOTKEY_ACTION_MOVE_END,
    HOTKEY_ACTION_HISTORY_PREV,
    HOTKEY_ACTION_HISTORY_NEXT,
    HOTKEY_ACTION_DELETE_TO_START,
    HOTKEY_ACTION_DELETE_TO_END,
    HOTKEY_ACTION_DELETE_PREV_WORD,
    HOTKEY_ACTION_CLEAR_SCREEN,
    HOTKEY_ACTION_EXIT_OR_EOF,
    HOTKEY_ACTION_COPY,
    HOTKEY_ACTION_PASTE,
    HOTKEY_ACTION_COUNT
} HotkeyAction;

typedef struct {
    HotkeyAction action;
    KeySym       keysym;
    unsigned int modifiers;  /* ControlMask, ShiftMask, Mod1Mask */
    char         name[32];   /* Action name in config, e.g. "move_home" */
} HotkeyBinding;

typedef struct {
    HotkeyBinding bindings[HOTKEY_ACTION_COUNT];
    char          config_path[MAX_PATH_LEN];
    int           loaded;
} HotkeyConfig;

/*
 * Initialize config with default bash-style hotkeys.
 */
void hotkey_cfg_init_defaults(HotkeyConfig *cfg);

/*
 * Load and parse ~/.terminatorrc (or specified path).
 * Detects invalid entries and conflicting bindings, recording error messages
 * and falling back to default bindings for affected actions.
 * Returns 0 on clean load, >0 (count of errors/conflicts) on fallback, or -1 if file missing.
 */
int hotkey_cfg_load(HotkeyConfig *cfg, const char *path, char *err_msg, size_t err_sz);

/*
 * Match KeySym and modifier state against configured hotkeys.
 * Returns the matching HotkeyAction, or HOTKEY_ACTION_NONE if no match.
 */
HotkeyAction hotkey_cfg_match(const HotkeyConfig *cfg, KeySym keysym, unsigned int state);

/*
 * Convert HotkeyAction enum to textual name.
 */
const char *hotkey_action_name(HotkeyAction action);

/*
 * Convert textual action name to HotkeyAction enum.
 */
HotkeyAction hotkey_action_from_name(const char *name);

#endif /* INPUT_HOTKEY_CFG_H */
