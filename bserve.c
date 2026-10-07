/*
 * bserve.c — Binary HTTP Server
 *
 * Usage:  ./bserve <root_dir> <port>
 *
 * Behaviour:
 *   - Accepts a TCP connection and keeps it open (persistent).
 *   - Reads one binary request frame at a time.
 *   - Maps the requested :path to a file under root_dir.
 *   - Replies with a binary response frame:
 *       200 + file contents        if the file exists
 *       404                        if the file is not found
 *       400                        if the frame is malformed
 *   - Unknown frame types are skipped cleanly (read length bytes,
 *     discard, continue) — this leaves room for a version 2.
 */

#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
  #include <signal.h>
#endif

/* ── Build and send a response frame ──────────────────────────────── */

static int send_response(sock_t client, uint32_t stream_id, uint16_t status,
                         header_t *hdrs, int nhdr,
                         const uint8_t *body, size_t body_len) {
    /*
     * Response payload layout:
     *   [status   : 2 bytes, big-endian]
     *   [hdr_count: 1 byte]
     *   [headers  : variable, self-delimiting]
     *   [body     : remaining bytes]
     */
    uint8_t meta[8192];
    size_t  mlen = 0;

    /* Status code */
    meta[mlen++] = (uint8_t)((status >> 8) & 0xFF);
    meta[mlen++] = (uint8_t)( status       & 0xFF);

    /* Header count */
    meta[mlen++] = (uint8_t)nhdr;

    /* Encode headers */
    for (int i = 0; i < nhdr; i++) {
        int n = encode_header(&hdrs[i], meta + mlen, sizeof(meta) - mlen);
        if (n < 0) {
            fprintf(stderr, "[bserve] error encoding response header %d\n", i);
            return -1;
        }
        mlen += (size_t)n;
    }

    /* Frame header */
    uint32_t total_payload = (uint32_t)(mlen + body_len);
    frame_header_t fh = {
        .length    = total_payload,
        .type      = FRAME_RESPONSE,
        .flags     = FLAG_END_STREAM | FLAG_END_HEADERS,
        .stream_id = stream_id,
    };

    uint8_t wire_hdr[FRAME_HEADER_SIZE];
    encode_frame_header(&fh, wire_hdr);

    /* Send: frame header → meta (status + headers) → body */
    if (write_exact(client, wire_hdr, FRAME_HEADER_SIZE) < 0) return -1;
    if (write_exact(client, meta, mlen)                  < 0) return -1;
    if (body_len > 0 && body != NULL) {
        if (write_exact(client, body, body_len) < 0) return -1;
    }

    /* Hexdump the outgoing frame */
    {
        size_t  full_len = FRAME_HEADER_SIZE + mlen + body_len;
        uint8_t *full    = malloc(full_len);
        if (full) {
            memcpy(full, wire_hdr, FRAME_HEADER_SIZE);
            memcpy(full + FRAME_HEADER_SIZE, meta, mlen);
            if (body_len > 0 && body)
                memcpy(full + FRAME_HEADER_SIZE + mlen, body, body_len);
            hexdump("RESPONSE", full, full_len);
            free(full);
        }
    }

    fprintf(stderr, "[bserve] → %u (stream %u, %zu body bytes)\n",
            status, stream_id, body_len);
    return 0;
}

/* Convenience: send an error response with a text body */
static void send_error(sock_t client, uint32_t stream_id,
                       uint16_t status, const char *msg) {
    header_t hdrs[2];
    int n = 0;

    strncpy(hdrs[n].name,  "content-type",  MAX_HDR_NAME);
    strncpy(hdrs[n].value, "text/plain",    MAX_HDR_VALUE);
    n++;

    strncpy(hdrs[n].name,  "server",        MAX_HDR_NAME);
    strncpy(hdrs[n].value, "bserve/1.0",    MAX_HDR_VALUE);
    n++;

    send_response(client, stream_id, status, hdrs, n,
                  (const uint8_t *)msg, strlen(msg));
}

/* ── Handle a single persistent connection ────────────────────────── */

static void handle_client(sock_t client, const char *root) {
    uint8_t raw[FRAME_HEADER_SIZE];

    /* Persistent connection: keep reading frames until EOF */
    while (1) {
        /* 1. Read frame header */
        if (read_exact(client, raw, FRAME_HEADER_SIZE) < 0)
            break;                              /* connection closed  */

        frame_header_t fh;
        decode_frame_header(raw, &fh);

        fprintf(stderr, "[bserve] ← frame type=0x%02x len=%u stream=%u flags=0x%02x\n",
                fh.type, fh.length, fh.stream_id, fh.flags);

        /* 2. Read payload */
        uint8_t *payload = NULL;
        if (fh.length > 0) {
            if (fh.length > MAX_PAYLOAD) {
                fprintf(stderr, "[bserve] payload exceeds maximum (%u)\n", fh.length);
                break;
            }
            payload = malloc(fh.length);
            if (!payload) break;
            if (read_exact(client, payload, fh.length) < 0) {
                free(payload);
                break;
            }
        }

        /* Hexdump the incoming frame */
        {
            size_t  total = FRAME_HEADER_SIZE + fh.length;
            uint8_t *full = malloc(total);
            if (full) {
                memcpy(full, raw, FRAME_HEADER_SIZE);
                if (payload)
                    memcpy(full + FRAME_HEADER_SIZE, payload, fh.length);
                hexdump("REQUEST", full, total);
                free(full);
            }
        }

        /* 3. Unknown frame type → skip cleanly, keep reading.
         *    This is the forward-compatibility rule:
         *    "A receiver meeting a frame type it does not know
         *     MUST skip it cleanly."                                 */
        if (fh.type != FRAME_REQUEST) {
            fprintf(stderr, "[bserve] unknown frame type 0x%02x — "
                    "skipped %u bytes (v2-safe)\n", fh.type, fh.length);
            free(payload);
            continue;
        }

        /* 4. Parse REQUEST payload: [header_count:1] [headers…]     */
        if (!payload || fh.length < 1) {
            send_error(client, fh.stream_id, STATUS_BAD_REQUEST,
                       "400 Bad Request: empty payload\n");
            free(payload);
            continue;
        }

        size_t  pos          = 0;
        uint8_t header_count = payload[pos++];

        header_t headers[MAX_HEADERS];
        int      ok = 1;

        for (int i = 0; i < header_count && i < MAX_HEADERS; i++) {
            int n = decode_header(payload + pos, fh.length - pos, &headers[i]);
            if (n < 0) { ok = 0; break; }
            pos += (size_t)n;
        }

        if (!ok) {
            send_error(client, fh.stream_id, STATUS_BAD_REQUEST,
                       "400 Bad Request: malformed headers\n");
            free(payload);
            continue;
        }

        /* 5. Extract :method and :path */
        const char *method = NULL;
        const char *path   = NULL;
        for (int i = 0; i < header_count && i < MAX_HEADERS; i++) {
            if (strcmp(headers[i].name, ":method") == 0) method = headers[i].value;
            if (strcmp(headers[i].name, ":path")   == 0) path   = headers[i].value;
        }

        if (!method || !path) {
            send_error(client, fh.stream_id, STATUS_BAD_REQUEST,
                       "400 Bad Request: missing :method or :path\n");
            free(payload);
            continue;
        }

        fprintf(stderr, "[bserve] %s %s\n", method, path);

        /* 6. Sanitize: reject path-traversal attempts */
        if (strstr(path, "..") != NULL) {
            send_error(client, fh.stream_id, STATUS_BAD_REQUEST,
                       "400 Bad Request: path traversal rejected\n");
            free(payload);
            continue;
        }

        /* 7. Map path → file under root */
        char filepath[MAX_PATH_LEN];
        snprintf(filepath, sizeof(filepath), "%s%s", root, path);

        FILE *fp = fopen(filepath, "rb");
        if (!fp) {
            send_error(client, fh.stream_id, STATUS_NOT_FOUND,
                       "404 Not Found\n");
            free(payload);
            continue;
        }

        /* Read entire file */
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        uint8_t *file_data = NULL;
        if (fsize > 0) {
            file_data = malloc((size_t)fsize);
            if (!file_data) { fclose(fp); free(payload); break; }
            fread(file_data, 1, (size_t)fsize, fp);
        }
        fclose(fp);

        /* 8. Build and send 200 response */
        header_t resp_hdrs[3];
        int rn = 0;

        strncpy(resp_hdrs[rn].name,  "content-type", MAX_HDR_NAME);
        strncpy(resp_hdrs[rn].value, guess_mime(filepath), MAX_HDR_VALUE);
        rn++;

        strncpy(resp_hdrs[rn].name, "content-length", MAX_HDR_NAME);
        snprintf(resp_hdrs[rn].value, MAX_HDR_VALUE, "%ld", fsize > 0 ? fsize : 0);
        rn++;

        strncpy(resp_hdrs[rn].name,  "server", MAX_HDR_NAME);
        strncpy(resp_hdrs[rn].value, "bserve/1.0", MAX_HDR_VALUE);
        rn++;

        send_response(client, fh.stream_id, STATUS_OK,
                      resp_hdrs, rn,
                      file_data, fsize > 0 ? (size_t)fsize : 0);

        free(file_data);
        free(payload);
    }

    sock_close(client);
    fprintf(stderr, "[bserve] connection closed\n");
}

/* ── main ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <root_dir> <port>\n", argv[0]);
        return 1;
    }

    const char *root = argv[1];
    int         port = atoi(argv[2]);

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: invalid port '%s'\n", argv[2]);
        return 1;
    }

    sock_init();

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    /* Create socket */
    sock_t server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == SOCK_INVALID) {
        perror("socket");
        return 1;
    }

    /* Allow address reuse (quick restart) */
    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));

    /* Bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        sock_close(server);
        return 1;
    }

    /* Listen */
    if (listen(server, 5) < 0) {
        perror("listen");
        sock_close(server);
        return 1;
    }

    fprintf(stderr, "[bserve] listening on port %d, root = %s\n", port, root);

    /* Accept loop (one client at a time — simple sequential server) */
    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t          cli_len = sizeof(cli_addr);

        sock_t client = accept(server,
                               (struct sockaddr *)&cli_addr, &cli_len);
        if (client == SOCK_INVALID) {
            perror("accept");
            continue;
        }

        fprintf(stderr, "[bserve] connection from %s:%d\n",
                inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port));

        handle_client(client, root);
    }

    sock_close(server);
    sock_cleanup();
    return 0;
}
