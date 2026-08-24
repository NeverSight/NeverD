.text
.p2align 2
.globl jt_i386_gotpc_text_base
jt_i386_gotpc_text_base:
.Lgotpc_text_base:

// A real i386 PIC get-PC seed.  The relocation addend is expressed so the
// loader's model-zero convention expects the exact address pushed by the
// call-next instruction.
.globl jt_i386_gotpc_call_pop_seed
.type jt_i386_gotpc_call_pop_seed, @function
jt_i386_gotpc_call_pop_seed:
  call .Lgotpc_real_pc
.Lgotpc_real_pc:
  popl %esi
  .byte 0x81, 0xc6                 // addl imm32, %esi
.Lgotpc_real_field:
  .long .Lgotpc_real_field - .Lgotpc_real_pc
  .reloc .Lgotpc_real_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl %esi, %eax
  ret
.size jt_i386_gotpc_call_pop_seed, .-jt_i386_gotpc_call_pop_seed

// The bytes are a syntactically valid call-next/pop sequence, but the pop is
// also an address-taken interior entry.  Entering at that label does not run
// the call and therefore pops an arbitrary stack value; it must not publish a
// scalar GOT-base model for the whole function.
.p2align 2
.globl jt_i386_gotpc_rooted_pop
.type jt_i386_gotpc_rooted_pop, @function
jt_i386_gotpc_rooted_pop:
  call .Lgotpc_rooted_pop
.Lgotpc_rooted_pop:
  popl %esi
  .byte 0x81, 0xc6                 // addl imm32, %esi
.Lgotpc_rooted_field:
  .long .Lgotpc_rooted_field - .Lgotpc_rooted_pop
  .reloc .Lgotpc_rooted_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl %esi, %eax
  ret
.size jt_i386_gotpc_rooted_pop, .-jt_i386_gotpc_rooted_pop

// The same numeric seed materialized by a relocation-free absolute LEA is not
// a get-PC occurrence.  It remains fixed when the object is linked elsewhere,
// so pairing it with R_386_GOTPC must not authenticate the model-zero result.
.p2align 2
.globl jt_i386_gotpc_absolute_seed
.type jt_i386_gotpc_absolute_seed, @function
jt_i386_gotpc_absolute_seed:
  leal .Lgotpc_fake_pc - .Lgotpc_text_base, %esi
.Lgotpc_fake_pc:
  .byte 0x81, 0xc6                 // addl imm32, %esi
.Lgotpc_fake_field:
  .long .Lgotpc_fake_field - .Lgotpc_fake_pc
  .reloc .Lgotpc_fake_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl %esi, %eax
  ret
.size jt_i386_gotpc_absolute_seed, .-jt_i386_gotpc_absolute_seed

// A guard-bounded i386 PIC switch.  The table displacement is an exact
// R_386_GOTOFF occurrence and the base is established by the same authenticated
// call/pop + R_386_GOTPC sequence exercised above.
.p2align 2
.globl jt_i386_gotoff_switch_call_pop
.type jt_i386_gotoff_switch_call_pop, @function
jt_i386_gotoff_switch_call_pop:
  call .Lgotoff_switch_real_pc
.Lgotoff_switch_real_pc:
  popl %ebx
  .byte 0x81, 0xc3                 // addl imm32, %ebx
.Lgotoff_switch_real_field:
  .long .Lgotoff_switch_real_field - .Lgotoff_switch_real_pc
  .reloc .Lgotoff_switch_real_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl 4(%esp), %ecx
  cmpl $4, %ecx
  ja .Lgotoff_switch_default
  movl .Lgotoff_switch_table@GOTOFF(%ebx,%ecx,4), %eax
  addl %ebx, %eax
.globl jt_i386_gotoff_switch_call_pop_branch
jt_i386_gotoff_switch_call_pop_branch:
  jmp *%eax
.Lgotoff_switch_case0:
  movl $5100, %eax
  ret
.Lgotoff_switch_case1:
  movl $5101, %eax
  ret
.Lgotoff_switch_case2:
  movl $5102, %eax
  ret
.Lgotoff_switch_case3:
  movl $5103, %eax
  ret
.Lgotoff_switch_case4:
  movl $5104, %eax
  ret
.Lgotoff_switch_default:
  movl $-1, %eax
  ret
.size jt_i386_gotoff_switch_call_pop, .-jt_i386_gotoff_switch_call_pop

// Two value-writing relocations own the same encoded immediate.  A real ELF
// linker evaluates each REL relocation from the original in-place addend; it
// does not feed the R_386_32 result into the following R_386_GOTPC.  The
// global text symbol must remain at a non-zero offset from .Lgotpc_text_base
// so the two interpretations disagree by a visible K.
.p2align 2
.globl jt_i386_gotpc_conflict_bias
.type jt_i386_gotpc_conflict_bias, @object
jt_i386_gotpc_conflict_bias:
  .long 0
.size jt_i386_gotpc_conflict_bias, .-jt_i386_gotpc_conflict_bias

.p2align 2
.globl jt_i386_gotoff_same_field_conflict
.type jt_i386_gotoff_same_field_conflict, @function
jt_i386_gotoff_same_field_conflict:
  movl 4(%esp), %edx
  testl %edx, %edx
  je .Lgotoff_conflict_dispatch
  cmpl $1, %edx
  je .Lgotoff_conflict_callback
  jmp .Lgotoff_conflict_valid

.Lgotoff_conflict_dispatch:
  call .Lgotoff_conflict_pc
.globl jt_i386_gotoff_same_field_conflict_pc
jt_i386_gotoff_same_field_conflict_pc:
.Lgotoff_conflict_pc:
  popl %ebx
  .byte 0x81, 0xc3                 // addl imm32, %ebx
.globl jt_i386_gotoff_same_field_conflict_field
jt_i386_gotoff_same_field_conflict_field:
.Lgotoff_conflict_field:
  .long .Lgotoff_conflict_field - .Lgotoff_conflict_pc - (jt_i386_gotpc_conflict_bias - .Lgotpc_text_base)
  .reloc .Lgotoff_conflict_field, R_386_32, jt_i386_gotpc_conflict_bias
  .reloc .Lgotoff_conflict_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  cmpl $0, 16(%esp)
  je .Lgotoff_conflict_keep_ambiguous_base
.globl jt_i386_gotoff_same_field_conflict_zero_base
jt_i386_gotoff_same_field_conflict_zero_base:
  xorl %ebx, %ebx
.Lgotoff_conflict_keep_ambiguous_base:
  movl 8(%esp), %ecx
  cmpl $4, %ecx
  ja .Lgotoff_conflict_default
  movl .Lgotoff_conflict_table@GOTOFF(%ebx,%ecx,4), %eax
  addl %ebx, %eax
.globl jt_i386_gotoff_same_field_conflict_ambiguous_branch
jt_i386_gotoff_same_field_conflict_ambiguous_branch:
  jmp *%eax
.Lgotoff_conflict_case0:
  movl $5150, %eax
  ret
.Lgotoff_conflict_case1:
  movl $5151, %eax
  ret
.Lgotoff_conflict_case2:
  movl $5152, %eax
  ret
.Lgotoff_conflict_case3:
  movl $5153, %eax
  ret
.Lgotoff_conflict_case4:
  movl $5154, %eax
  ret
.Lgotoff_conflict_default:
  movl $-1, %eax
  ret

.Lgotoff_conflict_callback:
  movl 12(%esp), %eax
.globl jt_i386_gotoff_same_field_conflict_callback_branch
jt_i386_gotoff_same_field_conflict_callback_branch:
  jmp *%eax

.Lgotoff_conflict_valid:
  call .Lgotoff_conflict_valid_pc
.Lgotoff_conflict_valid_pc:
  popl %ebx
  .byte 0x81, 0xc3                 // addl imm32, %ebx
.Lgotoff_conflict_valid_field:
  .long .Lgotoff_conflict_valid_field - .Lgotoff_conflict_valid_pc
  .reloc .Lgotoff_conflict_valid_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl 8(%esp), %ecx
  cmpl $1, %ecx
  ja .Lgotoff_conflict_valid_default
  movl .Lgotoff_conflict_valid_table@GOTOFF(%ebx,%ecx,4), %eax
  addl %ebx, %eax
.globl jt_i386_gotoff_same_field_conflict_valid_branch
jt_i386_gotoff_same_field_conflict_valid_branch:
  jmp *%eax
.Lgotoff_conflict_valid_case0:
  movl $5170, %eax
  ret
.Lgotoff_conflict_valid_case1:
  movl $5171, %eax
  ret
.Lgotoff_conflict_valid_default:
  movl $-1, %eax
  ret
.size jt_i386_gotoff_same_field_conflict, .-jt_i386_gotoff_same_field_conflict

// The GOT base itself is authenticated by one ordinary call/pop + GOTPC
// field, but the table displacement has two value-writing relocation owners.
// Both writers happen to leave the same model-zero bytes in this fixture;
// numeric equality is still not occurrence provenance.  The affected dispatch
// must remain an opaque branch while the independent callback below remains a
// tail call.
.p2align 2
.globl jt_i386_gotoff_displacement_conflict
.type jt_i386_gotoff_displacement_conflict, @function
jt_i386_gotoff_displacement_conflict:
  movl 4(%esp), %edx
  testl %edx, %edx
  jne .Lgotoff_displacement_conflict_callback

  call .Lgotoff_displacement_conflict_pc
.Lgotoff_displacement_conflict_pc:
  popl %ebx
  .byte 0x81, 0xc3                 // addl imm32, %ebx
.Lgotoff_displacement_conflict_gotpc_field:
  .long .Lgotoff_displacement_conflict_gotpc_field - .Lgotoff_displacement_conflict_pc
  .reloc .Lgotoff_displacement_conflict_gotpc_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl 8(%esp), %ecx
  cmpl $1, %ecx
  ja .Lgotoff_displacement_conflict_default
  .byte 0x8b, 0x84, 0x8b          // movl disp32(%ebx,%ecx,4), %eax
.globl jt_i386_gotoff_displacement_conflict_field
jt_i386_gotoff_displacement_conflict_field:
  .long 0
  .reloc jt_i386_gotoff_displacement_conflict_field, R_386_GOTOFF, jt_i386_gotoff_displacement_conflict_table
  .reloc jt_i386_gotoff_displacement_conflict_field, R_386_32, jt_i386_gotoff_displacement_conflict_table
  addl %ebx, %eax
.globl jt_i386_gotoff_displacement_conflict_branch
jt_i386_gotoff_displacement_conflict_branch:
  jmp *%eax
.Lgotoff_displacement_conflict_case0:
  movl $5160, %eax
  ret
.Lgotoff_displacement_conflict_case1:
  movl $5161, %eax
  ret
.Lgotoff_displacement_conflict_default:
  movl $-1, %eax
  ret

.Lgotoff_displacement_conflict_callback:
  movl 12(%esp), %eax
.globl jt_i386_gotoff_displacement_conflict_callback_branch
jt_i386_gotoff_displacement_conflict_callback_branch:
  jmp *%eax
.size jt_i386_gotoff_displacement_conflict, .-jt_i386_gotoff_displacement_conflict

// The first immutable graph proves the ambiguous GOTPC path below.  Its
// independent valid table then discovers a late case edge that enters the
// ambiguous branch after the table-load slice, so the stable graph cannot
// replay exactI386ModelZeroReaches.  The earlier proof must remain only as a
// fail-closed pending identity; global fixed-point stability is not a negative
// replay certificate.
.p2align 2
.globl jt_i386_gotoff_ambiguous_late_shape_loss
.type jt_i386_gotoff_ambiguous_late_shape_loss, @function
jt_i386_gotoff_ambiguous_late_shape_loss:
  movl 4(%esp), %edx
  testl %edx, %edx
  jne .Lgotoff_late_valid

  call .Lgotoff_late_pc
.Lgotoff_late_pc:
  popl %ebx
  .byte 0x81, 0xc3                 // addl imm32, %ebx
.Lgotoff_late_field:
  .long .Lgotoff_late_field - .Lgotoff_late_pc - (jt_i386_gotpc_conflict_bias - .Lgotpc_text_base)
  .reloc .Lgotoff_late_field, R_386_32, jt_i386_gotpc_conflict_bias
  .reloc .Lgotoff_late_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl 8(%esp), %ecx
  cmpl $1, %ecx
  ja .Lgotoff_late_default
  movl .Lgotoff_late_table@GOTOFF(%ebx,%ecx,4), %eax
  addl %ebx, %eax
.globl jt_i386_gotoff_ambiguous_late_shape_loss_branch
jt_i386_gotoff_ambiguous_late_shape_loss_branch:
  jmp *%eax
.Lgotoff_late_case0:
  movl $5180, %eax
  ret
.Lgotoff_late_case1:
  movl $5181, %eax
  ret
.Lgotoff_late_default:
  movl $-1, %eax
  ret

.Lgotoff_late_valid:
  call .Lgotoff_late_valid_pc
.Lgotoff_late_valid_pc:
  popl %ebx
  .byte 0x81, 0xc3                 // addl imm32, %ebx
.globl jt_i386_gotoff_ambiguous_late_shape_loss_valid_gotpc_field
jt_i386_gotoff_ambiguous_late_shape_loss_valid_gotpc_field:
.Lgotoff_late_valid_gotpc_field:
  .long .Lgotoff_late_valid_gotpc_field - .Lgotoff_late_valid_pc
  .reloc .Lgotoff_late_valid_gotpc_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl 8(%esp), %ecx
  cmpl $1, %ecx
  ja .Lgotoff_late_valid_default
  movl jt_i386_gotoff_late_valid_table@GOTOFF(%ebx,%ecx,4), %eax
  addl %ebx, %eax
.globl jt_i386_gotoff_ambiguous_late_shape_loss_valid_branch
jt_i386_gotoff_ambiguous_late_shape_loss_valid_branch:
  jmp *%eax
.Lgotoff_late_valid_case0:
  movl $5190, %eax
  ret
.Lgotoff_late_valid_case1:
.globl jt_i386_gotoff_ambiguous_late_shape_loss_edge
jt_i386_gotoff_ambiguous_late_shape_loss_edge:
  xorl %eax, %eax
  jmp jt_i386_gotoff_ambiguous_late_shape_loss_branch
.Lgotoff_late_valid_default:
  movl $-1, %eax
  ret
.size jt_i386_gotoff_ambiguous_late_shape_loss, .-jt_i386_gotoff_ambiguous_late_shape_loss

// The same switch spelling with the same numeric GOTPC input, but no
// lifter-authenticated call/pop seed.  The GOTOFF field and readable table do
// not authorize this base on their own.
.p2align 2
.globl jt_i386_gotoff_switch_absolute_seed
.type jt_i386_gotoff_switch_absolute_seed, @function
jt_i386_gotoff_switch_absolute_seed:
  leal .Lgotoff_switch_fake_pc - .Lgotpc_text_base, %ebx
.Lgotoff_switch_fake_pc:
  .byte 0x81, 0xc3                 // addl imm32, %ebx
.Lgotoff_switch_fake_field:
  .long .Lgotoff_switch_fake_field - .Lgotoff_switch_fake_pc
  .reloc .Lgotoff_switch_fake_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl 4(%esp), %ecx
  cmpl $4, %ecx
  ja .Lgotoff_switch_fake_default
  movl .Lgotoff_switch_fake_table@GOTOFF(%ebx,%ecx,4), %eax
  addl %ebx, %eax
  jmp *%eax
.Lgotoff_switch_fake_case0:
  movl $5200, %eax
  ret
.Lgotoff_switch_fake_case1:
  movl $5201, %eax
  ret
.Lgotoff_switch_fake_case2:
  movl $5202, %eax
  ret
.Lgotoff_switch_fake_case3:
  movl $5203, %eax
  ret
.Lgotoff_switch_fake_case4:
  movl $5204, %eax
  ret
.Lgotoff_switch_fake_default:
  movl $-1, %eax
  ret
.size jt_i386_gotoff_switch_absolute_seed, .-jt_i386_gotoff_switch_absolute_seed

// The GOTPC relocation belongs to the instruction's immediate field.  The
// displacement has the same post-relocation scalar value, but must not borrow
// that field and publish its effective-address ADD as a GOT model producer.
.p2align 2
.globl jt_i386_gotpc_operand_collision
.type jt_i386_gotpc_operand_collision, @function
jt_i386_gotpc_operand_collision:
  call .Lgotpc_collision_pc
.Lgotpc_collision_pc:
  popl %esi
  .byte 0xc7, 0x86                 // movl imm32, disp32(%esi)
.Lgotpc_collision_disp:
  .long .Lgotpc_text_base - .Lgotpc_collision_pc
.Lgotpc_collision_field:
  .long .Lgotpc_collision_field - .Lgotpc_collision_pc
  .reloc .Lgotpc_collision_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  ret
.size jt_i386_gotpc_operand_collision, .-jt_i386_gotpc_operand_collision

// R_386_32 and R_386_GOTOFF both yield a complete DataAddress operand in an
// ET_REL image, but only GOTOFF is relative to the authenticated GOT base.
// Feeding an absolute table address through the PIC base must therefore stay
// an unsafe indirect branch.
.p2align 2
.globl jt_i386_abs32_displacement_switch
.type jt_i386_abs32_displacement_switch, @function
jt_i386_abs32_displacement_switch:
  call .Lgotpc_abs32_pc
.Lgotpc_abs32_pc:
  popl %ebx
  .byte 0x81, 0xc3
.Lgotpc_abs32_field:
  .long .Lgotpc_abs32_field - .Lgotpc_abs32_pc
  .reloc .Lgotpc_abs32_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl 4(%esp), %ecx
  cmpl $4, %ecx
  ja .Labs32_default
  .byte 0x8b, 0x84, 0x8b          // movl disp32(%ebx,%ecx,4), %eax
.Labs32_table_field:
  .long 0
  .reloc .Labs32_table_field, R_386_32, jt_i386_abs32_switch_table
  addl %ebx, %eax
  jmp *%eax
.Labs32_case0:
  movl $5300, %eax
  ret
.Labs32_case1:
  movl $5301, %eax
  ret
.Labs32_case2:
  movl $5302, %eax
  ret
.Labs32_case3:
  movl $5303, %eax
  ret
.Labs32_case4:
  movl $5304, %eax
  ret
.Labs32_default:
  movl $-1, %eax
  ret
.size jt_i386_abs32_displacement_switch, .-jt_i386_abs32_displacement_switch

// A real GOTPC producer elsewhere in the function does not authenticate a
// different register that independently computes the same scalar zero.  This
// must remain unauthenticated even if a value folder reduces 1 + -1 to zero.
.p2align 2
.globl jt_i386_gotoff_literal_zero_base
.type jt_i386_gotoff_literal_zero_base, @function
jt_i386_gotoff_literal_zero_base:
  call .Lgotpc_literal_pc
.Lgotpc_literal_pc:
  popl %esi
  .byte 0x81, 0xc6
.Lgotpc_literal_field:
  .long .Lgotpc_literal_field - .Lgotpc_literal_pc
  .reloc .Lgotpc_literal_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl $1, %ebx
  addl $-1, %ebx
  movl 4(%esp), %ecx
  cmpl $4, %ecx
  ja .Lliteral_default
  movl .Lliteral_table@GOTOFF(%ebx,%ecx,4), %eax
  addl %ebx, %eax
  jmp *%eax
.Lliteral_case0:
  movl $5400, %eax
  ret
.Lliteral_case1:
  movl $5401, %eax
  ret
.Lliteral_case2:
  movl $5402, %eax
  ret
.Lliteral_case3:
  movl $5403, %eax
  ret
.Lliteral_case4:
  movl $5404, %eax
  ret
.Lliteral_default:
  movl $-1, %eax
  ret
.size jt_i386_gotoff_literal_zero_base, .-jt_i386_gotoff_literal_zero_base

// One predecessor carries the authenticated model and the other carries an
// unauthenticated literal zero.  Must-origin proof rejects the mixed merge.
.p2align 2
.globl jt_i386_gotoff_mixed_zero_base
.type jt_i386_gotoff_mixed_zero_base, @function
jt_i386_gotoff_mixed_zero_base:
  call .Lgotpc_mixed_pc
.Lgotpc_mixed_pc:
  popl %esi
  .byte 0x81, 0xc6
.Lgotpc_mixed_field:
  .long .Lgotpc_mixed_field - .Lgotpc_mixed_pc
  .reloc .Lgotpc_mixed_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl 4(%esp), %ecx
  testl %ecx, %ecx
  jz .Lmixed_literal
  movl %esi, %ebx
  jmp .Lmixed_join
.Lmixed_literal:
  movl $0, %ebx
.Lmixed_join:
  andl $7, %ecx
  cmpl $4, %ecx
  ja .Lmixed_default
  movl .Lmixed_table@GOTOFF(%ebx,%ecx,4), %eax
  addl %ebx, %eax
  jmp *%eax
.Lmixed_case0:
  movl $5500, %eax
  ret
.Lmixed_case1:
  movl $5501, %eax
  ret
.Lmixed_case2:
  movl $5502, %eax
  ret
.Lmixed_case3:
  movl $5503, %eax
  ret
.Lmixed_case4:
  movl $5504, %eax
  ret
.Lmixed_default:
  movl $-1, %eax
  ret
.size jt_i386_gotoff_mixed_zero_base, .-jt_i386_gotoff_mixed_zero_base

// Size-optimized computed-goto form: the entry scale is already present in
// the masked byte offset, so the effective-address scale is one.  Recovery
// must still bind the displacement to this instruction's exact GOTOFF field
// and prove that this exact base input reaches the authenticated GOT-base-zero
// model.
.p2align 2
.globl jt_i386_gotoff_prescaled_call_pop
.type jt_i386_gotoff_prescaled_call_pop, @function
jt_i386_gotoff_prescaled_call_pop:
  call .Lprescaled_real_pc
.Lprescaled_real_pc:
  popl %ebx
  .byte 0x81, 0xc3
.Lprescaled_real_gotpc:
  .long .Lprescaled_real_gotpc - .Lprescaled_real_pc
  .reloc .Lprescaled_real_gotpc, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl 4(%esp), %ecx
  andl $0xc, %ecx
  movl .Lprescaled_real_table@GOTOFF(%ebx,%ecx), %eax
  addl %ebx, %eax
  jmp *%eax
.Lprescaled_real_case0:
  movl $5600, %eax
  ret
.Lprescaled_real_case1:
  movl $5601, %eax
  ret
.Lprescaled_real_case2:
  movl $5602, %eax
  ret
.Lprescaled_real_case3:
  movl $5603, %eax
  ret
.size jt_i386_gotoff_prescaled_call_pop, .-jt_i386_gotoff_prescaled_call_pop

// Numerically identical address arithmetic, but the displacement is an
// absolute R_386_32 field.  A relocation run at the resulting address does not
// grant GOTOFF semantics to this operand.
.p2align 2
.globl jt_i386_abs32_prescaled_switch
.type jt_i386_abs32_prescaled_switch, @function
jt_i386_abs32_prescaled_switch:
  call .Lprescaled_abs32_pc
.Lprescaled_abs32_pc:
  popl %ebx
  .byte 0x81, 0xc3
.Lprescaled_abs32_gotpc:
  .long .Lprescaled_abs32_gotpc - .Lprescaled_abs32_pc
  .reloc .Lprescaled_abs32_gotpc, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl 4(%esp), %ecx
  andl $0xc, %ecx
  .byte 0x8b, 0x84, 0x0b          // movl disp32(%ebx,%ecx), %eax
.Lprescaled_abs32_table_field:
  .long 0
  .reloc .Lprescaled_abs32_table_field, R_386_32, jt_i386_abs32_prescaled_table
  addl %ebx, %eax
  jmp *%eax
.Lprescaled_abs32_case0:
  movl $5700, %eax
  ret
.Lprescaled_abs32_case1:
  movl $5701, %eax
  ret
.Lprescaled_abs32_case2:
  movl $5702, %eax
  ret
.Lprescaled_abs32_case3:
  movl $5703, %eax
  ret
.size jt_i386_abs32_prescaled_switch, .-jt_i386_abs32_prescaled_switch

// The function does contain a real model-zero producer, but the table address
// uses a different register that is independently assigned literal zero.  The
// exact GOTOFF field cannot authenticate that unrelated base input.
.p2align 2
.globl jt_i386_gotoff_prescaled_literal_zero
.type jt_i386_gotoff_prescaled_literal_zero, @function
jt_i386_gotoff_prescaled_literal_zero:
  call .Lprescaled_literal_pc
.Lprescaled_literal_pc:
  popl %esi
  .byte 0x81, 0xc6
.Lprescaled_literal_gotpc:
  .long .Lprescaled_literal_gotpc - .Lprescaled_literal_pc
  .reloc .Lprescaled_literal_gotpc, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  xorl %ebx, %ebx
  movl 4(%esp), %ecx
  andl $0xc, %ecx
  movl .Lprescaled_literal_table@GOTOFF(%ebx,%ecx), %eax
  addl %ebx, %eax
  jmp *%eax
.Lprescaled_literal_case0:
  movl $5800, %eax
  ret
.Lprescaled_literal_case1:
  movl $5801, %eax
  ret
.Lprescaled_literal_case2:
  movl $5802, %eax
  ret
.Lprescaled_literal_case3:
  movl $5803, %eax
  ret
.size jt_i386_gotoff_prescaled_literal_zero, .-jt_i386_gotoff_prescaled_literal_zero

.section .rodata,"a",@progbits
.p2align 2
.globl jt_i386_gotpc_rooted_pop_pointer
.type jt_i386_gotpc_rooted_pop_pointer, @object
jt_i386_gotpc_rooted_pop_pointer:
  .long .Lgotpc_rooted_pop
.size jt_i386_gotpc_rooted_pop_pointer, .-jt_i386_gotpc_rooted_pop_pointer

.p2align 2
.globl jt_i386_gotoff_switch_table
.type jt_i386_gotoff_switch_table, @object
jt_i386_gotoff_switch_table:
.Lgotoff_switch_table:
  .long .Lgotoff_switch_case0@GOTOFF
  .long .Lgotoff_switch_case1@GOTOFF
  .long .Lgotoff_switch_case2@GOTOFF
  .long .Lgotoff_switch_case3@GOTOFF
  .long .Lgotoff_switch_case4@GOTOFF
.size jt_i386_gotoff_switch_table, .-jt_i386_gotoff_switch_table

.p2align 2
.globl jt_i386_gotoff_conflict_table
.type jt_i386_gotoff_conflict_table, @object
jt_i386_gotoff_conflict_table:
.Lgotoff_conflict_table:
  .long .Lgotoff_conflict_case0@GOTOFF
  .long .Lgotoff_conflict_case1@GOTOFF
  .long .Lgotoff_conflict_case2@GOTOFF
  .long .Lgotoff_conflict_case3@GOTOFF
  .long .Lgotoff_conflict_case4@GOTOFF
.size jt_i386_gotoff_conflict_table, .-jt_i386_gotoff_conflict_table

.p2align 2
.globl jt_i386_gotoff_conflict_valid_table
.type jt_i386_gotoff_conflict_valid_table, @object
jt_i386_gotoff_conflict_valid_table:
.Lgotoff_conflict_valid_table:
  .long .Lgotoff_conflict_valid_case0@GOTOFF
  .long .Lgotoff_conflict_valid_case1@GOTOFF
.size jt_i386_gotoff_conflict_valid_table, .-jt_i386_gotoff_conflict_valid_table

.p2align 2
.globl jt_i386_gotoff_displacement_conflict_table
.type jt_i386_gotoff_displacement_conflict_table, @object
jt_i386_gotoff_displacement_conflict_table:
  .long .Lgotoff_displacement_conflict_case0@GOTOFF
  .long .Lgotoff_displacement_conflict_case1@GOTOFF
.size jt_i386_gotoff_displacement_conflict_table, .-jt_i386_gotoff_displacement_conflict_table

.p2align 2
.globl jt_i386_gotoff_late_table
.type jt_i386_gotoff_late_table, @object
jt_i386_gotoff_late_table:
.Lgotoff_late_table:
  .long .Lgotoff_late_case0@GOTOFF
  .long .Lgotoff_late_case1@GOTOFF
.size jt_i386_gotoff_late_table, .-jt_i386_gotoff_late_table

.p2align 2
.globl jt_i386_gotoff_late_valid_table
.type jt_i386_gotoff_late_valid_table, @object
jt_i386_gotoff_late_valid_table:
.Lgotoff_late_valid_table:
  .long .Lgotoff_late_valid_case0 - .Lgotpc_text_base
  .long .Lgotoff_late_valid_case1 - .Lgotpc_text_base
.size jt_i386_gotoff_late_valid_table, .-jt_i386_gotoff_late_valid_table

.p2align 2
.globl jt_i386_gotoff_switch_fake_table
.type jt_i386_gotoff_switch_fake_table, @object
jt_i386_gotoff_switch_fake_table:
.Lgotoff_switch_fake_table:
  .long .Lgotoff_switch_fake_case0@GOTOFF
  .long .Lgotoff_switch_fake_case1@GOTOFF
  .long .Lgotoff_switch_fake_case2@GOTOFF
  .long .Lgotoff_switch_fake_case3@GOTOFF
  .long .Lgotoff_switch_fake_case4@GOTOFF
.size jt_i386_gotoff_switch_fake_table, .-jt_i386_gotoff_switch_fake_table

.p2align 2
.globl jt_i386_abs32_switch_table
.type jt_i386_abs32_switch_table, @object
jt_i386_abs32_switch_table:
  .long .Labs32_case0@GOTOFF
  .long .Labs32_case1@GOTOFF
  .long .Labs32_case2@GOTOFF
  .long .Labs32_case3@GOTOFF
  .long .Labs32_case4@GOTOFF
.size jt_i386_abs32_switch_table, .-jt_i386_abs32_switch_table

.p2align 2
.globl jt_i386_literal_zero_table
.type jt_i386_literal_zero_table, @object
jt_i386_literal_zero_table:
.Lliteral_table:
  .long .Lliteral_case0@GOTOFF
  .long .Lliteral_case1@GOTOFF
  .long .Lliteral_case2@GOTOFF
  .long .Lliteral_case3@GOTOFF
  .long .Lliteral_case4@GOTOFF
.size jt_i386_literal_zero_table, .-jt_i386_literal_zero_table

.p2align 2
.globl jt_i386_mixed_zero_table
.type jt_i386_mixed_zero_table, @object
jt_i386_mixed_zero_table:
.Lmixed_table:
  .long .Lmixed_case0@GOTOFF
  .long .Lmixed_case1@GOTOFF
  .long .Lmixed_case2@GOTOFF
  .long .Lmixed_case3@GOTOFF
  .long .Lmixed_case4@GOTOFF
.size jt_i386_mixed_zero_table, .-jt_i386_mixed_zero_table

.p2align 2
.globl jt_i386_gotoff_prescaled_table
.type jt_i386_gotoff_prescaled_table, @object
jt_i386_gotoff_prescaled_table:
.Lprescaled_real_table:
  .long .Lprescaled_real_case0@GOTOFF
  .long .Lprescaled_real_case1@GOTOFF
  .long .Lprescaled_real_case2@GOTOFF
  .long .Lprescaled_real_case3@GOTOFF
.size jt_i386_gotoff_prescaled_table, .-jt_i386_gotoff_prescaled_table

.p2align 2
.globl jt_i386_abs32_prescaled_table
.type jt_i386_abs32_prescaled_table, @object
jt_i386_abs32_prescaled_table:
  .long .Lprescaled_abs32_case0@GOTOFF
  .long .Lprescaled_abs32_case1@GOTOFF
  .long .Lprescaled_abs32_case2@GOTOFF
  .long .Lprescaled_abs32_case3@GOTOFF
.size jt_i386_abs32_prescaled_table, .-jt_i386_abs32_prescaled_table

.p2align 2
.globl jt_i386_gotoff_prescaled_literal_table
.type jt_i386_gotoff_prescaled_literal_table, @object
jt_i386_gotoff_prescaled_literal_table:
.Lprescaled_literal_table:
  .long .Lprescaled_literal_case0@GOTOFF
  .long .Lprescaled_literal_case1@GOTOFF
  .long .Lprescaled_literal_case2@GOTOFF
  .long .Lprescaled_literal_case3@GOTOFF
.size jt_i386_gotoff_prescaled_literal_table, .-jt_i386_gotoff_prescaled_literal_table

.section .note.GNU-stack,"",@progbits
