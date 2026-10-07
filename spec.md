# BHTTP — Binary HTTP Protocol Spec

*A lightweight binary framing protocol for serving files over TCP.*

## What this is

We wanted something like HTTP but binary — fixed-size headers you can parse without scanning for `\r\n`, and a compressed way to send the same header names over and over without wasting bytes.

The whole thing runs on a single persistent TCP connection. Client connects, sends request frames, server sends response frames. Neither side opens a second connection, ever.

## Frame layout

Every frame starts with an 8-byte header, followed by whatever payload the frame carries.

```
Byte:   0        1        2        3        4        5        6        7
     +--------+--------+--------+--------+--------+--------+--------+--------+
     |          Length (24b)     |  Type  |  Flags |      Stream ID (24b)     |
     +--------+--------+--------+--------+--------+--------+--------+--------+
     |                                                                        |
     |                       Payload (Length bytes)                            |
     |                                                                        |
     +--------+--------+--------+--------+--------+--------+--------+--------+
```

- **Length** (3 bytes): how many bytes of payload follow. Max ~16 MB.
- **Type** (1 byte): what kind of frame this is.
- **Flags** (1 byte): boolean flags, meaning depends on the type.
- **Stream ID** (3 bytes): which logical stream this belongs to (start at 1).

Everything is big-endian on the wire.

We went with 24-bit length and type the same as HTTP/2 since those sizes have been proven to work. We shrank stream ID from HTTP/2's 31 bits down to 24 — we're not doing multiplexing so 16 million streams is way more than enough, and it makes the header land on an even 8 bytes, which is nice for alignment.

## Frame types

We only define two types right now:

| Code | Name     | Direction       |
|------|----------|-----------------|
| 0x01 | REQUEST  | client → server |
| 0x02 | RESPONSE | server → client |

Both can carry these flags:
- Bit 0 (0x01): `END_STREAM` — this is the last frame on this stream
- Bit 1 (0x02): `END_HEADERS` — all headers are in this frame

**The important rule:** if you receive a frame with a type you don't recognise, you MUST read exactly `Length` bytes, throw them away, and keep going. Don't close the connection, don't send an error. Just skip it. That's how we can add new frame types in v2 without breaking old implementations.

## How headers work

We borrowed the first two ideas from HPACK (HTTP/2's header compression):

**Idea 1 — static table.** The ten header names we use most often get a number. Instead of sending the whole string, we send one byte with the high bit set:

| # | Name             | # | Name             |
|---|------------------|---|------------------|
| 0 | `:method`        | 5 | `host`           |
| 1 | `:path`          | 6 | `server`         |
| 2 | `:status`        | 7 | `date`           |
| 3 | `content-type`   | 8 | `connection`     |
| 4 | `content-length` | 9 | `accept`         |

To send an indexed header:
```
[0x80 | index]  [value_len : 2 bytes]  [value]
```

So `:method` = `GET` becomes `80 00 03 47 45 54` — six bytes instead of typing out "method" and "GET" in ASCII with separators.

**Idea 2 — length-prefixed literals.** Anything not in the table gets sent with explicit lengths:
```
[0x00]  [name_len : 2 bytes]  [name]  [value_len : 2 bytes]  [value]
```

No ambiguity, no delimiter scanning. You always know exactly how many bytes to read.

## Request format

A REQUEST frame's payload looks like:
```
[header_count : 1 byte]
[header 0]
[header 1]
...
```

Every request must have at least `:method` and `:path`.

## Response format

A RESPONSE frame's payload:
```
[status_code : 2 bytes]     (e.g. 0x00C8 = 200)
[header_count : 1 byte]
[header 0]
[header 1]
...
[body bytes]                 (whatever's left after the headers)
```

Since every header knows its own length (either via the static table index + value_len, or via name_len + value_len), you just parse `header_count` headers in sequence, and everything remaining in the payload is the body.

Status codes we use:
- **200** — here's your file
- **400** — your frame was garbage, couldn't parse it
- **404** — that file doesn't exist

## Server (`bserve`)

```
$ ./bserve <root_dir> <port>
```

1. Bind to the port, wait for a connection.
2. Read 8 bytes (frame header). Parse them.
3. If it's an unknown frame type — skip `Length` bytes, go back to step 2.
4. If it's a REQUEST — pull out `:path`, look for that file under `root_dir`.
5. Send back a RESPONSE frame with 200 + the file, 404 if missing, or 400 if the request was broken.
6. Stay on the same connection, go back to step 2.

## Client (`bcurl`)

```
$ ./bcurl [-v] [--dump-frames <prefix>] <host>:<port>/<path>
```

1. Connect to the server. One connection only.
2. Build a REQUEST frame with `:method=GET` and `:path=<whatever>`.
3. Send it, read the response.
4. Dump the body to stdout.
5. If `-v` is set, print the request and response hex trace to stderr.
6. Exit 0 on success, exit 1 on 4xx/5xx.

`--dump-frames hello` saves `hello-request.bin` and `hello-response.bin`,
including each 8-byte frame header. Use `xxd -g 1` to inspect them. The prefix
can include an existing directory; reusing it overwrites the previous captures.
If a capture cannot be written, the client reports the error and exits 1.
