# BHTTP — Binary HTTP Framing Protocol

A custom binary framing protocol for serving files over a persistent TCP connection, built from scratch in C.

Includes a complete specification ([spec.md](spec.md)), annotated frame dumps using `xxd` ([xxd.md](xxd.md)), server (`bserve`), and client (`bcurl`).

The fun part is seeing a file come back as a few binary frames. The less fun part is counting offsets by hand.

---

## Overview

Text-based HTTP/1.1 requires scanning arbitrary streams for `\r\n` delimiters and repeatedly transmitting identical ASCII headers. BHTTP replaces that with a binary framing mechanism inspired by the core ideas of HTTP/2:

- **Fixed 8-byte frame header:** Easy to parse with a single `recv()` call, no string scanning.
- **Header compression:** Uses a 10-entry static dictionary (1 byte on the wire for common headers like `:method`, `:path`, `:status`) and length-prefixed literals for custom headers.
- **Strict single-connection persistence:** Everything runs over one TCP connection without renegotiation.
- **Extensible forward-compatibility:** Receivers encountering unknown frame types cleanly skip `Length` bytes and continue processing.

---

## Wire Format

Every frame begins with a fixed 8-byte header:

```text
Byte:   0        1        2        3        4        5        6        7
     +--------+--------+--------+--------+--------+--------+--------+--------+
     |          Length (24b)     |  Type  |  Flags |      Stream ID (24b)     |
     +--------+--------+--------+--------+--------+--------+--------+--------+
     |                                                                        |
     |                       Payload (Length bytes)                           |
     |                                                                        |
     +--------+--------+--------+--------+--------+--------+--------+--------+
```

- **Length (3 bytes, big-endian):** Payload size up to 16 MB.
- **Type (1 byte):** `0x01` (REQUEST), `0x02` (RESPONSE). Unknown types are skipped without terminating the stream.
- **Flags (1 byte):** Bit 0 (`0x01`) = `END_STREAM`, Bit 1 (`0x02`) = `END_HEADERS`.
- **Stream ID (3 bytes, big-endian):** Logical stream identifier.

---

## Project Structure

```text
.
├── protocol.h      # Frame wire layout, header constants, static table, prototypes
├── protocol.c      # Binary frame packing, parsing, socket I/O helpers
├── bserve.c        # Binary HTTP server (serves files from webroot)
├── bcurl.c         # Binary HTTP client, verbose trace, and raw frame capture
├── spec.md         # Full protocol specification
├── xxd.md          # Actual xxd output and a byte-by-byte frame walkthrough
├── build.ps1       # Windows PowerShell build script
├── Makefile        # GCC / POSIX Makefile
└── www/
    └── hello.txt   # Sample static file for testing
```

---

## Building

### Windows (GCC / MinGW / Clang)

Run the PowerShell build script:

```powershell
.\build.ps1
```

Or compile manually linking Winsock (`ws2_32`):

```bash
gcc -O2 -Wall -Wextra -o bserve.exe bserve.c protocol.c -lws2_32
gcc -O2 -Wall -Wextra -o bcurl.exe bcurl.c protocol.c -lws2_32
```

### Linux / macOS

```bash
make
```

---

## Usage & Verification

### 1. Start the Server

Run `bserve` specifying the root directory and port:

```bash
# Windows
.\bserve.exe .\www 9000

# Linux / macOS
./bserve ./www 9000
```

### 2. Fetch a File

In another terminal, fetch `/hello.txt`:

```bash
# Windows
.\bcurl.exe localhost:9000/hello.txt

# Linux / macOS
./bcurl localhost:9000/hello.txt
```

**Output:**
```text
Hello from bserve! This is a plain-text test file.
```

### 3. Inspect the frames with xxd

I don't like the hexdump layout. `xxd -g 1` is the GOAT here: offsets, bytes, text, done.

Save the request and response as raw bytes:

```powershell
# Windows
.\bcurl.exe --dump-frames hello localhost:9000/hello.txt
```

```bash
# Linux / macOS
./bcurl --dump-frames hello localhost:9000/hello.txt
```

Then run these with `xxd` installed and on your PATH:

```bash
xxd -g 1 hello-request.bin
xxd -g 1 hello-response.bin
```

On Windows, Git for Windows includes `xxd`. If PowerShell can't find it,
call it directly, for example:

```powershell
& 'C:\Program Files\Git\usr\bin\xxd.exe' -g 1 hello-request.bin
& 'C:\Program Files\Git\usr\bin\xxd.exe' -g 1 hello-response.bin
```

The request from a local `/hello.txt` exchange:

```text
00000000: 00 00 14 01 03 00 00 01 02 80 00 03 47 45 54 81  ............GET.
00000010: 00 0a 2f 68 65 6c 6c 6f 2e 74 78 74              ../hello.txt
```

The response:

```text
00000000: 00 00 55 02 03 00 00 01 00 c8 03 83 00 0a 74 65  ..U...........te
00000010: 78 74 2f 70 6c 61 69 6e 84 00 02 35 31 86 00 0a  xt/plain...51...
00000020: 62 73 65 72 76 65 2f 31 2e 30 48 65 6c 6c 6f 20  bserve/1.0Hello
00000030: 66 72 6f 6d 20 62 73 65 72 76 65 21 20 54 68 69  from bserve! Thi
00000040: 73 20 69 73 20 61 20 70 6c 61 69 6e 2d 74 65 78  s is a plain-tex
00000050: 74 20 74 65 73 74 20 66 69 6c 65 2e 0a           t test file..
```

Each capture includes the frame header and its payload. Running again with the
same prefix replaces the captures; they're ignored by Git. For a quick trace
without saving files, `bcurl -v` still prints the built-in hex view to stderr.
The field-by-field breakdown is in [xxd.md](xxd.md).

### 4. Error Handling (404 Not Found)

```bash
.\bcurl.exe localhost:9000/does-not-exist.txt
```

Prints the server's 404 response payload and exits with a non-zero exit code (`1`).

---

## Documentation

- [spec.md](spec.md) — Two-page formal protocol specification covering framing, header tables, status codes, and error recovery.
- [xxd.md](xxd.md) — Capture commands, actual `xxd` output, and the meaning of each field.
