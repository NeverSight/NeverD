// GNU ld moves each function's TOC save into its prologue.
#ifndef TOCSAVE_RELOC
#define TOCSAVE_RELOC R_PPC64_TOCSAVE
#endif
.abiversion 1

.macro begin_function name
.section ".opd","aw",@progbits
.p2align 3
.globl \name
.type \name,@function
\name:
  .quad .Lentry_\name, .TOC.@tocbase, 0
.text
.p2align 2
.Lentry_\name:
  mflr 0
  stdu 1, -112(1)
  std 0, 128(1)
.Lsave_\name:
  .reloc ., TOCSAVE_RELOC, .Lsave_\name
  nop
.endm

.macro plt_call caller, callee
  bl \callee
  .reloc ., TOCSAVE_RELOC, .Lsave_\caller
  nop
.endm

.macro return_with_global
  ld 4, .Lvalue@toc(2)
  add 3, 3, 4
  addi 1, 1, 112
  ld 0, 16(1)
  mtlr 0
  blr
.endm

begin_function cross
  addi 3, 3, 3
  plt_call cross, remote_apply
  return_with_global

begin_function twice
  addi 3, 3, 1
  plt_call twice, remote_apply
  addi 3, 3, 2
  plt_call twice, remote_other
  return_with_global

.data
.p2align 3
.Lvalue:
  .quad 37
