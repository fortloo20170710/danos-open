# Management-plane resource limits

The gNMI/gRPC server is reachable by unauthenticated peers during the TLS and
HTTP/2 handshake, so every buffer and queue has a fixed bound. The limits are
compile-time constants in `danos-mgmt/src/gnmi/gnmi_grpc.c`; this page records
the numbers an operator needs to size a deployment and to interpret a rejected
connection.

## Connection admission

| Limit | Value | Behaviour at the limit |
| --- | --- | --- |
| Listen backlog | 64 | Kernel-level; excess SYNs wait or are dropped |
| Concurrent connections | 128 | The accepted socket is closed immediately |
| Socket receive/send timeout | 10 s | Idle or stalled connections are torn down |
| TLS handshake timeout | 10 s | Handshake failure closes the connection |

Each admitted connection runs in its own detached thread, so the process needs
one thread and one default-sized stack (8 MiB of address space on glibc) per
connection during peak load.

## HTTP/2 and gRPC per-connection limits

| Limit | Value | Behaviour at the limit |
| --- | --- | --- |
| Max inbound frame payload | 16 KiB | Larger frames close the connection |
| Max outbound frame payload | peer `SETTINGS_MAX_FRAME_SIZE` | Honoured from 16 KiB to 16 MiB; the server sends no larger frame |
| Initial flow-control window | 64 KiB per stream and connection | `SETTINGS_INITIAL_WINDOW_SIZE` and `WINDOW_UPDATE` grow it |
| Tracked streams | 256 | A new stream that finds no slot is dropped |
| In-flight request bodies | 32 | Additional request streams are refused |
| Request body size | 256 KiB | The stream is discarded; the connection stays up |
| Response message size | 256 KiB | Larger `Get` results return a gRPC error |
| Header list size | 64 fields, 16 KiB block | The request is dropped when the block exceeds the cap |
| Parked frame queue | 64 frames | The server sends GOAWAY `ENHANCE_YOUR_CALM` (0xB) and closes |
| Active subscriptions | 32 | Further Subscribe requests are rejected |
| HPACK dynamic table | Per RFC 7541 defaults | Bounded by the decoder |

The worst-case resident cost of one connection is roughly 10 MiB when an
adversarial peer pipelines 32 near-limit request bodies while also filling the
parked-frame queue. A normal management session stays under 1 MiB. With the
connection cap, the management listener can therefore commit on the order of
1 GiB of address space in the worst case; the resident set is much smaller
because buffers are touched only as data arrives.

## Flow-control and queue behaviour

A response is sent only as fast as the client's flow-control window allows. If
frames for other streams arrive while a response is blocked, they are parked
until the window opens. The queue is bounded; reaching 64 parked frames is
treated as a peer that will not let the server make progress, and the
connection is closed with GOAWAY 0xB. The server never drops a parked frame
silently, because that would desynchronise HTTP/2 stream state.

Our own SETTINGS advertise the defaults (16 KiB frames, 64 KiB windows), so an
inbound frame larger than 16 KiB is a protocol error regardless of what the
peer advertised for its own receive direction. A peer's
`SETTINGS_MAX_FRAME_SIZE` applies to frames the server sends; values outside
16 KiB–16 MiB are ignored as RFC 9113 requires.

## Operational guidance

- Raise the connection cap only after accounting for thread stacks and the
  per-connection worst case; prefer a separate listener or a proxy for large
  fan-out.
- Keep response messages below 256 KiB by scoping `Get` paths. Subtree `Get`
  on a large FIB can exceed the limit; query a specific prefix instead.
- A GOAWAY 0xB in the client log means the client stopped granting flow-control
  window or flooded unrelated streams while a response was blocked.
- `ulimit -v` and systemd `MemoryMax` interact with the thread-stack floor:
  the address-space limit must allow 8 MiB per admitted connection even when
  the resident set is small.
