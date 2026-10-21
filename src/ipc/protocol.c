/*
 * protocol.c — Safe read/write packet framing over pipes
 *
 * Implements reliable byte-level I/O and typed message send/recv
 * over pipe file descriptors. All IPC between GUI and tab workers
 * uses these functions.
 */
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "ipc/protocol.h"

/* ── Write exactly n bytes, handling partial writes ─────────────────── */
int proto_write_all(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t remaining = n;

    while (remaining > 0) {
        ssize_t written = write(fd, p, remaining);
        if (written < 0) {
            if (errno == EINTR)
                continue;   /* interrupted by signal, retry */
            return -1;
        }
        if (written == 0)
            return -1;      /* shouldn't happen on a pipe write */
        p += written;
        remaining -= (size_t)written;
    }
    return 0;
}

/* ── Read exactly n bytes, handling partial reads ───────────────────── */
int proto_read_all(int fd, void *buf, size_t n) {
    char *p = (char *)buf;
    size_t remaining = n;

    while (remaining > 0) {
        ssize_t rd = read(fd, p, remaining);
        if (rd < 0) {
            if (errno == EINTR)
                continue;   /* interrupted by signal, retry */
            return -1;
        }
        if (rd == 0)
            return -1;      /* EOF — pipe closed */
        p += rd;
        remaining -= (size_t)rd;
    }
    return 0;
}

/* ── Send a typed message with payload ──────────────────────────────── */
int proto_send(int fd, uint32_t type, const void *payload, uint32_t payload_len) {
    PacketHeader hdr;
    hdr.type   = type;
    hdr.length = payload_len;

    /* Validate payload size */
    if (payload_len > MAX_PAYLOAD_SIZE) {
        TERM_LOG("proto_send: payload too large (%u > %d)", payload_len, MAX_PAYLOAD_SIZE);
        return TERM_ERR_PROTOCOL;
    }

    /* Write header */
    if (proto_write_all(fd, &hdr, sizeof(hdr)) < 0) {
        return TERM_ERR_PROTOCOL;
    }

    /* Write payload (if any) */
    if (payload_len > 0 && payload != NULL) {
        if (proto_write_all(fd, payload, payload_len) < 0) {
            return TERM_ERR_PROTOCOL;
        }
    }

    return TERM_OK;
}

/* ── Receive a message from a pipe ──────────────────────────────────── */
int proto_recv(int fd, uint32_t *out_type, void *buf, size_t buf_size, uint32_t *out_len) {
    PacketHeader hdr;

    /* Read header */
    if (proto_read_all(fd, &hdr, sizeof(hdr)) < 0) {
        return -1;
    }

    *out_type = hdr.type;
    *out_len  = hdr.length;

    /* Validate payload size */
    if (hdr.length > MAX_PAYLOAD_SIZE) {
        TERM_LOG("proto_recv: payload too large (%u)", hdr.length);
        return -1;
    }

    /* Read payload */
    if (hdr.length > 0) {
        if (hdr.length > buf_size) {
            TERM_LOG("proto_recv: buffer too small (%u > %zu)", hdr.length, buf_size);
            /* Drain the payload so the pipe stays in sync */
            char drain[512];
            uint32_t left = hdr.length;
            while (left > 0) {
                size_t chunk = left < sizeof(drain) ? left : sizeof(drain);
                if (proto_read_all(fd, drain, chunk) < 0)
                    return -1;
                left -= (uint32_t)chunk;
            }
            return -1;
        }
        if (proto_read_all(fd, buf, hdr.length) < 0) {
            return -1;
        }
    }

    return 0;
}
