.text
.p2align 2
.globl jt_i386_tls_desc_text_base
jt_i386_tls_desc_text_base:

// R_386_TLS_DESC writes the descriptor word at r_offset+4 rather than at the
// relocation record's nominal field.  Place that biased footprint directly
// over an otherwise valid GOTPC immediate.  Exact model-zero provenance must
// be suppressed even though the two relocation records have different
// r_offset values.
.p2align 2
.globl jt_i386_gotoff_tls_desc_overlap
.type jt_i386_gotoff_tls_desc_overlap, @function
jt_i386_gotoff_tls_desc_overlap:
  call .Lgotoff_tls_desc_overlap_pc
.Lgotoff_tls_desc_overlap_pc:
  popl %ebx
.globl jt_i386_gotoff_tls_desc_overlap_record
jt_i386_gotoff_tls_desc_overlap_record:
  nop
  nop
  .byte 0x81, 0xc3                 // addl imm32, %ebx
.globl jt_i386_gotoff_tls_desc_overlap_field
jt_i386_gotoff_tls_desc_overlap_field:
  .long jt_i386_gotoff_tls_desc_overlap_field - .Lgotoff_tls_desc_overlap_pc
  .reloc jt_i386_gotoff_tls_desc_overlap_record, R_386_TLS_DESC, jt_i386_tls_desc_target
  .reloc jt_i386_gotoff_tls_desc_overlap_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl 4(%esp), %ecx
  cmpl $1, %ecx
  ja .Lgotoff_tls_desc_overlap_default
  movl .Lgotoff_tls_desc_overlap_table@GOTOFF(%ebx,%ecx,4), %eax
  addl %ebx, %eax
.globl jt_i386_gotoff_tls_desc_overlap_branch
jt_i386_gotoff_tls_desc_overlap_branch:
  jmp *%eax
.Lgotoff_tls_desc_overlap_case0:
  movl $5120, %eax
  ret
.Lgotoff_tls_desc_overlap_case1:
  movl $5121, %eax
  ret
.Lgotoff_tls_desc_overlap_default:
  movl $-1, %eax
  ret
.size jt_i386_gotoff_tls_desc_overlap, .-jt_i386_gotoff_tls_desc_overlap

// The same relocation kind with a writer footprint that is disjoint from the
// GOTPC immediate must not taint it.  Keep both records in one executable
// owner so the positive control differs only by the byte-range relationship.
.p2align 2
.globl jt_i386_gotoff_tls_desc_nonoverlap
.type jt_i386_gotoff_tls_desc_nonoverlap, @function
jt_i386_gotoff_tls_desc_nonoverlap:
  jmp .Lgotoff_tls_desc_nonoverlap_entry
.globl jt_i386_gotoff_tls_desc_nonoverlap_record
jt_i386_gotoff_tls_desc_nonoverlap_record:
  .long 0
  .long 0
.Lgotoff_tls_desc_nonoverlap_entry:
  call .Lgotoff_tls_desc_nonoverlap_pc
.Lgotoff_tls_desc_nonoverlap_pc:
  popl %ebx
  .byte 0x81, 0xc3                 // addl imm32, %ebx
.globl jt_i386_gotoff_tls_desc_nonoverlap_field
jt_i386_gotoff_tls_desc_nonoverlap_field:
  .long jt_i386_gotoff_tls_desc_nonoverlap_field - .Lgotoff_tls_desc_nonoverlap_pc
  .reloc jt_i386_gotoff_tls_desc_nonoverlap_record, R_386_TLS_DESC, jt_i386_tls_desc_target
  .reloc jt_i386_gotoff_tls_desc_nonoverlap_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl 4(%esp), %ecx
  cmpl $1, %ecx
  ja .Lgotoff_tls_desc_nonoverlap_default
  movl .Lgotoff_tls_desc_nonoverlap_table@GOTOFF(%ebx,%ecx,4), %eax
  addl %ebx, %eax
.globl jt_i386_gotoff_tls_desc_nonoverlap_branch
jt_i386_gotoff_tls_desc_nonoverlap_branch:
  jmp *%eax
.Lgotoff_tls_desc_nonoverlap_case0:
  movl $5130, %eax
  ret
.Lgotoff_tls_desc_nonoverlap_case1:
  movl $5131, %eax
  ret
.Lgotoff_tls_desc_nonoverlap_default:
  movl $-1, %eax
  ret
.size jt_i386_gotoff_tls_desc_nonoverlap, .-jt_i386_gotoff_tls_desc_nonoverlap

.section .rodata,"a",@progbits
.p2align 2
.globl jt_i386_gotoff_tls_desc_overlap_table
.type jt_i386_gotoff_tls_desc_overlap_table, @object
jt_i386_gotoff_tls_desc_overlap_table:
.Lgotoff_tls_desc_overlap_table:
  .long .Lgotoff_tls_desc_overlap_case0@GOTOFF
  .long .Lgotoff_tls_desc_overlap_case1@GOTOFF
.size jt_i386_gotoff_tls_desc_overlap_table, .-jt_i386_gotoff_tls_desc_overlap_table

.p2align 2
.globl jt_i386_gotoff_tls_desc_nonoverlap_table
.type jt_i386_gotoff_tls_desc_nonoverlap_table, @object
jt_i386_gotoff_tls_desc_nonoverlap_table:
.Lgotoff_tls_desc_nonoverlap_table:
  .long .Lgotoff_tls_desc_nonoverlap_case0@GOTOFF
  .long .Lgotoff_tls_desc_nonoverlap_case1@GOTOFF
.size jt_i386_gotoff_tls_desc_nonoverlap_table, .-jt_i386_gotoff_tls_desc_nonoverlap_table

// Mirror the biased-writer boundary with GOTOFF rather than GOTPC.  The
// descriptor record at P writes P+4, exactly overlapping the following
// GOTOFF word.  Every supported relocation is still applied from the original
// REL addend, but the GOTOFF field must retain only negative provenance.
.section .data.rel.ro,"aw",@progbits
.p2align 2
.globl jt_i386_tls_desc_gotoff_overlap_record
jt_i386_tls_desc_gotoff_overlap_record:
  .long 0
.globl jt_i386_tls_desc_gotoff_overlap_field
jt_i386_tls_desc_gotoff_overlap_field:
  .long 0
  .reloc jt_i386_tls_desc_gotoff_overlap_record, R_386_TLS_DESC, jt_i386_tls_desc_target
  .reloc jt_i386_tls_desc_gotoff_overlap_field, R_386_GOTOFF, jt_i386_gotoff_tls_desc_overlap_table

// Positive control: the TLS descriptor's P+4 word ends before this GOTOFF
// field, so the exact GOTOFF provenance and final bytes remain available.
.p2align 2
.globl jt_i386_tls_desc_gotoff_nonoverlap_record
jt_i386_tls_desc_gotoff_nonoverlap_record:
  .long 0
  .long 0
  .long 0
.globl jt_i386_tls_desc_gotoff_nonoverlap_field
jt_i386_tls_desc_gotoff_nonoverlap_field:
  .long 0
  .reloc jt_i386_tls_desc_gotoff_nonoverlap_record, R_386_TLS_DESC, jt_i386_tls_desc_target
  .reloc jt_i386_tls_desc_gotoff_nonoverlap_field, R_386_GOTOFF, jt_i386_gotoff_tls_desc_nonoverlap_table

.section .tdata,"awT",@progbits
.p2align 2
.globl jt_i386_tls_desc_target
.type jt_i386_tls_desc_target, @tls_object
jt_i386_tls_desc_target:
  .long 0
.size jt_i386_tls_desc_target, .-jt_i386_tls_desc_target

.section .note.GNU-stack,"",@progbits
