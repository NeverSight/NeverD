// ELF entry with a NOTYPE symbol. The loader has no STT_FUNC to publish, but
// the native analysis pipeline recovers the entry as a LowIR function.
.globl _start
.text
_start:
  xorl %eax, %eax
  testl %eax, %eax
  jne 1f
  movl $1, %eax
1:
  ret
