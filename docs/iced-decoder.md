# iced compatibility decoder

The default x86 classification path now uses Capstone's native batch decoder.
Function discovery uses that path with `NEVERD_ENABLE_ICED=OFF`; the option below
adds the experimental iced backend. Capstone still owns full operand decoding
and rendering in both configurations.

The native CALL sweep was measured on 2026-10-26 on an Apple M4 Max,
macOS arm64, Release, with the latest LLVM fork built from source. Its compact
CALL-target query avoids a second decode into printed text and full operand
detail. The baseline is the previous native batch implementation at Capstone
`39f4230cd1a24a39b61ad7e867be612dc80bf6ea`, rebuilt against the same LLVM and
NeverD libraries. Nine rotating trials used fresh processes, matching thread
QoS, about 180 ms of baseline CPU time, and separate identical A/A controls
for native Capstone and the optional iced adapter. All target sets and counts
matched, including an independent detailed single-step reference.

| CALL sweep workload | Previous native / new native time | iced adapter / new native time |
| --- | ---: | ---: |
| Seven 64-bit system-code streams | 1.384x–1.861x | 1.152x–1.441x |
| Five 32-bit PE code samples | 1.773x–1.884x | 1.077x–1.116x |
| Generated scalar64 / scalar32 / SIMD | 1.054x / 1.022x / 1.006x | 1.182x / 1.232x / 1.198x |

Native A/A medians ranged from 0.988x to 1.032x, and iced A/A medians from
0.965x to 1.010x under other host workloads. Small synthetic changes are within
that noise range. The iced column measures NeverD's compatibility adapter,
including its fallback costs; these are CALL-scan results, not full
decompilation measurements or a comparison of identical full-detail outputs.

Capstone `a0340fe4122fbf43f2a5f7000e20ea49daa7e5ec` now enters the
shared terminal readers directly for common legacy headers. Previously the
source-extension selector walked prefixes before the terminal reader parsed
the same header. Existing feature predicates preserve extension precedence,
including mixed mode flags; admission misses retain the original dispatch.
Operand reading, legality checks, raw state and full output semantics are
unchanged. No iced implementation code was copied.

The full Release library was compared with Capstone
`9a96a08dae5c0621dd3118d59dd4559b750d5197` on 2026-10-27 on the same Apple
M4 Max. Nine rotating fresh-process trials used about 180 ms of baseline CPU
time, matching thread QoS, and duplicate baseline, candidate and iced A/A
controls. Ratios are medians of paired trials; each trial combines duplicate
timings geometrically. All observations are retained, including host workload
noise. Separately taking each variant's median can misrepresent speed changes.

| Decode workload | Previous / new native batch time | Previous / new public iterator time |
| --- | ---: | ---: |
| Scalar64 / scalar32 / scalar16 | 1.119x / 1.235x / 1.286x | 1.059x / 1.190x / 1.000x |
| SIMD64 | 0.997x | 0.995x |
| Six 64-bit system-code streams | 1.032x–1.124x | 0.979x–1.115x |
| Five PE32 code samples | 1.168x–1.197x | 1.127x–1.176x |

Against native iced `8b8d90b13374fbf38e7279749e48c673f4ec243a`, native
batch decoding measured 1.232x–1.743x iced/Capstone time on the six system
streams, 1.259x / 1.442x / 1.196x on scalar64 / scalar32 / scalar16, 1.126x
on SIMD, and 1.208x–1.453x on the five PE32 samples. All 15 substantive cases
with matching instruction boundaries favor native Capstone on this host.
SIMD's change from the previous Capstone version is within A/A noise; some
individual system-stream measurements are also noisy. The public iterator
still trails native iced on scalar, SIMD, two system streams and PE32 cases,
so the broader performance goal remains open. One SSH stream has different
iced acceptance and is excluded from iced rankings. Two tiny MinGW controls
are separate and do not establish broad PE32 performance. Other hosts,
full-detail decoding and whole decompilation remain unqualified.

Validation included 272,920,462 frozen-baseline scan comparisons, 37,916,536
full API observations and 864,000 raw-state comparisons, all unchanged.
Release passed 116 tests with one x87 hardware skip on arm64; ASan/UBSan and
reduced builds each passed six checks, and the optional iced adapter parity
check passed. Eight AArch64/ARM/Thumb/RISC-V cases retained identical outputs
with no stable regression. The other 107 decoder/core objects are byte-identical
to the baseline; the x86 decoder object grows by about 26 KiB.

Capstone `44e5f3293124a6bc2c42ad5a6ff4ca9b82bc7962` retains the bounded
iterator byte copy introduced in `9e6e87177334998e122a5ba9d69d06f187ffbf09`
and improves three remaining costs. Generated public terminal flags separate
ModR/M reads from optional immediates while preserving the exact ordered
reader calls. Admitted terminal output stays separate from the fallback result
passed by address to cold helpers; fallback selection remains inline. VEX
W/L/pp select generated references to the existing validated opcode rows.
Native batches retain their original scalar operand dispatch. No iced source
was copied, and shared output, detail-pointer and callback policy is unchanged.

The separate result matters for 16-bit decoding: changing operand dispatch
alone increased register moves on an indexed terminal. An instruction trace of
`F6 C2 01` grew from 168 to 185 host instructions, including 15 extra register
moves. Keeping terminal output independent of the fallback result reduced the
same trace to 161. This diagnosis led to the combined implementation rather
than a mode-specific bypass.

The same nine-trial, paired fresh-process protocol compared the final Release
library with `9e6e87177334998e122a5ba9d69d06f187ffbf09` on 2026-10-27:

| Decode workload | Previous / new public iterator time | Native iced / new public iterator time |
| --- | ---: | ---: |
| Scalar64 / scalar32 / scalar16 | 1.063x / 1.039x / 1.133x | 0.909x / 0.990x / 0.896x |
| SIMD64 | 1.044x | 0.905x |
| bash / SQLite | 1.085x / 1.055x | 1.062x / 1.058x |
| Four held-out system streams | 0.967x–1.008x | 1.194x–1.268x |
| Two PE32 O0 samples | 1.076x–1.078x | 1.060x–1.061x |
| Three PE32 O2 samples | 1.049x–1.068x | 0.877x–0.949x |

A separate nine-trial follow-up retained all observations and measured scalar16
at 1.100x and PE32 SEH O0 at 1.078x against the previous version. The grep and
sed ratios changed from 0.984x / 0.967x to 0.995x / 0.991x. Those results remain
small regressions, with variation comparable to some A/A controls; they are not
reported as wins. Public iteration now leads iced on bash, SQLite and the two
PE32 O0 samples as well as the four held-out streams, but scalar, SIMD and PE32
O2 still trail. The broader performance goal remains open.

Native batch decoding remains faster than native iced on all 15 substantive
matching-boundary cases, measuring 1.161x–1.771x iced/Capstone time. SIMD batch
decoding improved 1.042x against the previous Capstone version. Most other batch
results are close to baseline; noisy individual results such as ls do not
establish additional speedups. SSH and the tiny controls retain the exclusions
above. These results do not establish universal or full-detail superiority.

The final build again matched 272,920,462 scan, 37,916,536 full API and 864,000
raw-state observations. Release passed 116 checks with the same x87 hardware
skip on arm64; ASan/UBSan and reduced builds each passed six checks. All 72
callback combinations, optional adapter parity and adapter length-boundary
checks passed. A generated-data audit confirms that only the intended public
reader flags changed; raw terminals, indexed decisions and VEX metadata retain
their original values. The other 107 core/decoder objects, including shared
`cs.c`, are byte-identical to the baseline. Eight AArch64/ARM/Thumb/RISC-V
workloads retained identical outputs and measured 0.992x–1.015x; no non-x86
speedup is claimed. Other hardware, full-detail performance and whole
application throughput still require separate qualification.

AArch64 also avoids unnecessary operand postprocessing in Capstone
`ff10148fdc21e37359bc9eb90164e26f44ca7ddf`. Generated flags identify which opcodes
require implicit SME operands or tied-operand handling. Other instructions skip
those descriptor walks; flagged instructions retain the original helpers and
ordering, including failed decodes. The generator reapplies the guards after
translation and requires review when the owning helper bodies change. A native
audit checks every descriptor, and generation is independent of checkout line
endings. Release and sanitizer comparisons preserve full API outputs. This
change leaves x86 and the remaining decoder/core objects unchanged; it does not
extend iced comparisons beyond x86.

Capstone `fe40ade693c17a7e13f48fa1dd3202da44f2dcfd` reduces public iterator
call overhead. On supported Clang targets, private decoder and output calls
preserve additional registers so the iterator needs fewer saves and restores.
Skip-data handling remains owned by the shared core, including callback-visible
detail state and restoration. An unprefixed C5 header uses the same VEX reader
inlined after source-feature selection, retaining operand and legality checks.
Public API and callback calling conventions are unchanged. Callback tests cover
success, refusal, oversized skips and detail-pointer replacement. Reduced tests
explicitly check rejection of generated vector opcodes omitted by that build.
The native batch path remains the default; broader performance qualification,
including the remaining public-iterator gaps against iced, is still ongoing.

Capstone `c21fe5a506c43d13e296b917fd021e021f9306d8` expands public scan coverage
for three-operand scalar and ordinary SSE/MMX descriptors. Admission reuses
existing ordered reads; handlers with the same reads share dispatch. Raw
selection and inherited size flags retain their previous behavior. Ordinary
C4 scans also avoid constructing a generic instruction until needed, while
preserving adapter and source-feature ordering.

ARM and RISC-V use generated tied-operand flags to skip writeback traversals
that perform no writes. Flagged instructions retain the original helper,
including on decode failure. Native descriptor audits, full-output comparisons
and sanitizer checks cover these changes. Performance qualification remains
specific to each interface and corpus; public-iterator gaps against iced are
still being investigated.

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

When iced is enabled, bounded function-discovery sweeps first try its batch
adapter through `Decoder::scanX86Light`. The separate closed map admits 741
scalar, legacy SSE and vector encodings.
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
ownership checks. CALL targets use Capstone's compact query over the exact
classified instruction span, sharing full decoding's relative-operand and
address-wrapping rules without printing or changing options. LEA operands still
use full detail. Text/detail queries may interleave only after restoring the
classification options. With iced
disabled, the same sweeps use Capstone's native batch entry. With iced enabled,
Capstone batches the suffix that iced declines before the single-instruction
compatibility fallback is needed.

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

Before native Capstone batching, the optional iced batch route was measured
separately in the actual `scanSegmentCallsX86` consumer on the same host: five
rotating trials, calibrated to roughly 150 ms
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
