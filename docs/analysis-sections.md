# Add code or data to the active analysis image

Analysis sections map caller-supplied bytes into a loaded native image. They
reuse `BinaryImage` segments, sections and symbols, then the existing decoder,
function analysis, LowIR → MedIR → HighIR → HighC pipeline. Original imports,
symbols and materialized globals remain in the same image. No LLVM IR conversion
or new decompiler is involved in adding or browsing these sections.

This is an analysis-database operation. It does not rewrite PE/ELF/Mach-O headers,
patch original functions, relocate the supplied machine code, or export a runnable
modified binary. The producer must encode its bytes for the requested virtual
address, including PC-relative calls and references. A `source_address` records
caller-declared provenance; it does not prove equivalence to the source function.

## Workbench

Open a native binary, then choose **Edit → Add analysis section…**. Select a raw
binary file, enter an unused hexadecimal virtual address, a section name and
permissions (`r`, `rw`, `rx`, or `rwx`). Optionally declare one function covering
the supplied bytes. On success the workbench navigates there; use the normal
function list, disassembly, Space/CFG and F5/native pseudocode views.

The dialog stays open with the engine diagnostic when validation fails. A
matching engine with this additive API is required; older engines disable the
action. Read-only projects cannot add or remove sections.

**Save** stores the bytes and declarations beside the input in
`<input>.neverd/analysis-sections.json`, bound to the original input SHA-256.
The `.nddb` project database packs this sidecar too. Opening the input or project
restores its saved sections. A foreign hash or malformed section fails opening
without replacing the active worker session. The original input bytes stay
unchanged. Unsaved additions disappear when the session is discarded.
Reload restores saved sections and discards unsaved section changes along with
other project edits; it also refreshes the normal code and function views. It
opens the input again with its open options and the PDB, DWARF, MAP and
signature files chosen for it.

## Public C ABI

The declarations are in `include/neverd/sdk/NeverDCAPISession.h` and included by
`NeverDCAPI.h`:

```c
int neverd_analysis_section_add(neverd_session_t session,
                               const char *description_json,
                               const unsigned char *data, size_t data_size);
int neverd_analysis_section_remove(neverd_session_t session, const char *name);
const char *neverd_analysis_sections_json(neverd_session_t session,
                                        int include_data);
int neverd_analysis_sections_import_json(neverd_session_t session,
                                        const char *state_json);
```

Add/remove return **1 on success, 0 on failure**. Read `neverd_last_error()` on
failure. Release returned strings with `neverd_free_string()`. The byte pointer
is borrowed only during the call; the engine owns a copy afterwards. Serialize
calls on a session as with other image edits.

One addition atomically declares a section and any number of functions within
it. Example for an already loaded x64 session:

```cpp
const unsigned char Code[] = {
    0xB8, 0x07, 0, 0, 0, 0xC3, // mov eax, 7; ret
    0xB8, 0x1F, 0, 0, 0, 0xC3  // mov eax, 31; ret
};
const char *Description = R"({
  "schema": "neverd.analysis-section.v1",
  "name": ".recovered",
  "address": "0x140100000",
  "permissions": "rx",
  "functions": [
    {"name": "restored_first", "offset": "0x0", "size": 6,
     "source_address": "0x140001000"},
    {"name": "restored_second", "offset": "0x6", "size": 6}
  ]
})";
if (neverd_analysis_section_add(Session, Description, Code, sizeof(Code))) {
  const char *C = neverd_decompile(Session, 0x140100000);
  // Display/use C, then release it. Normal disassembly and CFG APIs also work.
  neverd_free_string(C);
}
```

Choose addresses unused by the actual image; the example VA is not an allocator.
For a data section omit `functions`. Sizes are positive byte counts; addresses,
offsets and optional source addresses are **hexadecimal strings**, preserving
64-bit values through JSON. Function extents must fit the supplied data and not
overlap other declarations in this section.

The query returns `schema: "neverd.analysis-sections.v1"` and a `sections` array.
Each section retains its normalized description and gains `size`; nonzero
`include_data` also includes `data_hex` and a root `sample_sha256`. Remove
`size`/`data_hex` before passing
an exported description to add, and pass decoded bytes separately. The C ABI
does not implicitly read or write sidecars; persistence is explicit at the
caller/worker boundary.

`neverd_analysis_sections_import_json()` accepts the complete export with data,
validates its source SHA-256 and every section through the same section APIs,
then atomically replaces the active analysis sections. A malformed later section
leaves the live image and pipeline unchanged. CLI and worker use this common
state contract; disk I/O and writer-lock lifetime remain adapter responsibilities.

Validation includes mapped segment/section name and range collisions, address
overflow and target width, permission strings, symbol conflicts, function
extents, and a decodable first instruction. AArch64 entries require four-byte
alignment. It does not linearly decode embedded data as instructions or promise
that all instructions in an accepted function are supported by later lifting;
normal analysis reports unsupported semantics through its existing errors.

Initial supported architectures are **x86, x64 and AArch64**. Other architectures
fail explicitly. Each session allows at most 128 additions, 64 MiB of total
materialized bytes and 4096 declared functions. There is no implicit zero fill.
Added bytes have no original-file offset. Removal is restricted to sections
created through this API and removes their owned function declarations.
The existing section query emits optional `materialized_sz` for these initialized
bytes independently of `file_sz: 0`; listing consumers use it to distinguish
added code/data from uninitialized storage without inventing a file offset.

Successful edits retire pipeline, discovery, string and immediate-search
caches. Results of earlier patches and round trips, which read the input file,
stay valid, including the object `neverd_roundtrip_obj()` returned. Normal
function discovery can repopulate inferred functions afterwards. Failed
validation publishes no image change. Annotations and user renames remain
address-based; section operations are not entries in comment/rename undo history.

Loading a PDB, DWARF or MAP file into a loaded session reads the input again.
The session's sections then join the image that read produces through the same
validation as an edit; a debug file that names a symbol a section declares, or
otherwise conflicts with a section, is refused and leaves the session unchanged.

## Worker and GUI integration

The worker exposes three operations:

| Operation | Payload | Result |
|---|---|---|
| `analysis_sections` | `{}` | Versioned section descriptions without bytes |
| `analysis_section_add` | Description plus exactly one of `path` or `data_hex` | Updated metadata |
| `analysis_section_remove` | `{"name":".recovered"}` | Updated metadata |

`path` is a UTF-8 path to raw bytes, allowing up to the session limit without
embedding the bytes in IPC. Inline hex remains subject to the existing 8 MiB
frame limit. Batch functions use the same `functions` array as the C ABI.
Add/remove require a writer session and publish one revision. Rejections preserve
the revision. QueryService treats writes as command fences, and the workbench
refreshes function, region, listing, hex, CFG and source caches.

`metadata.analysis_sections_supported` indicates engine support. The worker
checks additive symbols dynamically, allowing older engines to keep their
existing browsing operations. A saved analysis-section sidecar requires a
supporting engine to reopen.

GUI-side callers use `Session::addAnalysisSection()` and
`Session::removeAnalysisSection()` so revision and view notifications are kept
in one place. Other tools can use the same worker operations or the public C
ABI. Producers retain ownership of code generation and supply fully encoded
bytes and function declarations through this common boundary.

## CLI

The standalone CLI uses the same library and sidecar as the workbench:

```powershell
neverd analysis-sections original.sys --json
neverd analysis-sections original.sys --add section.json --data restored.bin --json

# Each process restores saved sections before resolving names or addresses.
neverd sections original.sys --json
neverd funcs original.sys --json
neverd disasm original.sys --func restored_first --json
neverd decompile original.sys --func restored_first
neverd cfg original.sys --func restored_first --json

neverd analysis-sections original.sys --remove .recovered --json
```

`section.json` is the `neverd.analysis-section.v1` description shown above.
Its function array can describe many functions in the raw-byte file. With no
edit flags the command lists additions. Add requires both a description and
raw-byte file; add/remove are mutually exclusive. Successful edits atomically
publish the canonical input's `<input>.neverd/analysis-sections.json` sidecar. Failed
edits leave the saved state untouched. Source bytes and section headers in the
input binary stay unchanged.

The CLI acquires the existing project writer lock **before** restoring saved
state and holds it through publication. If a GUI/worker already owns that writer
lease, the CLI refuses the edit. Read commands need no writer lease. Open the
same binary in the GUI, or reload a session using that binary, to observe the
committed sidecar. An existing `.nddb` remains its own saved snapshot.

Native read commands using the shared session load see additions automatically,
including whole-image C output and text/DOT graph views. The SDK's explicit
analysis-view load retains the active image for the same input; file rewriting
and runtime execution retain their separate input contracts. This command works
on the input binary and its sidecar; it does not create or directly edit `.nddb`.

## Listing materialization regression

The first integration test exposed a file-backing assumption in the worker.
To reproduce that boundary, add `b8 2a 00 00 00 c3` as an executable analysis
section, declare its six-byte function and query `listing` at its entry.
Direct `bytes`, `disasm` and native `decompile` can succeed while the old listing
classifies it as uninitialized storage and returns no function on the cursor.
`buildRegions()` used `file_sz` as the initialized extent; an analysis addition
correctly has `file_sz: 0`, although it owns all its bytes. The fix exports
`materialized_sz` from the owning SDK and uses it independently of file-offset
metadata. `NeverDWorkerAnalysisSections` asserts instruction rows and function
identity, and `NeverDGuiAnalysisSections` asserts navigation followed by F5/CFG.

CLI testing also exposed a batch-view gap: adding a section and calling
`neverd decompile original.sys` or a text/DOT CFG previously reloaded only the
original file through `PipelineRunner::load()`, dropping active additions.
Read-only callers now use `loadAnalysis()` for the same input; rewriting keeps
the file-backed load. The CLI test covers named and whole-image C, JSON and
text/DOT CFG, and original-input preservation.

## Verification

`NeverDWorkerAnalysisSections` exercises the real shared library directly and
through IPC: x64/AArch64 including high VAs, multiple functions, byte ownership,
LowIR/MedIR/native HighC/CFG, references to original functions/globals, data
sections, rejected mutations, removal, read-only access and hash-bound save/load.
`NeverDGuiAnalysisSections` drives the production add-section dialog against the
real worker, corrects a rejected overlap, opens F5/CFG, saves a `.nddb`, reopens
and removes the section. `NeverDGuiUnits` checks database sidecar packing and
unpacking; `NeverDGuiQueries` checks edit fences and analysis-cache retirement.

`NeverDAnalysisSectionsCLI` runs actual add/list/remove subprocesses and native
views across PE32/PE64/ELF x64/AArch64. With the optional worker target it also
checks shared persistence in both directions and cross-process writer conflicts.

```sh
python scripts/build.py build --target neverd-worker neverd-gui-analysis-section-tests
ctest --test-dir build -R 'NeverD(WorkerAnalysisSections|GuiAnalysisSections|GuiUnits|GuiQueries)$' --output-on-failure

python scripts/build.py build --target neverd neverd-worker
ctest --test-dir build -R '^NeverDAnalysisSectionsCLI$' --output-on-failure
```

## Current GUI integration

The current GUI stores additions in the shared project folder,
`<input>.neverd/analysis-sections.json`. The project-file registry owns this
path and migration from `.neverd-analysis-sections.json`. Save persists project
changes; **File → Pack database** includes them in a portable `.nddb`. Background
analysis snapshots carry the active additions, including unsaved code, into the
read-only analysis worker. The snapshot carries their bytes as hexadecimal in
one worker frame; additions too large for the frame fail background analysis
with `budget_exceeded` before any bytes are encoded. A snapshot without
additions imports nothing into a replica that opened without any.
