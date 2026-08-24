        .text

// A PC-relative code relocation is also completed at the decoded instruction
// boundary.  The loader must not publish its historical field-end
// approximation, while the final reachable occurrence must recover the exact
// code identity for patching/symbolization.
        .globl  jt_identity_pcrel_code_call
        .type   jt_identity_pcrel_code_call,@function
jt_identity_pcrel_code_call:
        callq   jt_identity_pcrel_code_target
        retq
        .size   jt_identity_pcrel_code_call, .-jt_identity_pcrel_code_call

        .globl  jt_identity_pcrel_code_target
        .type   jt_identity_pcrel_code_target,@function
jt_identity_pcrel_code_target:
        movl    $3179, %eax
        retq
        .size   jt_identity_pcrel_code_target, .-jt_identity_pcrel_code_target

// R_X86_64_PC32's addend is relative to the containing instruction end, not
// to the four-byte relocation field end.  This cmp encodes an imm8 after its
// disp32, so the historical loader approximation S+A+4 names data-1.  Exact
// decode must instead recover the first byte of this data section.
        .globl  jt_identity_pcrel_nontrailing
        .type   jt_identity_pcrel_nontrailing,@function
jt_identity_pcrel_nontrailing:
        cmpl    $7, jt_identity_pcrel_nontrailing_data(%rip)
        sete    %al
        movzbl  %al, %eax
        retq
        .size   jt_identity_pcrel_nontrailing, .-jt_identity_pcrel_nontrailing

        .section .data.pcrel_nontrailing,"aw",@progbits
        .globl  jt_identity_pcrel_nontrailing_data
        .type   jt_identity_pcrel_nontrailing_data,@object
jt_identity_pcrel_nontrailing_data:
        .long   7
        .size   jt_identity_pcrel_nontrailing_data, .-jt_identity_pcrel_nontrailing_data

        .text

// The non-contiguous mask is the runtime table coordinate itself.  The
// redundant exclusive guard proves the same 31-slot domain as the mask, so the
// mask does not numerically tighten MaxEntries.  It must nevertheless cancel
// the normalization stride inferred from the mask's low zero bit: selector 2
// addresses physical slot 2, not slot 1.  Giving every physical slot a unique
// target makes a stale stride observable in the emitted case keys.
        .globl  jt_identity_mask_equal_bound_coordinate
        .type   jt_identity_mask_equal_bound_coordinate,@function
jt_identity_mask_equal_bound_coordinate:
        movl    %edi, %r10d
        andl    $0x1e, %r10d
        cmpl    $30, %r10d
        ja      .Lmask_equal_default
        leaq    .Lmask_equal_table(%rip), %rax
        movslq  (%rax,%r10,4), %rcx
        addq    %rax, %rcx
        jmpq    *%rcx
.Lmask_equal_default:
        movl    $3399, %eax
        retq
        .irp    slot,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
.Lmask_equal_case\slot:
        movl    $(3300+\slot), %eax
        retq
        .endr
        .size   jt_identity_mask_equal_bound_coordinate, .-jt_identity_mask_equal_bound_coordinate

// Only physical slots 0 and 2 belong to this dispatch.  Slot 1 is an
// independent address-taken root in the same relocation run.  Sparse-domain
// recovery must neither publish slot 1 as a switch case nor claim its storage
// for proof-root suppression / the LLVM pointer mirror.
        .text
        .globl  jt_identity_sparse_mask_gap_root
        .type   jt_identity_sparse_mask_gap_root,@function
jt_identity_sparse_mask_gap_root:
        movq    .Lsparse_gap_table+8(%rip), %r11
        movq    %r11, jt_identity_sparse_gap_observer(%rip)
        movl    %edi, %r10d
        andl    $2, %r10d
        leaq    .Lsparse_gap_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lsparse_gap_case0:
        movl    $3400, %eax
        retq
.Lsparse_gap_case2:
        movl    $3402, %eax
        retq
.Lsparse_gap_independent_root:
        movl    $3499, %eax
        retq
        .size   jt_identity_sparse_mask_gap_root, .-jt_identity_sparse_mask_gap_root

// A relocation in never-explored executable bytes is not enough to classify
// those bytes as a dead instruction: inline data/literals may live in .text.
// Without a final published instruction occurrence or a reachable data-use
// proof, module arbitration must conservatively retain the field and its
// target rather than suppressing it as table filler.
        .text
        .globl  jt_identity_sparse_mask_dead_consumer
        .type   jt_identity_sparse_mask_dead_consumer,@function
jt_identity_sparse_mask_dead_consumer:
        movl    %edi, %r10d
        andl    $2, %r10d
        leaq    .Lsparse_dead_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lsparse_dead_case0:
        movl    $3500, %eax
        retq
.Lsparse_dead_case2:
        movl    $3502, %eax
        retq
.Lsparse_dead_gap_target:
        movl    $3599, %eax
        retq
.Lsparse_dead_consumer:
        movq    .Lsparse_dead_table+8(%rip), %r11
        movq    %r11, jt_identity_sparse_gap_observer(%rip)
        retq
        .size   jt_identity_sparse_mask_dead_consumer, .-jt_identity_sparse_mask_dead_consumer

// A runtime slot can also have an independent address consumer.  Consuming it
// as a switch entry does not authorize dropping the externally visible pointer
// field from the relocation mirror.
        .text
        .globl  jt_identity_runtime_slot_escape
        .type   jt_identity_runtime_slot_escape,@function
jt_identity_runtime_slot_escape:
        movq    .Lruntime_slot_table(%rip), %r11
        movq    %r11, jt_identity_sparse_gap_observer(%rip)
        movl    %edi, %r10d
        andl    $1, %r10d
        leaq    .Lruntime_slot_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lruntime_slot_case0:
        movl    $3600, %eax
        retq
.Lruntime_slot_case1:
        movl    $3601, %eax
        retq
        .size   jt_identity_runtime_slot_escape, .-jt_identity_runtime_slot_escape

// Escaping the table base exposes an unknown downstream index/range, so every
// code-pointer field in the physical object must remain mirrored.  The exact
// dispatch LOAD is still recovered as a switch; only relocation suppression is
// withheld.
        .text
        .globl  jt_identity_table_base_escape
        .type   jt_identity_table_base_escape,@function
jt_identity_table_base_escape:
        leaq    .Lbase_escape_table(%rip), %r11
        movq    %r11, jt_identity_sparse_gap_observer(%rip)
        movl    %edi, %r10d
        andl    $1, %r10d
        leaq    .Lbase_escape_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lbase_escape_case0:
        movl    $3700, %eax
        retq
.Lbase_escape_case1:
        movl    $3701, %eax
        retq
        .size   jt_identity_table_base_escape, .-jt_identity_table_base_escape

// An escaped pointer to an interior table element exposes the containing
// physical object, not merely that one field: external code can index both
// forward and backward from the escaped address.  The dispatch itself still
// has the exact sparse runtime domain {0,2}.
        .text
        .globl  jt_identity_interior_address_escape
        .type   jt_identity_interior_address_escape,@function
jt_identity_interior_address_escape:
        leaq    .Linterior_escape_table+8(%rip), %r11
        movq    %r11, jt_identity_sparse_gap_observer(%rip)
        movl    %edi, %r10d
        andl    $2, %r10d
        leaq    .Linterior_escape_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Linterior_escape_case0:
        movl    $3800, %eax
        retq
.Linterior_escape_case2:
        movl    $3802, %eax
        retq
.Linterior_escape_gap_target:
        movl    $3899, %eax
        retq
        .size   jt_identity_interior_address_escape, .-jt_identity_interior_address_escape

// Cross-function consumers are invisible to the per-function CFGBuilder.
// The module-level arbitration pass must therefore veto suppression of both a
// sparse gap slot and an ordinary runtime slot, then rebuild this owner so the
// restored gap relocation is again a real CFG root.
        .text
        .globl  jt_identity_cross_function_sparse_dispatch
        .type   jt_identity_cross_function_sparse_dispatch,@function
jt_identity_cross_function_sparse_dispatch:
        movl    %edi, %r10d
        andl    $2, %r10d
        leaq    .Lcross_function_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lcross_function_case0:
        movl    $3900, %eax
        retq
.Lcross_function_case2:
        movl    $3902, %eax
        retq
.Lcross_function_gap_target:
        // This consumer is reachable only after the first module arbitration
        // round restores this sparse gap root.  It then vetoes suppression in
        // a second table owner, locking the protected-set fixed point rather
        // than a one-shot module scan.
        movq    .Lcross_function_chain_table+8(%rip), %r11
        movq    %r11, jt_identity_sparse_gap_observer(%rip)
        movl    $3999, %eax
        retq
        .size   jt_identity_cross_function_sparse_dispatch, .-jt_identity_cross_function_sparse_dispatch

        .globl  jt_identity_cross_function_gap_consumer
        .type   jt_identity_cross_function_gap_consumer,@function
jt_identity_cross_function_gap_consumer:
        movq    .Lcross_function_table+8(%rip), %r11
        movq    %r11, jt_identity_sparse_gap_observer(%rip)
        retq
        .size   jt_identity_cross_function_gap_consumer, .-jt_identity_cross_function_gap_consumer

        .globl  jt_identity_cross_function_runtime_consumer
        .type   jt_identity_cross_function_runtime_consumer,@function
jt_identity_cross_function_runtime_consumer:
        movq    .Lcross_function_table(%rip), %r11
        movq    %r11, jt_identity_sparse_gap_observer(%rip)
        retq
        .size   jt_identity_cross_function_runtime_consumer, .-jt_identity_cross_function_runtime_consumer

        .globl  jt_identity_cross_function_chain_dispatch
        .type   jt_identity_cross_function_chain_dispatch,@function
jt_identity_cross_function_chain_dispatch:
        movl    %edi, %r10d
        andl    $2, %r10d
        leaq    .Lcross_function_chain_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lcross_function_chain_case0:
        movl    $4000, %eax
        retq
.Lcross_function_chain_case2:
        movl    $4002, %eax
        retq
.Lcross_function_chain_gap_target:
        movl    $4099, %eax
        retq
        .size   jt_identity_cross_function_chain_dispatch, .-jt_identity_cross_function_chain_dispatch

// A writable code-pointer table can be consumed as a jump table only while its
// contents remain stable.  The mutator lives in a separate function, so the
// module arbitration pass must carry the write-through veto back to this
// owner.  The Low CFG remains an indirect branch with its conservative target
// successors, while static switch emission is forbidden.
        .globl  jt_identity_writable_cross_dispatch
        .type   jt_identity_writable_cross_dispatch,@function
jt_identity_writable_cross_dispatch:
        movl    %edi, %r10d
        andl    $1, %r10d
        leaq    .Lwritable_cross_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lwritable_cross_case0:
        movl    $4100, %eax
        retq
.Lwritable_cross_case1:
        movl    $4101, %eax
        retq
        .size   jt_identity_writable_cross_dispatch, .-jt_identity_writable_cross_dispatch

        .globl  jt_identity_writable_cross_mutator
        .type   jt_identity_writable_cross_mutator,@function
jt_identity_writable_cross_mutator:
        movq    .Lwritable_cross_table+8(%rip), %rax
        movq    %rax, .Lwritable_cross_table(%rip)
        retq
        .size   jt_identity_writable_cross_mutator, .-jt_identity_writable_cross_mutator

// Same-function mutation must not be skipped under the assumption that the
// candidate-local relocation-consumer audit modeled writes to global storage.
        .globl  jt_identity_writable_local_store
        .type   jt_identity_writable_local_store,@function
jt_identity_writable_local_store:
        movq    .Lwritable_local_table+8(%rip), %r11
        movq    %r11, .Lwritable_local_table(%rip)
        movl    %edi, %r10d
        andl    $1, %r10d
        leaq    .Lwritable_local_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lwritable_local_case0:
        movl    $4200, %eax
        retq
.Lwritable_local_case1:
        movl    $4201, %eax
        retq
        .size   jt_identity_writable_local_store, .-jt_identity_writable_local_store

// Passing a writable table base to an opaque callee also invalidates a static
// mapping: the callee may update any slot.  Keep the index in a callee-saved
// register so this test isolates table mutation from caller-saved index proof.
        .globl  jt_identity_writable_call_escape
        .type   jt_identity_writable_call_escape,@function
jt_identity_writable_call_escape:
        pushq   %r12
        movl    %edi, %r12d
        leaq    .Lwritable_call_table(%rip), %rdi
        callq   jt_identity_writable_unknown_callee
        movl    %r12d, %r10d
        andl    $1, %r10d
        leaq    .Lwritable_call_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lwritable_call_case0:
        popq    %r12
        movl    $4300, %eax
        retq
.Lwritable_call_case1:
        popq    %r12
        movl    $4301, %eax
        retq
        .size   jt_identity_writable_call_escape, .-jt_identity_writable_call_escape

        .globl  jt_identity_writable_unknown_callee
        .type   jt_identity_writable_unknown_callee,@function
jt_identity_writable_unknown_callee:
        retq
        .size   jt_identity_writable_unknown_callee, .-jt_identity_writable_unknown_callee

// A pointer to a private frame cell exposes the pointer stored in that cell to
// an opaque callee.  Treating only the frame address as an argument would lose
// the writable table identity and publish a stale switch.
        .globl  jt_identity_writable_frame_slot_escape
        .type   jt_identity_writable_frame_slot_escape,@function
jt_identity_writable_frame_slot_escape:
        pushq   %rbp
        movq    %rsp, %rbp
        pushq   %r12
        subq    $16, %rsp
        movl    %edi, %r12d
        leaq    .Lwritable_frame_slot_table(%rip), %r11
        movq    %r11, -16(%rbp)
        leaq    -16(%rbp), %rdi
        callq   jt_identity_writable_unknown_callee
        movl    %r12d, %r10d
        andl    $1, %r10d
        leaq    .Lwritable_frame_slot_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lwritable_frame_slot_case0:
        addq    $16, %rsp
        popq    %r12
        popq    %rbp
        movl    $4350, %eax
        retq
.Lwritable_frame_slot_case1:
        addq    $16, %rsp
        popq    %r12
        popq    %rbp
        movl    $4351, %eax
        retq
        .size   jt_identity_writable_frame_slot_escape, .-jt_identity_writable_frame_slot_escape

// Escaping a frame-cell address is persistent.  Even though the slot is empty
// at the call, a later store of the writable table base is externally
// observable through the retained pointer and must invalidate the static map.
        .globl  jt_identity_writable_delayed_frame_slot_escape
        .type   jt_identity_writable_delayed_frame_slot_escape,@function
jt_identity_writable_delayed_frame_slot_escape:
        pushq   %rbp
        movq    %rsp, %rbp
        pushq   %r12
        subq    $16, %rsp
        movl    %edi, %r12d
        leaq    -16(%rbp), %rdi
        callq   jt_identity_writable_unknown_callee
        leaq    .Lwritable_delayed_frame_slot_table(%rip), %r11
        movq    %r11, -16(%rbp)
        movl    %r12d, %r10d
        andl    $1, %r10d
        leaq    .Lwritable_delayed_frame_slot_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lwritable_delayed_frame_slot_case0:
        addq    $16, %rsp
        popq    %r12
        popq    %rbp
        movl    $4354, %eax
        retq
.Lwritable_delayed_frame_slot_case1:
        addq    $16, %rsp
        popq    %r12
        popq    %rbp
        movl    $4355, %eax
        retq
        .size   jt_identity_writable_delayed_frame_slot_escape, .-jt_identity_writable_delayed_frame_slot_escape

// Passing the writable table base in an outgoing stack-argument slot must be
// treated exactly like passing it in RDI.  The call target is intentionally
// opaque to module arbitration.
        .globl  jt_identity_writable_stack_arg_escape
        .type   jt_identity_writable_stack_arg_escape,@function
jt_identity_writable_stack_arg_escape:
        pushq   %r12
        movl    %edi, %r12d
        leaq    .Lwritable_stack_arg_table(%rip), %r11
        pushq   %r11
        callq   jt_identity_writable_unknown_callee
        addq    $8, %rsp
        movl    %r12d, %r10d
        andl    $1, %r10d
        leaq    .Lwritable_stack_arg_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lwritable_stack_arg_case0:
        popq    %r12
        movl    $4360, %eax
        retq
.Lwritable_stack_arg_case1:
        popq    %r12
        movl    $4361, %eax
        retq
        .size   jt_identity_writable_stack_arg_escape, .-jt_identity_writable_stack_arg_escape

// The escaped frame address is the second stack argument.  A scalar first
// argument occupies [rsp], while &slot lives at [rsp+8]; the callee can follow
// it to the writable table pointer stored in the local cell.
        .globl  jt_identity_writable_stack_arg_frame_escape
        .type   jt_identity_writable_stack_arg_frame_escape,@function
jt_identity_writable_stack_arg_frame_escape:
        pushq   %rbp
        movq    %rsp, %rbp
        pushq   %r12
        subq    $16, %rsp
        movl    %edi, %r12d
        leaq    .Lwritable_stack_arg_frame_table(%rip), %r11
        movq    %r11, -16(%rbp)
        leaq    -16(%rbp), %r11
        subq    $16, %rsp
        movl    $0, (%rsp)
        movq    %r11, 8(%rsp)
        callq   jt_identity_writable_unknown_callee
        addq    $16, %rsp
        movl    %r12d, %r10d
        andl    $1, %r10d
        leaq    .Lwritable_stack_arg_frame_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lwritable_stack_arg_frame_case0:
        addq    $16, %rsp
        popq    %r12
        popq    %rbp
        movl    $4364, %eax
        retq
.Lwritable_stack_arg_frame_case1:
        addq    $16, %rsp
        popq    %r12
        popq    %rbp
        movl    $4365, %eax
        retq
        .size   jt_identity_writable_stack_arg_frame_escape, .-jt_identity_writable_stack_arg_frame_escape

// A table base may be materialized through a private frame spill and then held
// in an ABI-preserved register across an unrelated call.  No address of the
// slot or table is passed to the callee, so this remains a safe static switch
// and prevents both an all-frame-memory-is-an-argument false positive and an
// all-registers-are-implicit-call-arguments false positive.  The reload is
// intentionally before the opaque call: the occurrence-level table resolver
// must not trace a memory reload through a call without a callee memory
// summary merely because module arbitration later proved this particular test
// slot private.
        .globl  jt_identity_writable_private_spill
        .type   jt_identity_writable_private_spill,@function
jt_identity_writable_private_spill:
        pushq   %rbp
        movq    %rsp, %rbp
        pushq   %r12
        pushq   %rbx
        subq    $16, %rsp
        movl    %edi, %r12d
        leaq    .Lwritable_private_spill_table(%rip), %r11
        movq    %r11, -24(%rbp)
        movq    -24(%rbp), %rbx
        callq   jt_identity_writable_unknown_callee
        movl    %r12d, %r10d
        andl    $1, %r10d
        jmpq    *(%rbx,%r10,8)
.Lwritable_private_spill_case0:
        addq    $16, %rsp
        popq    %rbx
        popq    %r12
        popq    %rbp
        movl    $4370, %eax
        retq
.Lwritable_private_spill_case1:
        addq    $16, %rsp
        popq    %rbx
        popq    %r12
        popq    %rbp
        movl    $4371, %eax
        retq
        .size   jt_identity_writable_private_spill, .-jt_identity_writable_private_spill

// A 32-bit write to an x86-64 argument register kills the complete 64-bit
// view.  The opaque callee receives zero, not the writable table base that was
// previously materialized in RDI; stale RDI:8 provenance must not poison the
// independent dispatch below.
        .globl  jt_identity_writable_arg_alias_clobber
        .type   jt_identity_writable_arg_alias_clobber,@function
jt_identity_writable_arg_alias_clobber:
        pushq   %r12
        movl    %edi, %r12d
        leaq    .Lwritable_arg_alias_table(%rip), %rdi
        xorl    %edi, %edi
        callq   jt_identity_writable_unknown_callee
        movl    %r12d, %r10d
        andl    $1, %r10d
        leaq    .Lwritable_arg_alias_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lwritable_arg_alias_case0:
        popq    %r12
        movl    $4380, %eax
        retq
.Lwritable_arg_alias_case1:
        popq    %r12
        movl    $4381, %eax
        retq
        .size   jt_identity_writable_arg_alias_clobber, .-jt_identity_writable_arg_alias_clobber

// A byte self-copy leaves the complete RDI value unchanged.  Reading DIL
// must therefore project the live RDI table provenance before the partial
// write merges the untouched upper lanes; the opaque callee still receives
// the writable table address.
        .globl  jt_identity_writable_dil_self_escape
        .type   jt_identity_writable_dil_self_escape,@function
jt_identity_writable_dil_self_escape:
        pushq   %r12
        movl    %edi, %r12d
        leaq    .Lwritable_dil_self_table(%rip), %rdi
        movb    %dil, %dil
        callq   jt_identity_writable_unknown_callee
        movl    %r12d, %r10d
        andl    $1, %r10d
        leaq    .Lwritable_dil_self_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lwritable_dil_self_case0:
        popq    %r12
        movl    $4382, %eax
        retq
.Lwritable_dil_self_case1:
        popq    %r12
        movl    $4383, %eax
        retq
        .size   jt_identity_writable_dil_self_escape, .-jt_identity_writable_dil_self_escape

// The legacy high-byte view is an offset lane of RDX, not an independent
// register.  DH:=DH likewise leaves the third SysV argument unchanged and
// must retain the full RDX table provenance across the call.
        .globl  jt_identity_writable_dh_self_escape
        .type   jt_identity_writable_dh_self_escape,@function
jt_identity_writable_dh_self_escape:
        pushq   %r12
        movl    %edi, %r12d
        leaq    .Lwritable_dh_self_table(%rip), %rdx
        movb    %dh, %dh
        callq   jt_identity_writable_unknown_callee
        movl    %r12d, %r10d
        andl    $1, %r10d
        leaq    .Lwritable_dh_self_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lwritable_dh_self_case0:
        popq    %r12
        movl    $4386, %eax
        retq
.Lwritable_dh_self_case1:
        popq    %r12
        movl    $4387, %eax
        retq
        .size   jt_identity_writable_dh_self_escape, .-jt_identity_writable_dh_self_escape

// Writing ESP zero-extends into RSP and starts a non-frame stack epoch.  The
// store therefore exposes the writable table base through an opaque scratch
// address; retaining the old RSP:8 entry-frame fact would incorrectly classify
// it as a private spill and publish a stale switch.
        .globl  jt_identity_writable_esp_pivot_escape
        .type   jt_identity_writable_esp_pivot_escape,@function
jt_identity_writable_esp_pivot_escape:
        pushq   %r12
        pushq   %r13
        movq    %rsp, %r12
        movl    %esi, %r13d
        movl    %edi, %esp
        leaq    .Lwritable_esp_pivot_table(%rip), %r11
        movq    %r11, (%rsp)
        movq    %r12, %rsp
        movl    %r13d, %r10d
        andl    $1, %r10d
        leaq    .Lwritable_esp_pivot_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lwritable_esp_pivot_case0:
        popq    %r13
        popq    %r12
        movl    $4384, %eax
        retq
.Lwritable_esp_pivot_case1:
        popq    %r13
        popq    %r12
        movl    $4385, %eax
        retq
        .size   jt_identity_writable_esp_pivot_escape, .-jt_identity_writable_esp_pivot_escape

// Even when the source is the old stack pointer itself, an ESP write truncates
// to 32 bits and zero-extends into RSP.  The resulting address is no longer the
// entry frame epoch, so storing the writable table base through it must escape.
        .globl  jt_identity_writable_esp_self_truncate_escape
        .type   jt_identity_writable_esp_self_truncate_escape,@function
jt_identity_writable_esp_self_truncate_escape:
        pushq   %r12
        pushq   %r13
        movq    %rsp, %r12
        movl    %edi, %r13d
        movl    %esp, %esp
        leaq    .Lwritable_esp_self_truncate_table(%rip), %r11
        movq    %r11, (%rsp)
        movq    %r12, %rsp
        movl    %r13d, %r10d
        andl    $1, %r10d
        leaq    .Lwritable_esp_self_truncate_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lwritable_esp_self_truncate_case0:
        popq    %r13
        popq    %r12
        movl    $4388, %eax
        retq
.Lwritable_esp_self_truncate_case1:
        popq    %r13
        popq    %r12
        movl    $4389, %eax
        retq
        .size   jt_identity_writable_esp_self_truncate_escape, .-jt_identity_writable_esp_self_truncate_escape

// The writer reaches the table through a relocation-backed global pointer.
// LOAD intentionally erases pointee provenance in the Low address lattice, so
// the authoritative DataPtr relocation itself must make a writable owner
// unsafe before this STORE is even inspected.
        .globl  jt_identity_writable_indirect_dispatch
        .type   jt_identity_writable_indirect_dispatch,@function
jt_identity_writable_indirect_dispatch:
        movl    %edi, %r10d
        andl    $1, %r10d
        leaq    .Lwritable_indirect_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lwritable_indirect_case0:
        movl    $4400, %eax
        retq
.Lwritable_indirect_case1:
        movl    $4401, %eax
        retq
        .size   jt_identity_writable_indirect_dispatch, .-jt_identity_writable_indirect_dispatch

        .globl  jt_identity_writable_indirect_mutator
        .type   jt_identity_writable_indirect_mutator,@function
jt_identity_writable_indirect_mutator:
        movq    jt_identity_writable_table_pointer(%rip), %rax
        movq    8(%rax), %r11
        movq    %r11, (%rax)
        retq
        .size   jt_identity_writable_indirect_mutator, .-jt_identity_writable_indirect_mutator

// A global pointer to immutable table storage is only a relocation/mirror
// consumer.  It must not poison an otherwise exact readonly switch.
        .globl  jt_identity_readonly_dataptr_dispatch
        .type   jt_identity_readonly_dataptr_dispatch,@function
jt_identity_readonly_dataptr_dispatch:
        movl    %edi, %r10d
        andl    $1, %r10d
        leaq    .Lreadonly_dataptr_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lreadonly_dataptr_case0:
        movl    $4500, %eax
        retq
.Lreadonly_dataptr_case1:
        movl    $4501, %eax
        retq
        .size   jt_identity_readonly_dataptr_dispatch, .-jt_identity_readonly_dataptr_dispatch

// An indexed function-pointer tail call is deliberately jump-table-shaped,
// but every slot names a distinct function entry rather than a basic block of
// this function.  Even when module arbitration exhausts its evidence budget,
// this must remain an INDIR_CALL+RETURN rather than being captured by the
// potential-table fail-closed path.
        .globl  jt_identity_callback_tailcall
        .type   jt_identity_callback_tailcall,@function
jt_identity_callback_tailcall:
        andl    $1, %edi
        leaq    jt_identity_callback_tailcall_table(%rip), %rax
        jmpq    *(%rax,%rdi,8)
        .size   jt_identity_callback_tailcall, .-jt_identity_callback_tailcall

// A mask on only one predecessor does not authenticate the merged runtime
// index.  The resulting incomplete-domain marker must still distinguish this
// external callback array from a local basic-block table and preserve the
// ordinary indirect-tailcall lowering.
        .globl  jt_identity_incomplete_mask_callback_tailcall
        .type   jt_identity_incomplete_mask_callback_tailcall,@function
jt_identity_incomplete_mask_callback_tailcall:
        movl    %edi, %r10d
        testl   %esi, %esi
        jne     .Lincomplete_mask_callback_dispatch
        andl    $1, %r10d
.Lincomplete_mask_callback_dispatch:
        leaq    jt_identity_incomplete_mask_callback_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
        .size   jt_identity_incomplete_mask_callback_tailcall, .-jt_identity_incomplete_mask_callback_tailcall

// A separately materialized address at slot two bounds the detector's initial
// relocation run to the first two local entries, while the sized object owns
// four entries and ends with external callbacks.  Incomplete-domain ownership
// must validate the whole sized object, never just that local prefix.
        .globl  jt_identity_incomplete_mask_prefix_callback_tailcall
        .type   jt_identity_incomplete_mask_prefix_callback_tailcall,@function
jt_identity_incomplete_mask_prefix_callback_tailcall:
        movl    %edi, %r10d
        testl   %esi, %esi
        jne     .Lincomplete_mask_prefix_callback_dispatch
        andl    $3, %r10d
.Lincomplete_mask_prefix_callback_dispatch:
        leaq    jt_identity_incomplete_mask_prefix_callback_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
.Lincomplete_mask_prefix_callback_case0:
        xorl    %eax, %eax
        retq
.Lincomplete_mask_prefix_callback_case1:
        leaq    jt_identity_incomplete_mask_prefix_callback_table+16(%rip), %r11
        movl    $1, %eax
        retq
        .size   jt_identity_incomplete_mask_prefix_callback_tailcall, .-jt_identity_incomplete_mask_prefix_callback_tailcall

// A sized outer body can contain independently callable typed entries.  A
// direct CFGBuilder has no KnownFuncEntries set, so ownership must still reject
// these symbol entries rather than treating them as ordinary interior blocks.
        .globl  jt_identity_incomplete_mask_nested_function_tailcall
        .type   jt_identity_incomplete_mask_nested_function_tailcall,@function
jt_identity_incomplete_mask_nested_function_tailcall:
        movl    %edi, %r10d
        testl   %esi, %esi
        jne     .Lincomplete_mask_nested_function_dispatch
        andl    $1, %r10d
.Lincomplete_mask_nested_function_dispatch:
        leaq    jt_identity_incomplete_mask_nested_function_table(%rip), %rax
        jmpq    *(%rax,%r10,8)
        .globl  jt_identity_nested_callback_a
        .type   jt_identity_nested_callback_a,@function
jt_identity_nested_callback_a:
        leal    303(%rdi), %eax
        retq
        .size   jt_identity_nested_callback_a, .-jt_identity_nested_callback_a
        .globl  jt_identity_nested_callback_b
        .type   jt_identity_nested_callback_b,@function
jt_identity_nested_callback_b:
        leal    404(%rdi), %eax
        retq
        .size   jt_identity_nested_callback_b, .-jt_identity_nested_callback_b
        .size   jt_identity_incomplete_mask_nested_function_tailcall, .-jt_identity_incomplete_mask_nested_function_tailcall

        .globl  jt_identity_callback_a
        .type   jt_identity_callback_a,@function
jt_identity_callback_a:
        leal    101(%rdi), %eax
        retq
        .size   jt_identity_callback_a, .-jt_identity_callback_a

        .globl  jt_identity_callback_b
        .type   jt_identity_callback_b,@function
jt_identity_callback_b:
        leal    202(%rdi), %eax
        retq
        .size   jt_identity_callback_b, .-jt_identity_callback_b

// No sized symbol, unwind body, or KnownCodeRange owns the local callback
// thunks below.  The next function entry is only a rough decoding boundary.
// The table address and LOAD are exact, but one incoming branch-target arm is
// an opaque callback.  The all-feasible-target proof must therefore reject a
// static switch while ordinary indirect-call lowering remains well defined.
        .globl  jt_identity_unsized_local_callback_tailcall
        .type   jt_identity_unsized_local_callback_tailcall,@function
jt_identity_unsized_local_callback_tailcall:
        andl    $1, %edi
        leaq    .Lunsized_callback_table(%rip), %rax
        movq    (%rax,%rdi,8), %rax
        testl   %esi, %esi
        je      .Lunsized_callback_dispatch
        movq    %rdx, %rax
.Lunsized_callback_dispatch:
        jmpq    *%rax
.Lunsized_callback_a:
        leal    303(%rdi), %eax
        retq
.Lunsized_callback_b:
        leal    404(%rdi), %eax
        retq
// Deliberately no .size for the dispatcher: this independently detected next
// entry bounds the rough envelope but does not prove that the thunks above are
// blocks owned by the dispatcher.
        .globl  jt_identity_unsized_callback_boundary
        .type   jt_identity_unsized_callback_boundary,@function
jt_identity_unsized_callback_boundary:
        retq
        .size   jt_identity_unsized_callback_boundary, .-jt_identity_unsized_callback_boundary

// An unsized self-tailcall table exercises the entry-only corner: both table
// slots name CurrentFuncEntry, so per-target interior checks alone are vacuous.
// Without authoritative body metadata this still must not become a potential
// switch during module-budget fallback.
        .globl  jt_identity_unsized_self_callback_tailcall
        .type   jt_identity_unsized_self_callback_tailcall,@function
jt_identity_unsized_self_callback_tailcall:
        andl    $1, %edi
        leaq    .Lunsized_self_callback_table(%rip), %rax
        movq    (%rax,%rdi,8), %rax
        testl   %esi, %esi
        je      .Lunsized_self_callback_dispatch
        movq    %rdx, %rax
.Lunsized_self_callback_dispatch:
        jmpq    *%rax
        .globl  jt_identity_unsized_self_callback_boundary
        .type   jt_identity_unsized_self_callback_boundary,@function
jt_identity_unsized_self_callback_boundary:
        retq
        .size   jt_identity_unsized_self_callback_boundary, .-jt_identity_unsized_self_callback_boundary

        .section .rodata.jt_mask_equal,"a",@progbits
        .p2align 2
.Lmask_equal_table:
        .irp    slot,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
        .long .Lmask_equal_case\slot-.Lmask_equal_table
        .endr

        .section .rodata.jt_sparse_gap,"a",@progbits
        .p2align 3
.Lsparse_gap_table:
        .quad .Lsparse_gap_case0
        .quad .Lsparse_gap_independent_root
        .quad .Lsparse_gap_case2
        .size .Lsparse_gap_table, .-.Lsparse_gap_table

        .section .rodata.jt_sparse_dead,"a",@progbits
        .p2align 3
.Lsparse_dead_table:
        .quad .Lsparse_dead_case0
        .quad .Lsparse_dead_gap_target
        .quad .Lsparse_dead_case2
        .size .Lsparse_dead_table, .-.Lsparse_dead_table

        .section .rodata.jt_runtime_slot,"a",@progbits
        .p2align 3
.Lruntime_slot_table:
        .quad .Lruntime_slot_case0
        .quad .Lruntime_slot_case1
        .size .Lruntime_slot_table, .-.Lruntime_slot_table

        .section .rodata.jt_base_escape,"a",@progbits
        .p2align 3
.Lbase_escape_table:
        .quad .Lbase_escape_case0
        .quad .Lbase_escape_case1
        .size .Lbase_escape_table, .-.Lbase_escape_table

        .section .rodata.jt_interior_escape,"a",@progbits
        .p2align 3
.Linterior_escape_table:
        .quad .Linterior_escape_case0
        .quad .Linterior_escape_gap_target
        .quad .Linterior_escape_case2
        .size .Linterior_escape_table, .-.Linterior_escape_table

        .section .rodata.jt_cross_function,"a",@progbits
        .p2align 3
.Lcross_function_table:
        .quad .Lcross_function_case0
        .quad .Lcross_function_gap_target
        .quad .Lcross_function_case2
        .size .Lcross_function_table, .-.Lcross_function_table

        .section .rodata.jt_cross_function_chain,"a",@progbits
        .p2align 3
.Lcross_function_chain_table:
        .quad .Lcross_function_chain_case0
        .quad .Lcross_function_chain_gap_target
        .quad .Lcross_function_chain_case2
        .size .Lcross_function_chain_table, .-.Lcross_function_chain_table

        .section .data.jt_writable_cross,"aw",@progbits
        .p2align 3
.Lwritable_cross_table:
        .quad .Lwritable_cross_case0
        .quad .Lwritable_cross_case1
        .size .Lwritable_cross_table, .-.Lwritable_cross_table

        .section .data.jt_writable_local,"aw",@progbits
        .p2align 3
.Lwritable_local_table:
        .quad .Lwritable_local_case0
        .quad .Lwritable_local_case1
        .size .Lwritable_local_table, .-.Lwritable_local_table

        .section .data.jt_writable_call,"aw",@progbits
        .p2align 3
.Lwritable_call_table:
        .quad .Lwritable_call_case0
        .quad .Lwritable_call_case1
        .size .Lwritable_call_table, .-.Lwritable_call_table

        .section .data.jt_writable_indirect,"aw",@progbits
        .p2align 3
.Lwritable_indirect_table:
        .quad .Lwritable_indirect_case0
        .quad .Lwritable_indirect_case1
        .size .Lwritable_indirect_table, .-.Lwritable_indirect_table

        .section .data.jt_writable_frame_slot,"aw",@progbits
        .p2align 3
.Lwritable_frame_slot_table:
        .quad .Lwritable_frame_slot_case0
        .quad .Lwritable_frame_slot_case1
        .size .Lwritable_frame_slot_table, .-.Lwritable_frame_slot_table

        .section .data.jt_writable_delayed_frame_slot,"aw",@progbits
        .p2align 3
.Lwritable_delayed_frame_slot_table:
        .quad .Lwritable_delayed_frame_slot_case0
        .quad .Lwritable_delayed_frame_slot_case1
        .size .Lwritable_delayed_frame_slot_table, .-.Lwritable_delayed_frame_slot_table

        .section .data.jt_writable_stack_arg,"aw",@progbits
        .p2align 3
.Lwritable_stack_arg_table:
        .quad .Lwritable_stack_arg_case0
        .quad .Lwritable_stack_arg_case1
        .size .Lwritable_stack_arg_table, .-.Lwritable_stack_arg_table

        .section .data.jt_writable_stack_arg_frame,"aw",@progbits
        .p2align 3
.Lwritable_stack_arg_frame_table:
        .quad .Lwritable_stack_arg_frame_case0
        .quad .Lwritable_stack_arg_frame_case1
        .size .Lwritable_stack_arg_frame_table, .-.Lwritable_stack_arg_frame_table

        .section .data.jt_writable_private_spill,"aw",@progbits
        .p2align 3
.Lwritable_private_spill_table:
        .quad .Lwritable_private_spill_case0
        .quad .Lwritable_private_spill_case1
        .size .Lwritable_private_spill_table, .-.Lwritable_private_spill_table

        .section .data.jt_writable_arg_alias,"aw",@progbits
        .p2align 3
.Lwritable_arg_alias_table:
        .quad .Lwritable_arg_alias_case0
        .quad .Lwritable_arg_alias_case1
        .size .Lwritable_arg_alias_table, .-.Lwritable_arg_alias_table

        .section .data.jt_writable_dil_self,"aw",@progbits
        .p2align 3
.Lwritable_dil_self_table:
        .quad .Lwritable_dil_self_case0
        .quad .Lwritable_dil_self_case1
        .size .Lwritable_dil_self_table, .-.Lwritable_dil_self_table

        .section .data.jt_writable_dh_self,"aw",@progbits
        .p2align 3
.Lwritable_dh_self_table:
        .quad .Lwritable_dh_self_case0
        .quad .Lwritable_dh_self_case1
        .size .Lwritable_dh_self_table, .-.Lwritable_dh_self_table

        .section .data.jt_writable_esp_pivot,"aw",@progbits
        .p2align 3
.Lwritable_esp_pivot_table:
        .quad .Lwritable_esp_pivot_case0
        .quad .Lwritable_esp_pivot_case1
        .size .Lwritable_esp_pivot_table, .-.Lwritable_esp_pivot_table

        .section .data.jt_writable_esp_self_truncate,"aw",@progbits
        .p2align 3
.Lwritable_esp_self_truncate_table:
        .quad .Lwritable_esp_self_truncate_case0
        .quad .Lwritable_esp_self_truncate_case1
        .size .Lwritable_esp_self_truncate_table, .-.Lwritable_esp_self_truncate_table

        .section .rodata.jt_readonly_dataptr,"a",@progbits
        .p2align 3
.Lreadonly_dataptr_table:
        .quad .Lreadonly_dataptr_case0
        .quad .Lreadonly_dataptr_case1
        .size .Lreadonly_dataptr_table, .-.Lreadonly_dataptr_table

        .section .data.rel.ro.jt_callback_tailcall,"aw",@progbits
        .p2align 3
        .globl jt_identity_callback_tailcall_table
        .type jt_identity_callback_tailcall_table,@object
jt_identity_callback_tailcall_table:
        .quad jt_identity_callback_a
        .quad jt_identity_callback_b
        .size jt_identity_callback_tailcall_table, .-jt_identity_callback_tailcall_table

        .globl  jt_identity_incomplete_mask_callback_table
        .type   jt_identity_incomplete_mask_callback_table,@object
jt_identity_incomplete_mask_callback_table:
        .quad jt_identity_callback_a
        .quad jt_identity_callback_b
        .size jt_identity_incomplete_mask_callback_table, .-jt_identity_incomplete_mask_callback_table

        .globl  jt_identity_incomplete_mask_prefix_callback_table
        .type   jt_identity_incomplete_mask_prefix_callback_table,@object
jt_identity_incomplete_mask_prefix_callback_table:
        .quad .Lincomplete_mask_prefix_callback_case0
        .quad .Lincomplete_mask_prefix_callback_case1
        .quad jt_identity_callback_a
        .quad jt_identity_callback_b
        .size jt_identity_incomplete_mask_prefix_callback_table, .-jt_identity_incomplete_mask_prefix_callback_table

        .globl  jt_identity_incomplete_mask_nested_function_table
        .type   jt_identity_incomplete_mask_nested_function_table,@object
jt_identity_incomplete_mask_nested_function_table:
        .quad jt_identity_nested_callback_a
        .quad jt_identity_nested_callback_b
        .size jt_identity_incomplete_mask_nested_function_table, .-jt_identity_incomplete_mask_nested_function_table

        .section .data.rel.ro.jt_unsized_callback_tailcall,"aw",@progbits
        .p2align 3
.Lunsized_callback_table:
        .quad .Lunsized_callback_a
        .quad .Lunsized_callback_b
        .size .Lunsized_callback_table, .-.Lunsized_callback_table

        .section .data.rel.ro.jt_unsized_self_callback_tailcall,"aw",@progbits
        .p2align 3
.Lunsized_self_callback_table:
        .quad jt_identity_unsized_self_callback_tailcall
        .quad jt_identity_unsized_self_callback_tailcall
        .size .Lunsized_self_callback_table, .-.Lunsized_self_callback_table

        .data
        .p2align 3
        .globl  jt_identity_sparse_gap_observer
        .type   jt_identity_sparse_gap_observer,@object
        .size   jt_identity_sparse_gap_observer,8
jt_identity_sparse_gap_observer:
        .quad 0

        .p2align 3
        .globl  jt_identity_writable_table_pointer
        .type   jt_identity_writable_table_pointer,@object
        .size   jt_identity_writable_table_pointer,8
jt_identity_writable_table_pointer:
        .quad .Lwritable_indirect_table

        .p2align 3
        .globl  jt_identity_readonly_table_pointer
        .type   jt_identity_readonly_table_pointer,@object
        .size   jt_identity_readonly_table_pointer,8
jt_identity_readonly_table_pointer:
        .quad .Lreadonly_dataptr_table

        .section .note.GNU-stack,"",@progbits
