# Windows SEH facts that decide correctness

Each fact here was checked against a runtime routine, an MSVC-built image or
an execution. Re-check before extending one to a new compiler or
architecture.

## x64, ARM and AArch64 scope tables

- A function with `__C_specific_handler` (or a GS wrapper around it) carries
  a scope table after its unwind info: a count, then rows of begin, end,
  handler and target RVAs. An `__except` row has a filter RVA (or the
  constant 1 for `EXCEPTION_EXECUTE_HANDLER`) and the `__except` body as its
  target. A `__finally` row has the termination handler and target 0.
- `__C_specific_handler` scans the rows in table order. Nested `__try`
  scopes appear as several rows, innermost first, and one IP range can be
  covered by several rows. The handler of row k runs with the scan already
  past k, so code that a row's own handler runs stands outside scope k and
  inside the rows around it.
- Windows' routine has a rule for an unwind whose target lies in another row
  with the same handler. wine64 10.0's `__C_specific_handler` lacks it and
  fails tests 14/16/18 (and 80/83/86) of the corpus xcpt4 /O2 x64 images,
  which pass on Windows. Never certify local-unwind placement with wine; a
  `windows-latest` job (or an MSVC-built fixture run there) is the
  certifier.

## GS handler wrappers

- `__GSHandlerCheck` (no SEH) checks the cookie and returns. Stripped images
  name it nowhere; its plain x64 bytes are
  `48 83 EC 28 4D 8B 41 38 48 8B CA 49 8B D1 E8 ?? ?? ?? ?? B8 01 00 00 00
  48 83 C4 28 C3`. On a 150-DLL Windows 10 sample, 98.8% of the "partial"
  exception records were this routine left unnamed.
- x64 `__GSHandlerCheck_SEH` checks the cookie, then calls
  `__C_specific_handler` only if the dispatcher context's record has the
  EHANDLER bit (dispatch) or the UHANDLER bit (unwind) set. So every
  `__except` row needs EHANDLER and every `__finally` row needs UHANDLER in
  the unwind info's flags; a table that does not meet that is not handled
  the way its rows say.
- The AArch64 wrapper tests the same bits (`tst (unwinding ? 2 : 1),
  record`). The 32-bit ARM wrapper calls `__C_specific_handler` always: its
  record has no such bits.

## Handler funclets

- A termination handler is `void handler(BOOLEAN AbnormalTermination,
  void *EstablisherFrame)`; a filter is `LONG filter(EXCEPTION_POINTERS *,
  void *EstablisherFrame)`. On x64 the frame arrives in RDX and the funclet
  reads the parent's locals through it (`mov rax, [rdx+40h]`).
- So the parent's frame layout is part of the handler's contract. A rewritten
  parent that passes its own `llvm.localaddress()` breaks handlers that read
  the original layout; the gate refuses those
  (`sehCallbackUsesOriginalFrame`). Passing `rsp_init - frame bytes` made
  such a handler run identically in an experiment; that output form is not
  built.
- On AArch64, MSVC filter and finally callbacks may read the parent's
  callee-saved registers directly (`ldr w0, [x27]`). LLVM C prints each
  callback alone, so it cannot give such a register one declaration on every
  page; HighC inlines the callbacks and fills them (`SEHFuncletRegisters`).

## Local unwind

- `_local_unwind(frame, target)` unwinds to `target` in the frame's function,
  running the termination handlers of the scopes it leaves. MSVC calls it
  when a `goto`, `return` or `break` leaves a `__try` that has a `__finally`.
- At /O2 a handler can itself call `_local_unwind` with a target in its
  parent. Execution then resumes in the parent at a block that no edge of the
  parent reaches; an optimizer that sees only the parent deletes it. HighC
  inlines the handler and prints a `goto` to the label; LLVM C names the
  target in a note (`neverd.source.unreached-unwind-targets`).
- Placement certified by execution: a local unwind's handler call stands
  where the table scan would run it. Seven wrong placements each changed the
  transcript of an MSVC-built fixture, and LLVM's `-EHa` table follows the
  `llvm.seh.try.*` markers, so a placement rule must tie the call to the
  markers.

## x86 registration SEH

- 32-bit x86 links an `EXCEPTION_REGISTRATION` on `FS:[0]`; MSVC's
  `_except_handler4` uses a scope table of try levels with filter and
  handler, encoded with the security cookie, set up by `__SEH_prolog4`.
  A frame-pointer-relative state variable selects the active try level.
- Filters and handlers run on the parent's EBP, not on their own frame.

## Control Flow Guard

- x64 CFG calls `mov rax, target; call [__guard_dispatch_icall_fptr]` and
  tail-jumps `jmp [__guard_dispatch_icall_fptr]`. The loader fills the slot
  the load configuration names (`GuardCFDispatchFunctionPointer`) with a
  dispatcher that checks RAX and jumps to it with the caller's registers and
  stack; while the guard is not enforced the slot keeps the image's
  `_guard_dispatch_icall_nop`, a plain `jmp rax`. So the transfer is one
  through RAX.
- GuardFlags `0x100` (CF_INSTRUMENTED) without the function-table flag means
  objects compiled with `/guard:cf` and an image linked without it (the
  corpus xcpt4 images). Their indirect calls still go through the slot.
- x86, ARM and AArch64 use the check form instead: `call
  [__guard_check_icall_fptr]` with the target in ECX, R0 or X15, then the
  plain call. The check routine has its own convention (LLVM's
  `CFGuard_Check`): read the exact preserved set there before modeling the
  check as anything but a call.
- A lifted module of a CF-instrumented image carries `cfguard=2`, so LLVM
  instruments its indirect calls again and the patch must bind the slot
  symbols to the image's load-configuration slots.
