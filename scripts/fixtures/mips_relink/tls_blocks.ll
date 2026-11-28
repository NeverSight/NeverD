source_filename = "tls_blocks.c"

@seed = TLS_STORAGE global i32 324508639, align 4
@zeroes = TLS_STORAGE global [259 x i8] zeroinitializer, align 256
@tls_blocks = global [1536 x i8] zeroinitializer, align 256
@__tls_image = external global i32
@callback = global ptr @calc

declare i32 @set_block(ptr)
declare i32 @calc(i32)

define internal i32 @step(ptr %block, i32 %tag) noinline nounwind {
  %status = call i32 @set_block(ptr %block)
  %ok = icmp eq i32 %status, 0
  br i1 %ok, label %active, label %failed
active:
  %p = load volatile ptr, ptr @callback
  %result = call i32 %p(i32 %tag)
  ret i32 %result
failed:
  ret i32 0
}

define i32 @run_calls() nounwind {
  ; Install the initialized word in two separate, zero-filled TLS blocks.
  %b = getelementptr i8, ptr @tls_blocks, i32 768
  %init = load volatile i32, ptr @__tls_image, align 4
  store i32 %init, ptr @tls_blocks, align 256
  store i32 %init, ptr %b, align 256
  %a1 = call i32 @step(ptr @tls_blocks, i32 3)
  %b1 = call i32 @step(ptr %b, i32 5)
  %a2 = call i32 @step(ptr @tls_blocks, i32 7)
  %b2 = call i32 @step(ptr %b, i32 11)
  %adjust = xor i32 %a1, 324508639
  %same = icmp eq i32 %a1, %b1
  ; The second reads see their own previous seed and the two .tbss bytes.
  %left = xor i32 %a2, %adjust
  %right = xor i32 %b2, %adjust
  %left.ok = icmp eq i32 %left, 324309730
  %right.ok = icmp eq i32 %right, 324179172
  %valid = and i1 %left.ok, %right.ok
  %isolated = and i1 %valid, %same
  %old = icmp eq i32 %adjust, 1
  %new = icmp eq i32 %adjust, 7
  %old.status = select i1 %old, i32 41, i32 99
  %status = select i1 %new, i32 73, i32 %old.status
  %exit = select i1 %isolated, i32 %status, i32 98
  ret i32 %exit
}
