source_filename = "caller.c"

declare i64 @calc(double, double, i64, i32, i32, i32, i32, i32, i32, i32)
@callback = global ptr @calc
@seed = SEED_STORAGE global i64 4294967315, align 8
@visits = global i32 0, align 4

define i64 @helper(i64 %value) noinline nounwind {
  %n = load volatile i32, ptr @visits
  %next = add i32 %n, 1
  store volatile i32 %next, ptr @visits
  %seed = load volatile i64, ptr @seed
  %result = add i64 %value, %seed
  ret i64 %result
}

define i32 @run_calls() nounwind {
  %p = load volatile ptr, ptr @callback
  %one = notail call i64 %p(double 0x3FF4000000000001, double 0x4002000000000005,
                            i64 72623859790382856, i32 9, i32 2, i32 291, i32 532,
                            i32 17, i32 6, i32 91)
  %q = load volatile ptr, ptr @callback
  %two = notail call i64 %q(double 0xC009000000000007, double 0x3FF000000000000B,
                            i64 8074989523484827760, i32 -5, i32 19, i32 55, i32 537,
                            i32 3, i32 28, i32 -11)
  %old1 = icmp eq i64 %one, OLD_1
  %old2 = icmp eq i64 %two, OLD_2
  %old = and i1 %old1, %old2
  %new1 = icmp eq i64 %one, NEW_1
  %new2 = icmp eq i64 %two, NEW_2
  %new = and i1 %new1, %new2
  %before = select i1 %old, i32 41, i32 99
  %after = select i1 %new, i32 73, i32 %before
  %count = load volatile i32, ptr @visits
  %twice = icmp eq i32 %count, 2
  %exit = select i1 %twice, i32 %after, i32 98
  ret i32 %exit
}
