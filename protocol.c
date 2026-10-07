/*
 * protocol.c — Binary HTTP Protocol Implementation
 *
 * Implements frame encoding/decoding, header compression (simplified
 * HPACK with static table + length-prefixed literals), hexdump, and
 * MIME type guessing.
 */

#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ══════════════════════════════════════════════════════════════════════
 *  Static header table — the ten names we actually send.
 *  Indexed by a single byte whose high bit is set (0x80 | index).
 * ══════════════════════════════════════════════════════════════════════ */

const char *STATIC_TABLE[STATIC_TABLE_SIZE] = {
    ":method",          /* 0 */
    ":path",            /* 1 */
    ":status",          /* 2 */
    "content-type",     /* 3 */
    "content-length",   /* 4 */
    "host",             /* 5 */
    "server",           /* 6 */
    "date",             /* 7 */
    "connection",       /* 8 */
    "accept",           /* 9 */
};

/* ══════════════════════════════════════════════════════════════════════
 *  Wire I/O — read / write exactly `len` bytes on a socket
 * ══════════════════════════════════════════════════════════════════════ */

int read_exact(sock_t sock, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t   remaining = len;

    while (remaining > 0) {
        int n = recv(sock, (char *)p, (int)remaining, 0);
        if (n <= 0) return -1;          /* closed or error */
        p         += n;
        remaining -= (size_t)n;
    }
    return 0;
}

int write_exact(sock_t sock, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        int n = send(sock, (const char *)p, (int)remaining, 0);
        if (n <= 0) return -1;
        p         += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Frame header encoding / decoding (8 bytes, big-endian)
 *
 *  Wire layout:
 *    byte 0-2  →  length   (24 bits)
 *    byte 3    →  type     ( 8 bits)
 *    byte 4    →  flags    ( 8 bits)
 *    byte 5-7  →  stream_id(24 bits)
 * ══════════════════════════════════════════════════════════════════════ */

void encode_frame_header(const frame_header_t *h, uint8_t out[FRAME_HEADER_SIZE]) {
    out[0] = (uint8_t)((h->length    >> 16) & 0xFF);
    out[1] = (uint8_t)((h->length    >>  8) & 0xFF);
    out[2] = (uint8_t)( h->length           & 0xFF);
    out[3] = h->type;
    out[4] = h->flags;
    out[5] = (uint8_t)((h->stream_id >> 16) & 0xFF);
    out[6] = (uint8_t)((h->stream_id >>  8) & 0xFF);
    out[7] = (uint8_t)( h->stream_id        & 0xFF);
}

void decode_frame_header(const uint8_t in[FRAME_HEADER_SIZE], frame_header_t *h) {
    h->length    = ((uint32_t)in[0] << 16) | ((uint32_t)in[1] << 8) | in[2];
    h->type      = in[3];
    h->flags     = in[4];
    h->stream_id = ((uint32_t)in[5] << 16) | ((uint32_t)in[6] << 8) | in[7];
}

/* ══════════════════════════════════════════════════════════════════════
 *  Header encoding / decoding — simplified HPACK
 *
 *  Mechanism 1 — Indexed representation (static table lookup):
 *    [0x80 | index]          ← 1 byte, high bit set
 *    [value_len : 2 BE]      ← 2 bytes, length of value
 *    [value]                 ← value_len bytes
 *
 *  Mechanism 2 — Literal representation (length-prefixed name+value):
 *    [0x00]                  ← 1 byte, tag = literal
 *    [name_len  : 2 BE]      ← 2 bytes, length of name
 *    [name]                  ← name_len bytes
 *    [value_len : 2 BE]      ← 2 bytes, length of value
 *    [value]                 ← value_len bytes
 * ══════════════════════════════════════════════════════════════════════ */

/* Look up a header name in the static table.  Returns index or -1. */
static int find_static_index(const char *name) {
    for (int i = 0; i < STATIC_TABLE_SIZE; i++) {
        if (strcmp(name, STATIC_TABLE[i]) == 0)
            return i;
    }
    return -1;
}

int encode_header(const header_t *h, uint8_t *buf, size_t cap) {
    int    idx  = find_static_index(h->name);
    size_t vlen = strlen(h->value);
    size_t pos  = 0;

    if (idx >= 0) {
        /* ── Indexed representation ────────────────────────────────── */
        size_t need = 1 + 2 + vlen;
        if (need > cap) return -1;

        buf[pos++] = (uint8_t)(INDEXED_BIT | (idx & 0x7F));
        buf[pos++] = (uint8_t)((vlen >> 8) & 0xFF);
        buf[pos++] = (uint8_t)( vlen       & 0xFF);
        memcpy(buf + pos, h->value, vlen);
        pos += vlen;
    } else {
        /* ── Literal representation ────────────────────────────────── */
        size_t nlen = strlen(h->name);
        size_t need = 1 + 2 + nlen + 2 + vlen;
        if (need > cap) return -1;

        buf[pos++] = 0x00;                       /* literal tag       */
        buf[pos++] = (uint8_t)((nlen >> 8) & 0xFF);
        buf[pos++] = (uint8_t)( nlen       & 0xFF);
        memcpy(buf + pos, h->name, nlen);
        pos += nlen;

        buf[pos++] = (uint8_t)((vlen >> 8) & 0xFF);
        buf[pos++] = (uint8_t)( vlen       & 0xFF);
        memcpy(buf + pos, h->value, vlen);
        pos += vlen;
    }

    return (int)pos;
}

int decode_header(const uint8_t *buf, size_t len, header_t *h) {
    if (len < 1) return -1;
    size_t  pos = 0;
    uint8_t tag = buf[pos++];

    if (tag & INDEXED_BIT) {
        /* ── Indexed ───────────────────────────────────────────────── */
        int idx = tag & 0x7F;
        if (idx >= STATIC_TABLE_SIZE) return -1;

        strncpy(h->name, STATIC_TABLE[idx], MAX_HDR_NAME - 1);
        h->name[MAX_HDR_NAME - 1] = '\0';

        if (pos + 2 > len) return -1;
        uint16_t vlen = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
        pos += 2;

        if (pos + vlen > len || vlen >= MAX_HDR_VALUE) return -1;
        memcpy(h->value, buf + pos, vlen);
        h->value[vlen] = '\0';
        pos += vlen;

    } else {
        /* ── Literal ───────────────────────────────────────────────── */
        if (pos + 2 > len) return -1;
        uint16_t nlen = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
        pos += 2;

        if (pos + nlen > len || nlen >= MAX_HDR_NAME) return -1;
        memcpy(h->name, buf + pos, nlen);
        h->name[nlen] = '\0';
        pos += nlen;

        if (pos + 2 > len) return -1;
        uint16_t vlen = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
        pos += 2;

        if (pos + vlen > len || vlen >= MAX_HDR_VALUE) return -1;
        memcpy(h->value, buf + pos, vlen);
        h->value[vlen] = '\0';
        pos += vlen;
    }

    return (int)pos;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Hexdump — classic 16-bytes-per-line format, printed to stderr
 * ══════════════════════════════════════════════════════════════════════ */

void hexdump(const char *label, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;

    fprintf(stderr, "\n── %s (%zu bytes) ", label, len);
    for (int i = 0; i < 48; i++) fputc('-', stderr);
    fputc('\n', stderr);

    for (size_t i = 0; i < len; i += 16) {
        fprintf(stderr, "  %08zx  ", i);

        /* hex columns */
        for (size_t j = 0; j < 16; j++) {
            if (j == 8) fputc(' ', stderr);
            if (i + j < len)
                fprintf(stderr, "%02x ", p[i + j]);
            else
                fprintf(stderr, "   ");
        }

        /* ASCII column */
        fprintf(stderr, " |");
        for (size_t j = 0; j < 16 && (i + j) < len; j++) {
            uint8_t c = p[i + j];
            fputc(isprint(c) ? c : '.', stderr);
        }
        fprintf(stderr, "|\n");
    }
    fputc('\n', stderr);
}

/* ══════════════════════════════════════════════════════════════════════
 *  MIME type guessing — maps file extension to Content-Type
 * ══════════════════════════════════════════════════════════════════════ */

const char *guess_mime(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";

    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html";
    if (strcmp(dot, ".css") == 0)   return "text/css";
    if (strcmp(dot, ".js") == 0)    return "application/javascript";
    if (strcmp(dot, ".txt") == 0)   return "text/plain";
    if (strcmp(dot, ".json") == 0)  return "application/json";
    if (strcmp(dot, ".png") == 0)   return "image/png";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".gif") == 0)   return "image/gif";
    if (strcmp(dot, ".svg") == 0)   return "image/svg+xml";
    if (strcmp(dot, ".ico") == 0)   return "image/x-icon";
    if (strcmp(dot, ".xml") == 0)   return "application/xml";
    if (strcmp(dot, ".pdf") == 0)   return "application/pdf";

    return "application/octet-stream";
}
