source_filename = "branch_caller.c"

@callback = global ptr @calc
declare i32 @calc(i32)

define i32 @left(i32 %x) nounwind "mips16" {
  ret i32 17
}

define i32 @right(i32 %x) nounwind "mips16" {
  ret i32 35
}

define i32 @run_calls() nounwind {
  %first = load volatile ptr, ptr @callback
  %a = call i32 %first(i32 0)
  %second = load volatile ptr, ptr @callback
  %b = call i32 %second(i32 1)
  %old.a = icmp eq i32 %a, 17
  %old.b = icmp eq i32 %b, 35
  %old = and i1 %old.a, %old.b
  %new.a = icmp eq i32 %a, 35
  %new.b = icmp eq i32 %b, 17
  %new = and i1 %new.a, %new.b
  %original = select i1 %old, i32 41, i32 99
  %status = select i1 %new, i32 73, i32 %original
  ret i32 %status
}
