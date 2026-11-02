// x86 intrinsics whose C name exists only in MSVC's <intrin.h>; included by
// HighCIntrinsicRenderX86.cpp.  For a GCC or Clang target HighC writes the
// instruction as an inline asm statement instead.  Each takes no operand and
// produces no value.
G(Intrinsic::Hlt, "hlt")
G(Intrinsic::Cli, "cli")
G(Intrinsic::Sti, "sti")
