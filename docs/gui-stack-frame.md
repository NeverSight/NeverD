# Stack frame corrections

In a native x64 or AArch64 function, **Edit → Stack variables** (`Ctrl+K`)
lists observed stack accesses by their signed offset from SP at function entry.
Enter a name and, optionally, an exact-width integer or pointer C type. An empty
name leaves the slot automatic. The same record names its disassembly operand
and both C representations. `K` starts from accesses at the current instruction.

**Edit → Change stack pointer** (`Alt+K`) records the entry-relative SP after
the selected instruction. When SP before the instruction is known, the dialog
also accepts the net SP change made by that instruction. A correction can
therefore recover a known SP after a dynamic adjustment; unknown is never zero.
An unknown value leaves the correction field blank until you enter an offset.
It does not move any machine instruction or change the loaded bytes.

The disassembly's context menu also offers **Stack variables** anywhere in a
function and **Change stack pointer** on an instruction. A code window's
context menu offers **Stack variables** of the function it shows, even when
the disassembly is elsewhere.

Stack offsets, widths and SP corrections use decimal by default, including
leading zeros. A `0x` or `0X` prefix selects hexadecimal: `-016` and `-0x10`
both mean minus sixteen bytes.

Corrections are analyst interpretations for source analysis. LowIR, ordinary
MedIR/LLVM views, lifting and patching keep the original machine semantics.
The private source variant has a marker rejected by ordinary LLVM emission.
When a physical stack object is named, LLVM C keeps that function's frame
storage and pauses its LLVM optimizations so scalar replacement cannot discard
the selected object. Reset restores automatic source analysis and optimization.

SP assertions require a retained non-branch instruction and a single ordinary
entry without incoming edges. A loop back to the entry would capture a new SP
basis on each iteration and is refused. Independently rooted and exceptional
graphs are refused rather than given an invented dominating entry SP. Corrections currently support native
64-bit x86 and AArch64; i386 callee-pop integration is not qualified. All
offsets are bounded to one MiB and queries have block/operation/work limits.

Manual variables bind observed negative entry-SP offsets with the exact access
width. They cannot redirect an unknown address, redefine the return address or
change the parameter ABI. Overlapping accesses retain shared backing storage;
an escaped, ambiguous or optimized object without independent storage in both
C routes is refused. Larger aggregates and floating-point reinterpretation
are not supported by this operation. Existing source type actions remain
available for their separately supported cases.

The input image digest and graph snapshot bind saved policies to the original
analysis. Graph identity includes ordinary/exception roots, edges, instruction
modes and function temporary lifetimes. Obsolete policies can be inspected and
reset. Edits use project history, replicas and the existing database/sidecar
ownership; rejected changes publish no history or partial policy.

## Command line

Inspect the same stack analysis and save an editable JSON policy:

```sh
neverd decompile program.exe --func 0x140001000 --stack-frame-info --json -o frame.json
```

The result includes `instructions`, `accesses`, and a `policy` object containing
the graph snapshot, input SHA-256, `points`, and `variables`. Edit the policy's
arrays using the same entry-SP coordinates as the GUI:

```json
{
  "points": [{"address": "0x140001000", "after": -40}],
  "variables": [{"offset": -16, "bytes": 8, "name": "saved_context", "type": "uint64_t"}]
}
```

Keep the returned `snapshot` and `input_sha256` fields. Apply the saved policy
to either C route, or inspect the corrected offsets:

```sh
neverd decompile program.exe --func 0x140001000 --stack-frame frame.json -o high.c
neverd decompile program.exe --func 0x140001000 --stack-frame frame.json --llvm -o llvm.c
neverd decompile program.exe --func 0x140001000 --stack-frame frame.json --stack-frame-info --json
```

`--stack-frame` accepts the full inspection result or a bare policy, including
the `stack_frame` object extracted from the GUI project's `code.json`. Both C
routes must retain every requested name and exact-width object. The SDK owns
this check for both frontends; stale graphs, mismatched images and unsupported
storage fail before an output file is opened.

These options require one `--func` and C output. They cannot combine with
`--no-opt` or `--devirtualize`. Source optimization flags remain available when
emitting C, but do not apply to `--stack-frame-info`. CLI policies are explicit
inputs to that invocation; the command does not save GUI project edits. Omit the
policy, or pass an empty `{}` policy, to return to automatic CLI analysis.

Regression coverage: `NeverDSourceStackFrameTests`, `NeverDWorkerStackFrame`,
`SourceVariableTypesTests::stackFrameDialogs` and
`SourceVariableTypesTests::unknownStackPointerRequiresAnExplicitValue`.
`NeverDWorkerCLIStackFrame`
compares CLI and worker offsets/names, executes both C routes against an
independent call-count oracle, and covers dynamic SP recovery, stale policies,
invalid input, lost storage and preservation of existing output files.
The worker regression checks
both C routes, compiles and executes their C with an independent call-count
oracle, checks listing names, native isolation, rejected edits, undo/redo,
reopen and a read-only replica.
