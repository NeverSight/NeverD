// Multi-output intrinsic metadata — included by intrinsics.cpp.
// Macro: MULTI_OUT(IntrinsicId, output_count)
//
// output_count = number of register-level outputs beyond the primary return.
// Used by cf_structurer to recognize the INTRINSIC + COPY-clobber pattern
// and by emitters to render proper multi-output C code.

// x86/x64
MULTI_OUT(Intrinsic::Cpuid,   4);  // eax, ebx, ecx, edx
MULTI_OUT(Intrinsic::Rdtsc,   2);  // eax(lo), edx(hi)
MULTI_OUT(Intrinsic::Rdtscp,  3);  // eax(lo), edx(hi), ecx(aux)
MULTI_OUT(Intrinsic::Xgetbv,  2);  // eax(lo), edx(hi)

// AArch64 FEAT_MOPS set prologue: destination, remaining count, NZCV.
MULTI_OUT(Intrinsic::A64_MopsSetP,   3);
MULTI_OUT(Intrinsic::A64_MopsSetPN,  3);
MULTI_OUT(Intrinsic::A64_MopsSetPT,  3);
MULTI_OUT(Intrinsic::A64_MopsSetPTN, 3);

// AArch64 FEAT_MOPS copy prologue: destination, source, count, NZCV.
MULTI_OUT(Intrinsic::A64_MopsCpyFP, 4);
