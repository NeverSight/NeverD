/* Independent kernels for MIPS source/LLVM roundtrips. Build with
 * --target=mips{,el}-linux-gnu -march=mips32r2 -mabi=32, or
 * --target=mips64{,el}-linux-gnuabi64 -march=mips64r2 -mabi=64 -DWIDE.
 * Use -mno-abicalls -fno-pic; link ET_EXEC at a fixed address with ld.lld.
 */
.set noreorder
.text

.macro field_kernel name, operation, insert, pos, size
.globl \name
.type \name,@function
\name:
.if \insert
  or $2,$5,$0
.endif
  jr $31
  \operation $2,$4,\pos,\size
.size \name,.-\name
.endm

.macro shuffle_kernel name, operation
.globl \name
.type \name,@function
\name:
  jr $31
  \operation $2,$4
.size \name,.-\name
.endm

#define MIPS_FIELD_KERNEL(Name, Op, Bits, Insert, Pos, Size) field_kernel Name,Op,Insert,Pos,Size
#define MIPS_SHUFFLE_KERNEL(Name, Op, Bits, Lane, Permutation) shuffle_kernel Name,Op
#include "MIPSBitfieldKernels.def"

/* Strict word checks must remain conditional on actual slot execution. */
.globl mips_word_likely
.type mips_word_likely,@function
mips_word_likely:
  addiu $2,$0,7
  beql $5,$0,1f
  addiu $2,$4,1
1:
  jr $31
  nop
.size mips_word_likely,.-mips_word_likely

.globl mips_word_zero
.type mips_word_zero,@function
mips_word_zero:
  addiu $0,$4,1
  jr $31
  addiu $2,$0,7
.size mips_word_zero,.-mips_word_zero

.globl mips_add
.type mips_add,@function
mips_add:
  jr $31
#ifdef WIDE
  daddu $2,$4,$5
#else
  addu $2,$4,$5
#endif
.size mips_add,.-mips_add

.globl mips_mul
.type mips_mul,@function
mips_mul:
  jr $31
  mul $2,$4,$5
.size mips_mul,.-mips_mul

/* Dead destinations still perform both original reads and their faults. */
.globl mips_merge_discard
.type mips_merge_discard,@function
mips_merge_discard:
  lwl $8,1($4)
  lwr $8,1($4)
  jr $31
  addiu $2,$0,7
.size mips_merge_discard,.-mips_merge_discard

.globl mips_merge_zero
.type mips_merge_zero,@function
mips_merge_zero:
  lwl $0,1($4)
  lwr $0,1($4)
  jr $31
  addiu $2,$0,7
.size mips_merge_zero,.-mips_merge_zero

/* Explicit arbitrary old bits test preserved lanes without relying on an
 * uninitialized C value. Both instruction orders must discard old high bits. */
.globl mips_merge_word
.type mips_merge_word,@function
mips_merge_word:
  or $2,$5,$0
#ifdef __MIPSEL__
  lwl $2,3($4)
  lwr $2,0($4)
#else
  lwl $2,0($4)
  lwr $2,3($4)
#endif
  jr $31
  nop
.size mips_merge_word,.-mips_merge_word

.globl mips_merge_word_reverse
.type mips_merge_word_reverse,@function
mips_merge_word_reverse:
  or $2,$5,$0
#ifdef __MIPSEL__
  lwr $2,0($4)
  lwl $2,3($4)
#else
  lwr $2,3($4)
  lwl $2,0($4)
#endif
  jr $31
  nop
.size mips_merge_word_reverse,.-mips_merge_word_reverse

.globl mips_merge_full
.type mips_merge_full,@function
mips_merge_full:
  or $2,$5,$0
#ifdef WIDE
#ifdef __MIPSEL__
  ldl $2,7($4)
  ldr $2,0($4)
#else
  ldl $2,0($4)
  ldr $2,7($4)
#endif
#else
#ifdef __MIPSEL__
  lwl $2,3($4)
  lwr $2,0($4)
#else
  lwl $2,0($4)
  lwr $2,3($4)
#endif
#endif
  jr $31
  nop
.size mips_merge_full,.-mips_merge_full

/* These destinations deliberately have no incoming source argument. */
.macro complete_merge name, wide, reverse
.globl \name
.type \name,@function
\name:
.if \wide
.if \reverse
#ifdef __MIPSEL__
  ldr $2,0($4)
  ldl $2,7($4)
#else
  ldr $2,7($4)
  ldl $2,0($4)
#endif
.else
#ifdef __MIPSEL__
  ldl $2,7($4)
  ldr $2,0($4)
#else
  ldl $2,0($4)
  ldr $2,7($4)
#endif
.endif
.else
.if \reverse
#ifdef __MIPSEL__
  lwr $2,0($4)
  lwl $2,3($4)
#else
  lwr $2,3($4)
  lwl $2,0($4)
#endif
.else
#ifdef __MIPSEL__
  lwl $2,3($4)
  lwr $2,0($4)
#else
  lwl $2,0($4)
  lwr $2,3($4)
#endif
.endif
.endif
  jr $31
  nop
.size \name,.-\name
.endm

complete_merge mips_uninitialized_word,0,0
complete_merge mips_uninitialized_word_reverse,0,1
#ifdef WIDE
complete_merge mips_uninitialized_full,1,0
complete_merge mips_uninitialized_full_reverse,1,1
#else
complete_merge mips_uninitialized_full,0,0
complete_merge mips_uninitialized_full_reverse,0,1
#endif

.globl mips_merge_ro_global
.type mips_merge_ro_global,@function
mips_merge_ro_global:
  lui $4,%hi(merge_ro_bytes+1)
  addiu $4,$4,%lo(merge_ro_bytes+1)
  or $2,$0,$0
#ifdef __MIPSEL__
  lwl $2,3($4)
  lwr $2,0($4)
#else
  lwl $2,0($4)
  lwr $2,3($4)
#endif
  jr $31
  nop
.size mips_merge_ro_global,.-mips_merge_ro_global

.globl mips_merge_rw_global
.type mips_merge_rw_global,@function
mips_merge_rw_global:
  lui $4,%hi(merge_rw_bytes+1)
  addiu $4,$4,%lo(merge_rw_bytes+1)
  or $2,$0,$0
#ifdef __MIPSEL__
  lwl $2,3($4)
  lwr $2,0($4)
#else
  lwl $2,0($4)
  lwr $2,3($4)
#endif
  jr $31
  nop
.size mips_merge_rw_global,.-mips_merge_rw_global

.section .rodata,"a",@progbits
.p2align 3
.type merge_ro_bytes,@object
merge_ro_bytes:
  .byte 0x83,0x91,0xa2,0xb3,0xc4,0x15,0x26,0x37
.size merge_ro_bytes,.-merge_ro_bytes
.data
.p2align 3
.type merge_rw_bytes,@object
merge_rw_bytes:
  .byte 0x83,0x91,0xa2,0xb3,0xc4,0x15,0x26,0x37
.size merge_rw_bytes,.-merge_rw_bytes
.text

.globl mips_likely
.type mips_likely,@function
mips_likely:
  beql $4,$0,1f
  addiu $5,$5,3
  jr $31
  or $2,$5,$0
1:
  jr $31
  or $2,$5,$0
.size mips_likely,.-mips_likely

.globl mips_likely_load
.type mips_likely_load,@function
mips_likely_load:
  addiu $2,$0,3
  beql $5,$0,1f
  lw $2,0($4)
1:
  jr $31
  nop
.size mips_likely_load,.-mips_likely_load

.globl mips_load
.type mips_load,@function
mips_load:
  jr $31
  lw $2,0($4)
.size mips_load,.-mips_load

.globl mips_store
.type mips_store,@function
mips_store:
  sw $5,0($4)
  jr $31
  lw $2,0($4)
.size mips_store,.-mips_store

.globl mips_stack
.type mips_stack,@function
mips_stack:
#ifdef WIDE
  daddu $2,$4,$5
  daddu $2,$2,$6
  daddu $2,$2,$7
  daddu $2,$2,$8
  daddu $2,$2,$9
  daddu $2,$2,$10
  daddu $2,$2,$11
  ld $12,0($29)
  jr $31
  daddu $2,$2,$12
#else
  addu $2,$4,$5
  addu $2,$2,$6
  addu $2,$2,$7
  lw $8,16($29)
  jr $31
  addu $2,$2,$8
#endif
.size mips_stack,.-mips_stack

/* The condition is captured before its delay slot overwrites a0. */
.globl mips_branch_capture
.type mips_branch_capture,@function
mips_branch_capture:
  beq $4,$0,1f
  addiu $4,$0,99
  jr $31
  addiu $2,$0,7
1:
  jr $31
  addiu $2,$0,11
.size mips_branch_capture,.-mips_branch_capture

/* A likely slot must leave memory untouched on the false path. */
.globl mips_likely_store
.type mips_likely_store,@function
mips_likely_store:
  beql $5,$0,1f
  sw $6,0($4)
1:
  jr $31
  lw $2,0($4)
.size mips_likely_store,.-mips_likely_store

/* Save/restore the caller link; the call slot creates the callee argument. */
.globl mips_call_slot
.type mips_call_slot,@function
mips_call_slot:
#ifdef WIDE
  daddiu $29,$29,-32
  sd $31,24($29)
#else
  addiu $29,$29,-24
  sw $31,20($29)
#endif
  jal mips_add
  addiu $5,$0,13
#ifdef WIDE
  ld $31,24($29)
  jr $31
  daddiu $29,$29,32
#else
  lw $31,20($29)
  jr $31
  addiu $29,$29,24
#endif
.size mips_call_slot,.-mips_call_slot

.globl mips_ro_global
.type mips_ro_global,@function
mips_ro_global:
  lui $4,%hi(ro_value)
  jr $31
  lw $2,%lo(ro_value)($4)
.size mips_ro_global,.-mips_ro_global

.globl mips_rw_global
.type mips_rw_global,@function
mips_rw_global:
  lui $4,%hi(rw_value)
  lw $2,%lo(rw_value)($4)
  jr $31
  sw $5,%lo(rw_value)($4)
.size mips_rw_global,.-mips_rw_global

.section .rodata,"a",@progbits
.p2align 2
.type ro_value,@object
ro_value: .word 0x12345678
.size ro_value,4
.data
.p2align 2
.globl rw_value
.type rw_value,@object
rw_value: .word 0x12345678
.size rw_value,4

.text
.globl mips_jump_slot_load
.type mips_jump_slot_load,@function
mips_jump_slot_load:
#ifdef WIDE
  daddiu $29,$29,-32
  sd $31,24($29)
#else
  addiu $29,$29,-24
  sw $31,20($29)
#endif
  beq $5,$0,1f
  nop
  addiu $5,$5,1
1:
  jal mips_add
  lw $4,0($4)
#ifdef WIDE
  ld $31,24($29)
  jr $31
  daddiu $29,$29,32
#else
  lw $31,20($29)
  jr $31
  addiu $29,$29,24
#endif
.size mips_jump_slot_load,.-mips_jump_slot_load

.globl mips_jalr_slot
.type mips_jalr_slot,@function
mips_jalr_slot:
#ifdef WIDE
  daddiu $29,$29,-32
  sd $31,24($29)
#else
  addiu $29,$29,-24
  sw $31,20($29)
#endif
  lui $25,%hi(mips_add)
  ori $25,$25,%lo(mips_add)
  jalr $31,$25
  addiu $25,$0,0
#ifdef WIDE
  ld $31,24($29)
  jr $31
  daddiu $29,$29,32
#else
  lw $31,20($29)
  jr $31
  addiu $29,$29,24
#endif
.size mips_jalr_slot,.-mips_jalr_slot

.globl mips_jalr_memory
.type mips_jalr_memory,@function
mips_jalr_memory:
#ifdef WIDE
  daddiu $29,$29,-32
  sd $31,24($29)
#else
  addiu $29,$29,-24
  sw $31,20($29)
#endif
  lui $25,%hi(mips_add)
  ori $25,$25,%lo(mips_add)
  jalr $31,$25
  sw $4,0($29)
#ifdef WIDE
  ld $31,24($29)
  jr $31
  daddiu $29,$29,32
#else
  lw $31,20($29)
  jr $31
  addiu $29,$29,24
#endif
.size mips_jalr_memory,.-mips_jalr_memory

/* The slot reads memory and overwrites t9 after JALR captured the callee.
 * Register arguments prepared before this read must still reach mips_add. */
.globl mips_jalr_merge_slot
.type mips_jalr_merge_slot,@function
mips_jalr_merge_slot:
#ifdef WIDE
  daddiu $29,$29,-32
  sd $31,24($29)
#else
  addiu $29,$29,-24
  sw $31,20($29)
#endif
  lui $25,%hi(mips_add)
  ori $25,$25,%lo(mips_add)
  jalr $31,$25
  lwl $25,1($6)
#ifdef WIDE
  ld $31,24($29)
  jr $31
  daddiu $29,$29,32
#else
  lw $31,20($29)
  jr $31
  addiu $29,$29,24
#endif
.size mips_jalr_merge_slot,.-mips_jalr_merge_slot

.globl mips_return13
.type mips_return13,@function
mips_return13:
  jr $31
  addiu $2,$0,13
.size mips_return13,.-mips_return13

.globl mips_conditional_link
.type mips_conditional_link,@function
mips_conditional_link:
#ifdef WIDE
  daddiu $29,$29,-32
  sd $31,24($29)
#else
  addiu $29,$29,-24
  sw $31,20($29)
#endif
  addiu $2,$0,3
  bgezal $4,mips_return13
  addiu $2,$2,2
#ifdef WIDE
  ld $31,24($29)
  jr $31
  daddiu $29,$29,32
#else
  lw $31,20($29)
  jr $31
  addiu $29,$29,24
#endif
.size mips_conditional_link,.-mips_conditional_link

/* Read a target-sized pointer stored in the image, then its scalar object. */
.globl mips_data_pointer
.type mips_data_pointer,@function
mips_data_pointer:
  lui $4,%hi(data_pointer)
#ifdef WIDE
  ld $4,%lo(data_pointer)($4)
#else
  lw $4,%lo(data_pointer)($4)
#endif
  jr $31
  lw $2,0($4)
.size mips_data_pointer,.-mips_data_pointer

.globl mips_code_pointer
.type mips_code_pointer,@function
mips_code_pointer:
#ifdef WIDE
  daddiu $29,$29,-32
  sd $31,24($29)
#else
  addiu $29,$29,-24
  sw $31,20($29)
#endif
  lui $25,%hi(code_pointer)
#ifdef WIDE
  ld $25,%lo(code_pointer)($25)
#else
  lw $25,%lo(code_pointer)($25)
#endif
  jalr $31,$25
  addiu $25,$0,0
#ifdef WIDE
  ld $31,24($29)
  jr $31
  daddiu $29,$29,32
#else
  lw $31,20($29)
  jr $31
  addiu $29,$29,24
#endif
.size mips_code_pointer,.-mips_code_pointer

.section .rodata,"a",@progbits
.p2align 3
.type data_pointer,@object
data_pointer:
#ifdef WIDE
  .dword rw_value
#else
  .word rw_value
#endif
.size data_pointer,.-data_pointer
.p2align 3
.type code_pointer,@object
code_pointer:
#ifdef WIDE
  .dword mips_add
#else
  .word mips_add
#endif
.size code_pointer,.-code_pointer

/* Real two-input encodings avoid assembler-inserted checks and traps. */
.text
.p2align 2
.macro division name,encoding,result
.globl \name
.type \name,@function
\name:
  .word \encoding
  jr $31
  \result $2
.size \name,.-\name
.endm
division mips_div_lo,0x0085001a,mflo
division mips_div_hi,0x0085001a,mfhi
division mips_divu_lo,0x0085001b,mflo
division mips_divu_hi,0x0085001b,mfhi
#ifdef WIDE
division mips_ddiv_lo,0x0085001e,mflo
division mips_ddiv_hi,0x0085001e,mfhi
division mips_ddivu_lo,0x0085001f,mflo
division mips_ddivu_hi,0x0085001f,mfhi
#endif

.globl mips_div_dead
.type mips_div_dead,@function
mips_div_dead:
  .word 0x0085001a
  jr $31
  addiu $2,$0,7
.size mips_div_dead,.-mips_div_dead

/* Only the low byte/halfword of the incoming value is consumed. */
.macro narrow_store name,store,load
.globl \name
.type \name,@function
\name:
  \store $5,0($4)
  jr $31
  \load $2,0($4)
.size \name,.-\name
.endm
narrow_store mips_store_byte,sb,lbu
narrow_store mips_store_half,sh,lhu

#include "merge-store-word.S"
