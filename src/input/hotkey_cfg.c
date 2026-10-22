/*
 * hotkey_cfg.c — Implementation of ~/.terminatorrc parser and KeySym-to-Action mapper
 */
#include "input/hotkey_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const struct {
    HotkeyAction action;
    const char  *name;
    KeySym       default_keysym;
    unsigned int default_mods;
} g_action_defs[HOTKEY_ACTION_COUNT] = {
    { HOTKEY_ACTION_NONE,             "none",             0,            0 },
    { HOTKEY_ACTION_MOVE_LEFT,         "move_left",        XK_Left,      0 },
    { HOTKEY_ACTION_MOVE_RIGHT,        "move_right",       XK_Right,     0 },
    { HOTKEY_ACTION_MOVE_HOME,         "move_home",        XK_a,         ControlMask },
    { HOTKEY_ACTION_MOVE_END,          "move_end",         XK_e,         ControlMask },
    { HOTKEY_ACTION_HISTORY_PREV,      "history_prev",     XK_Up,        0 },
    { HOTKEY_ACTION_HISTORY_NEXT,      "history_next",     XK_Down,      0 },
    { HOTKEY_ACTION_DELETE_TO_START,   "delete_to_start",  XK_u,         ControlMask },
    { HOTKEY_ACTION_DELETE_TO_END,     "delete_to_end",    XK_k,         ControlMask },
    { HOTKEY_ACTION_DELETE_PREV_WORD,  "delete_prev_word", XK_w,         ControlMask },
    { HOTKEY_ACTION_CLEAR_SCREEN,      "clear_screen",     XK_l,         ControlMask },
    { HOTKEY_ACTION_EXIT_OR_EOF,       "exit_or_eof",      XK_d,         ControlMask },
    { HOTKEY_ACTION_COPY,              "copy",             XK_c,         ControlMask | ShiftMask },
    { HOTKEY_ACTION_PASTE,             "paste",            XK_v,         ControlMask | ShiftMask },
    { HOTKEY_ACTION_SEARCH_HISTORY,    "search_history",   XK_r,         ControlMask }
};

const char *hotkey_action_name(HotkeyAction action) {
    if (action >= 0 && action < HOTKEY_ACTION_COUNT) {
        return g_action_defs[action].name;
    }
    return "unknown";
}

HotkeyAction hotkey_action_from_name(const char *name) {
    if (!name) return HOTKEY_ACTION_NONE;

    /* Normalize string (lowercase and convert hyphens to underscores) */
    char norm[64];
    size_t i = 0;
    while (name[i] && i < sizeof(norm) - 1) {
        char ch = (char)tolower((unsigned char)name[i]);
        if (ch == '-') ch = '_';
        norm[i] = ch;
        i++;
    }
    norm[i] = '\0';

    for (int a = 1; a < HOTKEY_ACTION_COUNT; a++) {
        if (strcmp(norm, g_action_defs[a].name) == 0) {
            return g_action_defs[a].action;
        }
    }
    return HOTKEY_ACTION_NONE;
}

void hotkey_cfg_init_defaults(HotkeyConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(HotkeyConfig));

    for (int a = 0; a < HOTKEY_ACTION_COUNT; a++) {
        cfg->bindings[a].action = g_action_defs[a].action;
        cfg->bindings[a].keysym = g_action_defs[a].default_keysym;
        cfg->bindings[a].modifiers = g_action_defs[a].default_mods;
        strncpy(cfg->bindings[a].name, g_action_defs[a].name, sizeof(cfg->bindings[a].name) - 1);
    }
    cfg->loaded = 1;
}

/* Helper: parse a single KeySym token */
static KeySym parse_keysym_token(const char *tok) {
    if (!tok || !*tok) return 0;

    /* Single characters */
    if (tok[1] == '\0') {
        char c = (char)tolower((unsigned char)tok[0]);
        return (KeySym)c;
    }

    /* Named keys */
    if (strcasecmp(tok, "Left") == 0) return XK_Left;
    if (strcasecmp(tok, "Right") == 0) return XK_Right;
    if (strcasecmp(tok, "Up") == 0) return XK_Up;
    if (strcasecmp(tok, "Down") == 0) return XK_Down;
    if (strcasecmp(tok, "Home") == 0) return XK_Home;
    if (strcasecmp(tok, "End") == 0) return XK_End;
    if (strcasecmp(tok, "BackSpace") == 0) return XK_BackSpace;
    if (strcasecmp(tok, "Delete") == 0) return XK_Delete;
    if (strcasecmp(tok, "Return") == 0 || strcasecmp(tok, "Enter") == 0) return XK_Return;
    if (strcasecmp(tok, "PageUp") == 0 || strcasecmp(tok, "Page_Up") == 0) return XK_Page_Up;
    if (strcasecmp(tok, "PageDown") == 0 || strcasecmp(tok, "Page_Down") == 0) return XK_Page_Down;
    if (strcasecmp(tok, "Tab") == 0) return XK_Tab;
    if (strcasecmp(tok, "Space") == 0) return XK_space;
    if (strcasecmp(tok, "Escape") == 0 || strcasecmp(tok, "Esc") == 0) return XK_Escape;

    /* Fallback to XStringToKeysym */
    KeySym ks = XStringToKeysym(tok);
    if (ks != NoSymbol) return ks;

    return 0;
}

/* Parse a key combination string like "Ctrl+Shift+C" or "Left" */
static int parse_key_combo(const char *str, KeySym *out_keysym, unsigned int *out_mods) {
    if (!str || !out_keysym || !out_mods) return -1;

    unsigned int mods = 0;
    KeySym ks = 0;

    char buf[128];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(buf, "+", &saveptr);
    while (token) {
        while (*token && isspace((unsigned char)*token)) token++;
        char *end = token + strlen(token);
        while (end > token && isspace((unsigned char)*(end - 1))) end--;
        *end = '\0';

        if (strcasecmp(token, "Ctrl") == 0 || strcasecmp(token, "Control") == 0) {
            mods |= ControlMask;
        } else if (strcasecmp(token, "Shift") == 0) {
            mods |= ShiftMask;
        } else if (strcasecmp(token, "Alt") == 0 || strcasecmp(token, "Mod1") == 0 || strcasecmp(token, "Option") == 0) {
            mods |= Mod1Mask;
        } else if (strcasecmp(token, "Cmd") == 0 || strcasecmp(token, "Command") == 0 ||
                   strcasecmp(token, "Super") == 0 || strcasecmp(token, "Mod4") == 0) {
            mods |= Mod4Mask;
        } else {
            /* Must be the base key */
            ks = parse_keysym_token(token);
            if (ks == 0) {
                return -1; /* Unknown key */
            }
        }
        token = strtok_r(NULL, "+", &saveptr);
    }

    if (ks == 0) return -1;

    /* Normalize letter keysym to lowercase */
    if (ks >= 'A' && ks <= 'Z') {
        ks = ks - 'A' + 'a';
    }

    *out_keysym = ks;
    *out_mods = mods;
    return 0;
}

static KeySym normalize_keysym(KeySym ks) {
    if (ks >= 'A' && ks <= 'Z') {
        return ks - 'A' + 'a';
    }
    if (ks >= XK_A && ks <= XK_Z) {
        return ks - XK_A + XK_a;
    }
    return ks;
}

int hotkey_cfg_load(HotkeyConfig *cfg, const char *path, char *err_msg, size_t err_sz) {
    if (!cfg) return -1;

    /* Always start with defaults */
    hotkey_cfg_init_defaults(cfg);

    char filepath[MAX_PATH_LEN];
    if (path && path[0]) {
        strncpy(filepath, path, sizeof(filepath) - 1);
        filepath[sizeof(filepath) - 1] = '\0';
    } else {
        const char *home = getenv("HOME");
        if (!home) home = ".";
        snprintf(filepath, sizeof(filepath), "%s/.terminatorrc", home);
    }

    strncpy(cfg->config_path, filepath, sizeof(cfg->config_path) - 1);
    cfg->config_path[sizeof(cfg->config_path) - 1] = '\0';

    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        /* Config file does not exist — clean fallback to defaults */
        return -1;
    }

    char line[512];
    int line_num = 0;
    int error_count = 0;
    size_t err_len = 0;
    if (err_msg && err_sz > 0) err_msg[0] = '\0';

    while (fgets(line, sizeof(line), fp)) {
        line_num++;

        /* Strip trailing newline and whitespace */
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == ';' || *p == '\0') {
            continue; /* Skip comments and blank lines */
        }

        /* Expect format: action = key_combo */
        char *eq = strchr(p, '=');
        if (!eq) {
            error_count++;
            if (err_msg && err_len < err_sz - 128) {
                int n = snprintf(err_msg + err_len, err_sz - err_len,
                                 "terminator: %s:%d: syntax error, missing '='\n",
                                 filepath, line_num);
                if (n > 0) err_len += (size_t)n;
            }
            continue;
        }

        *eq = '\0';
        char *act_name = p;
        char *key_str = eq + 1;

        /* Trim act_name */
        char *end = act_name + strlen(act_name);
        while (end > act_name && isspace((unsigned char)*(end - 1))) end--;
        *end = '\0';

        /* Trim key_str */
        while (*key_str && isspace((unsigned char)*key_str)) key_str++;
        end = key_str + strlen(key_str);
        while (end > key_str && isspace((unsigned char)*(end - 1))) end--;
        *end = '\0';

        HotkeyAction action = hotkey_action_from_name(act_name);
        if (action == HOTKEY_ACTION_NONE) {
            error_count++;
            if (err_msg && err_len < err_sz - 128) {
                int n = snprintf(err_msg + err_len, err_sz - err_len,
                                 "terminator: %s:%d: unknown action '%s', falling back to default\n",
                                 filepath, line_num, act_name);
                if (n > 0) err_len += (size_t)n;
            }
            continue;
        }

        KeySym ks = 0;
        unsigned int mods = 0;
        if (parse_key_combo(key_str, &ks, &mods) != 0) {
            error_count++;
            if (err_msg && err_len < err_sz - 128) {
                int n = snprintf(err_msg + err_len, err_sz - err_len,
                                 "terminator: %s:%d: invalid key combination '%s', falling back to default\n",
                                 filepath, line_num, key_str);
                if (n > 0) err_len += (size_t)n;
            }
            continue;
        }

        /* Conflict detection: check if another action is already bound to this (ks, mods) */
        int conflict = 0;
        for (int a = 1; a < HOTKEY_ACTION_COUNT; a++) {
            if ((HotkeyAction)a == action) continue;
            if (normalize_keysym(cfg->bindings[a].keysym) == normalize_keysym(ks) &&
                cfg->bindings[a].modifiers == mods) {
                conflict = 1;
                error_count++;
                if (err_msg && err_len < err_sz - 160) {
                    int n = snprintf(err_msg + err_len, err_sz - err_len,
                                     "terminator: %s:%d: conflict: '%s' is already bound to '%s', falling back to default\n",
                                     filepath, line_num, key_str, cfg->bindings[a].name);
                    if (n > 0) err_len += (size_t)n;
                }
                break;
            }
        }

        if (!conflict) {
            cfg->bindings[action].keysym = ks;
            cfg->bindings[action].modifiers = mods;
        }
    }

    fclose(fp);
    return error_count;
}

HotkeyAction hotkey_cfg_match(const HotkeyConfig *cfg, KeySym keysym, unsigned int state) {
    if (!cfg || keysym == NoSymbol) return HOTKEY_ACTION_NONE;

    unsigned int clean_mods = state & (ControlMask | ShiftMask | Mod1Mask | Mod4Mask);
    KeySym norm_ks = normalize_keysym(keysym);

    for (int a = 1; a < HOTKEY_ACTION_COUNT; a++) {
        if (cfg->bindings[a].modifiers == clean_mods) {
            if (normalize_keysym(cfg->bindings[a].keysym) == norm_ks) {
                return cfg->bindings[a].action;
            }
        }
    }

    /* macOS Convenience Fallback: support native Cmd+C and Cmd+V for copy/paste */
    if (clean_mods == Mod4Mask) {
        if (norm_ks == 'c' || norm_ks == XK_c) return HOTKEY_ACTION_COPY;
        if (norm_ks == 'v' || norm_ks == XK_v) return HOTKEY_ACTION_PASTE;
    }

    return HOTKEY_ACTION_NONE;
}
