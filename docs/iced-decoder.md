# iced compatibility decoder

NeverD can use the pinned NeverSight iced fork for selected x86 32/64-bit
classification scans. Enable it with `-DNEVERD_ENABLE_ICED=ON`; the
default remains off because whole-workload and platform qualification is
incomplete. The build needs
Cargo/Rust and the initialized `third_party/iced` submodule. Cargo uses the
adapter's checked-in lockfile. Cross builds additionally select
`NEVERD_ICED_RUST_TARGET`; a multi-architecture Apple build needs separate
architecture builds before combining outputs.

`NeverDDecode` registers a per-handle adapter, consulted only in Capstone's
vector scan slow path with both text and detail disabled. Compact scalar and
VEX readers keep their existing dispatch. `lib/decode/X86IcedScan.cpp` admits a generated
closed set of 96 encodings from 14 vector move, XOR and FMA families, beginning
directly with C4/C5/62. Scalar instructions use Capstone's compact scan path. IDs are
translated from the exact iced source built with the adapter. Unmapped,
malformed, new-extension, repeated-prefix and differing branch-profile cases
continue through the existing Capstone decoder and NeverD fixups. Each Decoder
owns an independent Rust scratch decoder and detaches it before returning a
Capstone handle to the spare pool. Only mode and dispatch tables persist;
an RAII guard clears the input pointer range before each call returns. Failure of
the adapter does not publish an instruction or advance a cursor. Capstone owns
output copying, IDs, iterator advancement, errors and option transitions;
the adapter returns only a mapped ID and length. NeverD enables Capstone's
`CAPSTONE_X86_SCAN_ADAPTER` only with this option; disabled builds compile out
the callback and its state. The private static-link hook does not extend the
installed Capstone API.

Bounded function-discovery sweeps additionally use `Decoder::scanX86Light`.
Its separate closed map admits 741 scalar, legacy SSE and vector encodings.
The fork decodes up to 64 instructions per call and writes packed ID/length
records directly into caller-owned storage. Map entries mark prefix-sensitive
control transfers; ordinary mapped forms reuse iced's full validity checks.
PUSH/POP/NOP/LEAVE width names are normalized explicitly, and only the
`Movsxd_r64_rm32` form is admitted. Other MOVSXD widths, moffs, far transfers,
loop counters and unlisted extensions retain Capstone fallback. A declined
instruction is neither emitted nor included in the returned byte extent.

`X86InstructionScan` keeps this batch within one immutable byte range and x86
mode. It remembers an invalid position after a partial batch, avoiding a
second decode of the same failure; a discontinuous offset starts a new batch.
Serial/parallel CALL sweeps and x64 code-pointer scans retain their instruction
ownership checks and decode CALL/LEA operands separately. Text/detail queries
may interleave only after restoring the classification options. Disabling
iced preserves the original per-instruction sweep route.

Full text, detail, lifting, AArch64 and ARM keep their existing paths. The fork
also supplies a detailed primitive with a fixed UD1 r32/rm32 profile; this
primitive is not yet used by NeverD lifting. Its operands alone do not provide
all Capstone groups, implicit registers, access flags and rendering contracts.

The [fork audit](../third_party/iced/src/rust/iced-x86-capstone/README.md)
records the earlier Capstone fixes, existing iced behavior, profile corrections
and unsupported extensions. Decoder acceptance differences must be resolved
before expanding the map; a faster but different instruction stream is not a
valid optimization. In particular, prefixed branch vendor behavior and E3
counter-register selection retain Capstone fallback. A blanket per-instruction
redirect to iced was measured and rejected: constructor overhead, compatibility
checks, copying and double decoding on fallback outweighed its core advantage.

Before batch integration, on an Apple M4 Max (macOS arm64, Release), six rotating trials of actual
`decodeOneLight` streams gave the following CPU-time ratios against a frozen
Capstone-only NeverD executable. Counts, rejection counts and boundary/ID
checksums matched, and each trial included a repeated baseline control.

| Workload | Baseline time / adapter time |
| --- | ---: |
| SIMD 64-bit | 1.1715x |
| Scalar 32-bit | 0.9950x |
| Scalar 64-bit | 0.9840x |
| Seven system-code streams | 0.9734x–1.0113x |

Most system-code streams have very few admitted vector forms. The SIMD result
does not qualify all x86 workloads or full decompilation. Small regressions,
link-layout sensitivity and untested host platforms keep the option off by
default. These ratios compare NeverD routes; they do not establish superiority
over native iced. Batch integration must be evaluated separately in the actual
consumer, with fallback, operand redecoding and output checks preserved.

The batch route was measured separately in the actual `scanSegmentCallsX86`
consumer on the same host: five rotating trials, calibrated to roughly 150 ms
of baseline CPU time, with an identical A/A baseline control. The baseline
uses the current single-step helper with no cache. All target-set checksums
matched. Independent full streams also matched instruction counts, rejected
byte counts and boundary/ID checksums.

| CALL sweep workload | Baseline time / batch time |
| --- | ---: |
| Scalar 32-bit generated stream | 1.1275x |
| Scalar 64-bit generated stream | 1.3408x |
| SIMD 64-bit generated stream | 1.5688x |
| Five 32-bit PE code samples (4.3–7.3 KiB) | 1.0250x–1.0795x |
| Seven 64-bit system-code streams | 0.9773x–1.0357x |
| Tiny 60-byte / 12-byte controls | 0.7939x / 0.9324x |

A/A medians ranged from 0.9720x to 1.0283x under other host workloads. Small
changes in system-code streams are not evidence of a consistent win. Tiny
buffers still lose to setup cost. The option therefore remains off by default;
these results neither qualify full decompilation nor establish superiority
over native iced. Larger buffers and representative user binaries still need
whole-pipeline and other-host qualification.

A standalone comparison builds the actual adapter without LLVM:

```sh
cmake -S unittests/decode -B build-iced -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-iced --parallel 4
ctest --test-dir build-iced --output-on-failure
cargo test --release --locked --manifest-path third_party/iced/src/rust/iced-x86-capstone/Cargo.toml
```

The differential checks random/truncated input, systematic opcode/ModR/M and
prefix combinations, valid output fields, rejection/cursor parity, and guard
page tails on Unix. Its optional `BITS FILE` arguments additionally check every
byte offset in a real code stream. It also consumes the pinned iced decoder
fixtures and requires every admitted instruction family to have accepted cases
in both bitness modes. `NeverDDecoderOptionsTests` checks actual Decoder option
transitions, failed initialization, fallback and architecture changes; run it
with the option both off and on. The Rust suite compares all instruction bits,
constant offsets and rejection reasons when reusing independent input buffers.
Source-only or skipped platform checks do
not qualify performance. Measure the actual `Decoder` route with matching
inputs and checksums; retain fallback costs and A/A controls in the result.
Also compare an unregistered hook against a frozen pre-hook executable so a
new core cost cannot disappear into both sides of the comparison.
