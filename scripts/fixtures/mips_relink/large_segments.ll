; The first and last bytes force the entire payload to be file-backed.
@payload = constant <{ i8, [16777214 x i8], i8 }> <{ i8 19, [16777214 x i8] zeroinitializer, i8 23 }>, align 16
@arena = global [67108864 x i8] zeroinitializer, align 16

define i32 @check_large_segments() nounwind {
  %first = load volatile i8, ptr @payload
  %last = load volatile i8, ptr getelementptr (i8, ptr @payload, i32 16777215)
  %start_zero = load volatile i8, ptr @arena
  %end_zero = load volatile i8, ptr getelementptr (i8, ptr @arena, i32 67108863)
  %left = icmp eq i8 %first, 19
  %right = icmp eq i8 %last, 23
  %both = and i1 %left, %right
  %zeros = or i8 %start_zero, %end_zero
  %empty = icmp eq i8 %zeros, 0
  %ready = and i1 %both, %empty
  br i1 %ready, label %write, label %failed

write:
  store volatile i8 %last, ptr getelementptr (i8, ptr @arena, i32 67108863)
  %written = load volatile i8, ptr getelementptr (i8, ptr @arena, i32 67108863)
  %untouched = load volatile i8, ptr @arena
  %saved = icmp eq i8 %written, 23
  %separate = icmp eq i8 %untouched, 0
  %valid = and i1 %saved, %separate
  br i1 %valid, label %calls, label %failed

calls:
  %result = call i32 @run_calls()
  ret i32 %result

failed:
  ret i32 181
}
