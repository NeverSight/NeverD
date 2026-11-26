source_filename = "calc.c"

declare i64 @helper(i64)
ASE_DECLARATIONS

; Two FP arguments, a 64-bit integer, and enough trailing integers to cross
; the argument-register boundary of all three ABIs.
define i64 @calc(double %x, double %y, i64 %wide, i32 %a, i32 %b, i32 %c,
                 i32 %d, i32 %e, i32 %f, i32 %last) nounwind section ".text.calc" {
X_BITS
  %ybits = bitcast double %y to i64
  %bits = xor i64 %xbits, %ybits
  %payload = add i64 %wide, %bits
  %ab = sub i32 %a, %b
  %cd = xor i32 %c, %d
  %ef = sub i32 %e, %f
  %abcd = add i32 %ab, %cd
  %abcdef = add i32 %abcd, %ef
  %sum = add i32 %abcdef, %last
  %ext = sext i32 %sum to i64
  %arg = add i64 %payload, %ext
  %mixed = notail call i64 @helper(i64 %arg)
ADJUST_RESULT
  ret i64 %result
}
