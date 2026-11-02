# Android log readers and explicit event writes

The Android native API-28 profile models these liblog functions over an explicit
visible snapshot: `android_logger_list_alloc`, `android_logger_list_alloc_time`,
`android_logger_open`, `android_logger_list_read`, and
`android_logger_list_free`. Direct imports and explicitly declared dynamic
providers use the same workload owner. No phone or host logging service is used.

Supply `android.log_records` as a timestamp-ordered array. Omission means that
the records are unknown; `[]` explicitly supplies an empty snapshot. Every entry
has all seven fields:

```json
{
  "log_id": 0,
  "pid": 42,
  "tid": 43,
  "seconds": 1700000000,
  "nanoseconds": 123,
  "uid": 10000,
  "payload": "0464656d6f0068656c6c6f00"
}
```

`payload` contains hexadecimal bytes, including any priority, tag, text or
binary event structure that the guest expects. The reader does not interpret
or manufacture those bytes. Fields are unsigned 32-bit numbers (decimal or
hexadecimal strings are also accepted); `pid` cannot exceed `INT32_MAX`, and
`nanoseconds` must be below one billion. Buffer IDs 0 through 5 are admitted.
Payloads are at most 4068 bytes. A snapshot contains at most 4096 entries and
1 MiB including the 28-byte headers; its size also counts toward the input
catalogue memory limit. C++ callers use `AndroidNativeOptions::LogRecords`
and share the same admission checks.

For `dlopen`/`dlsym`, separately list the exact exported names under
`android.libraries["liblog.so"]`. Having a model does not make a library present.
Closing a dynamic provider retains the existing inactive-provider refusal.

## Reader contract

- Each list has independent buffer selections and cursor. Reopening an already
  selected buffer returns its existing identity. Adding a new buffer restarts
  selection with the list's original filters, matching transport reinitialization.
- PID zero selects all visible records. A nonzero PID selects that process.
  `tail == 0` selects the whole matching snapshot; otherwise the last `tail`
  matching records are retained, in supplied order.
- `log_time` is the API-28 pair of 32-bit seconds/nanoseconds, passed together
  in an AAPCS64 integer argument register. Epoch zero selects the beginning.
  A nonzero start excludes records at that exact timestamp and earlier.
- A successful read writes a little-endian `logger_entry_v4` followed by the
  exact payload and returns the combined byte count. The whole 5124-byte
  AArch64 `log_msg` is zeroed first, including its alignment padding.
- Nonblocking mode `0x800` returns `-EAGAIN` at the end and clears the same full
  output buffer. The negative result is an `int`; the model does not convert it
  into a libc `-1`/errno response. Null or unselected lists return `-EINVAL`
  without touching the output. Invalid buffer IDs return a null logger.
- Mode zero may read already supplied records, but exhaustion stops as
  unsupported because no live producer was supplied. Missing snapshot input
  also stops without fabricating EOF. Wrap, persistent, write and unknown modes,
  negative PID filters, and security/kernel-buffer admission are unsupported.
- List and logger handles are opaque guest allocations with shared workload
  lifetime and memory accounting. Freeing a list retires its children. Null free
  is harmless; retired, unmapped or permission-tampered handles fail clearly.
  A failed output access does not advance the record cursor.

The supplied snapshot is explicitly the stream visible to this caller. This
does not reconstruct logd credentials, sockets/FDs, pruning/chatty records,
concurrent writes, alarms, persistent storage, or real-time streaming.
There is no inferred permission grant based on a record's UID.

## Event write contract

`__android_log_bwrite` and the shared API 28 ServiceManager slow/statistics
logging branches consume one ordered `android.event_log_writes` transcript.
Each entry has exactly `payload` (hexadecimal bytes including the little-endian
int32 tag) and `result` (a signed int32 liblog return value). For example:

```json
{"event_log_writes":[{"payload":"7b000000002a000000","result":9}]}
```

This supplies the result of writing tag 123 with an integer payload of 42.
It does not assert delivery to logd. Omission leaves results unknown; `[]`
explicitly supplies no writes. Missing input, exhaustion and a byte mismatch
stop as unsupported without consuming that entry. Identical repeated writes
need repeated entries. The report's `android.event_log_writes_consumed` appears
when a transcript is supplied, including zero if no entry has been consumed.
C++ callers use `AndroidNativeOptions::EventLogWrites` and the same validator.
Entries contain 4..4068 bytes; at most 4096 entries and 1 MiB including four
result bytes per entry are admitted and charged to catalogue memory.
Validated payload hex uses the shared JSON payload accounting. The separate
64 KiB metadata ceiling (including record/field overhead and escapes) and
overall wire ceiling still apply, so some large C++ transcripts require fewer
entries when supplied as JSON.

The raw native call narrows the tag to int32 and reads at most the first
4064 guest payload bytes, matching logd iovec clipping. A zero-length payload does not read its pointer. Returned values keep
their int bits and do not change errno. Dynamic calls require the explicit
`liblog.so` symbol catalogue, as readers do. Payload access failures preserve
the write cursor. Bytes beyond the clipped prefix are not accessed.

ServiceManager uses the pinned tags 230001 (slow) and 230000 (statistics),
binary list/int/string encoding, shared ART String conversion, Java signed
division and wrapping counters. A null service name encodes `NULL`. Slow-log
time uses a strict five-second comparison or a greater duration; statistics
time uses an inclusive comparison. The slow record precedes its timestamp
updates and the statistics record precedes counter reset. Negative liblog
results are returned normally and ignored by ServiceManager. Work/resource
stops preserve only earlier state changes. Encoding and truncation reuse the
same owner as the five general `EventLog.writeEvent` overloads.

`EventLog.writeEvent` accepts `int`, `long`, `float`, `String` and `Object[]`.
Arrays visit at most 255 items and accept only null, String, Integer, Long and
Float; nested arrays and other items throw `IllegalArgumentException` before
any write. Null String/items encode `NULL`, null arrays encode `[NULL]`, and
zero/one-item arrays omit the list header. IEEE-754 bits and shared ART JNI
string encoding are preserved, including NUL and surrogate units. Primitive
Integer/Long/Float constructors retain ordinary final value fields in the
shared heap. Wrapper factories/caches, comparison/formatting and JNI floating
field accessors are not supplied by this addition.

Strings truncate by encoded byte count, even inside a multibyte sequence.
API 28 reserves the list header before encoding, then removes it for single
items. Its original cursor advance after string truncation can leave one or
two zero bytes at the end of a singleton packet; these bytes are retained.
A scalar that does not fit is omitted atomically and stops further array
visits. The completed prefix is still written, but the earlier `-EIO` encoding
result takes precedence over the explicit transport result. A truncated string
alone does not set that error, so a following invalid item still throws.

JNI ordinary/V calls read promoted double values from the independent AAPCS64
FP register/save area or shared overflow stack. The shared ARM64 narrowing
model applies all four FPCR rounding modes, FZ and DN, and accumulates IOC,
OFC, UFC, IXC and IDC into FPSR while retaining prior status and QC. Tininess
is detected before rounding; FZ output flushing raises UFC without adding
IXC. Signaling NaNs raise IOC even with DN. Earlier conversion effects survive
a later event-write failure. Optional FP controls and exception traps remain
unsupported. A-form calls retain the low 32 jvalue bits, including signaling
NaNs, without conversion or added exceptions. FPCR, va_list and save areas
remain unchanged. DEX can pass float register bits to exact library leaves;
general floating DEX arithmetic/bodies and registered-native FP calls remain
unmodeled.

This finite contract supplies neither logd sockets, credentials, filtering,
timestamps, daemon persistence nor a live reader stream. Writes do not append
to `log_records`. Java event readers, text logging and security/stats-buffer
writes remain outside this subset.

## ABI evidence and verification

The implementation is independently written from the public Android 9 contract.
References are pinned to AOSP `android-9.0.0_r1`:

- [log_read.h](https://android.googlesource.com/platform/system/core/+/android-9.0.0_r1/liblog/include/log/log_read.h)
  defines the entry/message layouts, payload bound, reader signatures and modes.
- [logger_read.c](https://android.googlesource.com/platform/system/core/+/android-9.0.0_r1/liblog/logger_read.c)
  defines null/empty-list behavior, duplicate opens and transport reset.
- [logd_reader.c](https://android.googlesource.com/platform/system/core/+/android-9.0.0_r1/liblog/logd_reader.c)
  clears the complete message and reports nonblocking exhaustion.
- [LogBuffer.cpp](https://android.googlesource.com/platform/system/core/+/android-9.0.0_r1/logd/LogBuffer.cpp)
  and [LogTimes.cpp](https://android.googlesource.com/platform/system/core/+/android-9.0.0_r1/logd/LogTimes.cpp)
  define start positioning and PID/buffer-filtered tail selection.
- [ServiceManager.java](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/os/ServiceManager.java)
  and [EventLogTags.logtags](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/java/android/os/EventLogTags.logtags)
  define logging conditions, field update order and event tags.
- [eventlog_helper.h](https://android.googlesource.com/platform/frameworks/base/+/android-9.0.0_r1/core/jni/eventlog_helper.h),
  [log_event_list.c](https://android.googlesource.com/platform/system/core/+/android-9.0.0_r1/liblog/log_event_list.c)
  and [logger_write.c](https://android.googlesource.com/platform/system/core/+/android-9.0.0_r1/liblog/logger_write.c)
  establish binary encoding, null strings and the liblog result/errno boundary.
- [logd_writer.c](https://android.googlesource.com/platform/system/core/+/android-9.0.0_r1/liblog/logd_writer.c)
  clips the tag and payload iovecs to the maximum delivered payload.
- [reflection.cc](https://android.googlesource.com/platform/art/+/android-9.0.0_r1/runtime/reflection.cc)
  defines promoted variadic float conversion and raw jvalue float carriers.
- API 28 [Integer](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/java/lang/Integer.java),
  [Long](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/java/lang/Long.java) and
  [Float](https://android.googlesource.com/platform/libcore/+/android-9.0.0_r1/ojluni/src/main/java/java/lang/Float.java)
  define the final primitive value fields and primitive constructors.

`AndroidLog.*` compiles independent O0/O2 callers with ordinary, APS2 and RELR
relocations for Unicorn and HVF. Its byte oracle checks the complete output,
aggregate arguments, filters, tails, exact-time exclusion, independent cursors,
reopening, lifetime, errors and unsupported inputs. `AndroidLogOptions.*`
checks the public JSON boundary. These checks establish the finite contract
above, not equivalence to a complete Android logging daemon.

This is a versioned contract. Android 13's public
[LogdRead](https://android.googlesource.com/platform/system/logging/+/android-13.0.0_r1/liblog/logd_reader.cpp)
does not clear the whole message before receiving it, and its
[reader wrapper](https://android.googlesource.com/platform/system/logging/+/android-13.0.0_r1/liblog/logger_read.cpp)
only adds a terminator after a valid packet. Do not use this API-28 profile to
infer the untouched buffer bytes or logger-handle internals of later releases.
