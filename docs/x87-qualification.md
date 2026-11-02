# Scoped x87 execution qualification

This record addresses issue [#1677](https://github.com/NeverSight/NeverD/issues/1677)
for the explicitly admitted profiles in [x87 semantics](x87-semantics.md).
Instruction recognition, numerical behavior, state observations, native execution,
translation and formal equivalence remain separate claims. The shared
[capability inventory](x87-capabilities.md) describes routes and prerequisites;
it does not execute a host probe or turn regression sources into proof.

**Qualification status:** the scoped X0–X4 acceptance gates have passed. Native
x64 and ARM64 execution, ARM64 translation, cross-host numerical/state comparison,
measurements and public capability surfaces have recorded evidence. Qualification
is limited to the named profiles below; complete architectural and formal
equivalence remain unknown.

Portable FNOP/EMMS/FEMMS admission was added after those baseline records.
The [reset qualification ledger](evidence/x86-x87-resets-2026-10-28.json)
records their later generated checks at `710180d84eb925e6ee07fe6721774781055a0f20`: both
local x64 SDKs and native ARM64 execute 36 generated artifacts and 2,700 state
observations per profile. All 225 distinct seeded state transitions agree.
Native x64 supplies 576 original FNOP/EMMS comparisons per SDK. These results
are separate from the historical X0–X4 records. The subsequent seventeen-test
translation gate and reset-aware cross-host comparison passed at
`c27c87a9a1d2f816cf38392278d4017d96d7f832` in
[run 40697970738](https://github.com/NeverSight/NeverD/actions/runs/40697970738).
Physical FEMMS execution remains unqualified.

## Profiles and limits

| Interface | Admitted scope | Explicit limits |
| --- | --- | --- |
| Native typed projection | i386 and x64 System V; selected x64 Microsoft typed cases; raw binary80 values, validated stack/call contracts and defined status | Complete architectural state remains unknown; typed BCD/pair guards trap outside their domain; processor-dependent results belong to the measured CPU |
| Native owned context | x64 authenticated mutable state, native instruction/value/state replay, selected Linux fault boundaries | No arbitrary CPU/OS qualification; relocated/undefined fields and signal resumption are excluded |
| Portable concrete | `intel64-architectural-masked-v1`, x64 owned state, explicitly listed operations and control/input domains | Unsupported nonfinite arithmetic, stack faults, unmasked data operations, MMX data and general environment effects refuse; no native fallback |
| Portable LLVM/HighC/LLVM-C | Explicit C++ preparation and emission into Step V1; x64 owned integer source ABI; Linux little-endian x64/AArch64 | Source calls/floating ABIs, ordinary guest memory and x87 integer-flag forms are refused; ordinary decompilation does not select this interface |
| Portable translation | Explicit C++ profile and authenticated logical extension; x64 to native Linux AArch64 ELF, checked memory/objects/credentials | Legacy C/Python/CLI translation object APIs stay scalar-only; no i386 translation, non-Linux runtime, floating ABI, MMX data/general environment, arbitrary applications or termination proof |
| Symbolic/refinement | Conservative unknown for the unqualified x87 state/formal model | A native, differential or digest result cannot become a checked equivalence result |

## Recorded execution

The [machine-readable ledger](x87-qualification.json) binds each recorded result
to its source commit,
CPU, compiler/runtime configuration, record hash and retained artifacts. Passing
records check inputs again after execution. Any changed source, generated binary,
backend, runtime or assumption requires new applicable evidence. Historical
records are labelled by their actual tested commit; they are not automatically
proof for a later checkout.

Native execution is recorded at
`d8da52b927fd6d902800809db089762f0b0a8fbc` in
[run 40036032606](https://github.com/NeverSight/NeverD/actions/runs/40036032606).
Both native host jobs passed. The strict comparison of their downloaded artifacts
passed independently; the ledger distinguishes this local audit from the CI
comparison job. Local native replay at the same commit passed 112 tests with
1,568 retained executables. Every retained file manifest was rehashed after
recording.

Qualification found and fixed two blockers. JITLink pruned four audited,
symbol-free ELF metadata blocks; private zero-width live symbols now retain
those blocks without relaxing content, cardinality or relocation audits. A real
LLVM pruning regression covers O0/O2. The executable-store fixture also assumed
that `RejectExecutableWrites` admitted writable executable mappings, contrary to
the existing session contract. It now checks rejection during session admission
and successful invalidation under `InvalidateOnExecutableWrite`; the runtime
test separately checks rejected-store atomicity. All thirteen native translation
tests passed after these corrections.

Commit `83f6a3438ebfe7d88ed2ac213a5d00b0abcaddcb` subsequently corrected only
public prerequisite wording; a structural report comparison found three changed
string fields and no capability or runtime change. Merge
`51747fe8c1398b9a7b14c320a58c536c6ef81522` preserves the later `dev` descriptor-address
work. That change is confined to system-state intrinsics and their segmented
address rendering; it does not change the x87 semantic owners, runtime,
translation gate or LLVM revision. Subsequent local integration checks passed:
373 regressions, 49 explicitly skipped ARM-only tests, and all 36 generated
artifacts with 6,096 observations. These checks are recorded separately from
the frozen native ARM64 evidence.

The native differential layer retains original/generated C, LLVM/object files,
executables, commands, output, raw binary80 values and defined state. It covers
separate typed/owned and i386/x64 contracts, long-double calls and returns,
branches/backedges, BCD and stack management, selected transcendental/remainder
behavior, pending exceptions, environment transitions, MMX and Linux fault
boundaries. Undefined packed-BCD digits and out-of-profile typed pair behavior
remain refusals instead of convenient native answers. Processor-dependent
results are scoped to the recorded physical CPU.

The portable generated layer has 36 artifacts: three routes, three authored
body shapes, default/NoOpt source pipelines and host O0/O2. Each host records
6,096 observations, including all eight TOP values, all three valid PC settings,
all four RC settings, refusal/guard paths, raw memory and provenance. Native x64
adds 5,760 original-instruction comparisons. Cross-host comparison uses complete
file manifests and matches portable observations separately from native-only
reference fields.

The original translation gate required thirteen tests on native Linux AArch64
without skips. Its exhaustive fixture retained 192 TOP/PC/RC/O0/O2 observations and
1,152 actual AArch64 ELF objects given to JITLink, full logical state after each
block and agreement with production sessions. All thirteen tests executed
successfully without skips. Separate tests cover ten-byte memory faults,
self-modification, forged/stale invocation bindings, prefix retention, mixed
unsupported programs, branches/backedges and checkpoint/resume. Passing these
checks qualifies
this bounded profile; object compilation on x64 alone does not supply this result.

The current gate additionally requires reset-object admission and three native
FNOP/EMMS/FEMMS session tests, bringing the required test count to seventeen.
The reset fixture enumerates 225 instruction/state cases at O0/O2, retaining
450 observations and 882 direct JITLink input objects. It independently checks
full logical state, untouched extensions/memory, the completed MOV prefix on
refusal, and production-session agreement. Its eighteen refusals cover unknown
environment, pending exceptions and invalid control words. All seventeen tests
passed without skips on native Linux AArch64 in run 40697970738. Downloaded
manifests, all 2,034 baseline/reset ELF objects and their architecture were
independently checked. The same run's x64/ARM64 comparison passed with zero
differences across 2,700 reset, 6,096 prior generated and 57,888 arithmetic
observations per host; an independent local replay of the comparison agrees.

The pinned SDK passes 1,544 local tests at `c27c87a9a1`; the full asserting LLVM
passes 1,542 at `05307e249b4f16b1f2d42821a296060dd7c34e3e`. Each skips 52
ARM-only tests, one Windows observer and three unavailable public SEH fixtures.
The latter commit repairs only an invalid negative-test assembly constraint:
two arguments now have two input constraints, allowing the intended missing
clobber rejection to run. The original failure is retained. All nineteen pinned
constant/control tests pass after the repair, and both SDKs repeat the strict
generated reset and prior arithmetic checks successfully.

Three fixed arithmetic seeds (`0x871677`, `0x871678`, `0x871679`) produce 57,888
observations per host. Native x64 and ARM64 results match the native x64 original
instructions with zero differences. Raw result bits and defined IE/DE/ZE/OE/UE/PE/C1 bits
are compared with original x64 instructions. Additional local replay covers native
i386/x64 GCC/Clang at O0/O2: eight configurations and 154,368 observations, with
zero differences. Fixed boundary fixtures, LLVM APFloat and the pinned
Unicorn/Haswell reference remain distinct references with the limitations listed
in the semantic contract; missing Unicorn status fields are not counted as
exception evidence.

## Measurements and authorized replay

The completed local protocols retain thirty paired native workload configurations
and one hundred portable execution/transport trials at `d8da52b92`. CI retains
one hundred x64 and 105 ARM64 trials, including five full-session trials.
Record hashes and configurations are in the ledger;
machine-local timings and memory measurements remain in ignored artifacts, as
required by the repository contribution policy.

The portable protocol reports exact execution, every-32nd-call qNaN refusal,
raw 80-bit transport and explicit logical snapshots separately. AArch64 adds
whole-session create/compile/link/run/snapshot/unload cost. Default sampling is
five trials pinned to one allowed CPU, with CW=0x037f and TOP=5. Counts and final
raw state must match the frozen records, including failures and refusals. There
is no fallback dispatch: availability is false and observed fallback calls are
zero. VmHWM is whole-process peak resident memory, including libraries/fixtures.

Native workload replay runs an authored 2,048-sample FSIN/status stream and a
raw-transport baseline across five mode/ABI/state combinations and three output
routes. Original/generated trials alternate order. This is a deterministic,
authorized batch workload; no imported third-party application is executed.
Neither layer claims a speed improvement, all-control-word performance, isolated
allocator cost or arbitrary application compatibility. No reduced-precision
execution mode is offered or used to satisfy an exact-mode criterion.

## Acceptance mapping

All 24 issue acceptance items map to passing evidence within the declared scope.
The machine-readable ledger records each item and its evidence identifiers; this
table groups those records by phase.

| Phase | Evidence and boundary |
| --- | --- |
| X0 | One instruction/form inventory, all mode/ABI/state/consumer/backend/destination dimensions, shared runtime instruction gate, malformed/negative matrix tests, and regenerated documentation |
| X1 | Exact finite binary80 integer arithmetic, all valid PC/RC, raw transport/provenance and transactional state tests, independent references, adversarial/random observations and actual native ARM execution |
| X2 | Low/Med/LLVM/HighC/LLVM-C routes; separate native i386/x64 typed/owned ABI tests; environment/MMX synchronization; explicit ARM profile; C/CLI/Python/worker/GUI report and refusal parity |
| X3 | Typed stack/BCD/pair differences reconciled within guarded contracts; selected-CPU transcendental/remainder and fault/environment replay; unsupported portable effects and formal unknown retained |
| X4 | Frozen artifacts/configurations/seeds, raw/defined state and memory comparisons, all admitted destination hosts, separate exact/native-reference measurements, refusal/fallback and memory counts, deterministic authored replay |

This mapping uses the issue's named, narrow profiles and non-goals. It does not
assert support for every x87 instruction, CPU generation, ABI, OS exception model
or arbitrary program, or complete architectural/formal equivalence.

## Reproduction and retention

Use the commands in [testing](testing.md), the strict recorders under `scripts/`,
and the manually dispatched `x87-portable-generated.yml` workflow. Builds use
`scripts/build.py` with actual verified NeverSight fork snapshots. Linux AArch64
builds the pinned repository LLVM revision from source because the pinned
prebuilt package set has no ARM archive. Each record retains the actual compiler
version and hashes, rather than assuming that a major-version label identifies
one compiler binary.

The generated recorder accepts `--profile resets` for the separate
FNOP/EMMS/FEMMS corpus. It authenticates every seeded 528-byte input, expected
state transition, refusal and success-only return, and retains the same compiler,
runtime and file manifests as the arithmetic corpus. Cross-host comparison
requires both profiles at one source commit. Native reference fields remain
x64-only; their absence on ARM64 never counts as a physical FEMMS result.

CI artifacts retain success and failure evidence for thirty days. Download and
preserve them before expiry when long-term binary replay is required. A small
committed ledger preserves hashes, scope and run links; it is not a substitute
for the retained artifacts or a cryptographic correctness proof. Local records
remain in the original verification workspace. Unavailable hosts/tools and
skipped tests are explicitly distinguished from passes in the ledger.
