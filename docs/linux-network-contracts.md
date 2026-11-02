# Offline resolver, stream and datagram contracts

NeverD can execute bounded Android address resolution and Linux TCP/UDP callers
without a host resolver or socket. These are explicit analysis inputs, not a
network stack or observations of any deployed endpoint. Unknown query, peer,
readiness or outgoing bytes stop execution instead of manufacturing success.
These inputs do not implement an application's acknowledgement protocol.

`android.resolver_queries` matches the nullable node and service strings and
all four hint fields exactly. Null differs from an empty string. Each query
specifies either the complete ordered address list or a positive API 28 EAI
error. `system_errno` is required only for EAI_SYSTEM (11). For example:

```json
{
  "android": {
    "resolver_queries": [{
      "node": "node.invalid", "service": "9001",
      "flags": 2, "family": 2, "socket_type": 1, "protocol": 6,
      "addresses": [{
        "sockaddr_hex": "020023297f0000010000000000000000",
        "canonical_name": "node.invalid"
      }]
    }]
  },
  "linux_files": {"files": []},
  "linux_network": {
    "connections": [{
      "peer_hex": "020023297f0000010000000000000000",
      "expected_send_hex": "000142", "receive_hex": "4f4b",
      "receive_eof": true, "max_send": 2, "max_receive": 2
    }]
  }
}
```

API 28 LP64 `addrinfo` nodes are 48 bytes: flags/family/type/protocol at 0/4/8/12,
address length at 16, canonical name at 24, address at 32, and next at 40.
`getaddrinfo` allocates separate guest-owned nodes and `freeaddrinfo` checks their
ownership and links before releasing them. Null free is valid; repeated free
or altered list links fail. A null node and service return EAI_NONAME before
reading hints or writing the result pointer. Direct imports and `dlsym` use the
same Bionic owner. `inet_ntop` and `inet_pton` support IPv4 and IPv6. Formatting
uses the first longest zero run and dotted compatible/mapped suffixes, preserves
errno on success, and leaves a short output buffer untouched with ENOSPC.
Parsing preserves the destination on malformed input and uses
API 28 decimal dotted quads, including leading zeroes; an unknown family returns
EAFNOSUPPORT before accessing pointers.

`linux_network` also requires `linux_files`. Its connections are ordered virtual
attempts with canonical Linux `sockaddr_in` (16 bytes) or `sockaddr_in6` (28
bytes). Ports and addresses use network byte order; the family uses the guest's
little-endian ABI. A peer must match the next supplied attempt. `connect_errno`
can supply EINTR, ECONNREFUSED, ETIMEDOUT, ENETUNREACH, EHOSTUNREACH or EACCES instead of
success. A failed connection cannot carry stream data, and its socket must be
closed before retry. There are at most 4,096 stored TCP/UDP entries and 1 MiB of
payload bytes combined. Counted failures use separate finite counters.

`socket`, `connect`, `sendto` and `recvfrom` share one implementation across raw
x64/AArch64 Linux syscalls, Android named wrappers and `syscall`. Bionic `send`
and `recv` lower to that same owner. Connected TCP accepts no explicit peer
argument in send/receive. Scalar `read` and `write` share stream cursors;
`dup`/`dup3` aliases and closing/reusing descriptor numbers retain the same
open-description lifetime. Bionic alone converts raw negative errors to errno.

Every outgoing byte is compared before the cursor advances. Per-call ceilings
of 1–65536 bytes model short transfers. The receive stream ends only when
`receive_eof` explicitly says so; exhausted input without EOF remains unknown.
Bad peer or transfer bytes do not consume the connection or stream contract.
An outgoing TCP mismatch identifies the first differing byte by its absolute
stream offset and its offset in the current buffer, with expected and actual
byte values. Only the admitted short-transfer window is compared; a matching
prefix within a failed window is not consumed. Descriptor aliases report
offsets using their shared stream cursor. The diagnostic also identifies the
zero-based index in `linux_network.connections`. This index belongs to the
bound open description, so aliases retain it and a reused FD can refer to a
different index. Declared failed connection attempts occupy indices; rejected
peer or memory admission does not consume one.
Socket seek returns ESPIPE. `setsockopt`/`getsockopt` share SO_RCVTIMEO and
SO_SNDTIMEO state across aliases, accepting whole seconds up to one day;
negative seconds retain immediate-timeout state. Fractional timeouts stop
because kernel tick rounding is not supplied. Short getters copy only the
requested prefix and report its length. These options do not create peer
readiness or advance the guest clock: stream completions remain explicit inputs. Cross-page partial faults, scatter/gather sockets,
nonblocking TCP connect completion, edge-triggered epoll, other socket options,
shutdown and real transport are not covered by this bounded contract.

## Ordered receive completions

A TCP connection can use `receive_events` instead of `receive_hex`. Each event
contains exactly one of a nonempty `receive_hex` or positive `errno` (EINTR 4
or EAGAIN 11). Reset and fatal transport errors remain unsupported because they
also require connection-state transitions. Optional
`after_send` is the number of outgoing bytes that must already have been
accepted. Events retain their order and short-read cursor. `receive_eof` still
explicitly selects EOF after the last event; otherwise readiness stays unknown.
An error is a single explicit completion, not a permanent socket error or an
asynchronous signal model. EAGAIN requires nonblocking/MSG_DONTWAIT or nonzero
receive-timeout state. No host or guest time is advanced by these observations.

## Connected UDP

`datagram_connections` supplies ordered UDP `connect` route observations,
independently of the TCP attempt list. Either list may be omitted. A successful
UDP connect sets the peer; it does not perform a handshake or prove delivery.
Both IPv4 and IPv6 and protocol 0/17 are admitted, including nonblocking UDP
creation. An interrupted connect may be retried on the same description using
the next observation. Other failed connections require closing the description
in this bounded model. Reconnect, AF_UNSPEC disconnect, bind and unconnected
or explicit-destination sends remain unsupported. IPv6 flow information must
be zero; source addresses come from the explicitly supplied connected peer.

```json
{
  "linux_files": {"files": []},
  "linux_network": {
    "datagram_connections": [{
      "peer_hex": "020023297f0000010000000000000000",
      "expected_datagrams": ["00ff42", ""],
      "receive_events": [
        {"receive_hex": "4f4b", "after_send": 1},
        {"receive_hex": "", "after_send": 2},
        {"errno": 11, "after_send": 2}
      ]
    }]
  }
}
```

Each outgoing call must match a complete expected datagram; no short send is
invented. UDP `after_send` counts accepted datagrams, including empty ones.
Each receive event is one datagram or one error; there is no UDP EOF. A short
receive discards the packet suffix. MSG_PEEK keeps the packet, MSG_TRUNC returns
its full length, and MSG_WAITALL has no effect. A zero-size `recv` can consume a
packet, whereas a zero-size scalar `read` leaves it queued. Scalar writes and
reads use the same description and packet cursors as send/recv wrappers.
`recvfrom` can copy the declared peer, truncating to the input socklen and
publishing the full required length. A null peer pointer ignores socklen.

Admission allows at most 4096 outgoing packets and 4096 receive events per
connection, each UDP packet at most 65507 bytes. Failed memory admission,
unsupported flags, mismatched bytes and unmet send prerequisites stop without
consuming a packet. Partial-copy faults and payload overlap with the output
socklen control word are explicitly unsupported, since Linux may consume or
partially publish data on these paths. Valid output copies retain payload,
address, then length order. FD aliases share packet cursors, timeout values and
route identity; a reused descriptor gets fresh state.

The datagram and receive rules follow the pinned Linux 5.10
[UDP owner](https://github.com/torvalds/linux/blob/v5.10/net/ipv4/udp.c) and
[socket syscall owner](https://github.com/torvalds/linux/blob/v5.10/net/socket.c).
The implementation is an independent bounded model of these ABI contracts.

Android's `__memcpy_chk`, `__memmove_chk` and `__memset_chk` use the ordinary
Bionic memory owner after checking the supplied destination extent. An exceeded
extent fails before pointer access; overlap remains unsupported for memcpy and
supported for memmove. Zero-length calls touch no memory, and the compiler's
unknown-extent sentinel does not require the whole sentinel-sized region to be
mapped. Fortify failure is reported as a runtime failure, without fabricating
a libc return or claiming to deliver SIGABRT. This follows the API 28
[AArch64 checked-copy entry](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/arch-arm64/generic/bionic/__memcpy_chk.S)
and [fortify contracts](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/bionic/fortify.cpp).

These ABI rules follow the pinned [AOSP netdb definitions](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r1/libc/include/netdb.h),
[address resolver](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r1/libc/dns/net/getaddrinfo.c)
and [address formatter](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r1/libc/upstream-openbsd/lib/libc/net/inet_ntop.c).

Timeout option validation follows the [Android common kernel socket owner](https://android.googlesource.com/kernel/common/+/refs/heads/android12-5.10/net/core/sock.c).


## Timed readiness

Optional `ready_at_ns` on a data receive event declares its arrival in elapsed
virtual nanoseconds since workload start. It also asserts no earlier read
readiness while that event is next. Ordered timestamps must not decrease;
untimed events retain their prior completion contract. `after_send` is still a
separate prerequisite: unmet prerequisites do not prove a quiet socket and
remain unsupported. Completion errors cannot carry an arrival timestamp.

A TCP connection may pair `receive_eof: true` with `receive_eof_at_ns` to declare
when EOF becomes readable after all data. Alternatively TCP or UDP may provide
`receive_quiet_until_ns`: once all receive events are consumed, it asserts no
read readiness through that elapsed nanosecond, inclusive. The next nanosecond
is unknown, not an implied EOF, packet, EAGAIN completion or successful wait.
Quiet intervals cannot coexist with EOF. End times must follow supplied event
times. All times fit signed 64-bit nanoseconds; a quiet interval must have a
representable first unknown nanosecond. Failed connects cannot carry schedules.
Every timed input requires `linux_time.advance_on_idle: true`.

```json
{
  "linux_files": {"files": []},
  "linux_time": {"advance_on_idle": true},
  "linux_network": {
    "connections": [{
      "peer_hex": "020023297f0000010000000000000000",
      "receive_events": [{"receive_hex": "4f4b", "ready_at_ns": 7000000000}],
      "receive_quiet_until_ns": 10000000000
    }]
  }
}
```

`pselect6` can now suspend with a positive or infinite timeout when the requested
states are known and a future event exists. Finite waits on an empty set need
only their timeout. With cooperative Android threads, other runnable threads
run before time advances to the earliest I/O or sleep event. Without threads,
the same kernel owner advances the idle clock directly. The model uses exact
virtual deadlines, without host scheduling delay or kernel timer slack.
Readiness is rescanned on wake and wins at an exact deadline tie. Querying never
consumes a receive event; partial TCP reads and UDP peeks retain readiness.
A nonblocking receive before a declared arrival returns EAGAIN without consuming
input. Direct blocking `read`, `recv` and `recvfrom` use the same arrival and
idle-clock owner. A pending receive retains its open socket description,
scalar arguments and the timeout at entry. Closing or reusing the FD from
another thread cannot redirect that receive to another socket; aliases still
share the original stream or packet cursor. Another consumer can take the
available input first, in which case the pending operation waits for the next
declared event without restarting its timeout.

The supported whole-second `SO_RCVTIMEO` creates an exact virtual deadline.
Zero means no deadline; negative seconds mean no waiting. Known absence through
the deadline permits EAGAIN, and data or EOF at the deadline takes precedence.
Unknown readiness and unmet send prerequisites remain unsupported even with a
timeout or nonblocking flag. Explicit error completions keep their existing
untimed meaning. The model does not infer kernel timer slack, packet delivery
or missing observations from the timeout setting.

Receive output memory is checked when data arrives. UDP peer length is read at
completion, and existing payload/address/length copy order, PEEK/TRUNC, empty
datagrams and partial-copy refusal remain in force. Zero-size `read` does not
dequeue a datagram; zero-size UDP `recvfrom` can wait for and consume one.
TCP WAITALL/PEEK, signals and nonblocking connect completion remain unsupported.
Buffered stdio retains its own libc continuation over these same kernel reads;
see [Android streams](android-native-emulation.md).
The description lifetime and timeout behavior follow
[Linux close](https://man7.org/linux/man-pages/man2/close.2.html),
[receive](https://man7.org/linux/man-pages/man2/recv.2.html) and
[socket options](https://man7.org/linux/man-pages/man7/socket.7.html).

The full user buffer extent is admitted before waiting, including at EOF and
for zero-length calls. `recvfrom` checks the extent before the FD lookup; a
descriptor `read` checks the FD first. An out-of-range extent returns EFAULT;
page permissions remain a completion-time check. Extents above guest
`MAX_RW_COUNT` are explicitly unsupported because read and recv import/clamp
ordering differs. This follows the Linux 5.10
[socket entry](https://github.com/torvalds/linux/blob/v5.10/net/socket.c) and
[iterator import](https://github.com/torvalds/linux/blob/v5.10/lib/iov_iter.c).

A suspended select retains its copied bitmaps and absolute deadline. Other
threads may modify guest input memory without changing that copy; output access
is checked at completion. Bitmap outputs precede remaining timeout output,
including aliased pointers and ordinary copy errors. Raw traps and Bionic's
`syscall` share the pending operation, while libc alone translates errno.
Closing/replacing a watched FD during suspension is deliberately unsupported;
reusing its number cannot silently attach the wait to a new description.
These are bounded, explicit workload inputs, not measurements of a deployed
server or promises that Android's entire networking stack is reproduced.


## Poll and epoll waits

The shared Linux descriptor owner implements LP64 `poll`, `ppoll`,
`epoll_create`/`epoll_create1`, `epoll_ctl`, `epoll_wait` and `epoll_pwait`.
AArch64 uses its native `ppoll`/`epoll_pwait` trap numbers; Android's `poll`,
`ppoll`/`ppoll64` and epoll imports adapt to the same owner. No host socket,
polling API or wall-clock wait supplies readiness.

Ordinary IN/OUT and RDNORM/WRNORM queries use the socket's existing offline
arrival and expected-send state. Regular files, directories and modeled proc
files are immediately ready for ordinary reads/writes. Admitted live process
descriptors retain their existing non-exited observation. Other descriptor
kinds, priority/RDHUP queries and unobserved asynchronous errors remain
unsupported. Receive EOF makes IN readable; it does not invent a full HUP.

Poll copies fd/events at entry, counts nonzero revents entries (including
duplicates), ignores negative fds and reports NVAL for invalid fds. Revents are
written in array order after wake. A closed/reused descriptor during a pending
poll stops explicitly. Zero timeouts query immediately; finite and infinite
waits require a known future readiness or timeout event. Positive timeouts use
virtual idle time. Raw ppoll updates the remaining timespec after revents;
Android ppoll preserves its caller's timespec. Output faults retain earlier
stores, with mixed-access fields refused explicitly. Temporary signal masks
are unsupported.

Epoll owns a shared interest set across dup. Registration identity includes
both the original fd number and the open description: closing that number
retains a watch while an alias or in-flight operation still owns the description.
Last release removes it. Suspended waits retain their epoll set and observe
subsequent ADD/MOD/DEL operations. Level-triggered delivery and ONESHOT with MOD
rearming are supported, with deterministic round-robin delivery when maxevents
truncates a ready set. Each successful output commits its own one-shot state;
a later fault preserves earlier events. ARM64 padding is left untouched, while
x64 uses its packed 12-byte event ABI. Regular-file registration returns EPERM.
Nested epoll, edge/exclusive/wakeup modes and epoll fd polling remain explicit
unsupported boundaries. Registration count is bounded by the model's 4096-entry
budget; this is not an invented kernel ENOSPC result.

Semantics are independently implemented from
[Linux eventpoll](https://github.com/torvalds/linux/blob/v6.6/fs/eventpoll.c),
[Linux select/poll](https://github.com/torvalds/linux/blob/v6.6/fs/select.c) and
[Android's poll wrappers](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/bionic/poll.cpp).

### Counted TCP failures

An explicit `tcp_connect_failures` object contains a nonzero `errno` and finite
nonzero `count`. It becomes available only after all ordered TCP attempts have
been consumed. It admits canonical IPv4/IPv6 peers matching the socket family
and returns only the supplied failure; it creates no route, connection, bytes
or readiness and never contacts the host network. Missing or exhausted input,
wrong peers while exact attempts remain, nonblocking completion and failed
socket reuse stop explicitly. Descriptor aliases share the failed description.

The failure count accepts 1 through `UINT32_MAX`, independently of the 4,096
stored TCP/UDP entries. It retains one counter and creates no per-attempt
inventory. The exhausted count is checked before incrementing, including at
`UINT32_MAX`. Execution instruction, event and time budgets still bound each
run. JSON and typed input use the shared validator; decimal strings follow the
existing integer wire contract. Allowed errors match explicit TCP attempts:
EINTR, ECONNREFUSED, ETIMEDOUT, ENETUNREACH, EHOSTUNREACH and EACCES. The guest must
close a failed socket before retrying, including after EINTR; this bounded contract does not
model an asynchronous handshake.
