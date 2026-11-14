// i386 ELF PIC dispatches whose GOT base cannot be proved, and a genuine
// function-pointer tail call beside them.

.text

// A switch over a table of `case@GOTOFF` words.  The GOT base is spilled and
// reloaded around a store whose index nothing bounds: with %edx == -1 the
// store writes the spill slot itself, so the reload is not provably the GOT
// base and the table's targets cannot be published.  The jump still reads
// labels of its own function and must stay an indirect branch.
.p2align 4
.globl jt_i386_gotoff_spilled_base
.type jt_i386_gotoff_spilled_base, @function
jt_i386_gotoff_spilled_base:
  subl $0x2c, %esp
  movl 0x30(%esp), %ecx
  movl 0x34(%esp), %edx
  call .Lspilled_pc
.Lspilled_pc:
  popl %eax
.Lspilled_add:
  addl $_GLOBAL_OFFSET_TABLE_+(.Lspilled_add-.Lspilled_pc), %eax
  movl %eax, 0x18(%esp)
  movl %ecx, 0x1c(%esp,%edx,4)
  andl $3, %ecx
  movl 0x18(%esp), %edx
  movl .Lspilled_table@GOTOFF(%edx,%ecx,4), %ecx
  addl %edx, %ecx
.globl jt_i386_gotoff_spilled_base_branch
jt_i386_gotoff_spilled_base_branch:
  jmp *%ecx
.Lspilled_case0:
  movl $10, %eax
  jmp .Lspilled_out
.Lspilled_case1:
  movl $20, %eax
  jmp .Lspilled_out
.Lspilled_case2:
  movl $30, %eax
  jmp .Lspilled_out
.Lspilled_case3:
  movl $40, %eax
.Lspilled_out:
  addl $0x2c, %esp
  ret
.size jt_i386_gotoff_spilled_base, .-jt_i386_gotoff_spilled_base

// A tail call through a table of function pointers, addressed through the
// GOT like the switch table above.  Its words are callable entries, so the
// jump stays a tail call.
.p2align 4
.globl jt_i386_gotoff_callback_tail
.type jt_i386_gotoff_callback_tail, @function
jt_i386_gotoff_callback_tail:
  movl 8(%esp), %ecx
  call .Lcallback_pc
.Lcallback_pc:
  popl %eax
.Lcallback_add:
  addl $_GLOBAL_OFFSET_TABLE_+(.Lcallback_add-.Lcallback_pc), %eax
  andl $1, %ecx
  movl jt_i386_gotoff_callbacks@GOTOFF(%eax,%ecx,4), %eax
.globl jt_i386_gotoff_callback_tail_branch
jt_i386_gotoff_callback_tail_branch:
  jmp *%eax
.size jt_i386_gotoff_callback_tail, .-jt_i386_gotoff_callback_tail

.p2align 4
.type jt_i386_gotoff_callback0, @function
jt_i386_gotoff_callback0:
  movl 4(%esp), %eax
  incl %eax
  ret
.size jt_i386_gotoff_callback0, .-jt_i386_gotoff_callback0

.p2align 4
.type jt_i386_gotoff_callback1, @function
jt_i386_gotoff_callback1:
  movl 4(%esp), %eax
  decl %eax
  ret
.size jt_i386_gotoff_callback1, .-jt_i386_gotoff_callback1

// Two tables behind one GOT base that cannot be proved either.  The first
// is read once, before anything writes %ebx; its index is 0 or 2, and the
// word between names the top of the loop, which adds %ebx.  A jump there
// from the first dispatch would read a register the function has not
// written, so that word is none of the jump's targets.  The second table is
// read in the loop.
.p2align 4
.globl jt_i386_gotoff_two_tables
.type jt_i386_gotoff_two_tables, @function
jt_i386_gotoff_two_tables:
  pushl %ebx
  subl $0x28, %esp
  movl 0x30(%esp), %ecx
  movl 0x34(%esp), %edx
  call .Ltwo_pc
.Ltwo_pc:
  popl %eax
.Ltwo_add:
  addl $_GLOBAL_OFFSET_TABLE_+(.Ltwo_add-.Ltwo_pc), %eax
  movl %eax, 0x18(%esp)
  movl %ecx, 0x1c(%esp,%edx,4)
  movl %ecx, %edx
  andl $1, %edx
  addl %edx, %edx
  movl 0x18(%esp), %eax
  movl jt_i386_gotoff_two_tables_first_table@GOTOFF(%eax,%edx,4), %edx
  addl %eax, %edx
.globl jt_i386_gotoff_two_tables_first
jt_i386_gotoff_two_tables_first:
  jmp *%edx
.Ltwo_first0:
  movl $0, %ebx
  jmp .Ltwo_top
.Ltwo_first1:
  movl $1, %ebx
.Ltwo_top:
  addl %ebx, %ecx
  movl %ecx, %edx
  andl $1, %edx
  movl 0x18(%esp), %eax
  movl jt_i386_gotoff_two_tables_second_table@GOTOFF(%eax,%edx,4), %edx
  addl %eax, %edx
.globl jt_i386_gotoff_two_tables_second
jt_i386_gotoff_two_tables_second:
  jmp *%edx
.Ltwo_second0:
  incl %ebx
  cmpl $100, %ebx
  jb .Ltwo_top
.Ltwo_second1:
  movl %ebx, %eax
  addl $0x28, %esp
  popl %ebx
  ret
.size jt_i386_gotoff_two_tables, .-jt_i386_gotoff_two_tables

.section .rodata,"a",@progbits
.p2align 2
.globl jt_i386_gotoff_spilled_table
.type jt_i386_gotoff_spilled_table, @object
jt_i386_gotoff_spilled_table:
.Lspilled_table:
  .long .Lspilled_case0@GOTOFF
  .long .Lspilled_case1@GOTOFF
  .long .Lspilled_case2@GOTOFF
  .long .Lspilled_case3@GOTOFF
.size jt_i386_gotoff_spilled_table, .-jt_i386_gotoff_spilled_table
.globl jt_i386_gotoff_two_tables_first_table
.type jt_i386_gotoff_two_tables_first_table, @object
jt_i386_gotoff_two_tables_first_table:
  .long .Ltwo_first0@GOTOFF
  .long .Ltwo_top@GOTOFF
  .long .Ltwo_first1@GOTOFF
.size jt_i386_gotoff_two_tables_first_table, .-jt_i386_gotoff_two_tables_first_table
.globl jt_i386_gotoff_two_tables_second_table
.type jt_i386_gotoff_two_tables_second_table, @object
jt_i386_gotoff_two_tables_second_table:
  .long .Ltwo_second0@GOTOFF
  .long .Ltwo_second1@GOTOFF
.size jt_i386_gotoff_two_tables_second_table, .-jt_i386_gotoff_two_tables_second_table

.section .data.rel.ro,"aw",@progbits
.p2align 2
.globl jt_i386_gotoff_callbacks
.type jt_i386_gotoff_callbacks, @object
jt_i386_gotoff_callbacks:
  .long jt_i386_gotoff_callback0
  .long jt_i386_gotoff_callback1
.size jt_i386_gotoff_callbacks, .-jt_i386_gotoff_callbacks
