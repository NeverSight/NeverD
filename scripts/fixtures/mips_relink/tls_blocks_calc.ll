source_filename = "tls_blocks_calc.c"

@seed = external TLS_STORAGE global i32, align 4
@zeroes = external TLS_STORAGE global [259 x i8], align 1

define i32 @calc(i32 %tag) nounwind section ".text.calc" {
  %address = ptrtoint ptr @zeroes to POINTER_INT
  %low = and POINTER_INT %address, 255
  %aligned = icmp eq POINTER_INT %low, 0
  br i1 %aligned, label %access, label %invalid
invalid:
  ret i32 0
access:
  %end = getelementptr i8, ptr @zeroes, i32 258
  %seed = load volatile i32, ptr @seed, align 4
  %first = load volatile i8, ptr @zeroes
  %last = load volatile i8, ptr %end
  %next = add i32 %seed, %tag
  store volatile i32 %next, ptr @seed, align 4
  %byte = trunc i32 %tag to i8
  %first.next = add i8 %first, %byte
  %triple = mul i8 %byte, 3
  %last.next = add i8 %last, %triple
  store volatile i8 %first.next, ptr @zeroes
  store volatile i8 %last.next, ptr %end
  %f = zext i8 %first to i32
  %l = zext i8 %last to i32
  %f.shift = shl i32 %f, 16
  %l.shift = shl i32 %l, 8
  %bits = or i32 %f.shift, %l.shift
  %value = xor i32 %bits, %seed
  %result = xor i32 %value, ADJUST
  ret i32 %result
}
