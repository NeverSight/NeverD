// Branchless XLEN-wide selection, as emitted by Clang for cond ? x : y.
.text
.globl eval
.type eval,@function
eval:
 czero.nez a1,a1,a2
 czero.eqz a0,a0,a2
 or a0,a1,a0
 ret
.size eval,.-eval
