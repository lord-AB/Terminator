/*
 * protocol.h — Fixed-header message packets between GUI ↔ Tab workers
 *
 * All IPC between the GUI main loop and tab workers (whether threads or
 * forked processes) goes through pipes using these packet structures.
 * Each packet has a fixed header (type + payload length) followed by
 * variable-length payload data.
 */
#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include "common.h"

/* ── GUI → Tab message types ────────────────────────────────────────── */
typedef enum {
    GUI_MSG_KEYPRESS,       /* single keystroke character               */
    GUI_MSG_LINE,           /* completed input line (after Enter)       */
    GUI_MSG_PASTE,          /* pasted text block                        */
    GUI_MSG_SIGNAL,         /* deliver a signal (Ctrl+C → SIGINT, etc.) */
    GUI_MSG_CLOSE,          /* request tab to shut down                 */
    GUI_MSG_RESIZE          /* terminal dimensions changed              */
} GuiToTabType;

/* ── Tab → GUI message types ────────────────────────────────────────── */
typedef enum {
    TAB_MSG_OUTPUT,         /* stdout/stderr text to display            */
    TAB_MSG_PROMPT,         /* updated prompt string                    */
    TAB_MSG_CWD,            /* current working directory changed        */
    TAB_MSG_CLEAR,          /* clear screen request (Ctrl+L)            */
    TAB_MSG_EXIT,           /* tab is exiting (with exit code)          */
    TAB_MSG_READY           /* tab finished startup, ready for input    */
} TabToGuiType;

/* ── Packet header (fixed, 8 bytes) ─────────────────────────────────── */
typedef struct {
    uint32_t type;          /* message type (one of the enums above)    */
    uint32_t length;        /* payload length in bytes (0 = no payload) */
} PacketHeader;

/* Max payload size to prevent runaway reads */
#define MAX_PAYLOAD_SIZE  (64 * 1024)

/* ── GUI → Tab packet ───────────────────────────────────────────────── */
typedef struct {
    PacketHeader header;
    char payload[MAX_CMD_LEN];   /* variable-length, actual size in header.length */
} GuiToTabMsg;

/* ── Tab → GUI packet ───────────────────────────────────────────────── */
typedef struct {
    PacketHeader header;
    char payload[PIPE_BUF_SIZE]; /* variable-length, actual size in header.length */
} TabToGuiMsg;

/* ── Wire I/O functions ─────────────────────────────────────────────── */

/*
 * Write exactly `n` bytes to fd, handling partial writes.
 * Returns 0 on success, -1 on error.
 */
int proto_write_all(int fd, const void *buf, size_t n);

/*
 * Read exactly `n` bytes from fd, handling partial reads.
 * Returns 0 on success, -1 on error/EOF.
 */
int proto_read_all(int fd, void *buf, size_t n);

/*
 * Send a typed message with payload over a pipe.
 * `payload` may be NULL if `payload_len` is 0.
 * Returns 0 on success, negative on error.
 */
int proto_send(int fd, uint32_t type, const void *payload, uint32_t payload_len);

/*
 * Receive a message from a pipe.
 * Reads the header first, then the payload into `buf` (up to `buf_size`).
 * Sets *out_type and *out_len on success.
 * Returns 0 on success, -1 on error/EOF.
 */
int proto_recv(int fd, uint32_t *out_type, void *buf, size_t buf_size, uint32_t *out_len);

#endif /* IPC_PROTOCOL_H */
