declare <4 x i32> @llvm.mips.fill.w(i32)
declare <4 x i32> @llvm.mips.addv.w(<4 x i32>, <4 x i32>)
declare i32 @llvm.mips.copy.s.w(<4 x i32>, i32)
; body
  %word = trunc i64 %mixed to i32
  %words = call <4 x i32> @llvm.mips.fill.w(i32 %word)
  %offsets = call <4 x i32> @llvm.mips.fill.w(i32 ADJUST)
  %lanes = call <4 x i32> @llvm.mips.addv.w(<4 x i32> %words, <4 x i32> %offsets)
  %lane = call i32 @llvm.mips.copy.s.w(<4 x i32> %lanes, i32 2)
  %change = sub i32 %lane, %word
  %delta = sext i32 %change to i64
  %result = add i64 %mixed, %delta
