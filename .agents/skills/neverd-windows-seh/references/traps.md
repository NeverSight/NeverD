# Mistakes made in this area

Each entry happened. The fix is the habit after the colon.

## Claims

- A commit message stated why 12 AArch64 handlers stayed refused; the parent
  frame it blamed was certified, and the real cause (no scope-table row
  names those handlers) came out later: run the claim before writing it.
  `dev` history cannot be rewritten, so a wrong message needs a correction
  in the next commit's message.
- "The va_list goes to an import" was written down from reading; a
  synthetic wrapper whose walker reads nothing disproved it: label
  code-reading conclusions as such.
- A sorted-set ordinal was named as the cause of unstable HighC text; a
  private library that sorted by name disproved it. The real cause was a
  set of raw type pointers whose addresses were reused: fingerprint the
  address instead of trusting weak pointers, and run A/B 20 times each,
  interleaved.
- An item was closed as "by design" while code it produced was wrong (a
  handler that printed `_local_unwind(x, 1)`, because the optimizer had
  deleted the parent block it named): check what the output does before
  closing an item.
- A speed-up skipped x87 call proofs in x64 functions that name no x87
  register ("their callers' long doubles are popped"). Corpus and capstone
  output stayed byte for byte the same, and `X87CallStack` disproved it: a
  wrapper returns its callee's ST(0) without naming a register. Identical
  samples do not make a shortcut sound; run the whole x87 suite (its tests
  iterate X86 and X64) and keep a memo with dependencies instead of a skip.
- A docs sentence counted the local unwind fixture's evidence as "eight
  builds"; the script ran four builds with two forms each, and two more
  builds (/Od) refuse. Read every count from the script that ran, and say
  which builds were left out and why.

## Scope of a fix

- A blanket "any cross-function code address" rule broke 24 C++ catch
  funclets' continuation proof; the narrow rule (handler unwind targets
  only) did not: keep a rule to the shape you proved.
- A refusal placed in the native emitter un-lowered 660 of 886 functions and
  broke three tests that print native-mode IR; the same refusal in the
  output gate refused exactly the wrong binaries: refuse where the wrong
  output is produced, not upstream of every consumer.
- An accept rule alone in the output gate changes nothing when an earlier
  classification refuses the same output: find every refusal on the path
  before relaxing one.
- Making a forwarder's own `jmp [slot]` a jump through RAX printed a trap in
  its body (RAX is the caller's): a rewrite that is right for callers can be
  wrong for the function that is only the transfer.
- LLVM C's typed member reads: a use walk that followed values through a
  call's result made every call its own intervening writer, and the x86
  return-address push, which prints as a frame store before the call, made
  every member call through a loaded pointer lose its member name. A call is
  a statement where it stands, and a pointer the function had before a
  local's address got out cannot point into that local. Two session tests
  caught both; the LLVM C unit tests did not.
- A gate relaxation (a catchret whose continuation re-enters an outer row
  through range-enter markers) was written for four xcpt4 functions before
  anyone checked what else refused them; patched with it, all four stopped
  at the original-frame gate, so no run could show the relaxation right and
  it was set aside. Patch each target function with a relaxation before
  keeping it.
- LLVM requires every unwind edge out of a funclet pad to share one
  destination: when printed C gives a `__finally` pad a catch to unwind to,
  both the callback's invoke inside the pad and the `cleanupret` must go
  there, or the verifier rejects the function.

## Tests and builds

- A test suite passed and a sweep was clean, and a different test program
  turned red after the push: run the whole set and diff by name.
- `NeverDLibraryRecognitionTests` was never in the pre-push set and stayed
  red for a day after a destructor-only classification change: include it
  whenever LLVM C or the source map changes.
- Never rebuild a build directory while a test program runs from it (tests
  shell out to `bin/neverd`). A mutation harness that rebuilds only the
  test target leaves `bin/libneverd.so` as last linked.
- clang skips `-Wuninitialized` once a file has any error: count
  never-assigned locals with a script.
- A test program compiled against an earlier LLVM package's headers called
  `llvm.x86.avx2.gather.q.ps` where its source named `psllv.d.256`: the
  intrinsic numbers moved. `ninja -t deps <object>` shows which package's
  headers an object read; check it before suspecting the code.
- CI's Linux job excludes `NeverDPatchFullTests` (about two hours here): run
  it locally whenever the COFF patch path changes.
- The emitter dissolves `phi; ret` joins: a test PHI needs more code in its
  block to survive.
- A whole-image `patch` sweep reports each image's first failing function
  only, so a change that admits one function moves the image's error to the
  next one, and changes from a newer base mix in. Attribute every moved image
  with a library that reverts only the change on the same base: of 24 moved
  images, 17 AArch64 ones moved with the nested-table change and 7 x86 ones
  with `dev`'s code-identity commits.

## Shell and process

- `grep` is a ugrep function in this shell: use `command grep -a`.
- A kill loop whose pattern also matches text elsewhere in the same command
  line kills the calling shell (exit 144): put the kill in a command of its
  own and bracket every alternative (`[x]pattern`).
- zsh does not word-split `$FILES`: use an array or `${=FILES}`.
- Load generators and sweeps must end by themselves: give every background
  job a timeout and check `ps` afterwards.
- A run stopped by hand leaves its waiters polling for a marker that never
  comes; it looks like a crash later. Stop the waiters with the run, and
  check the transcript before calling a stopped run a failure.
- `LD_PRELOAD` set on `nice` or `timeout` arms the sampler's `ITIMER_PROF`
  in that process; the timer survives `exec` and SIGPROF kills neverd
  (rc 155). Write `nice ... env LD_PRELOAD=... neverd ...`.

## Runtime

- wine64's `__C_specific_handler` is not Windows' (semantics.md): a wine run
  that fails the local-unwind tests says nothing about placement.
- A rewritten parent passed its own frame to a termination handler that read
  the original frame layout, and the image printed `count = -1`: execute
  every newly accepted shape before relying on the gate.
- A filter that only keeps a value in its parent's frame and reads it back
  behaves the same when its frame moves by a few bytes: such a probe proves
  nothing. Move the frame off the stack instead.
- A transcript equal to the original's proves nothing about a pad the test
  never reaches. Patch each accepted rewrite again with the pad's frame
  offset moved and run it: only a changed transcript proves the pad ran. The
  real gate refuses the moved offset, so patch the probe with an engine whose
  gate takes any offset (`seh/emu/mutprobe.sh` with the spike library).
- Native IR is also printed as LLVM C (`llvmcOnlyFunction` in the
  `LLVMCPointerAddresses` corpus tests, which compile and run the C): a new
  native form needs a C spelling there, and tests that encode the old
  contract (a filter passed `llvm.localaddress`) change with it. A function
  LLVM C calls needs its component linked by the LLVM C library, or every
  LLVM C test program fails to link.
- An emitter that marks protected loads volatile must leave loads of mapped
  globals alone: a volatile import-pointer load made LLVM C declare the
  import slot as data.
- A function the compiler adds with no source address (a filter adapter)
  stays private: it is not in `CompiledImage::SymbolAddrs`. Find it among the
  compiler's receipts for the definitions it compiled
  (`CompiledImage::SourceFunctionOwners`, by IR name).
- `0x140002fa0` of the fh3 no-gs /O2 xcpt4 build fails the sizing compile
  ("invalid Windows EH semantic provenance") with or without the filter
  adapter: its outer row covers only a `jmp` past the inner one. Bisect
  with IR variants (adapter removed, escape removed) before blaming a new
  output form.
- An emulated-frame sweep with a gate that only checked offsets and bounds
  accepted `0x140001360`, whose inner `__finally` passes `_local_unwind` the
  RSP its parent saved in the frame; it ran as the original only because no
  test faults in that range. Look for a callback's calls before trusting a
  run.
- In-place patching bound replacements and callees by name; IR lifted
  without the PDB had its direct calls written to the wrong place while the
  patch exited 0 with a warning: bind by source identity first, and treat a
  warning about unresolved symbols as a bug.
