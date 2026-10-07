/*
 * protocol.h — Binary HTTP Protocol Definition
 *
 * Frame Header (8 bytes, big-endian on the wire):
 *   ┌───────────────────────────┬──────────┐
 *   │     Length (24 bits)      │ Type (8) │   bytes 0-3
 *   ├──────────┬────────────────┴──────────┤
 *   │ Flags(8) │    Stream ID (24 bits)    │   bytes 4-7
 *   └──────────┴───────────────────────────┘
 *
 * Header Encoding (simplified HPACK):
 *   Indexed:   [0x80 | index]  [value_len:2 BE] [value]
 *   Literal:   [0x00]  [name_len:2 BE] [name] [value_len:2 BE] [value]
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* ── Platform abstraction ─────────────────────────────────────────── */

#ifdef _WIN32
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0601      /* Windows 7+ */
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #ifdef _MSC_VER
  #pragma comment(lib, "ws2_32.lib")
  #endif

  typedef SOCKET sock_t;
  #define SOCK_INVALID  INVALID_SOCKET
  #define sock_close(s) closesocket(s)

  static inline void sock_init(void) {
      WSADATA w;
      WSAStartup(MAKEWORD(2, 2), &w);
  }
  static inline void sock_cleanup(void) { WSACleanup(); }

  #ifdef _MSC_VER
    typedef int socklen_t;            /* MSVC compat */
  #endif

#else /* POSIX */
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <netdb.h>

  typedef int sock_t;
  #define SOCK_INVALID  (-1)
  #define sock_close(s) close(s)

  static inline void sock_init(void)    { (void)0; }
  static inline void sock_cleanup(void) { (void)0; }
#endif

/* ── Frame constants ──────────────────────────────────────────────── */

#define FRAME_HEADER_SIZE   8          /* fixed frame header size     */

/* Frame types */
#define FRAME_REQUEST       0x01
#define FRAME_RESPONSE      0x02
/* 0x03–0xFF reserved for future versions.
 * A receiver that encounters an unknown type MUST read and discard
 * exactly `length` bytes, then continue reading the next frame.     */

/* Flags (per-frame bitfield) */
#define FLAG_END_STREAM     0x01       /* last frame on this stream   */
#define FLAG_END_HEADERS    0x02       /* all headers have been sent  */

/* Status codes */
#define STATUS_OK           200
#define STATUS_BAD_REQUEST  400
#define STATUS_NOT_FOUND    404

/* Limits */
#define MAX_PAYLOAD   ((1 << 24) - 1)  /* 16 777 215 bytes            */
#define MAX_HEADERS   32
#define MAX_HDR_NAME  256
#define MAX_HDR_VALUE 8192
#define MAX_PATH_LEN  4096

/* ── Static header table (simplified HPACK, first mechanism) ──────── */

#define STATIC_TABLE_SIZE  10
#define INDEXED_BIT        0x80        /* high bit → indexed header   */

extern const char *STATIC_TABLE[STATIC_TABLE_SIZE];
/*  Index │ Name
 *  ──────┼──────────────
 *   0    │ :method
 *   1    │ :path
 *   2    │ :status
 *   3    │ content-type
 *   4    │ content-length
 *   5    │ host
 *   6    │ server
 *   7    │ date
 *   8    │ connection
 *   9    │ accept                                                    */

/* ── Structures ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t length;       /* 24-bit payload length   */
    uint8_t  type;         /* 8-bit  frame type       */
    uint8_t  flags;        /* 8-bit  flag bitfield     */
    uint32_t stream_id;    /* 24-bit stream identifier */
} frame_header_t;

typedef struct {
    char name[MAX_HDR_NAME];
    char value[MAX_HDR_VALUE];
} header_t;

/* ── Wire I/O helpers ─────────────────────────────────────────────── */

int  read_exact  (sock_t sock, void *buf, size_t len);
int  write_exact (sock_t sock, const void *buf, size_t len);

/* ── Frame header codec ───────────────────────────────────────────── */

void encode_frame_header(const frame_header_t *h, uint8_t out[FRAME_HEADER_SIZE]);
void decode_frame_header(const uint8_t in[FRAME_HEADER_SIZE], frame_header_t *h);

/* ── Header codec ─────────────────────────────────────────────────── */

/*  Returns bytes written (>0) or -1 on error.                        */
int  encode_header(const header_t *h, uint8_t *buf, size_t cap);
/*  Returns bytes consumed (>0) or -1 on error.                       */
int  decode_header(const uint8_t *buf, size_t len, header_t *h);

/* ── Utilities ────────────────────────────────────────────────────── */

void        hexdump   (const char *label, const void *data, size_t len);
const char *guess_mime(const char *path);

#endif /* PROTOCOL_H */
