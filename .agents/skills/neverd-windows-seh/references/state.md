# State of Windows SEH work

Basis: `dev` at `8f1b084dd2` (2026-11-18) plus the commits named below.
Update this file when an item closes or a new gap is proven.

## Landed

| Commit | What it settled |
| --- | --- |
| `061341806f` | Repeated scope ranges and local-unwind handler placement for printed C |
| `174821f21b` | LLVM C block form prints no `__try` around blocks that only jump |
| `ae1a4ad036` | The not-lowered note follows the scope table, not the handler's name |
| `abe929def5` | Source-map anchors for block-form invokes and local-unwind handler calls |
| `c47aa744eb` | A `__finally` callback that never returns is lowered for printed C |
| `f67eff9ab6` | A table behind x64 `__GSHandlerCheck_SEH` is lowered when its cookie record passes every row |
| `70acd86f79` | A destructor-only C++ table is `UnsupportedCxxUnwindAction`: 21 corpus units optimize |
| `833544b500` | Native IR uses the same local-unwind placement; each handler call carries provenance role `LocalUnwindHandlerCall` |
| `b5c55f3e75` | LLVM C: blocks under the same clauses share one `__try` (corpus tries 34,386 to 11,665) |
| `8ee343b417` | A handler that local-unwinds into its parent prints in a whole unit (parent+offset spelling) |
| `78b5e1ecc6` | ARM/AArch64 `__GSHandlerCheck_SEH` wrappers read the same way as x64 |
| `34d35d8f4b` | HighC recomputes labels from nothing in its second pass (78 unreferenced labels gone) |
| `0070543de0`, `871a5a192b` | Source map records PHIs that print as member reads; HighC record declarations stable |
| `c129d2b847` | Output gate refuses callbacks that read the original frame (`sehCallbackUsesOriginalFrame`) |
| `ee5204910b` | LLVM C: a typed member read that a store to the same member follows reads into a temporary |
| `d1c42b86af`, `3e94887b6e` | In-place patch binds replacements and callees by source identity, statics by their symbol |
| `abe37f9dcd` | LLVM C names the parent blocks a handler's `_local_unwind` resumes that nothing printed reaches |
| `7eb9d5d741` | x64 Control Flow Guard calls and tail jumps through the dispatch slot lift as transfers through RAX; LLVM C bounds `p < end` table walks and never loads in an empty one |
| `e044faf6c4` | COFF patches bind the guard slot names to the load configuration's slots and refuse any reference they cannot bind |
| `d06da7fa8d` | A Win64 va_list home held in RSI/RDI across a call keeps its callee variadic; a function decompiled alone reads its variadic callees' signatures as a whole unit does |
| `2cb5bf2bb0` | Control Flow Guard check calls (`__guard_check_icall_fptr`, x86/x64/ARM/ARM64) lower to nothing; 180 corpus files print 246 (HighC) and 244 (LLVM C) fewer traps |
| `df965d9f65` | LLVM C: a typed member read takes a temporary when anything may write it before a use (calls, aliasing stores, computed indexes); calls are statements, and a pointer had before a local escapes cannot point into it |
| `796c513988` | x87 call contracts are proved once per session (kept with their no-return dependencies); capstone LLVM C -3.3% |
| `e3743938d5` | Native output takes nested scope tables (generated rows tied to the source scope chain) and source tables that repeat a range, and places a local unwind's handler calls by the row each ends and by the range markers; the MSVC local unwind fixture (/O1, /O2) runs as the original in all 64 wine runs |
| `8f1b084dd2` | LLVM C prints the parent code a handler's `_local_unwind` always resumes, with a `goto` at the end of the `__finally` (36 xcpt4 corpus builds, 1 to 13 each) |
| `043e6fdb22` | Native output passes a termination handler that reads its frame the parent's emulated frame; the gate rebuilds it from the unwind codes, bounds what the callback reaches and orders protected memory with faults |
| `4ccfcc23b6` | A filter that reads its frame runs behind an adapter (`llvm.eh.recoverfp`, `llvm.localrecover`); rows bind to the adapter by the compiler's receipt; in-place hands such modules to the section patch; frame-register parents take the emulated frame; LLVM C of native IR calls the filter with the emulated frame |

## Open

- `e3743938d5` admits no corpus function yet. Per function with the final
  engine on four vs2019 x64 xcpt4 corpus builds (`fh3/gs/o2`,
  `fh3/no-gs/o2`, `fh3/gs/o0`, `fh4/no-gs/o2`), each build has 24 functions
  with nested or repeated rows, and every one stops at another gate; the two
  functions per o2 build that patch run as the original in all eight runs
  and were accepted before the change too.
- The emulated frame (user decision 2026-11-18, both halves, landed in
  `043e6fdb22` and `4ccfcc23b6`): termination handlers that read their
  frame are passed it, and filters run behind an adapter; see architecture.md
  for the rules. Parents with a frame register take it too. No MSVC xcpt4
  filter that reads its frame reaches output yet (each such function stops
  at another gate). The MSVC `/Od` `nested_collided` builds' `0x140001160`
  (vs2019) and `0x140001170` (vs2026, unversioned) run behind two adapters
  as the original in all 12 builds; their filters write their exception
  pointers into the parent's frame and read them back, so an offset moved by
  8 changes nothing and only a frame moved off the stack proves the adapters
  ran. The MinGW fixture below is the frame-register case.
- With the `__finally` half, per function on the four vs2019 x64 xcpt4
  builds: the 15 functions per build the original-frame gate stopped now
  give 4 (5 at o0) that patch and run as the original in every run, 2 per
  build with a pad a planted offset proves ran (the rest are CRT functions
  whose pads the test never reaches); 6 whose handler passes `_local_unwind`
  the frame it reads from its parent (a handler that resumes its parent,
  which native output has no form for); 4 that stop at the catchret check;
  and at o2 one at the filter frame gate.
- MinGW fixture for the emulated frame (`seh/emu/fx/filt2.c`, local): build
  with `clang --target=x86_64-w64-mingw32
  --sysroot=/usr/x86_64-w64-mingw32 -fms-extensions -O1 -fuse-ld=lld
  -L/usr/lib/gcc/x86_64-w64-mingw32/13-posix`. Clang's SEH there covers
  calls only, so each fault happens in a callee. `filter_reads_local`
  (0x140001470 at -O1 and -O2) is the adapter case; the other two stop at the
  detached region-dispatch check.
- The other native gates those functions meet: incomplete native lowering
  (8, 9 at o0: a local unwind handler that does not return, or scope bounds
  that are not block bounds, refused in `MedLLVMNativeSEH.cpp`), a region
  dispatch detached from its funclet (4, 3 at o0), at o0 an establisher
  frame proof that finds an incomplete decoded CFG (1), and a catchret whose
  continuation re-enters an outer row through range-enter markers before the
  handler (1). A relaxation of that catchret check (follow blocks that hold
  only marker anchors and the marker invoke, each entering a row that holds
  the handler) was written and set aside: every function it would admit
  stopped at the original-frame gate, so no run could show it right.
- At /Od the local unwind fixture's unwinds are not certified (its native IR
  carries no handler-call marks), so native lowering stays incomplete and
  `patch` refuses; the cause is not examined yet.
- No corpus image patches whole: every one of the 384 stops at some gate
  (217 at incomplete native exception lowering, 64 at pointer-table call
  slots, 72 at code identities, the other 31 at the COFF exception patch's
  personality, metadata and C++ group checks). Since `e3743938d5`, 10
  AArch64 images of the unversioned and vs2026 MSVC builds stop at a later
  function whose personality the loader does not name (a handler the image
  defines), and 7 at the AArch64 regeneration checks
  (`unsupported-seh-scope-graph`, `unsupported-seh-callback-abi`).
- A handler that may also return, or whose resume target a row guards, keeps
  the note that nothing printed reaches the target; one vs2026 AArch64 /GS
  /O2 xcpt4 build keeps one such note (cause not examined).
- ARM and AArch64 output keeps one catch-all row per function: a filter or
  `__finally` callback runs with the parent's callee-saved registers
  restored, and no ARM64 Windows host here can run a rewrite.
- AArch64 callbacks that read the parent's callee-saved registers stay
  noted in LLVM C (one declaration per page is not possible); HighC fills
  them.
- Native IR keeps a table behind `__GSHandlerCheck_SEH` as metadata (the
  cookie check is lowered for printed C only).
- MSVC's `return` form of a local unwind splits its rows and is not lowered.
- Two clang-cl /O2 `seh_probe` functions have rows that overlap without
  nesting: every capability refuses them.
- Stripped images name a plain `__GSHandlerCheck` nowhere, which keeps
  whole shards unoptimized (98.8% of "partial" records on a Windows 10
  sample); recognizing its bytes in the loader is the highest-value step for
  real files.
- Unresolved symbols in a lifted-IR ELF patch are a warning, not a refusal
  (COFF refuses since `e044faf6c4`, Mach-O always did).
- 168 LLVM C refusals in ARM/AArch64 corpus images ("pointer-table load used
  as an indirect-call target ... no complete code-slot role"): calls through
  writable function-pointer variables without a null test.
- Performance: about a quarter of capstone's LLVM C time is the x87 graph
  each callee is lifted for; a decode scan that skips x87-free callees is
  exact but jump tables defeat it (`neverd-performance`,
  `references/remaining-work.md`, "x87 call proofs").
