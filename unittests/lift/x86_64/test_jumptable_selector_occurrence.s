// The dispatch index is already a byte offset when it reaches the table LOAD.
// R10 is then reused for unrelated arithmetic before the indirect branch.  A
// selector consumer that scans backward by physical register number chooses
// the new R10 lifetime; the exact table-address use occurrence chooses the old
// masked byte offset.

        .text
.Lselector_occurrence_text_base:
        .globl  jt_selector_prescaled_postload_reuse
        .type   jt_selector_prescaled_postload_reuse,@function
jt_selector_prescaled_postload_reuse:
        movl    %edi, %r10d
        shll    $3, %r10d
        andl    $24, %r10d
        leaq    .Lselector_prescaled_table(%rip), %r9
        movq    (%r9,%r10), %rax
        movl    %esi, %r10d
        imull   $3, %r10d, %r10d
        testl   %r10d, %r10d
        jmpq    *%rax
.Lselector_prescaled_case0:
        movl    $6100, %eax
        retq
.Lselector_prescaled_case1:
        movl    $6101, %eax
        retq
.Lselector_prescaled_case2:
        movl    $6102, %eax
        retq
.Lselector_prescaled_case3:
        movl    $6103, %eax
        retq
        .size   jt_selector_prescaled_postload_reuse, .-jt_selector_prescaled_postload_reuse

        .section .data.rel.ro.jt_selector_occurrence,"aw",@progbits
        .p2align 3
.Lselector_prescaled_table:
        .quad   .Lselector_prescaled_case0
        .quad   .Lselector_prescaled_case1
        .quad   .Lselector_prescaled_case2
        .quad   .Lselector_prescaled_case3

// The table target is relayed through a private spill while R10 is reused for
// unrelated arithmetic.  The switch selector is the exact old R10 value
// consumed by the table-address scale, not the later R10 lifetime and not the
// raw pre-mask argument.
        .text
        .globl  jt_selector_masked_same_reg
        .type   jt_selector_masked_same_reg,@function
jt_selector_masked_same_reg:
        movl    %edi, %r10d
        andl    $7, %r10d
        leaq    .Lselector_masked_table(%rip), %r9
        movq    (%r9,%r10,8), %rax
        movq    %rax, -8(%rsp)
        movl    %esi, %r10d
        imull   $3, %r10d, %r10d
        testl   %r10d, %r10d
        movq    -8(%rsp), %rax
        jmpq    *%rax
.Lselector_masked_case0:
        movl    $6200, %eax
        retq
.Lselector_masked_case1:
        movl    $6201, %eax
        retq
.Lselector_masked_case2:
        movl    $6202, %eax
        retq
.Lselector_masked_case3:
        movl    $6203, %eax
        retq
.Lselector_masked_case4:
        movl    $6204, %eax
        retq
.Lselector_masked_case5:
        movl    $6205, %eax
        retq
.Lselector_masked_case6:
        movl    $6206, %eax
        retq
.Lselector_masked_case7:
        movl    $6207, %eax
        retq
        .size   jt_selector_masked_same_reg, .-jt_selector_masked_same_reg

        .section .data.rel.ro.jt_selector_occurrence,"aw",@progbits
        .p2align 3
.Lselector_masked_table:
        .quad   .Lselector_masked_case0
        .quad   .Lselector_masked_case1
        .quad   .Lselector_masked_case2
        .quad   .Lselector_masked_case3
        .quad   .Lselector_masked_case4
        .quad   .Lselector_masked_case5
        .quad   .Lselector_masked_case6
        .quad   .Lselector_masked_case7

// The table base and byte index are both authenticated before the target is
// relayed through memory.  R10 is then reused for unrelated arithmetic.  A
// backend that rediscovers the two-table selector from the final branch-target
// DAG stops at the relay LOAD; the public occurrence recipe must retain the
// original address ADD's byte-coordinate input and the exact CMOV condition.
        .text
        .globl  jt_selector_twotable_relay
        .type   jt_selector_twotable_relay,@function
jt_selector_twotable_relay:
        movl    %edi, %r10d
        shll    $3, %r10d
        andl    $24, %r10d
        leaq    .Lselector_twotable_a(%rip), %r8
        leaq    .Lselector_twotable_b(%rip), %r9
        testl   %esi, %esi
        cmovneq %r9, %r8
        movq    (%r8,%r10), %rax
        movq    %rax, -8(%rsp)
        movl    %edx, %r10d
        imull   $3, %r10d, %r10d
        testl   %r10d, %r10d
        movq    -8(%rsp), %rax
        jmpq    *%rax
.Lselector_twotable_a0:
        movl    $6300, %eax
        retq
.Lselector_twotable_a1:
        movl    $6301, %eax
        retq
.Lselector_twotable_a2:
        movl    $6302, %eax
        retq
.Lselector_twotable_a3:
        movl    $6303, %eax
        retq
.Lselector_twotable_b0:
        movl    $6400, %eax
        retq
.Lselector_twotable_b1:
        movl    $6401, %eax
        retq
.Lselector_twotable_b2:
        movl    $6402, %eax
        retq
.Lselector_twotable_b3:
        movl    $6403, %eax
        retq
        .size   jt_selector_twotable_relay, .-jt_selector_twotable_relay

        .section .data.rel.ro.jt_selector_occurrence,"aw",@progbits
        .p2align 3
.Lselector_twotable_a:
        .quad   .Lselector_twotable_a0
        .quad   .Lselector_twotable_a1
        .quad   .Lselector_twotable_a2
        .quad   .Lselector_twotable_a3
.Lselector_twotable_b:
        .quad   .Lselector_twotable_b0
        .quad   .Lselector_twotable_b1
        .quad   .Lselector_twotable_b2
        .quad   .Lselector_twotable_b3

// A large unrelated prefix makes the exact selector query exceed the legacy
// no-aggregate 4096-work fallback.  The composite detector must share the
// candidate graph budget and report an incomplete query fail-closed instead
// of treating resource exhaustion as a definitive non-table result.
        .text
        .globl  jt_selector_twotable_match_budget
        .type   jt_selector_twotable_match_budget,@function
jt_selector_twotable_match_budget:
        .rept   384
        movl    %ecx, %eax
        andl    $7, %eax
        .endr
        movl    %edi, %r10d
        shll    $3, %r10d
        andl    $24, %r10d
        leaq    .Lselector_twotable_budget_a(%rip), %r8
        leaq    .Lselector_twotable_budget_b(%rip), %r9
        testl   %esi, %esi
        cmovneq %r9, %r8
        movq    (%r8,%r10), %rax
        jmpq    *%rax
.Lselector_twotable_budget_a0:
        movl    $6700, %eax
        retq
.Lselector_twotable_budget_a1:
        movl    $6701, %eax
        retq
.Lselector_twotable_budget_a2:
        movl    $6702, %eax
        retq
.Lselector_twotable_budget_a3:
        movl    $6703, %eax
        retq
.Lselector_twotable_budget_b0:
        movl    $6800, %eax
        retq
.Lselector_twotable_budget_b1:
        movl    $6801, %eax
        retq
.Lselector_twotable_budget_b2:
        movl    $6802, %eax
        retq
.Lselector_twotable_budget_b3:
        movl    $6803, %eax
        retq
        .size   jt_selector_twotable_match_budget, .-jt_selector_twotable_match_budget

        .section .data.rel.ro.jt_selector_occurrence,"aw",@progbits
        .p2align 3
.Lselector_twotable_budget_a:
        .quad   .Lselector_twotable_budget_a0
        .quad   .Lselector_twotable_budget_a1
        .quad   .Lselector_twotable_budget_a2
        .quad   .Lselector_twotable_budget_a3
.Lselector_twotable_budget_b:
        .quad   .Lselector_twotable_budget_b0
        .quad   .Lselector_twotable_budget_b1
        .quad   .Lselector_twotable_budget_b2
        .quad   .Lselector_twotable_budget_b3

// A two-table callback whose byte index is hidden behind more than the bounded
// quasi-copy depth must retain the original JMP: the composite shape is known,
// but its exact selector proof is incomplete.  The adjacent ordinary one-table
// callback is the control: a complete all-callable envelope remains an
// indirect tail call.
        .text
        .globl  jt_selector_budget_callback_a
        .type   jt_selector_budget_callback_a,@function
jt_selector_budget_callback_a:
        movl    $6900, %eax
        retq
        .size   jt_selector_budget_callback_a, .-jt_selector_budget_callback_a

        .globl  jt_selector_budget_callback_b
        .type   jt_selector_budget_callback_b,@function
jt_selector_budget_callback_b:
        movl    $6901, %eax
        retq
        .size   jt_selector_budget_callback_b, .-jt_selector_budget_callback_b

        .globl  jt_selector_twotable_callback_budget
        .type   jt_selector_twotable_callback_budget,@function
jt_selector_twotable_callback_budget:
        movl    %edi, %r10d
        shll    $3, %r10d
        andl    $24, %r10d
        .rept   9
        movl    %r10d, %eax
        movl    %eax, %r10d
        .endr
        leaq    .Lselector_twotable_callback_a(%rip), %r8
        leaq    .Lselector_twotable_callback_b(%rip), %r9
        testl   %esi, %esi
        cmovneq %r9, %r8
        jmpq    *(%r8,%r10)
        .size   jt_selector_twotable_callback_budget, .-jt_selector_twotable_callback_budget

        .globl  jt_selector_single_callback_budget_control
        .type   jt_selector_single_callback_budget_control,@function
jt_selector_single_callback_budget_control:
        andl    $1, %edi
        leaq    .Lselector_single_callback_budget(%rip), %rax
        jmpq    *(%rax,%rdi,8)
        .size   jt_selector_single_callback_budget_control, .-jt_selector_single_callback_budget_control

// Two mapped table loads alone are not a composite jump-table certificate.
// The inner object deliberately contains no code-pointer relocation slots, so
// evidence before relocation identity must not create persistent composite
// authority; genuine resource exhaustion remains transactionally fail-closed.
        .globl  jt_selector_twotable_raw_callback_control
        .type   jt_selector_twotable_raw_callback_control,@function
jt_selector_twotable_raw_callback_control:
        andl    $1, %edi
        movabsq $2, %r10
        movl    $1, %r11d
        jmp     .Lselector_twotable_raw_dispatch
.Lselector_twotable_raw_dispatch:
        leaq    .Lselector_twotable_raw_index(%rip), %rax
        movzbl  (%rax,%rdi), %r15d
        .rept   10
        movl    %r15d, %eax
        movl    %eax, %r15d
        .endr
        btq     %rdi, %r10
        cmovcq  %r11, %r15
        leaq    jt_selector_twotable_raw_inner(%rip), %rax
        .globl  jt_selector_twotable_raw_callback_branch
        .type   jt_selector_twotable_raw_callback_branch,@notype
jt_selector_twotable_raw_callback_branch:
        jmpq    *(%rax,%r15,8)
        .size   jt_selector_twotable_raw_callback_control, .-jt_selector_twotable_raw_callback_control

// A real relocation-backed inner table whose complementary-mask outer arm is
// deliberately transported through more than kMaxQuasiCopyDepth COPYs.  The
// partial clamp must never fall through to publishing the inner table on its
// intermediate index.
        .globl  jt_selector_clamped_depth_limit
        .type   jt_selector_clamped_depth_limit,@function
jt_selector_clamped_depth_limit:
        andl    $1, %edi
        movabsq $2, %r10
        movl    $1, %r11d
        jmp     .Lselector_clamped_depth_dispatch
.Lselector_clamped_depth_dispatch:
        leaq    jt_selector_clamped_depth_outer(%rip), %rax
        movzbl  (%rax,%rdi), %r15d
        .rept   10
        movl    %r15d, %eax
        movl    %eax, %r15d
        .endr
        btq     %rdi, %r10
        cmovcq  %r11, %r15
        leaq    jt_selector_clamped_depth_inner(%rip), %rax
        .globl  jt_selector_clamped_depth_branch
        .type   jt_selector_clamped_depth_branch,@notype
jt_selector_clamped_depth_branch:
        jmpq    *(%rax,%r15,8)
        .size   jt_selector_clamped_depth_limit, .-jt_selector_clamped_depth_limit

        .section .data.rel.ro.jt_selector_occurrence,"aw",@progbits
        .p2align 3
.Lselector_twotable_callback_a:
        .quad   jt_selector_budget_callback_a
        .quad   jt_selector_budget_callback_b
        .quad   jt_selector_budget_callback_a
        .quad   jt_selector_budget_callback_b
.Lselector_twotable_callback_b:
        .quad   jt_selector_budget_callback_b
        .quad   jt_selector_budget_callback_a
        .quad   jt_selector_budget_callback_b
        .quad   jt_selector_budget_callback_a

        .section .data.rel.ro.jt_selector_single_callback_control,"aw",@progbits
        .p2align 3
        .type   .Lselector_single_callback_budget,@object
.Lselector_single_callback_budget:
        .quad   jt_selector_budget_callback_a
        .quad   jt_selector_budget_callback_b
        .size   .Lselector_single_callback_budget, .-.Lselector_single_callback_budget

        .section .rodata.jt_selector_twotable_raw_callback,"a",@progbits
        .p2align 3
        .type   .Lselector_twotable_raw_index,@object
.Lselector_twotable_raw_index:
        .byte   0, 1
        .size   .Lselector_twotable_raw_index, .-.Lselector_twotable_raw_index
        .p2align 3
        .globl  jt_selector_twotable_raw_inner
        .type   jt_selector_twotable_raw_inner,@object
jt_selector_twotable_raw_inner:
        .quad   jt_selector_budget_callback_a - .Lselector_occurrence_text_base
        .quad   jt_selector_budget_callback_b - .Lselector_occurrence_text_base
        .size   jt_selector_twotable_raw_inner, .-jt_selector_twotable_raw_inner

        .section .rodata.jt_selector_clamped_depth,"a",@progbits
        .p2align 3
        .globl  jt_selector_clamped_depth_outer
        .type   jt_selector_clamped_depth_outer,@object
jt_selector_clamped_depth_outer:
        .byte   0, 1
        .size   jt_selector_clamped_depth_outer, .-jt_selector_clamped_depth_outer
        .p2align 3
        .globl  jt_selector_clamped_depth_inner
        .type   jt_selector_clamped_depth_inner,@object
jt_selector_clamped_depth_inner:
        .quad   jt_selector_budget_callback_a
        .quad   jt_selector_budget_callback_b
        .size   jt_selector_clamped_depth_inner, .-jt_selector_clamped_depth_inner

// Both paths compute the same authenticated selected-base+byte-index address,
// but with different ADD occurrences.  A single MedVar from either arm does
// not dominate the join.  Until the public plan grows an EdgeMerged recipe,
// recovery may keep the target CFG but selector lowering must fail closed.
        .text
        .globl  jt_selector_twotable_diamond
        .type   jt_selector_twotable_diamond,@function
jt_selector_twotable_diamond:
        movl    %edi, %r10d
        shll    $3, %r10d
        andl    $24, %r10d
        leaq    .Lselector_twotable_diamond_a(%rip), %r8
        leaq    .Lselector_twotable_diamond_b(%rip), %r9
        testl   %esi, %esi
        cmovneq %r9, %r8
        testl   %edx, %edx
        je      .Lselector_twotable_diamond_right
        leaq    (%r8,%r10), %r11
        jmp     .Lselector_twotable_diamond_join
.Lselector_twotable_diamond_right:
        movq    %r8, %r11
        addq    %r10, %r11
.Lselector_twotable_diamond_join:
        movq    (%r11), %rax
        jmpq    *%rax
.Lselector_twotable_diamond_a0:
        movl    $6500, %eax
        retq
.Lselector_twotable_diamond_a1:
        movl    $6501, %eax
        retq
.Lselector_twotable_diamond_a2:
        movl    $6502, %eax
        retq
.Lselector_twotable_diamond_a3:
        movl    $6503, %eax
        retq
.Lselector_twotable_diamond_b0:
        movl    $6600, %eax
        retq
.Lselector_twotable_diamond_b1:
        movl    $6601, %eax
        retq
.Lselector_twotable_diamond_b2:
        movl    $6602, %eax
        retq
.Lselector_twotable_diamond_b3:
        movl    $6603, %eax
        retq
        .size   jt_selector_twotable_diamond, .-jt_selector_twotable_diamond

        .section .data.rel.ro.jt_selector_occurrence,"aw",@progbits
        .p2align 3
.Lselector_twotable_diamond_a:
        .quad   .Lselector_twotable_diamond_a0
        .quad   .Lselector_twotable_diamond_a1
        .quad   .Lselector_twotable_diamond_a2
        .quad   .Lselector_twotable_diamond_a3
.Lselector_twotable_diamond_b:
        .quad   .Lselector_twotable_diamond_b0
        .quad   .Lselector_twotable_diamond_b1
        .quad   .Lselector_twotable_diamond_b2
        .quad   .Lselector_twotable_diamond_b3

        .section .note.GNU-stack,"",@progbits
