# Arrays, alignment and enum operands

The disassembly **Edit** menu includes **Make array** (`*`) and **Make
alignment** (`L`); the disassembly's context menu offers both on a data item.
**Operand type → Enum operand** (`M`) selects an enumeration
declared in Local Types. The normal operand format reset (`#`) restores numeric
spelling; `U` undefines a complete array or a manually marked alignment span.
All three edits use project undo/redo, sidecars and database/replica state.

An array records element width, count and an optional local type name. The
complete span must be initialized, bounded to 64 MiB, and disjoint from function
code and other definitions. Untyped scalar elements are 1, 2, 4 or 8 bytes;
typed elements require the exact current type layout. Each element has an
indexed listing row, including when navigating into the middle of a large
array. Record rows show a bounded byte preview with the complete element size.
They do not expand a record into an unbounded number of member rows.
Code-overlap validation discovers functions even in a cold CLI/SDK session.
Unknown extents in the affected executable segment require bounded function
preparation; an unproven boundary is refused instead of treating zero size as
evidence that the bytes are data.

Saved numeric extent and element width remain authoritative for presentation
when a local type later changes. A missing or differently sized type produces
`stale element type` and raw bytes, without resizing the original definition.
Type edits invalidate both array layout and enum spelling caches.

Alignment is an explicit padding span ending at the requested power-of-two
boundary (2 bytes to one MiB). At an already aligned address it marks one full
alignment unit. It changes the listing's interpretation, never the input bytes.

Enum operands currently use the existing native x86/x64 operand formatter.
The SDK validates an actual decoded immediate, its logical width and the enum's
underlying width. Signed values retain their width; aliases choose the first
declared exact member deterministically. Flags mode combines named single bits
and includes every remaining bit numerically, for example
`QA_READ | QA_WRITE | 0x8`. Deleting the enum restores numeric display until
the type returns. Original operand identity survives classic operand folding.

Definitions are presentation state. They do not supply a new machine semantic
contract to decompilation, lifting or native rewriting. Structural state loading
does not eagerly bind type-dependent rows against a preceding type generation;
new edits validate against the current library, and render binding uses the
current library without changing saved extents.

## Command line

`items` and `operands` edit the same project files as the GUI, through the
same SDK validation. Declare any named record or enum in Local Types first:

```sh
neverd types program.exe --declare types.h --save
neverd items program.exe --array 0x140002000 --size 4 --count 16
neverd items program.exe --array 0x140002040 --size 12 --count 4 --type QA_ROW
neverd items program.exe --alignment 0x140002073 --boundary 16
neverd operands program.exe --addr 0x140001000 --operand 1 --enum QA_FLAGS --flags
```

For an array, `--size` (or `--element-size`) is the element width and `--count` is required. `--type`
is optional. Alignment computes its span through the next boundary; at an
already aligned address it marks a complete unit. `--boundary` defaults to 16
and cannot combine with `--size`.

Numeric presentation options (`--size`, `--element-size`, `--count`,
`--boundary`, `--operand` and structure `--delta`) use decimal by default,
including leading zeros. Use `0x` or `0X` for hexadecimal. For example,
`--count 010` means ten elements, matching the GUI.

`types --declare --save` (also spelled `--save-declarations`) keeps successfully parsed declarations in the binary's
project for subsequent commands and GUI sessions. Without `--save`, declarations
remain local to the current `types` invocation. Saving requires a binary and
only runs after all supplied declaration files parse successfully.

`--enum` selects an exact enum member; `--flags` additionally combines single
bits and preserves residual bits numerically. `--operand` uses the original
decoded operand index, starting at zero, so the immediate of `imul r,r,imm` is
operand 2 even when the GUI folds its text to `imul r,imm`.
The display uses the same decoded operand width as SDK admission; a byte shift
count does not take the width of its destination register.
SDK/CLI disassembly preserves raw `op_str` and adds `display_op_str` when an
enum changes its spelling. Text output uses the same enum formatting. Missing
or deleted enum types leave the numeric operand visible.

Run `neverd items program.exe --json` or `neverd operands program.exe --json`
to read the saved rows. Text output includes array extent, element type,
alignment boundary and enum flags mode. `items --clear ADDRESS` removes a
definition; `operands --addr ADDRESS --operand INDEX --clear` restores numeric
spelling. Reopen the project in the GUI to see CLI edits. The ordinary project
writer lock prevents concurrent edits by a running GUI session.

Checks: `NeverDWorkerDataPresentation` uses a real ELF with code and a separate
data segment and covers pagination, records, enum aliases/signed values/unknown
bits, layout changes, code overlap, atomic refusals, undo/redo, reopen, replicas
and ordinary IR isolation. `NeverDWorkerFormat` covers logical-width plumbing
and folded operands. The Qt source/workbench test includes all three dialogs.
`NeverDWorkerCLIDataPresentation` checks byte-identical CLI/worker sidecars,
GUI rendering of CLI edits, native IR isolation, resets and atomic refusals.
It also calls the SDK directly to reject malformed array field types without
replacing an existing definition.
