/*
 * screen_buf.h — Per-tab text ring-buffer with scrollback, cursor state,
 *                and input line management for the Terminator terminal emulator.
 *
 * Each tab owns one ScreenBuf (~1 MB), so it is always heap-allocated.
 */

#ifndef GUI_SCREEN_BUF_H
#define GUI_SCREEN_BUF_H

#include "common.h"   /* MAX_CMD_LEN, MAX_PATH_LEN */

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

#define SCROLLBACK_MAX  2000   /* max lines kept in ring buffer       */
#define LINE_MAXLEN      512   /* max characters per scrollback line   */

/* ------------------------------------------------------------------ */
/*  ScreenBuf structure                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    /* ---- Scrollback ring buffer ---------------------------------- */
    char    lines[SCROLLBACK_MAX][LINE_MAXLEN];
    int     count;          /* lines currently stored (0..SCROLLBACK_MAX)  */
    int     head;           /* next write position in ring                 */
    int     total;          /* total lines ever added (monotonic counter)  */
    int     scroll_offset;  /* 0 = showing bottom, positive = scrolled up */

    /* ---- Current input line -------------------------------------- */
    char    prompt[256];
    char    input[MAX_CMD_LEN];   /* editable command buffer          */
    int     input_len;            /* length of text in input[]        */
    int     cursor_pos;           /* 0..input_len                     */

    /* ---- Tab metadata -------------------------------------------- */
    char    cwd[MAX_PATH_LEN];    /* current working directory        */

    /* ---- Multiline command accumulator ---------------------------- */
    char    multiline_buf[MAX_CMD_LEN * 4];
    int     in_multiline;         /* 1 if gathering multiline command */

    /* ---- Command history browsing --------------------------------- */
    int     history_idx;          /* -1 = not browsing, >= 0 = index  */
    char    saved_input[MAX_CMD_LEN];
} ScreenBuf;

/* ------------------------------------------------------------------ */
/*  Creation / Destruction                                            */
/* ------------------------------------------------------------------ */

/* Allocate a zeroed ScreenBuf, set default prompt "$ ", and capture cwd. */
ScreenBuf *screen_buf_create(void);

/* Free a heap-allocated ScreenBuf (NULL-safe). */
void screen_buf_destroy(ScreenBuf *sb);

/* ------------------------------------------------------------------ */
/*  Scrollback line management                                        */
/* ------------------------------------------------------------------ */

/* Append a single line to the ring buffer. */
void screen_buf_add_line(ScreenBuf *sb, const char *line);

/* Split text on '\n' and add each piece as a separate line. */
void screen_buf_add_text(ScreenBuf *sb, const char *text);

/*
 * Retrieve a line by display index.
 *   display_index 0         = oldest available line
 *   display_index count-1   = newest line
 * Returns NULL if out of range or sb is NULL.
 */
const char *screen_buf_get_line(ScreenBuf *sb, int display_index);

/* Return the number of lines currently available (min(total, SCROLLBACK_MAX)). */
int screen_buf_visible_count(ScreenBuf *sb);

/* ------------------------------------------------------------------ */
/*  Input line management                                             */
/* ------------------------------------------------------------------ */

void screen_buf_set_prompt(ScreenBuf *sb, const char *prompt);
void screen_buf_set_cwd(ScreenBuf *sb, const char *cwd);

/* Insert a character at cursor_pos, shifting text right. */
void screen_buf_input_insert(ScreenBuf *sb, char ch);

/* Delete the character before cursor_pos (Backspace). */
void screen_buf_input_delete_back(ScreenBuf *sb);

/* Delete the character at cursor_pos (Delete key). */
void screen_buf_input_delete_forward(ScreenBuf *sb);

/* Clear the input buffer entirely. */
void screen_buf_input_clear(ScreenBuf *sb);

/*
 * Commit the current input: build "prompt+input", add it to scrollback,
 * then clear the input buffer.  The prompt itself is NOT reset.
 */
void screen_buf_input_commit(ScreenBuf *sb);

/* Cursor movement helpers. */
void screen_buf_input_move_left(ScreenBuf *sb);
void screen_buf_input_move_right(ScreenBuf *sb);
void screen_buf_input_home(ScreenBuf *sb);
void screen_buf_input_end(ScreenBuf *sb);

/* ------------------------------------------------------------------ */
/*  Scrolling                                                         */
/* ------------------------------------------------------------------ */

void screen_buf_scroll_up(ScreenBuf *sb, int n);
void screen_buf_scroll_down(ScreenBuf *sb, int n);
void screen_buf_scroll_reset(ScreenBuf *sb);

/* ------------------------------------------------------------------ */
/*  Clear (Ctrl+L behaviour)                                          */
/* ------------------------------------------------------------------ */

/* Reset scrollback but preserve the current input line and prompt. */
void screen_buf_clear(ScreenBuf *sb);

#endif /* GUI_SCREEN_BUF_H */
