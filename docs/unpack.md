**Languages**: [English](unpack.md) | [简体中文](zh-CN/unpack.md) | [繁體中文](zh-TW/unpack.md) | [日本語](ja/unpack.md) | [한국어](ko/unpack.md) | [Français](fr/unpack.md) | [Deutsch](de/unpack.md) | [Español](es/unpack.md) | [Italiano](it/unpack.md) | [Русский](ru/unpack.md) | [العربية](ar/unpack.md)

# Unpacking packed executables

`neverd unpack` recovers the program that a packed executable rebuilds in its own address space. It runs the input as a bounded guest process, observes where the stub hands control to the code it produced, and writes that image as a new file of the same container. It does not devirtualize: functions a protector virtualized stay virtualized. Build with `NEVERD_ENABLE_CPU_EMULATION=ON`.

## Supported inputs

The container selects how a file is validated and rebuilt, the instruction set selects how a transfer is judged, and both select the guest process profile. An input outside this table is rejected by name before anything executes.

| Container (`format`) | Instruction set | Guest profile | Entry evidence |
| --- | --- | --- | --- |
| PE32+ (`pe64`) | x86-64 | [`windows-pe64-v1`](process-emulation.md) | runtime observation |
| PE32+ (`pe64`) | ARM64 | [`windows-pe64-v1`](process-emulation.md) | runtime observation |
| PE32+ native (`.sys`) | x86-64 | [`wdm-x64-scheduled-v92`](driver-emulation.md) | `DriverEntry` |

PE32+ DLL inputs are selected by `IMAGE_FILE_DLL`. A modeled guest EXE calls `LoadLibraryA`, then `FreeLibrary`, using the ordinary dependency, TLS and `DllMain` lifecycle. The accepted DLL entry is its process-attach invocation; arbitrary exports are not called with invented arguments. Export names, ordinals, aliases, data and forwarders remain in the rebuilt DLL. Pointers to its own exports remain internal pointers rather than self-imports. This also covers helper-returned addresses: an internal result withdraws earlier import-repair evidence for that site.

## Windows x64 drivers

With `NEVERD_ENABLE_DRIVER_EMULATION=ON`, native-subsystem x64 PE images (`.sys`) run through the driver environment. `DriverEntry` owns entry provenance; dispatch and unload callbacks cannot become its default recovered entry. The optional `driver` object accepts the [driver scenario](driver-emulation.md), including service name, registry, requests and scheduling. Common backend, contract and resource limits still apply. User-process arguments, environment and PEB inputs are rejected.

Recovery checks the incoming driver arguments, return/shadow frame, nonvolatile registers, direction flag and floating-point controls, together with kernel object ownership. Retained pools, borrowed kernel pointers, changed loader objects or unaccounted kernel effects return `unsupported_state`; explicit `snapshot_only` keeps their diagnostics. Kernel state has no `restore_runtime` materializer. Import rebuilding uses kernel export identities, validates original exports and recomputes the PE checksum. The fixed-base result does not establish a Windows kernel load, signature validity or execution of unreached driver paths.

```bash
neverd unpack packed.sys -o unpacked.sys --options='{"backend":"kvm","driver":{"service_name":"Example"}}'
```

## Use

```bash
neverd unpack packed.exe -o unpacked.exe
neverd unpack packed.exe -o unpacked.exe \
  --options='{"backend":"unicorn","instruction_limit":400000000,"transfer":2}'
```

The command prints one JSON report. Exit code 0 means an `unpacked` image or an explicitly requested `snapshot` / `restored` was written; 3 means `no_entry` or `unsupported_state`, with no file created or truncated; 1 means an invalid input, option or setup failure. The report names the `format`, the `architecture` and the `profile` that ran. The C entry point is `neverd_unpack_json`; Python exposes `Session.unpack`. Options are the [process options](process-emulation.md) plus `transfer` and `snapshot_only` / `restore_runtime`. Defaults differ where a stub needs more room: 100000000 instructions, 600 seconds and 512 MiB, and `windows.defer_unmodeled` is on.

## Explicit runtime restoration

`restore_runtime:true` requests a self-contained Windows x64 initializer and returns `restored` (exit code 0). It is mutually exclusive with `snapshot_only`. Clang and `lld-link` must be in `PATH`; `windows.peb_version` must explicitly identify the intended native environment. The initializer checks that version without changing the native PEB. It restores owned heap backing, encoded-pointer operations, FLS values and callbacks, recursive critical sections, private virtual reservations and page protections, and `LastError`. Export gates retain observed call forms. Code, data and metadata occupy separate `.nd*` sections with their respective permissions; no helper DLL is emitted.

The preceding refusal rules describe default recovery. This explicit mode keeps the captured `runtime_state` diagnostics and skips fresh-process import discovery. Fixed addresses, the selected DLL transfer and captured arguments/environment remain requirements. Historical direct-service counts are retained; service numbers and unreached paths are not certified as portable. Dynamic TLS, open handles, mapped sections, suspended exception state and other guest DLL state remain unsupported. Unsupported state or initializer construction failure publishes no output. `restored` describes construction under these conditions, not proof of every native path.

## Runtime state and analysis snapshots

At the accepted transfer, the process profile supplies its live heap allocation inventory. Recovery conservatively scans every pointer-sized value in captured image and current-thread TLS bytes, including unaligned values and interior allocation addresses. A match may be an integer or unused data; it is not a typed pointer or permission to relocate it. An unknown inventory or a possible reference produces `unsupported_state` by default, even with an accepted `entry`. Neither the CLI nor C API creates or truncates the output in this case.

`runtime_state.heap_inventory_known` distinguishes an empty known inventory from missing provenance. `possible_heap_references` counts all matches; `heap_references` retains at most the first 64, image first and then TLS, ordered by `offset`. Each record names its `storage` (`image` or `thread_local`), address and allocation extent. `rva` is present as a hexadecimal value for image storage and null for TLS. Addresses and offsets use hexadecimal strings.

`runtime_state.direct_service_calls` counts direct model service calls witnessed during entry observation and import discovery. Their numeric binding has not been reconstructed for native Windows, so recovery returns `unsupported_state`, including when the call first executes after the captured entry. Explicit `snapshot_only` retains the count and diagnostic. A copied syscall is not treated as a repairable imported function call. The count is also retained in `no_entry` reports; without a captured entry, the heap inventory remains unknown.

`runtime_state.encoded_pointer_inventory_known` records whether the process profile supplies its observed pointer-encoding values. `possible_encoded_pointers` counts exact pointer-sized matches in captured image and main-thread TLS bytes, including unaligned storage; `encoded_pointer_references` retains the first 64 locations with `storage`, `offset`, `rva` and `value`. Windows supplies values from completed `EncodePointer`/`RtlEncodePointer` results and `DecodePointer`/`RtlDecodePointer` inputs. A missing inventory or any match returns `unsupported_state`; explicit snapshots keep these diagnostics. Matches may be integers or unused data, so they do not authorize re-encoding. Values cleared before capture and encodings created only after capture do not cause this refusal. This inventory does not cover custom encodings, partial or transformed values, registers, stack state, or unreached paths.

`runtime_state.dynamic_thread_local_inventory_known` records whether the profile supplies dynamic thread/fiber-local slot state at capture. `live_dynamic_tls_slots` and `live_dynamic_fls_slots` count retained slots separately from PE static TLS: an allocated slot counts even when its value is zero; a nonzero TLS cell also counts without an allocation record. Missing inventory or retained slots returns `unsupported_state`, and explicit snapshots retain the counts and diagnostic. Slots freed before capture, unallocated TLS cells cleared before capture, and state first created after capture do not trigger this refusal. These counts do not reconstruct slot ownership, values or callbacks and do not certify other threads or fibers.

For the current fiber, `FlsFree` clears a nonzero value before invoking its registered callback through the guest ABI. If the callback writes another nonzero value, cleanup repeats with that value before releasing the index. Callbacks may invoke modeled APIs and free other slots; recursive release of the same active slot stops as `unsupported_service`. Normal entry return and `ExitProcess` run remaining callbacks before DLL/TLS process detach. Exit cleanup captures the historical allocation upper bound: later indices within that bound can be initialized or reused, while higher allocations do not extend the sweep. An exit callback reads its old value; completion clears its writes and keeps the index allocated. All callbacks share the process execution budgets. Loader operations during exit cleanup remain explicitly unsupported.

`--options='{"snapshot_only":true}'` explicitly requests analysis bytes. Once an entry is accepted and rebuilding succeeds, the outcome is always `snapshot`; the runtime-state diagnostic remains visible. This option does not restore heap data, infer relocations, certify native execution or devirtualize code. An empty heap-reference count also does not certify other OS state or unreached paths.

## How the entry is established

Generation zero is the input image when the guest loader maps it. A transfer begins execution of newer code. `transfers` records the RVA, `generation`, stack equality (`stack_balanced`) and ownership by the input entry invocation (`program_invocation`). For DLLs this is process attach. Before that invocation, the first initializer supplies the stack baseline.

1. The default entry requires both `stack_balanced` and `program_invocation`: the stub has returned the stack of the input entry invocation. `entry_source` is `transfer`.
2. OS-owned TLS callbacks, dependency DLL entries and detach calls cannot become the default entry, even with a balanced stack. Deeper guest calls continue. No callback is skipped and no stub signature predicts an entry.
3. `transfer` explicitly selects a listed transfer by position, including initialization callbacks or staged transfers. Selecting an initializer also captures the current thread's TLS and initializers that already completed.

No entry is predicted from the shape of compiler startup code. A run that stops first reports `no_entry` with the process `stop_reason`.

Execution does not permanently retire a 4 KiB page from observation. Committed guest RAM writes invalidate visited pages through physical aliases; the stopped process model also rechecks pages after OS services. Transfer evidence uses the CPU decoder’s actual instruction extent, so changed neighboring data alone cannot establish an entry. Mixed old and new code and subsequent rewrites remain watched, including callbacks returning to older stub code in the same page.

## The rebuilt image

Sections keep their RVAs and hold the observed memory, including effects of initializers that already ran; each section has its observed page access. A final `.neverd` section holds a new import directory over existing export cells. Additional cells needed for observed export calls and address loads live in that new section, so original zero-filled storage is preserved. `imports` lists every cell with its `origin`: `static` cells were bound from the input directory, `runtime` cells were stored by guest code or added for a repaired call. The image is fixed at its observed base: relocations of generated content were not observed, so the relocation directory is removed and `IMAGE_FILE_RELOCS_STRIPPED` is set.

User-mode section ranges round `VirtualSize` (or `SizeOfRawData` when zero) up to guest pages; `SectionAlignment` positions RVAs. Raw bytes within the final mapped page are retained; bytes beyond that range are not mapped. Rebuilding preserves `FileAlignment` and zero-fills file padding without reading section gaps. Native evidence for this rule covers only x64 Windows DLLs. Runtime admission, metadata readers and fixed-image validation share the unrounded logical extent. Zero `VirtualSize` uses `SizeOfRawData`; a nonzero value still bounds metadata. The loader uses private reader storage for LLVM compatibility and retains original bytes for execution, debug identities and authentication. Driver mapping remains outside the user-page rule.

Rewriting debug-data file offsets requires complete file backing for both the debug directory and every retained payload. With `AddressOfRawData == 0`, a payload wholly inside the retained overlay keeps its offset relative to the overlay’s new file location. An invalid nonzero RVA cannot fall back to the overlay.

Existing runtime IAT candidates must form a contiguous export-pointer array for one provider with an intact zero terminator inside the original section. A neighboring provider's terminator cannot validate another array.

Writable import cells need no native IAT protection range; their descriptors still bind every cell. Read-only cells require one contiguous range of read-only, non-executable sections. A range crossing writable or executable sections fails explicitly instead of changing their protection.

An original TLS directory matching the loader's allocation identity may have no callback array or an empty one. A fully validated original directory remains usable without callback execution evidence: captured TLS is still validated and restored as needed, and pending callbacks are not counted as completed. Scanned replacement records still require a complete terminated callback list and observed callback execution. Multiple matching records fail explicitly. This rule uses PE metadata and execution witnesses, without protector-specific bytes or names. Generated callback execution takes precedence over a completed loader initializer when selecting the directory. Revisiting a completed loader initializer from the program remains fallback evidence, including after that initializer has been rewritten. Without a recoverable replacement, an invalid original TLS directory fails explicitly instead of producing an `unpacked` result.

`materialized_tls_callbacks` counts input-image TLS process-attach callbacks that returned before capture. Completion requires an OS invocation record, or an observed generated entry with arguments `(image_base, 1, 0)` followed by its ABI return address and restored stack pointer. Their memory effects are already in the snapshot. Adapters in the new `.neverd` section bypass only that repeated process-attach call and tail-call the original callback for other reasons. The directory, callback table and adapters use new storage; original bytes stay intact. The section is executable when it contains adapters and writable only when new IAT cells require it. Guest-internal callbacks without this completion evidence keep their original behavior. On process attach, an adapter restores captured main-thread TLS bytes that differ from the template, even when no callbacks completed. Missing or mismatched live TLS evidence fails explicitly. The original template remains intact for future threads; other notification reasons never restore the captured block.

Deferred loading permits executable callback and entry targets that earlier initializers materialize in zero-filled memory. Callback arrays and TLS allocation metadata still require validated backing; ordinary strict loading retains its file-backing checks. The OS model supplies invocation provenance and notifies observers when it prepares an invocation or restores a suspended caller. Transfer watches are rearmed at those boundaries, including when a callback and the generated entry share a page.

`import_repair` covers two additional bounded runs after entry capture: export-call discovery, then helper-state observation. Each run has its own process limits. Instruction/event counts are their saturating sum; `stop_reason` and the diagnostic describe the last run, and observed calls count discovery only. If discovery finds no continuations, observation is skipped. Conflicting export identities cannot rewrite a site. Results cover reached paths; `unpacked` does not certify all imports or successful program execution.

Import binding order can change export addresses. Repair retains the identity from the proving run and observes exports resolved after entry; an address from another run cannot authorize a rewrite.

Discovery observes export dispatch, including the unmodeled export that stops execution, rather than relying on modeled-call logs. An unreadable ABI return location cannot seed a helper proof. Repairing a pure call to an opaque export preserves the explicit unsupported-service stop.

Observation examines at most 256 candidate starts in the 256 bytes preceding actual export-call continuations. A direct `CALL rel32`, optionally preceded by a GPR PUSH or POP, identifies a boundary to observe; bytes after CALL are not a signature. A witnessed six- to eight-byte call window can become `call [rip+IAT]` only when API entry has exactly one pushed return address, unchanged other registers (including flags and SIMD), unchanged mappings and persistent RAM, and no earlier OS call. Leading NOPs retain the exact API return address. A seven- or eight-byte window can instead become `mov r64, [rip+IAT]` when it returns a known export in exactly one GPR with a balanced stack and the same preservation requirements. The result register comes from state comparison and may be any GPR except RSP, including R8-R15; an eight-byte load leaves a trailing NOP. Only callee scratch below the caller stack pointer is excluded; caller-owned stack bytes are compared. A later impure, unresolved or incomplete invocation invalidates earlier evidence. Executed starts are required; a preceding byte cannot authorize a REX prefix, and overlapping starts sharing a return are rejected. All validated windows of a conflicting continuation still block overlapping repairs after rejection, regardless of witness order. `observed_loads` and `repaired_loads` count address loads separately from calls.

`ImportObserver` retains one outer snapshot across intermediate execution stops. A nested candidate withdraws its own earlier evidence because it has no separate snapshot. The outer call still requires the complete register, mapping, persistent-memory and OS-call comparisons; recursive, interrupted and incomplete invocations withdraw their evidence. `ProcessImportsTests.cpp` and the independently linked DLL fixture cover these boundaries. Only a candidate with an observed export continuation retains its snapshot across nested stops.

The original section table, import directory layout and relocation table are not reconstructed; a packer does not restore them in memory.

## Identification

The compatibility fields `packer.kind` and `packer.evidence` report `unidentified` and an empty array. No protector registry, pack header parser or stub signature affects recovery. The legacy identification API validates the container and returns the same empty identity.

## Limits

Checked execution admits one instruction at a time, on the order of 10^5 per second. x64 Unicorn, KVM and WHP support `direct-user-x64-v1`, bounded by time and events without an instruction count. Mixed generations and repeated writes can require additional processor steps. Recovery covers reached paths in the input image; generated code outside that image is not promoted to its entry. Initialization may modify image data or create external process state before the entry; that state is not generally portable into a fresh process, and rerunning TLS callbacks may have additional effects. Unmodeled APIs stop execution explicitly. Import repair covers validated six- to eight-byte x64 call windows and seven- or eight-byte address loads; unreachable sites and other protected call forms remain unresolved. Virtualized code stays virtualized.

RVA-based delay imports preserve pending in-image thunks and rebind already resolved cells from observed export identities, so the program's helper does not repeat completed work. A separate, terminated lookup table limits each binding run without consuming the next pending IAT cell. Rebuilding clears process-local DLL handles and bound-cache references; delay-owned metadata stays outside ordinary IAT discovery. Descriptors and pointer arrays must terminate, and names, ranges and storage owners must validate. Unknown targets, invalid thunks, legacy VA descriptors and conflicting storage fail explicitly.

## Verification

`NeverDUnpackTests` checks container validation, safe import-cell allocation, conflicting witnesses and TLS metadata refusal. `NeverDUnpackExecutionTests` unpacks checked-in UPX fixtures (NRV2B, NRV2D, NRV2E, LZMA and a C runtime program) using the same generic path on Unicorn, KVM and WHP. It compares sections with the independent linked image observed after its own initializers and requires identical recovered files across available backends. `UnpackGeneratedTests.cpp` packs independent x86-64 and ARM64 programs inside the test and covers transfers, staged loading and import repair, including checked execution and x64 Unicorn/KVM/WHP direct execution. Native CI requires the matching KVM/WHP direct cases rather than accepting skips. `NeverDUnpackPublicTests` covers the C ABI and CLI. `unittests/unpack/fixtures/Makefile` regenerates the UPX fixtures. The native KVM/WHP CI inventory (`NativeCPUTests.def`) also requires key PE, TLS, import and transfer unit tests to pass. Format-only unit tests remain available with both CPU and driver emulation disabled.


`UnpackLibraryTests.cpp` packs independent x64/ARM64 DLLs and checks dependency order, ordinary and generated TLS callbacks, failed attach cleanup, input/host identity, self-file access, export names/ordinals/data/forwarders, and absence of self-imports. Native Windows loads original and rebuilt DLLs through a separate EXE and calls their declared exports; checked and direct WHP cases are mandatory. `CompletedGeneratedTLSCallsRequireTheAttachABI` rejects changed entry/arguments; `GeneratedCallsNeedTheirReturnedStackAtTheContinuation` rejects a wrong return stack. These tests establish unpacking behavior, without devirtualization.

`ExportObserver` also watches executable exports of resident guest dependencies, while modeled providers keep their service-dispatch observation. Input-image exports are excluded. Module changes refresh the watches, and live export identity still authorizes each repair. Discovery retains at most the declared import limit. DLL fixtures require repair of both a system API helper and a guest dependency helper; native loading verifies that neither retains an emulated address.


`WrappedEntriesRequireExplicitTransferEvidence` covers a DLL wrapper calling its restored entry on a deeper stack. The default remains `no_entry`; selecting that observed call with `transfer` rebuilds a loadable DLL. A deeper call alone cannot distinguish an entry from an initializer.

When a recovered DLL entry differs from its original PE entry, the writer emits a loader-notification adapter: process attach goes to the selected entry; detach and thread notifications go to the original live executable entry so outer-wrapper cleanup remains reachable. An unavailable original entry fails rebuilding. Reported `entry_rva` still identifies the selected program entry; the PE header can point to the adapter. The independent wrapped-DLL fixture checks cleanup outside the selected function on both emulated architectures and native Windows.

`runtime_state.additional_state_inventory_known` and `has_additional_dependencies` report the OS owner’s remaining private heaps, virtual reservations, locks, handles, views and exception state. Missing inventory or retained resources blocks default recovery even when no heap-pointer match exists. Explicit snapshots retain this diagnostic; runtime restoration must reconstruct the supported owners.
