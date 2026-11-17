# Library recognition and source views

Library feature packs attach evidence to existing functions and instructions.
They support readable identities and reversible source folds in HighC and
LLVMC. Recognition does not replace a body, introduce a callee for an inline
operation, change call arguments, or authorize binary rewriting.

## Supported profiles

The pinned `signatures/features` tree contains 59 rules. Its
[coverage table](../signatures/features/COVERAGE.md), manifests and archived
compiler outputs record source origins, hashes, toolsets and configurations.

| Profile | Operations | Evidence required |
| --- | --- | --- |
| libc++ `cc7be199`, Clang 22.1.6, arm64 Mach-O, ABI 1 alternate string layout, C++17, O2 | `basic_string<char/wchar_t>` and `vector<uint32_t/uint64_t>` size, empty, data, capacity: 16 rules | Exact member symbol or authenticated receiver type, layout and matching dataflow |
| MSVC STL 14.44.35207, compiler 19.44.35229.0, x64 PE/COFF, `/MT`, iterator debug 0, C++17, O2 | Same string/vector accessors: 16 rules | Exact member symbol or authenticated PE/PDB receiver type, layout and matching dataflow |
| Shared ATL/MFC string support, same MSVC profile, `StrTraitATL<ChTraitsCRT>` | Narrow/wide construction, copy, assignment, GetBuffer and ReleaseBuffer: 10 whole-function byte rules; inherited length/empty/data: 6 expression rules | Independent member identity plus verified bytes, or typed accessor dataflow |
| ATL COM, same MSVC profile, `CComPtr<IUnknown>` | Construction, copy, assignment, release and destruction, including inherited Release: 6 rules | Receiver identity, exact virtual slots, guards, stores and AddRef/Release order |
| musl 1.2.5, Clang 22.1.6, x64 ELF | memcpy, memmove, memset, memcmp, strlen: 5 whole-function byte rules | Existing byte matcher with fully checked patterns and conservative conflict handling |

Accessors and COM policies have compiler-produced standalone and inline
fixtures. CString construction/assignment/buffer rules and libc byte rules
claim whole-function coverage only. Direct calls to an independently recognized
whole function can carry its evidence; their operands and targets stay intact.

`CStringT` identifies shared ATL/MFC infrastructure. General C++ constructor,
destructor and assignment names do not identify any library by themselves.
The source revision identifies the rule's provenance, not the exact version
used to compile a target. Different layouts, custom allocators or traits,
`vector<bool>`, debug/hardening configurations, other architectures and LTO are
outside these initial profiles. Stripped structural lookalikes without an
independent receiver identity remain unclassified.

## Load and inspect

Use one feature file or automatic selection from a signature tree:

```sh
neverd funcs input.exe --auto --sig-base signatures --json
neverd decompile input.exe --func 0x140001000 \
  --sig-file signatures/features/rules/msvc-14.44.35207-x64-release-stl.json
neverd decompile input.exe --func 0x140001000 --llvm \
  --auto --sig-base signatures --json -o source-view.json
```

`decompile --json` returns `schema_version:1` and an array of C source pages.
It requires `--func` and the default C language, optionally `--llvm`; it does
not combine with `--no-opt` or devirtualization. Plain C exports remain fully
expanded. Page text is limited to 2 MiB, a function to 32 MiB, and the CLI JSON
result to 64 MiB. Exceeding a limit reports an error.

In the desktop workbench, use **Analysis → Load Signature Pack…** after opening
the binary. Choose a `.json` feature pack or an existing `.pat` file. C and
LLVMC panes offer **Library operations**, **Fold all / Expand all**, and
**Details**. Click a summary or use Enter to restore its source. Copy and
export retain the complete original source. Details retain rule/profile
hashes, original instruction addresses and provenance. Loading a different
pack invalidates analysis and fold state; a rejected pack retains the previous
valid snapshot.

## Identity and mapping contract

The session keeps raw names, original linkage names, display names and emitted
C identifiers separate. User names take precedence, followed by names stated
by debug information, symbols, imports/exports and linker MAP data. Recognition
can annotate these names; only an identity without an authoritative name may
use a unique whole-function match as its display name. Demangling preserves
template/overload distinctions. Existing C identifier allocation, prototypes
and formatting remain in force. PDB-informed views can still refer to external
record types such as `vector` or `basic_string`; loading a recognition pack does
not generate a replacement STL type library or make those views self-contained
for compilation. The libc++ scalar projection fixtures compile on both routes;
MSVC fixtures additionally check unchanged source and template-name collisions.

`neverd_resolve_addr` exposes optional `display_name`, `linkage_name`, name and
display origins, `recognition_state` and `library_annotations`. Function lists,
cross-references, call graphs and source pages consume this shared identity.
Queries before analysis report `pending` rather than implying no match.

`neverd_ir_view_json` adds `function_identity` and `library_regions` to C
pages. Regions carry a scope (`whole-function`, `inline-expression`,
`inline-region`, or `call-site`), original occurrences, rule/profile/evidence
hashes and precise UTF-8 byte spans. Call-site regions additionally carry the
original `callee` address and `callee_identity`. No C ABI structure grows;
existing clients may ignore these optional JSON fields. See the
[worker protocol](../tools/neverd-worker/PROTOCOL.md#ir-and-c-source-mapping).

Recognition and foldability are separate results. A fold requires complete
mapping of every retained occurrence to the actual emitted source. Weak LLVM
handles and HighIR expression observations follow surviving values; deleted or
ambiguous observations stay unknown. A load the optimizer replaces by a PHI of
loads of one member leaves its observation on the PHI; LLVMC prints such a PHI
as that member read, and the read is where the occurrence is emitted. The
marked emission must reproduce the
ordinary C byte for byte before any spans are published. Disjoint spans do not
hide intervening caller code. Overlapping regions stay expanded, except that
a proved containing operation can own a nested fold.

Shared-tail functions can retain byte identity while their expanded source
body stays unfolded. The retained musl memmove fixture exercises this case.
The real ATL noexcept fixtures preserve their exception metadata; when the
native rewrite contract excludes optimization, LLVMC uses the existing
verified source fallback with temporary-alloca promotion. A table of
destructors alone does not exclude it: those fixtures print optimized, and
their regions map there too. This does not claim
native exception rewriting or execution support. Unknown cleanup targets,
partial metadata and unsupported exception paths cannot prove a COM policy.

## Verification

Build and run `NeverDLibraryRecognitionTests` and `NeverDSessionCAPITests` as
described in the [testing guide](testing.md#library-recognition). Tests consume
the feature repository's original compiler outputs, exercise all five profiles,
and check near misses, conflicting candidates, naming priority, unchanged IR,
both source routes, optimized/unoptimized LLVMC and reversible source mapping.
Data validation and engine validation remain separate recorded results.
The feature repository's [consumer receipt](https://github.com/NeverSight/signatures/blob/c64a088bb107abbb3bdcc04fc1b69230badce7a3/features/consumer-tests.md)
records the tested engine revision, exact counts, baseline failures and skipped
coverage. A passing profile matrix does not imply a green full-repository test
aggregate or support for an untested runtime configuration.
