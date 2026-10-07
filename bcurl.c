/*
 * bcurl.c — Binary HTTP Client
 *
 * Usage:  ./bcurl [-v] <host>:<port>/<path>
 *
 * Behaviour:
 *   - Opens ONE TCP connection (never a second one).
 *   - Builds a binary request frame (GET for the given path).
 *   - Sends it, reads the response frame.
 *   - Prints the response body to stdout.
 *   - With -v, hexdumps every frame to stderr.
 *   - Exits non-zero on 4xx / 5xx status codes.
 *   - Unknown response frame types are skipped cleanly.
 */

#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Parse URL  host:port/path ────────────────────────────────────── */

static int parse_url(const char *url,
                     char *host, size_t hcap,
                     int *port,
                     char *path, size_t pcap) {
    const char *colon = strchr(url, ':');
    if (!colon) return -1;

    /* host */
    size_t hlen = (size_t)(colon - url);
    if (hlen == 0 || hlen >= hcap) return -1;
    memcpy(host, url, hlen);
    host[hlen] = '\0';

    /* port and path */
    const char *slash = strchr(colon + 1, '/');
    if (!slash) {
        *port = atoi(colon + 1);
        strncpy(path, "/", pcap);
    } else {
        char tmp[16];
        size_t plen = (size_t)(slash - colon - 1);
        if (plen == 0 || plen >= sizeof(tmp)) return -1;
        memcpy(tmp, colon + 1, plen);
        tmp[plen] = '\0';
        *port = atoi(tmp);
        strncpy(path, slash, pcap - 1);
        path[pcap - 1] = '\0';
    }

    if (*port <= 0 || *port > 65535) return -1;
    return 0;
}

/* ── main ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    int         verbose = 0;
    const char *url     = NULL;

    /* Parse CLI arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
        else
            url = argv[i];
    }

    if (!url) {
        fprintf(stderr, "Usage: %s [-v] <host>:<port>/<path>\n", argv[0]);
        return 1;
    }

    char host[256];
    int  port;
    char path[MAX_PATH_LEN];

    if (parse_url(url, host, sizeof(host), &port, path, sizeof(path)) < 0) {
        fprintf(stderr, "Error: invalid URL.  Use  host:port/path\n");
        return 1;
    }

    if (verbose)
        fprintf(stderr, "[bcurl] → %s port %d path %s\n", host, port, path);

    sock_init();

    /* ── Resolve host ─────────────────────────────────────────────── */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    struct hostent *he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "Error: cannot resolve '%s'\n", host);
        sock_cleanup();
        return 1;
    }
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    /* ── Open ONE TCP connection (never a second one) ─────────────── */
    sock_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == SOCK_INVALID) {
        perror("socket");
        sock_cleanup();
        return 1;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        sock_close(sock);
        sock_cleanup();
        return 1;
    }

    /* ══════════════════════════════════════════════════════════════════
     *  Build the request frame
     *
     *  Payload:
     *    [header_count : 1]
     *    [header 0 : :method = GET   (indexed)]
     *    [header 1 : :path   = <path> (indexed)]
     * ══════════════════════════════════════════════════════════════════ */

    uint8_t payload[8192];
    size_t  plen = 0;

    /* Header count */
    payload[plen++] = 2;

    /* :method = GET */
    header_t h0;
    strncpy(h0.name,  ":method", MAX_HDR_NAME);
    strncpy(h0.value, "GET",     MAX_HDR_VALUE);
    {
        int n = encode_header(&h0, payload + plen, sizeof(payload) - plen);
        if (n < 0) {
            fprintf(stderr, "Error: failed to encode :method\n");
            sock_close(sock);
            sock_cleanup();
            return 1;
        }
        plen += (size_t)n;
    }

    /* :path = <path> */
    header_t h1;
    strncpy(h1.name,  ":path",  MAX_HDR_NAME);
    strncpy(h1.value, path,     MAX_HDR_VALUE);
    h1.value[MAX_HDR_VALUE - 1] = '\0';
    {
        int n = encode_header(&h1, payload + plen, sizeof(payload) - plen);
        if (n < 0) {
            fprintf(stderr, "Error: failed to encode :path\n");
            sock_close(sock);
            sock_cleanup();
            return 1;
        }
        plen += (size_t)n;
    }

    /* Frame header */
    frame_header_t req_fh = {
        .length    = (uint32_t)plen,
        .type      = FRAME_REQUEST,
        .flags     = FLAG_END_STREAM | FLAG_END_HEADERS,
        .stream_id = 1,
    };

    uint8_t wire_hdr[FRAME_HEADER_SIZE];
    encode_frame_header(&req_fh, wire_hdr);

    /* Verbose: hexdump the outgoing request */
    if (verbose) {
        size_t  total = FRAME_HEADER_SIZE + plen;
        uint8_t *full = malloc(total);
        if (full) {
            memcpy(full, wire_hdr, FRAME_HEADER_SIZE);
            memcpy(full + FRAME_HEADER_SIZE, payload, plen);
            hexdump("REQUEST", full, total);
            free(full);
        }
    }

    /* ── Send request ─────────────────────────────────────────────── */
    if (write_exact(sock, wire_hdr, FRAME_HEADER_SIZE) < 0 ||
        write_exact(sock, payload, plen) < 0) {
        fprintf(stderr, "Error: failed to send request\n");
        sock_close(sock);
        sock_cleanup();
        return 1;
    }

    /* ══════════════════════════════════════════════════════════════════
     *  Read the response
     * ══════════════════════════════════════════════════════════════════ */

    uint8_t resp_raw[FRAME_HEADER_SIZE];
    if (read_exact(sock, resp_raw, FRAME_HEADER_SIZE) < 0) {
        fprintf(stderr, "Error: no response from server\n");
        sock_close(sock);
        sock_cleanup();
        return 1;
    }

    frame_header_t resp_fh;
    decode_frame_header(resp_raw, &resp_fh);

    /*  Unknown frame type → skip cleanly, read next frame.
     *  "A receiver meeting a frame type it does not know
     *   MUST skip it cleanly."                                      */
    while (resp_fh.type != FRAME_RESPONSE) {
        fprintf(stderr, "[bcurl] unknown frame type 0x%02x — "
                "skipping %u bytes\n", resp_fh.type, resp_fh.length);

        if (resp_fh.length > 0) {
            uint8_t *discard = malloc(resp_fh.length);
            if (!discard) break;
            if (read_exact(sock, discard, resp_fh.length) < 0) {
                free(discard);
                break;
            }
            free(discard);
        }

        if (read_exact(sock, resp_raw, FRAME_HEADER_SIZE) < 0) {
            fprintf(stderr, "Error: connection closed while skipping\n");
            sock_close(sock);
            sock_cleanup();
            return 1;
        }
        decode_frame_header(resp_raw, &resp_fh);
    }

    /* Read response payload */
    uint8_t *resp_payload = NULL;
    if (resp_fh.length > 0) {
        if (resp_fh.length > MAX_PAYLOAD) {
            fprintf(stderr, "Error: response too large (%u bytes)\n",
                    resp_fh.length);
            sock_close(sock);
            sock_cleanup();
            return 1;
        }
        resp_payload = malloc(resp_fh.length);
        if (!resp_payload) {
            fprintf(stderr, "Error: out of memory\n");
            sock_close(sock);
            sock_cleanup();
            return 1;
        }
        if (read_exact(sock, resp_payload, resp_fh.length) < 0) {
            fprintf(stderr, "Error: truncated response\n");
            free(resp_payload);
            sock_close(sock);
            sock_cleanup();
            return 1;
        }
    }

    /* Verbose: hexdump the incoming response */
    if (verbose && resp_payload) {
        size_t  total = FRAME_HEADER_SIZE + resp_fh.length;
        uint8_t *full = malloc(total);
        if (full) {
            memcpy(full, resp_raw, FRAME_HEADER_SIZE);
            memcpy(full + FRAME_HEADER_SIZE, resp_payload, resp_fh.length);
            hexdump("RESPONSE", full, total);
            free(full);
        }
    }

    /* ── Parse response: [status:2] [hdr_count:1] [headers…] [body] */

    if (!resp_payload || resp_fh.length < 3) {
        fprintf(stderr, "Error: malformed response (too short)\n");
        free(resp_payload);
        sock_close(sock);
        sock_cleanup();
        return 1;
    }

    size_t   rpos   = 0;
    uint16_t status = ((uint16_t)resp_payload[0] << 8) | resp_payload[1];
    rpos += 2;

    uint8_t hdr_count = resp_payload[rpos++];

    if (verbose)
        fprintf(stderr, "[bcurl] status %u, %u headers\n", status, hdr_count);

    /* Decode and (optionally) print headers */
    for (int i = 0; i < hdr_count && i < MAX_HEADERS; i++) {
        header_t hdr;
        int n = decode_header(resp_payload + rpos,
                              resp_fh.length - rpos, &hdr);
        if (n < 0) {
            fprintf(stderr, "Warning: could not decode header %d\n", i);
            break;
        }
        rpos += (size_t)n;

        if (verbose)
            fprintf(stderr, "[bcurl]   %s: %s\n", hdr.name, hdr.value);
    }

    /* Everything after the headers is the body → stdout */
    size_t body_len = resp_fh.length - rpos;
    if (body_len > 0) {
        fwrite(resp_payload + rpos, 1, body_len, stdout);
        fflush(stdout);
    }

    if (verbose)
        fprintf(stderr, "[bcurl] body: %zu bytes\n", body_len);

    /* ── Clean up ─────────────────────────────────────────────────── */
    free(resp_payload);
    sock_close(sock);
    sock_cleanup();

    /* Exit non-zero on 4xx / 5xx */
    if (status >= 400) {
        fprintf(stderr, "[bcurl] exiting with error status %u\n", status);
        return 1;
    }

    return 0;
}
