/* Two-level ("index-byte") jump-table dispatch — the classic MSVC sparse-switch
   lowering that C compilers on ELF do not emit, provided here as module-level
   assembly.  A compact byte index table maps the switch variable to a small
   entry index, which then indexes the real address table:

       target = jmptab[idxtab[switchvar]]

   The resolver must dispatch on the *real* switch variable (with one case per
   value, 0..20) rather than on the intermediate table index (which would
   collapse the case set and lose the true labels). */

__asm__(
    ".text\n"
    ".globl twolevel_abs\n"
    ".type twolevel_abs,@function\n"
    ".p2align 4, 0x90\n"
    "twolevel_abs:\n"
    "    cmpl $20, %edi\n"
    "    ja .Ltl_default\n"
    "    movl %edi, %eax\n"
    "    leaq tl_idxtab(%rip), %rcx\n"
    "    movzbl (%rcx,%rax), %eax\n"          /* eax = idxtab[x] (byte index) */
    "    leaq tl_jmptab(%rip), %rcx\n"
    "    movq (%rcx,%rax,8), %rax\n"          /* rax = jmptab[eax] (absolute)  */
    "    jmpq *%rax\n"
    ".Ltl_c0:\n    movl $100, %eax\n    ret\n"
    ".Ltl_c1:\n    movl $200, %eax\n    ret\n"
    ".Ltl_c2:\n    movl $300, %eax\n    ret\n"
    ".Ltl_c3:\n    movl $400, %eax\n    ret\n"
    ".Ltl_c4:\n    movl $500, %eax\n    ret\n"
    ".Ltl_default:\n    movl $-1, %eax\n    ret\n"
    ".size twolevel_abs, .-twolevel_abs\n"

    ".section .rodata,\"a\",@progbits\n"
    ".p2align 4\n"
    "tl_idxtab:\n"
    "    .byte 0,1,2,0,1,3,2,0,1,4,0,1,2,3,4,0,1,2,3,4,0\n"
    ".p2align 4\n"
    "tl_jmptab:\n"
    "    .quad .Ltl_c0, .Ltl_c1, .Ltl_c2, .Ltl_c3, .Ltl_c4\n"
    ".text\n");

void _start(void) {
  __asm__ volatile("syscall" ::"a"(60), "D"(0));
}
