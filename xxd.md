# Reading BHTTP frames with xxd

One full round-trip: client asks for `/hello.txt`, server sends it back.

The file on disk is 51 bytes: `Hello from bserve! This is a plain-text test file.\n`

---

## The Request (28 bytes on the wire)

These bytes were saved with `bcurl --dump-frames hello localhost:9000/hello.txt`
and printed with `xxd -g 1 hello-request.bin`. For Windows, use `bcurl.exe`;
the README has the PowerShell commands.

`hello-request.bin` is 28 bytes; `hello-response.bin` is 93 bytes.
No copy-pasting hex out of a terminal needed.

```text
00000000: 00 00 14 01 03 00 00 01 02 80 00 03 47 45 54 81  ............GET.
00000010: 00 0a 2f 68 65 6c 6c 6f 2e 74 78 74              ../hello.txt
```

The first eight bytes are the frame header. After that, it is headers and body.

**Frame header (bytes 0–7):**

```
00 00 14    Length = 0x14 = 20. There are 20 bytes of payload after this header.
01          Type = 0x01 = REQUEST.
03          Flags = 0x03 = END_STREAM + END_HEADERS (bits 0 and 1 set).
00 00 01    Stream ID = 1. First (and only) stream on this connection.
```

**Payload (bytes 8–27):**

```
02          header_count = 2. Two headers follow.
```

Header 0 — `:method = GET`:
```
80          0x80 | 0 = indexed header, index 0 → that's ":method" from our static table.
00 03       value_len = 3.
47 45 54    "GET"
```

Header 1 — `:path = /hello.txt`:
```
81          0x80 | 1 = indexed header, index 1 → ":path".
00 0a       value_len = 10.
2f 68 65 6c 6c 6f 2e 74 78 74    "/hello.txt"
```

So the whole request is 8 (frame header) + 20 (payload) = **28 bytes**. An equivalent `GET /hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n` is 44 bytes of text — this particular binary request saves 16 bytes. The difference depends on the headers.

---

## The Response (93 bytes on the wire)

From `xxd -g 1 hello-response.bin`:

```text
00000000: 00 00 55 02 03 00 00 01 00 c8 03 83 00 0a 74 65  ..U...........te
00000010: 78 74 2f 70 6c 61 69 6e 84 00 02 35 31 86 00 0a  xt/plain...51...
00000020: 62 73 65 72 76 65 2f 31 2e 30 48 65 6c 6c 6f 20  bserve/1.0Hello
00000030: 66 72 6f 6d 20 62 73 65 72 76 65 21 20 54 68 69  from bserve! Thi
00000040: 73 20 69 73 20 61 20 70 6c 61 69 6e 2d 74 65 78  s is a plain-tex
00000050: 74 20 74 65 73 74 20 66 69 6c 65 2e 0a           t test file..
```

**Frame header (bytes 0–7):**

```
00 00 55    Length = 0x55 = 85. Payload is 85 bytes.
02          Type = 0x02 = RESPONSE.
03          Flags = 0x03 = END_STREAM + END_HEADERS.
00 00 01    Stream ID = 1. Same stream as the request.
```

**Payload starts at byte 8:**

```
00 c8       Status code = 0x00C8 = 200. OK.
03          header_count = 3.
```

Header 0 — `content-type: text/plain`:
```
83          0x80 | 3 = indexed, index 3 → "content-type".
00 0a       value_len = 10.
74 65 78 74 2f 70 6c 61 69 6e    "text/plain"
```

Header 1 — `content-length: 51`:
```
84          0x80 | 4 = indexed, index 4 → "content-length".
00 02       value_len = 2.
35 31       "51"
```

Header 2 — `server: bserve/1.0`:
```
86          0x80 | 6 = indexed, index 6 → "server".
00 0a       value_len = 10.
62 73 65 72 76 65 2f 31 2e 30    "bserve/1.0"
```

**Body (bytes 42–92, the remaining 51 bytes):**
```
48 65 6c 6c 6f 20 66 72 6f 6d 20 62 73 65 72 76
65 21 20 54 68 69 73 20 69 73 20 61 20 70 6c 61
69 6e 2d 74 65 78 74 20 74 65 73 74 20 66 69 6c
65 2e 0a

= "Hello from bserve! This is a plain-text test file.\n"
```

Total: 8 (frame header) + 85 (payload) = **93 bytes**.

Payload breakdown: 2 (status) + 1 (header count) + 13 + 5 + 13 (three headers) + 51 (body) = 85. Checks out.

---

## Quick sanity checks

- Length fields actually match the payload sizes? The length field doesn't get to freestyle. Request says 20, payload is 20 bytes. Response says 85, payload is 85 bytes. ✓
- All the static table indices point to the right names? 0→:method, 1→:path, 3→content-type, 4→content-length, 6→server. ✓
- Value lengths match the strings? "GET"=3, "/hello.txt"=10, "text/plain"=10, "51"=2, "bserve/1.0"=10. ✓
- Body matches the file on disk? 51 bytes, identical content. ✓
- Stream IDs match between request and response? Both are 1. ✓
