declare <4 x i8> @llvm.mips.addu.qb(<4 x i8>, <4 x i8>)
; body
  %word = trunc i64 %mixed to i32
  %bytes = bitcast i32 %word to <4 x i8>
  %offset = add i32 %word, ADJUST
  %offsets = bitcast i32 %offset to <4 x i8>
  %lanes = call <4 x i8> @llvm.mips.addu.qb(<4 x i8> %bytes, <4 x i8> %offsets)
  %packed = bitcast <4 x i8> %lanes to i32
  %lane_sum = sub i32 %packed, %word
  %change = sub i32 %lane_sum, %word
  %masked = and i32 %change, 255
  %delta = zext i32 %masked to i64
  %result = add i64 %mixed, %delta
