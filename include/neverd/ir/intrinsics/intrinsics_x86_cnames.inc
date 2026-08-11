// x86 standard C intrinsic names — included by intrinsics.cpp.

// SSE/AVX shuffle & unpack
C(Intrinsic::Pshufb,           "_mm_shuffle_epi8");
C(Intrinsic::Pshufd,           "_mm_shuffle_epi32");
C(Intrinsic::Pshufhw,          "_mm_shufflehi_epi16");
C(Intrinsic::Pshuflw,          "_mm_shufflelo_epi16");
C(Intrinsic::Palignr,          "_mm_alignr_epi8");
C(Intrinsic::Shufps,           "_mm_shuffle_ps");
C(Intrinsic::Shufpd,           "_mm_shuffle_pd");
C(Intrinsic::Punpckhqdq,       "_mm_unpackhi_epi64");
C(Intrinsic::Punpcklqdq,       "_mm_unpacklo_epi64");
C(Intrinsic::Punpckhdq,        "_mm_unpackhi_epi32");
C(Intrinsic::Punpckldq,        "_mm_unpacklo_epi32");
C(Intrinsic::Punpckhwd,        "_mm_unpackhi_epi16");
C(Intrinsic::Punpcklwd,        "_mm_unpacklo_epi16");
C(Intrinsic::Punpckhbw,        "_mm_unpackhi_epi8");
C(Intrinsic::Punpcklbw,        "_mm_unpacklo_epi8");
C(Intrinsic::Unpckhps,         "_mm_unpackhi_ps");
C(Intrinsic::Unpcklps,         "_mm_unpacklo_ps");
C(Intrinsic::Unpckhpd,         "_mm_unpackhi_pd");
C(Intrinsic::Unpcklpd,         "_mm_unpacklo_pd");

// Packed shift
C(Intrinsic::PsllD,            "_mm_slli_epi32");
C(Intrinsic::PsllQ,            "_mm_slli_epi64");
C(Intrinsic::PsllW,            "_mm_slli_epi16");
C(Intrinsic::PsrlD,            "_mm_srli_epi32");
C(Intrinsic::PsrlQ,            "_mm_srli_epi64");
C(Intrinsic::PsrlW,            "_mm_srli_epi16");
C(Intrinsic::PsraD,            "_mm_srai_epi32");
C(Intrinsic::PsraW,            "_mm_srai_epi16");

// AES-NI
C(Intrinsic::AesEnc,           "_mm_aesenc_si128");
C(Intrinsic::AesEncLast,       "_mm_aesenclast_si128");
C(Intrinsic::AesDec,           "_mm_aesdec_si128");
C(Intrinsic::AesDecLast,       "_mm_aesdeclast_si128");
C(Intrinsic::AesImc,           "_mm_aesimc_si128");
C(Intrinsic::AesKeyGenAssist,  "_mm_aeskeygenassist_si128");

// CLMUL / SHA
C(Intrinsic::Pclmulqdq,        "_mm_clmulepi64_si128");
C(Intrinsic::Sha1Rnds4,        "_mm_sha1rnds4_epu32");
C(Intrinsic::Sha1Nexte,        "_mm_sha1nexte_epu32");
C(Intrinsic::Sha1Msg1,         "_mm_sha1msg1_epu32");
C(Intrinsic::Sha1Msg2,         "_mm_sha1msg2_epu32");
C(Intrinsic::Sha256Rnds2,      "_mm_sha256rnds2_epu32");
C(Intrinsic::Sha256Msg1,       "_mm_sha256msg1_epu32");
C(Intrinsic::Sha256Msg2,       "_mm_sha256msg2_epu32");
C(Intrinsic::Gf2p8MulB,        "_mm_gf2p8mul_epi8");
C(Intrinsic::Gf2p8AffineQb,    "_mm_gf2p8affine_epi64_epi8");
C(Intrinsic::Gf2p8AffineInvQb, "_mm_gf2p8affineinv_epi64_epi8");

// SSE misc
C(Intrinsic::Pmovmskb,         "_mm_movemask_epi8");
C(Intrinsic::Dpps,             "_mm_dp_ps");
C(Intrinsic::Mpsadbw,          "_mm_mpsadbw_epu8");
C(Intrinsic::Pcmpistri,        "_mm_cmpistri");
C(Intrinsic::Pcmpistrm,        "_mm_cmpistrm");
C(Intrinsic::Pcmpestri,        "_mm_cmpestri");
C(Intrinsic::Pcmpestrm,        "_mm_cmpestrm");
C(Intrinsic::PcmpistrFlag,     "_mm_cmpistr_flag");
C(Intrinsic::PcmpestrFlag,     "_mm_cmpestr_flag");
C(Intrinsic::Movddup,          "_mm_movedup_pd");

// AVX broadcast
C(Intrinsic::BroadcastSS,      "_mm_broadcastss_ps");
C(Intrinsic::BroadcastSD,      "_mm_broadcastsd_pd");
C(Intrinsic::BroadcastB,       "_mm_broadcastb_epi8");
C(Intrinsic::BroadcastW,       "_mm_broadcastw_epi16");
C(Intrinsic::BroadcastD,       "_mm_broadcastd_epi32");
C(Intrinsic::BroadcastQ,       "_mm_broadcastq_epi64");
C(Intrinsic::Blendd,           "_mm_blend_epi32");
C(Intrinsic::Perm2f128,        "_mm256_permute2f128_si256");
C(Intrinsic::Permd,            "_mm256_permutevar8x32_epi32");

// AVX-512 / advanced
C(Intrinsic::Phminposuw,       "_mm_minpos_epu16");
C(Intrinsic::MaskedLoadD,      "_mm_maskload_epi32");
C(Intrinsic::MaskedLoadQ,      "_mm_maskload_epi64");
C(Intrinsic::MaskedStoreD,     "_mm_maskstore_epi32");
C(Intrinsic::MaskedStoreQ,     "_mm_maskstore_epi64");

// BMI
C(Intrinsic::Pdep,             "_pdep_u64");
C(Intrinsic::Pext,             "_pext_u64");

// Cache control (additional)
C(Intrinsic::Clflushopt,      "_mm_clflushopt");
C(Intrinsic::Clwb,            "_mm_clwb");

// TSX
C(Intrinsic::Xbegin,          "_xbegin");
C(Intrinsic::Xend,            "_xend");
C(Intrinsic::Xtest,           "_xtest");

// xsave/xrstor
C(Intrinsic::Xsave64,         "_xsave64");
C(Intrinsic::Xrstor64,        "_xrstor64");
C(Intrinsic::Xsaves64,        "_xsaves64");
C(Intrinsic::Xrstors64,       "_xrstors64");
C(Intrinsic::Xsavec64,        "_xsavec64");
C(Intrinsic::Xsaveopt64,      "_xsaveopt64");

// CET
C(Intrinsic::CetIncSsp,       "_inc_ssp");
C(Intrinsic::CetRdSsp,        "_rdsspq");

// System / control
C(Intrinsic::Cpuid,            "__cpuid");
C(Intrinsic::Rdtsc,            "__rdtsc");
C(Intrinsic::Rdtscp,           "__rdtscp");
C(Intrinsic::Rdpmc,            "__rdpmc");
C(Intrinsic::Xgetbv,           "_xgetbv");
C(Intrinsic::Hlt,              "__halt");
C(Intrinsic::Int3,             "__debugbreak");
C(Intrinsic::Ud2,              "__ud2");
C(Intrinsic::Wbinvd,           "_wbinvd");
C(Intrinsic::Fxsave,           "_fxsave64");
C(Intrinsic::Fxrstor,          "_fxrstor64");
C(Intrinsic::Rdmsr,            "__readmsr");
C(Intrinsic::Wrmsr,            "__writemsr");
C(Intrinsic::Invlpg,           "__invlpg");
C(Intrinsic::Cli,              "_disable");
C(Intrinsic::Sti,              "_enable");
C(Intrinsic::Vmcall,           "__vmx_vmcall");
C(Intrinsic::Swapgs,           "__swapgs");
C(Intrinsic::Nop,              "__nop");
C(Intrinsic::In,               "__inbyte");
C(Intrinsic::Out,              "__outbyte");
C(Intrinsic::Insb,             "__inbytestring");
C(Intrinsic::Outsb,            "__outbytestring");
C(Intrinsic::Wrpkru,           "_wrpkru");
C(Intrinsic::Rdpkru,           "_rdpkru");
C(Intrinsic::Pause,            "_mm_pause");
C(Intrinsic::Mfence,           "_mm_mfence");
C(Intrinsic::Lfence,           "_mm_lfence");
C(Intrinsic::Sfence,           "_mm_sfence");
C(Intrinsic::Clflush,          "_mm_clflush");
C(Intrinsic::Prefetch,         "_mm_prefetch");
C(Intrinsic::Ldmxcsr,          "_mm_setcsr");
C(Intrinsic::Stmxcsr,          "_mm_getcsr");

// String ops
C(Intrinsic::Movsb,            "__movsb");
C(Intrinsic::Stosb,            "__stosb");
C(Intrinsic::Movsw,            "__movsw");
C(Intrinsic::Stosw,            "__stosw");
C(Intrinsic::Movsd,            "__movsd");
C(Intrinsic::Stosd,            "__stosd");
C(Intrinsic::Movsq,            "__movsq");
C(Intrinsic::Stosq,            "__stosq");

// AVX-512 advanced
C(Intrinsic::Vpternlog,        "_mm512_ternarylogic_epi32");
C(Intrinsic::Kunpck,           "_kunpckbw");
C(Intrinsic::Vpcompress,       "_mm512_mask_compress_epi32");
C(Intrinsic::Vpexpand,         "_mm512_mask_expand_epi32");
C(Intrinsic::Vpconflict,       "_mm512_conflict_epi32");
C(Intrinsic::Vpscatter,        "_mm512_i32scatter_epi32");
C(Intrinsic::Vpgather,         "_mm512_i32gather_epi32");
C(Intrinsic::VGather,          "_mm256_i32gather_epi32");
C(Intrinsic::VScatter,         "_mm256_i32scatter_epi32");
C(Intrinsic::Vpmultishiftqb,   "_mm512_multishift_epi64_epi8");
C(Intrinsic::Vgetexp,          "_mm512_getexp_ps");
C(Intrinsic::Vgetmant,         "_mm512_getmant_ps");
C(Intrinsic::Vfixupimm,        "_mm512_fixupimm_ps");
C(Intrinsic::Vfpclass,         "_mm512_fpclass_ps_mask");
C(Intrinsic::Vscalef,          "_mm512_scalef_ps");
C(Intrinsic::Vexp2,            "_mm512_exp2a23_ps");
C(Intrinsic::Vreduce,          "_mm512_reduce_ps");
C(Intrinsic::Vdbpsadbw,        "_mm512_dbsad_epu8");
C(Intrinsic::Vpshufbitqmb,     "_mm512_bitshuffle_epi64_mask");
C(Intrinsic::Vrange,           "_mm512_range_ps");
C(Intrinsic::PackedShift,      "_mm_sll_epi32");
C(Intrinsic::AesGeneric,       "_mm_aesenc_si128");

// SSE3 duplicate
C(Intrinsic::Movshdup,         "_mm_movehdup_ps");
C(Intrinsic::Movsldup,         "_mm_moveldup_ps");

// MMX legacy
C(Intrinsic::Pshufw,           "_mm_shuffle_pi16");
