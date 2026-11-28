source_filename = "relocation_bounds.c"

@callback = constant ptr @calc, section ".rodata.callback"
@gp_low = global i8 10, section ".gp.first", align 1
@gp_high = global i8 15, section ".gp.last", align 1

declare WORD @calc()

define i32 @run_calls() nounwind {
  %callee = load volatile ptr, ptr @callback
  %value = call WORD %callee()
  %original = icmp eq WORD %value, BEFORE
  %updated = icmp eq WORD %value, AFTER
  %other = select i1 %updated, i32 73, i32 97
  %result = select i1 %original, i32 41, i32 %other
  ret i32 %result
}
