source_filename = "branch_slots.c"

@callback = constant ptr @calc, section ".rodata.callback"
declare i32 @calc(i32)

define i32 @run_calls() nounwind {
  %callee = load volatile ptr, ptr @callback
  %zero = call i32 %callee(i32 0)
  %one = call i32 %callee(i32 1)
  %base.zero = sub i32 %zero, ZERO_INCREMENT
  %base.one = sub i32 %one, ONE_INCREMENT
  %same = icmp eq i32 %base.zero, %base.one
  %original = icmp eq i32 %base.zero, 8
  %updated = icmp eq i32 %base.zero, 31
  %which = select i1 %original, i32 41, i32 97
  %status = select i1 %updated, i32 73, i32 %which
  %result = select i1 %same, i32 %status, i32 97
  ret i32 %result
}
