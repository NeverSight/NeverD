---
name: neverd-return-recovery
description: Diagnose and verify NeverD return recovery across shared exits, PHI joins, register views, variable shifts, MIPS call forwarding and delay slots. Use when HighC or LLVMC loses an integer result, invents one, or accepts an unproven stack-saved return address.
---

# NeverD return recovery

## Find the decision that failed

Start with a small, neutral function whose original machine code distinguishes
the paths. Record the value in the ABI return register, the return PC and the
stack pointer at exit. A wrong C return type, a correct value computed in the
wrong order, and a jump to an unproven address have different owners.

| Question | First place to inspect |
| --- | --- |
| Does a reachable choice produce distinct integer results despite a bare exit? | `settleReturnContracts` and `selectsLiteralReturns` in [PipelineReturnContracts.cpp](../../../lib/pipeline/PipelineReturnContracts.cpp) |
| Is the final jump a return to the entry link on every relevant path? | [MedMIPSReturnTargets.cpp](../../../lib/ir/med/pass/MIPS/MedMIPSReturnTargets.cpp) |
| Is the MedIR already correct, but a C route changes the order or the edge on which an operation runs? | Compare both C outputs with the corresponding MedIR block and its outgoing edges before changing the return contract. |

## Establish what reaches the caller

- Use the inferred result width and physical register views. On x86, an AL or AH write preserves other bytes and an EAX write clears the upper half of RAX. On AArch64, X0 can carry a real incoming argument on a bare return. MIPS n32 carries 64-bit integers in GPRs despite its four-byte pointers.
- Pair a PHI input with its actual predecessor. At a join that merges a value and a shift count, trace both on the same edge. For example, `1 << 1` and `2 << 0` both return two. Reversing the order of either PHI's input list must preserve the answer. If folding leaves a physical count write beside a constant PHI input, verify its register view and value on that edge.
- Compare bits visible *after* masks, byte writes, shifts and sign extension. For a variable count, account for every allowed effective count, including zero. The lifter supplies the ISA's count mask. When legal counts can make the observed results equal, look for independent evidence before inferring a value from that branch. The same rule applies when later code returns only a byte.
- Keep sparse count masks exact. The low bit of `5 >> (n & 2)` is one for both possible counts. Changing the mask to `3` admits a count of one, which clears that bit.
- When a predecessor performs the shift, trace its result through the last PHI, mask or byte extraction to the return. For example, shifting `(2,0)` and `(129,1)` left gives 2 and 258. The low nine bits distinguish them. The low byte is two on both paths. If another predecessor shifts right, include both directions before comparing outcomes. On MIPS, put the shift in a branch delay slot and the mask in the return delay slot to check their order.
- Test word and doubleword shifts on wide-register targets. Use the instruction's operand width for the count and the fill, then apply the architecture's extension to the result. Check a separately merged register view against the corresponding bytes of the full register on the same predecessor.
- If the epilogue copies a shifted scratch register into the ABI result, follow the scratch register through the joins and inspect the ABI value after the copy. If the scratch register also supplies the count, check its count byte against the full word on that edge. Exercise a partial write with bit eight both clear and set on entry to that write. On i386, EAX may hold the loop counter until the exit copies EDX into it.
- When one arm writes the return register directly and the other writes a scratch register, follow the guard that selects the carrier as well as the shift count. A latch can change either one. Put the handoff in its own block and in a MIPS delay slot. Compare the low and full register views on each edge, even when SSA prints the same register name for an entry value and a first write.
- Before source argument recovery, an unwritten PHI input can use the other carrier on that edge only when it is defined there and every feasible observation excludes the unwritten value. Check copies, later PHIs, stores and the zero-trip exit. Flip the selection bit on a latch so that the previously idle register is read. Vary the entry register contents and execute the optimized C to catch an eagerly evaluated missing-state trap. On i386 also check that the two stack arguments keep their positions.
- Trace the selector at its instruction width. AArch64 `TBZ` can test W3 while `LSLV` consumes X3. Check the byte-sized bit-test result and the width of any immediate that updates X3 on the latch.
- Across successive joins, carry the value and count together from the actual predecessor. Read all PHI operands from the predecessor's snapshot before installing any PHI output. Check the enclosing choice when both inner paths return two but a third path can return four. Swap the PHI order in a test where the value and count exchange roles on a backedge. An x86 loop test can define both ZF and a separate SSA predicate. An integer equality writing ZF preserves the paired operands.
- At a loop header, revisit a block whenever either binding changes. Include the zero-trip exit and every reachable latch until the paired states repeat. In one left-shift fixture the entry pairs are `(1,0)` and `(4,1)`. XOR both operands with two on each trip and the sets of returns are `{1,12}` and `{8,48}`. Change only the count update to XOR with one and both paths can return six. Exercise zero, odd and even trips on each C route. When separate latches update the value and count, take both in a single execution to expose collisions that neither creates alone. An unknown returned operand, an unfinished search, or an intersection leaves that choice unproven. See [the paired-path proof](../../../lib/ir/med/pass/MedShiftedReturnPaths.cpp).
- Preserve calls, stores, callee-saved registers and architectural guards. A common `SEB` on MIPS64 requires a valid sign-extended word. Keep a bare exit's missing-result behavior distinct from an instruction that fails its input precondition.
- For a stack-saved link, follow the original store, the bytes reloaded, and the proof that reaches the final jump. Check both incoming edges and loop backedges. A half-width comparison, an overlapping store or a rewritten difference can invalidate an otherwise matching path. A branch-likely delay slot executes on the taken edge. A return delay slot may change the result after the target is captured.

When an unknown call forwards a previous result, keep the ABI argument
register, the returned data and the saved link as distinct SSA values.
For a split spill, establish every byte at its actual address in target byte
order. A zero difference split into high and low comparisons proves equality
only on paths that check both halves of the same values. For PIC/CPIC calls
through the GOT, check the relation between the entry function address in
`$t9`, the established `$gp`, and the loaded slot before naming a callee.
The GP recovery lives in [PICGlobalPointer.cpp](../../../lib/ir/med/lower/MIPS/PICGlobalPointer.cpp).

## Verify a change

Pair each positive with a near-identical counterexample. Useful pairs change a
single count or mask bit, swap the count definitions across two edges, clobber
one stack byte or move an update into an annulled delay slot.

Run the policy case in [ReturnContractTests.cpp](../../../unittests/lift/core/ReturnContractTests.cpp).
Then decompile the linked fixture through `neverd decompile` and
`neverd decompile --llvm`, compile the emitted C at `-O0` and `-O2`, and
exercise both branch outcomes against the original machine code. Check
defined values, conservative return types and expected traps separately.
For a cross-architecture change, exercise the affected x86 and AArch64 widths
and MIPS n32/n64, both byte orders, and the affected PIC/CPIC linkage. Check
`EF_MIPS_PIC` and `EF_MIPS_CPIC` on the object defining the return and on
the linked ELF. A second object can change the link flags while preserving
the leaf's instructions. For a single-object CPIC control, assemble the leaf
with `.abicalls` and `.option pic0` and link it as an executable.
Use neutral fixture symbols and inspect the staged patch and generated source
for private names before publication. See the
[test layout](../../../docs/testing.md#test-layout) for the owning suites.
