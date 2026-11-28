; The driver supplies one relocation per pointer in this original input.
@callback_table = constant [COUNT x ptr] [ENTRIES], section ".rodata.callbacks", align 8

define i32 @check_callback_table() nounwind {
entry:
  br label %check

check:
  %i = phi i32 [ 0, %entry ], [ %next, %advance ]
  %cell = getelementptr [COUNT x ptr], ptr @callback_table, i32 0, i32 %i
  %target = load volatile ptr, ptr %cell
  %matches = icmp eq ptr %target, @calc
  br i1 %matches, label %advance, label %failed

advance:
  %next = add nuw i32 %i, 1
  %done = icmp eq i32 %next, COUNT
  br i1 %done, label %calls, label %check

calls:
  %result = call i32 @run_calls()
  ret i32 %result

failed:
  ret i32 182
}
