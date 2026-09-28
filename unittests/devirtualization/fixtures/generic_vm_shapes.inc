// Original public fixture scaffolding. The machines use only caller-saved GPRs.
#if defined(_WIN32) || defined(GENERIC_VM_MS_ABI)
#define SHAPE_X %rcx
#define SHAPE_Y %rdx
#define SHAPE_OUT %r8
#else
#define SHAPE_X %rdi
#define SHAPE_Y %rsi
#define SHAPE_OUT %rdx
#endif
#if defined(_WIN32)
#define SHAPE_READONLY .section .rdata,"dr"
#else
#define SHAPE_READONLY .section .rodata,"a",@progbits
#endif

// Private frame: x=0, y=8, accumulator=16, output=24, status=32.
.macro shape_entry name, bytes
.text
.p2align 4
.globl \name
#if defined(__ELF__)
.type \name,@function
#elif defined(_WIN32)
.def \name; .scl 2; .type 32; .endef
#endif
\name:
  subq $\bytes, %rsp
  movq SHAPE_X, 0(%rsp)
  movq SHAPE_Y, 8(%rsp)
  movq SHAPE_OUT, 24(%rsp)
  movq SHAPE_X, %rax
  addq SHAPE_Y, %rax
  movq %rax, 16(%rsp)
  movq $0, 32(%rsp)
  movq %rsp, %r11
.endm

.macro shape_return bytes
  movq 16(%r11), %rax
  movq 24(%r11), %rcx
  movq %rax, 0(%rcx)
  movq 32(%r11), %rdx
  movq %rdx, 8(%rcx)
  addq $\bytes, %rsp
  retq
.endm

// The bytecode contains absolute handler pointers, not opcode-table indices
// or signed offsets. Every handler advances this cursor independently.
.macro shape_thread_next
  movq (%r10), %rax
  addq $8, %r10
  jmpq *%rax
.endm
