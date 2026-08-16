// AArch64 standard C intrinsic names — included by intrinsics.cpp.

// Barriers
C(Intrinsic::Dmb,              "__dmb");
C(Intrinsic::Dsb,              "__dsb");
C(Intrinsic::Isb,              "__isb");

// NEON permute / table
C(Intrinsic::A64_Ext,          "vextq");
C(Intrinsic::A64_Tbl,          "vqtbl1q");
C(Intrinsic::A64_Tbx,          "vqtbx1q");
C(Intrinsic::A64_Xtn,          "vmovn");
C(Intrinsic::A64_Trn1,         "vtrn1q");
C(Intrinsic::A64_Trn2,         "vtrn2q");
C(Intrinsic::A64_Zip1,         "vzip1q");
C(Intrinsic::A64_Zip2,         "vzip2q");
C(Intrinsic::A64_Uzp1,         "vuzp1q");
C(Intrinsic::A64_Uzp2,         "vuzp2q");

// NEON arithmetic / reduce
C(Intrinsic::A64Shadd,         "vhaddq_s");
C(Intrinsic::A64Srhadd,        "vrhaddq_s");
C(Intrinsic::A64Uhadd,         "vhaddq_u");
C(Intrinsic::A64Urhadd,        "vrhaddq_u");
C(Intrinsic::A64_Addv,         "vaddvq");
C(Intrinsic::A64_Saddlp,       "vpaddlq_s");
C(Intrinsic::A64_Uaddlp,       "vpaddlq_u");
C(Intrinsic::A64_Smaxv,        "vmaxvq_s");
C(Intrinsic::A64_Umaxv,        "vmaxvq_u");
C(Intrinsic::A64_Sminv,        "vminvq_s");
C(Intrinsic::A64_Uminv,        "vminvq_u");
C(Intrinsic::A64_Fmaxv,        "vmaxvq_f");
C(Intrinsic::A64_Fminv,        "vminvq_f");
C(Intrinsic::A64_Fmaxnmv,     "vmaxnmvq_f32");
C(Intrinsic::A64_Fminnmv,     "vminnmvq_f32");
C(Intrinsic::A64_Saddlv,       "vaddlvq_s");
C(Intrinsic::A64_Uaddlv,       "vaddlvq_u");
C(Intrinsic::A64_Sadalp,       "vpadalq_s");
C(Intrinsic::A64_Uadalp,       "vpadalq_u");
C(Intrinsic::A64_Sqxtn,        "vqmovnq_s");
C(Intrinsic::A64_Uqxtn,        "vqmovnq_u");
C(Intrinsic::A64_Sqxtun,       "vqmovunq_s");
C(Intrinsic::A64_Urecpe,       "vrecpeq_u");
C(Intrinsic::A64_Ursqrte,      "vrsqrteq_u");
C(Intrinsic::A64_Cls,          "vclsq");
C(Intrinsic::A64Addp,          "vpaddq");
C(Intrinsic::A64Sqadd,         "vqaddq_s");
C(Intrinsic::A64Uqadd,         "vqaddq_u");
C(Intrinsic::A64Sqsub,         "vqsubq_s");
C(Intrinsic::A64Uqsub,         "vqsubq_u");
C(Intrinsic::A64Smax,          "vmaxq_s");
C(Intrinsic::A64Umax,          "vmaxq_u");
C(Intrinsic::A64Smin,          "vminq_s");
C(Intrinsic::A64Umin,          "vminq_u");
C(Intrinsic::A64Sabd,          "vabdq_s");
C(Intrinsic::A64Uabd,          "vabdq_u");

// NEON bit / reverse
C(Intrinsic::A64_Rbit,         "vrbitq");
C(Intrinsic::A64_Rev16,        "vrev16q");
C(Intrinsic::A64_Rev32,        "vrev32q");
C(Intrinsic::A64_Rev64,        "vrev64q");

// NEON reciprocal / sqrt estimate
C(Intrinsic::A64_Frecpe,       "vrecpeq");
C(Intrinsic::A64_Frecpx,       "vrecpxs_f32");
C(Intrinsic::A64_Bfmmla, "vbfmmlaq_f32");
C(Intrinsic::A64_Frsqrte,      "vrsqrteq");
C(Intrinsic::A64_Frecps,       "vrecpsq");
C(Intrinsic::A64_Frsqrts,      "vrsqrtsq");
C(Intrinsic::A64_Fmulx,        "vmulxq");
C(Intrinsic::A64_Fjcvtzs,      "__jcvt");
C(Intrinsic::A64_Fcvtxn,       "vcvtx_f32");
C(Intrinsic::A64_ScvtfFixed,   "__builtin_arm_scvtf_fixed");
C(Intrinsic::A64_UcvtfFixed,   "__builtin_arm_ucvtf_fixed");
C(Intrinsic::A64_FcvtzsFixed,  "__builtin_arm_fcvtzs_fixed");
C(Intrinsic::A64_FcvtzuFixed,  "__builtin_arm_fcvtzu_fixed");
C(Intrinsic::A64_GetFPSR,       "__builtin_arm_rsr64");
C(Intrinsic::A64_SetFPSR,       "__builtin_arm_wsr64");
C(Intrinsic::A64_Frinti,        "__builtin_elementwise_nearbyint");
C(Intrinsic::A64_GetFPCR,       "__builtin_arm_rsr64");
C(Intrinsic::A64_SetFPCR,       "__builtin_arm_wsr64");
C(Intrinsic::A64_Pmull,        "vmull_p64");
C(Intrinsic::A64_Pmul,         "vmul_p8");

// Crypto — AES / SHA1 / SHA256
C(Intrinsic::A64_Aese,         "vaeseq_u8");
C(Intrinsic::A64_Aesd,         "vaesdq_u8");
C(Intrinsic::A64_Aesmc,        "vaesmcq_u8");
C(Intrinsic::A64_Aesimc,       "vaesimcq_u8");
C(Intrinsic::A64_Sha1c,        "vsha1cq_u32");
C(Intrinsic::A64_Sha1p,        "vsha1pq_u32");
C(Intrinsic::A64_Sha1m,        "vsha1mq_u32");
C(Intrinsic::A64_Sha1h,        "vsha1h_u32");
C(Intrinsic::A64_Sha1su0,      "vsha1su0q_u32");
C(Intrinsic::A64_Sha1su1,      "vsha1su1q_u32");
C(Intrinsic::A64_Sha256h,      "vsha256hq_u32");
C(Intrinsic::A64_Sha256h2,     "vsha256h2q_u32");
C(Intrinsic::A64_Sha256su0,    "vsha256su0q_u32");
C(Intrinsic::A64_Sha256su1,    "vsha256su1q_u32");

// Crypto — SHA512 / SM3 / SM4 (ARMv8.2)
C(Intrinsic::A64_Sha512h,      "vsha512hq_u64");
C(Intrinsic::A64_Sha512h2,     "vsha512h2q_u64");
C(Intrinsic::A64_Sha512su0,    "vsha512su0q_u64");
C(Intrinsic::A64_Sha512su1,    "vsha512su1q_u64");
C(Intrinsic::A64_Sm3partw1,    "vsm3partw1q_u32");
C(Intrinsic::A64_Sm3partw2,    "vsm3partw2q_u32");
C(Intrinsic::A64_Sm3ss1,       "vsm3ss1q_u32");
C(Intrinsic::A64_Sm3tt1a,      "vsm3tt1aq_u32");
C(Intrinsic::A64_Sm3tt1b,      "vsm3tt1bq_u32");
C(Intrinsic::A64_Sm3tt2a,      "vsm3tt2aq_u32");
C(Intrinsic::A64_Sm3tt2b,      "vsm3tt2bq_u32");
C(Intrinsic::A64_Sm4e,         "vsm4eq_u32");
C(Intrinsic::A64_Sm4ekey,      "vsm4ekeyq_u32");

// FEAT_FAMINMAX is rendered width-specifically by HighCIntrinsicRenderARM.
C(Intrinsic::A64_Famax,        "vamax");
C(Intrinsic::A64_Famin,        "vamin");

// FEAT_THE + FEAT_D128 pair compare-and-swap builtins provided by the paired
// Clang toolchain.
C(Intrinsic::A64_Rcwcasp,      "__builtin_arm_rcwcasp");
C(Intrinsic::A64_Rcwcaspa,     "__builtin_arm_rcwcaspa");
C(Intrinsic::A64_Rcwcaspal,    "__builtin_arm_rcwcaspal");
C(Intrinsic::A64_Rcwcaspl,     "__builtin_arm_rcwcaspl");
C(Intrinsic::A64_Rcwscasp,     "__builtin_arm_rcwscasp");
C(Intrinsic::A64_Rcwscaspa,    "__builtin_arm_rcwscaspa");
C(Intrinsic::A64_Rcwscaspal,   "__builtin_arm_rcwscaspal");
C(Intrinsic::A64_Rcwscaspl,    "__builtin_arm_rcwscaspl");

// Memory tagging
C(Intrinsic::Stg,              "__arm_mte_set_tag");
C(Intrinsic::Ldg,              "__arm_mte_get_tag");

// System
C(Intrinsic::Yield_A64,        "__yield");
C(Intrinsic::Wfe,              "__wfe");
C(Intrinsic::Wfi,              "__wfi");
C(Intrinsic::Sev,              "__sev");
C(Intrinsic::Sevl,             "__sevl");

// Trap / debug
C(Intrinsic::Brk,              "__builtin_debugtrap");
C(Intrinsic::Hlt_A64,          "__builtin_trap");
C(Intrinsic::A64_Clrex,        "__clrex");

// PAC (ARMv8.3 Pointer Authentication)
C(Intrinsic::Pacia,            "__pacia");
C(Intrinsic::Pacib,            "__pacib");
C(Intrinsic::Pacda,            "__pacda");
C(Intrinsic::Pacdb,            "__pacdb");
C(Intrinsic::Autia,            "__autia");
C(Intrinsic::Autib,            "__autib");
C(Intrinsic::Autda,            "__autda");
C(Intrinsic::Autdb,            "__autdb");
C(Intrinsic::Xpaci,            "__xpaci");
C(Intrinsic::Xpacd,            "__xpacd");
C(Intrinsic::A64_Pacga,        "__builtin_arm_pacga");
C(Intrinsic::A64_SveCntb,      "svcntb");
C(Intrinsic::A64_SveCnth,      "svcnth");
C(Intrinsic::A64_SveCntw,      "svcntw");
C(Intrinsic::A64_SveCntd,      "svcntd");
C(Intrinsic::A64_SvePtrue,     "svptrue_b8");
C(Intrinsic::A64_SveDup,       "svdup_n_u8");
C(Intrinsic::A64_SveLastb,     "svlastb_u8");
C(Intrinsic::A64_WspWrite,     "__builtin_arm_wsp_write");
C(Intrinsic::A64_WspZeroExtend, "__builtin_arm_wsp_zero_extend");
