//===- intrinsics_x86_llvm_cnames.inc - x86 LLVM/C spellings -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact x86 LLVM intrinsic names and their standard C intrinsic spellings.
/// Included as table entries by the shared LLVM-to-C name inventory.
//===----------------------------------------------------------------------===//
// clang-format off

// x86 SSE/SSSE3
{"llvm.x86.ssse3.pshuf.b.128",     "_mm_shuffle_epi8"},

// x86 SSE2 packed shifts
{"llvm.x86.sse2.psrl.d",           "_mm_srl_epi32"},
{"llvm.x86.sse2.psrl.q",           "_mm_srl_epi64"},
{"llvm.x86.sse2.psrl.w",           "_mm_srl_epi16"},
{"llvm.x86.sse2.psll.d",           "_mm_sll_epi32"},
{"llvm.x86.sse2.psll.q",           "_mm_sll_epi64"},
{"llvm.x86.sse2.psll.w",           "_mm_sll_epi16"},
{"llvm.x86.sse2.psra.d",           "_mm_sra_epi32"},
{"llvm.x86.sse2.psra.w",           "_mm_sra_epi16"},
{"llvm.x86.sse2.psrli.d",          "_mm_srli_epi32"},
{"llvm.x86.sse2.psrli.q",          "_mm_srli_epi64"},
{"llvm.x86.sse2.psrli.w",          "_mm_srli_epi16"},
{"llvm.x86.sse2.pslli.d",          "_mm_slli_epi32"},
{"llvm.x86.sse2.pslli.q",          "_mm_slli_epi64"},
{"llvm.x86.sse2.pslli.w",          "_mm_slli_epi16"},
{"llvm.x86.sse2.psrai.d",          "_mm_srai_epi32"},
{"llvm.x86.sse2.psrai.w",          "_mm_srai_epi16"},

// x86 SSE2 misc
{"llvm.x86.sse2.pmovmskb.128",     "_mm_movemask_epi8"},
{"llvm.x86.sse2.pause",            "_mm_pause"},

// x86 SSE4.1
{"llvm.x86.sse41.dpps",            "_mm_dp_ps"},
{"llvm.x86.sse41.mpsadbw",         "_mm_mpsadbw_epu8"},
{"llvm.x86.sse41.phminposuw",      "_mm_minpos_epu16"},

// x86 SSE4.2 string
{"llvm.x86.sse42.pcmpistri128",    "_mm_cmpistri"},
{"llvm.x86.sse42.pcmpistrm128",    "_mm_cmpistrm"},
{"llvm.x86.sse42.pcmpestri128",    "_mm_cmpestri"},
{"llvm.x86.sse42.pcmpestrm128",    "_mm_cmpestrm"},

// x86 AES-NI
{"llvm.x86.aesni.aesenc",          "_mm_aesenc_si128"},
{"llvm.x86.aesni.aesenclast",      "_mm_aesenclast_si128"},
{"llvm.x86.aesni.aesdec",          "_mm_aesdec_si128"},
{"llvm.x86.aesni.aesdeclast",      "_mm_aesdeclast_si128"},
{"llvm.x86.aesni.aesimc",          "_mm_aesimc_si128"},
{"llvm.x86.aesni.aeskeygenassist", "_mm_aeskeygenassist_si128"},

// x86 CLMUL / SHA
{"llvm.x86.pclmulqdq",             "_mm_clmulepi64_si128"},
{"llvm.x86.sha1rnds4",             "_mm_sha1rnds4_epu32"},
{"llvm.x86.sha1nexte",             "_mm_sha1nexte_epu32"},
{"llvm.x86.sha1msg1",              "_mm_sha1msg1_epu32"},
{"llvm.x86.sha1msg2",              "_mm_sha1msg2_epu32"},
{"llvm.x86.sha256rnds2",           "_mm_sha256rnds2_epu32"},
{"llvm.x86.sha256msg1",            "_mm_sha256msg1_epu32"},
{"llvm.x86.sha256msg2",            "_mm_sha256msg2_epu32"},

// x86 AVX2
{"llvm.x86.avx2.permd",            "_mm256_permutevar8x32_epi32"},

// x86 BMI
{"llvm.x86.bmi.pdep.32",           "_pdep_u32"},
{"llvm.x86.bmi.pdep.64",           "_pdep_u64"},
{"llvm.x86.bmi.pext.32",           "_pext_u32"},
{"llvm.x86.bmi.pext.64",           "_pext_u64"},

// x86 transactional execution
{"llvm.x86.xabort",               "_xabort"},
{"llvm.x86.xtest",                "_xtest"},

    // clang-format on
