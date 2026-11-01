# Native x64 RTM regions

The explicit V6 source exporter can execute a selected transaction through
`native_rtm_regions: true`. The generated provider retains one native outer
XBEGIN checkpoint until the outer XEND or an abort. Its calls to translated
instruction steps update the explicit machine state and the original mapped
RAM inside that same transaction. The processor commits or rolls back those
writes together. The provider never returns with an active transaction.

This binding is distinct from the smaller
[immediate-abort pair](x64-continuation-source.md#immediate-abort-rtm-regions).
Select one binding per export. Individual XBEGIN steps still report a native
boundary; the provider owns their surrounding transaction.

## Invocation

For example, an entry-based request is:

```json
{
  "profile": "kernel-events-v6",
  "mxcsr_mask": "0xffbf",
  "paging_address_bits": 48,
  "entries": ["0x1000"],
  "native_rtm_regions": true,
  "follow_internal_calls": true,
  "callee_depth": 2
}
```

Both HighC and LLVM-to-C emit `neverd_x64_rtm_environment_v6`. Pass it to
`neverd_x64_run_v6` with a `neverd_x64_rtm_context_v6` whose
`max_instructions` is the caller's budget for one attempt. That budget includes
the outer XBEGIN, nested controls, ordinary steps and XEND. The context is
read-only. One outer environment-budget unit covers each invocation;
translated-instruction budgets count steps outside that invocation.

Entry discovery follows canonical XBEGIN bodies and fallbacks and XEND/XTEST
fallthroughs. Calls require `follow_internal_calls` and selected targets;
`callee_depth` bounds automatic direct-callee discovery. Explicit instruction
lists select exactly those addresses. Both outer dispatch styles use the same
transaction provider.

The generated function uses the standard native XBEGIN, XEND, XABORT and
XTEST builtins and a native CPUID availability query in NeverC. A compatible
Clang uses its RTM builtins and the CPUID assembly fallback. The RTM target
attribute belongs to the provider; callers need not enable RTM globally.

## Completion and rollback

On commit, the state PC is the original outer XEND fallthrough. Nested XEND
only reduces nesting; it does not publish a partial transaction. Original
CALL/RET use the explicit guest stack, including its return-slot writes.

On an original explicit or processor-selected abort, hardware restores all
speculative explicit-state and RAM writes. The provider then zero-extends the
actual EAX status into explicit RAX and sets PC to the **outermost** original
fallback. Every other represented byte remains as it was at entry. A nested
XBEGIN's encoded fallback never replaces the outer fallback.

An exhausted attempt, unselected destination, unsupported body instruction or
rejected step state triggers a private XABORT. Hardware rolls back the entire
attempt, and the provider returns zero with state and RAM unchanged. The
existing native boundary remains available to another environment. The private
status is never published as an original abort. To distinguish it from every
original XABORT, export reserves an immediate absent from the selected bytes.
A selection containing all 256 original immediates fails with a diagnostic;
it must be split into selections with an available code.

Standalone canonical XABORT and XTEST are also bound outside a transaction.
XABORT preserves state apart from PC; XTEST sets ZF, clears CF/PF/AF/SF/OF and
preserves other flags. Standalone XEND requires fault delivery and stays a
native boundary in this nonfaulting profile. CPUID and PAUSE inside a
transaction execute their native instructions, which abort RTM; no abort
reason is synthesized.

## Admission and limits

The original semantic operations determine `rtm_body` in the analysis layer,
before explicit-state lowering can hide system-register effects as RAM stores.
Admitted steps modify ordinary RAM, represented GPR/vector values and status
flags, using the existing continuation semantics. Profile projections,
virtualized system effects, nonstatus-flag writes such as CLD/STD, and other
intrinsics need separate transaction contracts. The provider's manifest lists
roots, admitted steps, native controls and the private abort immediate.

The caller supplies a valid V6 state, an RTM-capable native execution environment
outside existing RTM/HLE, and successful ordinary RAM accesses. Original RAM
must exclude explicit state, context, generated code and the compiler frame.
Instruction bytes and mappings remain stable. MMIO, external active
transactions, faults, debug traps and asynchronous delivery are outside this
binding. Missing CPUID RTM or a zero attempt budget declines before execution.

RTM may abort without reaching an original XABORT. Translated code has its own
instruction and cache footprint, so it does not promise progress or identical
capacity/conflict outcomes to the original code. The status is always the
actual native status for the executed attempt. This is selected x64 source
support, not an ordinary C ABI for arbitrary processor state, an i386 state
provider, or complete x86 instruction coverage.

The instruction and nesting rules follow the
[Intel Software Developer's Manual](https://cdrdv2-public.intel.com/671110/325383-sdm-vol-2abcd.pdf).

## Reproduce verification

Set `NEVERD_ISA_REGRESSION_ARTIFACT_DIR` while running
`X64ContinuationExportTest.RtmRegions*`. The fixtures export both C routes and
both outer dispatchers. Their harnesses compare every represented state byte,
original RAM and guest-stack canaries across commit, explicit and nested abort,
nested commit, loops, CALL/RET, callee abort, CPUID/PAUSE abort and rollback on
unsupported paths or exhausted budgets. Additional checks cover standalone
controls, input rejection, discovery and the all-256-immediates diagnostic.

Run `scripts/isa/x86/verify_rtm_regions.py` with `--compiler`, `--sde`,
`--sources` and `--keep`. Add `--backend-llc` and optionally `--new-pm` to use
the LLVM fork's two backend pipelines. The verifier preserves the generated
source and harness verbatim, checks native intrinsic use, and executes at
O0/O2. In SDE's full RTM mode the independent oracle executes the original
relative machine code, including nested branches and calls. Forced-abort mode
clears CPUID RTM in SDE 10.13.1, so that mode qualifies refusal only.

[Intel SDE](https://www.intel.com/content/www/us/en/developer/articles/tool/software-development-emulator.html)
is a processor model. These checks do not establish physical RTM execution or
fault/event continuation. Keep those limits distinct in qualification records.
