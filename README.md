# Single-threaded epoll TCP server

[![CI](https://github.com/de-brasi/single-thread-multi-client-server/actions/workflows/ci.yml/badge.svg)](https://github.com/de-brasi/single-thread-multi-client-server/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A Linux server that serves up to N concurrent clients **in one thread, without blocking calls**, reassembles messages from the TCP stream and appends them to a file in the order they arrived. Runs as a daemon and shuts down cleanly on `SIGTERM`.

Grown out of a take-home assignment: no dependencies, 24 tests, CI and sanitizer builds.

## Quick start

Needs Linux (kernel ≥ 2.6.28 for `epoll_create1`, `signalfd` and `accept4`), a C++17 compiler and `make`.

```bash
make                    # build the server and both test binaries into build/
make test               # 24 tests: unit and integration
make SANITIZE=1 test    # the same under AddressSanitizer and UBSan
```

```bash
./build/server 100 /var/log/received.txt        # daemon
./build/server -f -p 9000 100 ./received.txt    # foreground, logs to stderr

printf 'first\nsecond\n' | nc 127.0.0.1 8080
```

## Usage

```
server [options] <max-connections> <output-file>
```

| Option | |
| --- | --- |
| `-H, --host ADDR` | address to bind to (default `127.0.0.1`) |
| `-p, --port PORT` | TCP port to listen on (default `8080`) |
| `-f, --foreground` | stay in the foreground and log to stderr instead of syslog |
| `--pidfile PATH` | write the pid of the running server to `PATH` |
| `-h, --help` | show usage |

Configuration errors — a busy port, an unwritable output file — are reported *before* the process detaches, so they reach the terminal instead of only syslog.

## Protocol

Messages are delimited by `\n`.

The delimiter is a necessity, not decoration. TCP is a byte stream rather than a sequence of packets: one client `send()` can arrive across several `read()` calls, and several sends can be coalesced into one. Treating "whatever a single `read()` returned" as a message corrupts both the contents and the boundaries of the stored data.

- A message is the bytes between newlines. Every other byte, `\0` included, is payload.
- A single `\r` before `\n` is stripped, so CRLF clients work as expected.
- Bytes left unterminated when a client disconnects are still stored: losing received data is worse than storing a partial message.
- A message above 1 MiB is refused and its connection dropped, so a client that never sends `\n` cannot consume unbounded memory. Other clients keep running.

## How it works

```
   SIGTERM ──→ signalfd ─────────┐
                                 │
   connect() ──→ listen socket ──┼──→ epoll_wait ──→ dispatch ──→ output file
                                 │                       │
   data ──────→ N client ────────┘                       │
                sockets  ←────────────────────────────---┘
                            (each with its own reassembly buffer)
```

- **Edge-triggered epoll.** Both the listening socket and the client sockets are registered with `EPOLLET`, so the accept backlog and the incoming data are drained in a loop until `EAGAIN` — otherwise the remaining events would never be reported again.
- **Disconnects are detected by `read() == 0`, not by an event flag.** `EPOLLRDHUP` only means "the peer will not send any more", while the bytes it already sent may still sit in the receive buffer. Sockets are always drained first and closed on EOF, or the last messages are lost.
- **One bad connection never takes the server down.** A failing `accept()` or `read()` closes that socket and logs a line; the process keeps serving everyone else.
- **`SIGTERM` arrives through a `signalfd`** and is handled as an ordinary readable descriptor in the loop. That removes both the async-signal-safety problem (almost nothing may be called from a signal handler, `exit()` included) and the race in the "set a flag and hope `epoll_wait` notices" approach.
- **Framing lives in [`MessageStream`](src/message_stream.h)**, a class with no system calls in it, so the most error-prone part of the server is unit tested without sockets, processes or timing.

One subtlety that cost real debugging: `epoll_ctl()` attaches its wait queue entry to the signal state of the **calling** process, so a `signalfd` registered before `fork()` reports nothing in the child. Set it up before daemonizing and the result is a daemon that ignores `SIGTERM` forever while foreground mode keeps working. The event loop is therefore created after the process detaches, and a regression test keeps it that way.

## Guarantees

| | |
| --- | --- |
| Ordering | Messages reach the file in the order the server read them. Within one connection that is the order they were sent (a TCP guarantee); across connections it is the order of arrival, not the order of the clients' `send()` calls. |
| Durability | Every message is flushed immediately, so it survives even a `SIGKILL` of the server. |
| Connection limit | A client above the limit is disconnected at once rather than left waiting. Slots are released as soon as a client goes away. |
| Loss | There are no acknowledgements: bytes read but not yet written are lost if the process is killed in between. Delivery guarantees would require a reply and client-side retries. |
| Concurrency | One thread, so throughput is bounded by a single core. Multiple cores would need `SO_REUSEPORT` and one process per core. |

## Signals

| Signal | Behaviour |
| --- | --- |
| `SIGTERM` | clean shutdown: the loop exits, the file is closed, exit code 0 |
| `SIGINT`, `SIGQUIT`, `SIGHUP`, `SIGCONT` | ignored |
| `SIGPIPE` | ignored, so a write to a closed socket cannot kill the process |
| `SIGSTOP` | **cannot** be caught, blocked or ignored — a POSIX guarantee, not an omission. The kernel stops the process regardless of what the program asks for. |

## Tests

```bash
make test
```

**Unit tests** (12) cover message framing with no I/O involved: one message split across reads, several messages in one read, `\r\n`, empty messages, `\0` inside a payload, ordering, an unterminated tail, and the size limit.

**Integration tests** (12) start a real server process on a private port and talk to it over TCP: ordering, several clients, fragmented and coalesced messages, a 200 KiB message, the connection limit, the message size limit, daemonization, and the required signal behaviour.

Two are regression tests for defects this code actually had: releasing a connection slot after a disconnect, and a daemon that responds to `SIGTERM`.

Nothing waits a fixed second and hopes: the tests poll for the state they expect with a timeout, so they are fast and do not flake. The assertions are a ~100-line header rather than an external framework, so a compiler and `make` are all that is needed.

CI builds with g++ and clang++ and runs everything, plus a separate job under AddressSanitizer and UndefinedBehaviorSanitizer.

## Layout

```
src/message_stream.h        message reassembly, free of I/O and therefore unit testable
src/server.cpp              options, daemonization, signals, event loop
tests/unit_tests.cpp        framing tests
tests/integration_tests.cpp end-to-end tests against a real server process
tests/test_framework.h      minimal assertions
Makefile                    build, tests, sanitizers
```

## Limitations

- IPv4 and Linux only (`epoll`, `signalfd`, `accept4`).
- Flushing every message trades throughput for durability; flushing once per event loop iteration would be considerably faster and would risk at most one batch.
- No idle timeout: a silent client holds its slot until it disconnects.
- Clients above the limit are dropped immediately; a waiting queue might suit some uses better.
- The 1 MiB message limit is compiled in rather than configurable.

## License

[MIT](LICENSE)
