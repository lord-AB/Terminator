/*
 * screen_buf.c — Implementation of the per-tab ring-buffer, input line
 *                editing, and scroll management for Terminator.
 *
 * The ScreenBuf is ~1 MB (SCROLLBACK_MAX * LINE_MAXLEN) so it is always
 * heap-allocated with calloc.
 */

#include "gui/screen_buf.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ================================================================== */
/*  Creation / Destruction                                            */
/* ================================================================== */

/*
 * screen_buf_create — allocate a zeroed ScreenBuf, set the default
 *                     prompt to "$ ", and snapshot the current working
 *                     directory.
 */
ScreenBuf *screen_buf_create(void)
{
    ScreenBuf *sb = calloc(1, sizeof(ScreenBuf));
    if (!sb)
        return NULL;

    /* Default prompt */
    strncpy(sb->prompt, "user@terminator> ", sizeof(sb->prompt) - 1);
    sb->prompt[sizeof(sb->prompt) - 1] = '\0';

    /* Capture the current working directory */
    if (!getcwd(sb->cwd, sizeof(sb->cwd))) {
        /* Fallback if getcwd fails (shouldn't happen normally) */
        strncpy(sb->cwd, "/", sizeof(sb->cwd) - 1);
        sb->cwd[sizeof(sb->cwd) - 1] = '\0';
    }

    return sb;
}

/*
 * screen_buf_destroy — free a heap-allocated ScreenBuf (NULL-safe).
 */
void screen_buf_destroy(ScreenBuf *sb)
{
    free(sb);   /* free(NULL) is a well-defined no-op */
}

/* ================================================================== */
/*  Scrollback line management                                        */
/* ================================================================== */

/*
 * screen_buf_add_line — append a single line to the scrollback ring buffer.
 *
 * The line is copied into lines[head].  head advances, count is capped at
 * SCROLLBACK_MAX, and total is incremented monotonically.
 */
void screen_buf_add_line(ScreenBuf *sb, const char *line)
{
    if (!sb || !line)
        return;

    strncpy(sb->lines[sb->head], line, LINE_MAXLEN - 1);
    sb->lines[sb->head][LINE_MAXLEN - 1] = '\0';

    sb->head = (sb->head + 1) % SCROLLBACK_MAX;

    if (sb->count < SCROLLBACK_MAX)
        sb->count++;

    sb->total++;
}

/*
 * screen_buf_add_text — split text by '\n' and add each piece as a
 *                       separate line.  A trailing fragment without a
 *                       newline is still added.
 */
void screen_buf_add_text(ScreenBuf *sb, const char *text)
{
    if (!sb || !text)
        return;

    const char *p = text;

    while (*p) {
        /* Find the next newline (or end-of-string). */
        const char *nl = strchr(p, '\n');

        if (nl) {
            /* Temporary buffer for the segment before '\n'. */
            size_t len = (size_t)(nl - p);
            if (len >= LINE_MAXLEN)
                len = LINE_MAXLEN - 1;

            char buf[LINE_MAXLEN];
            memcpy(buf, p, len);
            buf[len] = '\0';

            screen_buf_add_line(sb, buf);
            p = nl + 1;   /* skip past the newline */
        } else {
            /* No more newlines — add the remaining text as-is. */
            screen_buf_add_line(sb, p);
            break;
        }
    }
}

/*
 * screen_buf_get_line — retrieve a line by display index.
 *
 *   display_index 0         → oldest available line
 *   display_index count-1   → newest line
 *
 * Returns NULL if the index is out of range or sb is NULL.
 */
const char *screen_buf_get_line(ScreenBuf *sb, int display_index)
{
    if (!sb)
        return NULL;

    int visible = (sb->total < SCROLLBACK_MAX) ? sb->total : SCROLLBACK_MAX;

    if (display_index < 0 || display_index >= visible)
        return NULL;

    int slot;
    if (sb->total <= SCROLLBACK_MAX) {
        /* Ring hasn't wrapped yet — display_index maps directly. */
        slot = display_index;
    } else {
        /* Ring has wrapped — head points to the oldest slot. */
        slot = (sb->head + display_index) % SCROLLBACK_MAX;
    }

    return sb->lines[slot];
}

/*
 * screen_buf_visible_count — number of lines currently in the buffer.
 */
int screen_buf_visible_count(ScreenBuf *sb)
{
    if (!sb)
        return 0;
    return (sb->total < SCROLLBACK_MAX) ? sb->total : SCROLLBACK_MAX;
}

/* ================================================================== */
/*  Input line management                                             */
/* ================================================================== */

/*
 * screen_buf_set_prompt — update the shell prompt string.
 */
void screen_buf_set_prompt(ScreenBuf *sb, const char *prompt)
{
    if (!sb || !prompt)
        return;
    strncpy(sb->prompt, prompt, sizeof(sb->prompt) - 1);
    sb->prompt[sizeof(sb->prompt) - 1] = '\0';
}

/*
 * screen_buf_set_cwd — update the stored current working directory.
 */
void screen_buf_set_cwd(ScreenBuf *sb, const char *cwd)
{
    if (!sb || !cwd)
        return;
    strncpy(sb->cwd, cwd, sizeof(sb->cwd) - 1);
    sb->cwd[sizeof(sb->cwd) - 1] = '\0';
}

/*
 * screen_buf_input_insert — insert a printable character at cursor_pos.
 *
 * Characters to the right of cursor_pos are shifted, and both input_len
 * and cursor_pos are incremented.  Insertion is silently ignored if the
 * buffer is already at MAX_CMD_LEN-1.
 */
void screen_buf_input_insert(ScreenBuf *sb, char ch)
{
    if (!sb)
        return;
    if (sb->input_len >= MAX_CMD_LEN - 1)
        return;   /* buffer full */

    /* Shift everything from cursor_pos rightward by one byte. */
    memmove(&sb->input[sb->cursor_pos + 1],
            &sb->input[sb->cursor_pos],
            (size_t)(sb->input_len - sb->cursor_pos));

    sb->input[sb->cursor_pos] = ch;
    sb->input_len++;
    sb->cursor_pos++;
    sb->input[sb->input_len] = '\0';
}

/*
 * screen_buf_input_delete_back — Backspace: delete the character
 *                                immediately before cursor_pos.
 */
void screen_buf_input_delete_back(ScreenBuf *sb)
{
    if (!sb)
        return;
    if (sb->cursor_pos <= 0)
        return;

    /* Shift left from cursor_pos onward, overwriting cursor_pos-1. */
    memmove(&sb->input[sb->cursor_pos - 1],
            &sb->input[sb->cursor_pos],
            (size_t)(sb->input_len - sb->cursor_pos));

    sb->cursor_pos--;
    sb->input_len--;
    sb->input[sb->input_len] = '\0';
}

/*
 * screen_buf_input_delete_forward — Delete key: delete the character
 *                                   at cursor_pos.
 */
void screen_buf_input_delete_forward(ScreenBuf *sb)
{
    if (!sb)
        return;
    if (sb->cursor_pos >= sb->input_len)
        return;

    /* Shift left from cursor_pos+1 onward. */
    memmove(&sb->input[sb->cursor_pos],
            &sb->input[sb->cursor_pos + 1],
            (size_t)(sb->input_len - sb->cursor_pos - 1));

    sb->input_len--;
    sb->input[sb->input_len] = '\0';
}

/*
 * screen_buf_input_clear — zero the input buffer, reset cursor.
 */
void screen_buf_input_clear(ScreenBuf *sb)
{
    if (!sb)
        return;

    memset(sb->input, 0, sizeof(sb->input));
    sb->input_len  = 0;
    sb->cursor_pos = 0;
}

/*
 * screen_buf_input_commit — build "prompt + input", add that combined
 *                           string to scrollback, then clear the input.
 *                           The prompt string itself is NOT reset.
 */
void screen_buf_input_commit(ScreenBuf *sb)
{
    if (!sb)
        return;

    /* Build the combined line:  prompt + input.
     * Both prompt (256) and input (4096) fit comfortably in a local buf.
     * The scrollback line will be truncated to LINE_MAXLEN by add_line. */
    char combined[256 + MAX_CMD_LEN];
    snprintf(combined, sizeof(combined), "%s%s", sb->prompt, sb->input);

    screen_buf_add_line(sb, combined);
    screen_buf_input_clear(sb);
}

/* ---- Cursor movement -------------------------------------------- */

void screen_buf_input_move_left(ScreenBuf *sb)
{
    if (!sb)
        return;
    if (sb->cursor_pos > 0)
        sb->cursor_pos--;
}

void screen_buf_input_move_right(ScreenBuf *sb)
{
    if (!sb)
        return;
    if (sb->cursor_pos < sb->input_len)
        sb->cursor_pos++;
}

void screen_buf_input_home(ScreenBuf *sb)
{
    if (!sb)
        return;
    sb->cursor_pos = 0;
}

void screen_buf_input_end(ScreenBuf *sb)
{
    if (!sb)
        return;
    sb->cursor_pos = sb->input_len;
}

/* ================================================================== */
/*  Scrolling                                                         */
/* ================================================================== */

/*
 * screen_buf_scroll_up — increase scroll_offset by n lines, capped so
 *                        that we never scroll past the oldest line.
 */
void screen_buf_scroll_up(ScreenBuf *sb, int n)
{
    if (!sb || n <= 0)
        return;

    sb->scroll_offset += n;

    /* Cap: can't scroll beyond the total number of stored lines minus 1,
     * but must stay non-negative if the buffer is empty. */
    int max_offset = (sb->count > 0) ? (sb->count - 1) : 0;
    if (sb->scroll_offset > max_offset)
        sb->scroll_offset = max_offset;
}

/*
 * screen_buf_scroll_down — decrease scroll_offset by n, floored at 0.
 */
void screen_buf_scroll_down(ScreenBuf *sb, int n)
{
    if (!sb || n <= 0)
        return;

    sb->scroll_offset -= n;
    if (sb->scroll_offset < 0)
        sb->scroll_offset = 0;
}

/*
 * screen_buf_scroll_reset — jump back to the bottom (most recent output).
 */
void screen_buf_scroll_reset(ScreenBuf *sb)
{
    if (!sb)
        return;
    sb->scroll_offset = 0;
}

/* ================================================================== */
/*  Clear (Ctrl+L)                                                    */
/* ================================================================== */

/*
 * screen_buf_clear — wipe the scrollback history but keep the current
 *                    input line and prompt intact.
 */
void screen_buf_clear(ScreenBuf *sb)
{
    if (!sb)
        return;

    sb->count         = 0;
    sb->head          = 0;
    sb->total         = 0;
    sb->scroll_offset = 0;
}
