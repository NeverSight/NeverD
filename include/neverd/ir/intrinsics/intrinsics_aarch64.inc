// AArch64 intrinsic enum entries — included by intrinsics.h.
// Range: 6000 – 10999.  New entries go at the bottom.

// Supervisor / hypervisor / secure-monitor calls.
Svc = 6000,
Hvc, Smc,

// Debug / breakpoint.
Brk, Hlt_A64,

// Memory barriers.
Dmb, Dsb, Isb,

// System register access.
Mrs, Msr,

// Cache / TLB maintenance.
Dc, Ic, Tlbi, At,

// Yield / wait.
Yield_A64, Wfe, Wfi, Sev, Sevl,

// Counter/timer.
CntRead,

// Exception return.
Eret,

// Pointer authentication (ARMv8.3).
Pacia, Pacib, Pacda, Pacdb,
Autia, Autib, Autda, Autdb,
Xpaci, Xpacd,

// Memory tagging (ARMv8.5-MTE).
Irg, Gmi, Addg, Subg,
Stg, Stzg, St2g, Stz2g, Ldg,

// Flag manipulation.
A64_Xaflag, A64_Rmif, A64_Axflag,

// System / performance.
A64_Brb, A64_Wfet, A64_Wfit, A64_Clrex,

// TME.
A64_Tstart, A64_Tcommit, A64_Tcancel, A64_Ttest,

// GCS.
A64_Gcssttr, A64_Gcsstr, A64_Genter, A64_Gexit,

// SVE.
A64_Wrffr, A64_Setffr,

// Misc system.
A64_Wkdmc, A64_Wkdmd,

// Bit manipulation.
A64_Rbit, A64_Rev16, A64_Rev32, A64_Rev64,

// NEON / SIMD intrinsics.
A64_Ext, A64_Tbl, A64_Tbx,
A64_Xtn, A64_Sqxtn, A64_Uqxtn, A64_Sqxtun,
A64_Addv, A64_Saddlv, A64_Uaddlv,
A64_Smaxv, A64_Umaxv, A64_Sminv, A64_Uminv,
A64_Fmaxv, A64_Fminv, A64_Fmaxnmv, A64_Fminnmv,
A64_Saddlp, A64_Uaddlp, A64_Sadalp, A64_Uadalp,
A64_Frecpe, A64_Frsqrte, A64_Urecpe, A64_Ursqrte,
A64_Pmull, A64_Pmul, A64_Cls,

// NEON saturating / halving ops.
A64Sqadd, A64Uqadd, A64Sqsub, A64Uqsub,
A64Shadd, A64Srhadd, A64Uhadd, A64Urhadd,
A64Smax, A64Umax, A64Smin, A64Umin,
A64Sabd, A64Uabd, A64Addp,

// NEON saturating / rounding shifts (mapped to LLVM AArch64 NEON intrinsics).
A64_Sqshl, A64_Uqshl, A64_Sqshlu, A64_Sqrshl, A64_Uqrshl, A64_Srshl, A64_Urshl,
A64_Sqshrn, A64_Sqrshrn, A64_Uqshrn, A64_Sqshrun, A64_Sqrshrun, A64_Uqrshrn,

// NEON permute.
A64_Trn1, A64_Trn2,
A64_Zip1, A64_Zip2,
A64_Uzp1, A64_Uzp2,

// NEON reciprocal step.
A64_Frecps, A64_Frsqrts,

// FP multiply-extended (FMULX): like FMUL but 0*Inf = +/-2.0; per-lane.
A64_Fmulx,

// FJCVTZS (FEAT_JSCVT): JavaScript double->int32 convert -- round toward zero
// with modulo-2^32 wrap on overflow (NaN/Inf -> 0).  Maps to
// llvm.aarch64.fjcvtzs so codegen emits the real `fjcvtzs` (plain FPToSI
// saturates instead of wrapping).
A64_Fjcvtzs,

// FCVTXN/FCVTXN2: FP inexact narrowing f64->f32 with round-to-ODD (jamming),
// used for double-rounding-safe narrowing.  Maps to the scalar
// llvm.aarch64.sisd.fcvtxn / vector llvm.aarch64.neon.fcvtxn so codegen emits
// the real `fcvtxn` (plain FLOAT_FLOAT2FLOAT rounds to nearest-even instead).
A64_Fcvtxn,

// CRC32 (ARMv8.0-CRC).
A64_Crc32b, A64_Crc32h, A64_Crc32w, A64_Crc32x,
A64_Crc32cb, A64_Crc32ch, A64_Crc32cw, A64_Crc32cx,

// Crypto — AES (FEAT_AES) / SHA1 (FEAT_SHA1) / SHA256 (FEAT_SHA2).  Each maps
// to the matching LLVM AArch64 crypto intrinsic so codegen emits the real
// aese/aesmc/sha1c/sha256h instruction (Unicorn executes it bit-exactly).
A64_Aese, A64_Aesd, A64_Aesmc, A64_Aesimc,
A64_Sha1c, A64_Sha1p, A64_Sha1m, A64_Sha1h, A64_Sha1su0, A64_Sha1su1,
A64_Sha256h, A64_Sha256h2, A64_Sha256su0, A64_Sha256su1,

// Crypto — SHA512 (FEAT_SHA512) / SM3 (FEAT_SM3) / SM4 (FEAT_SM4), ARMv8.2.
// Each maps to the matching LLVM AArch64 crypto intrinsic so codegen emits the
// real sha512h/sm3partw1/sm4e instruction (Unicorn's MAX CPU executes it).
A64_Sha512h, A64_Sha512h2, A64_Sha512su0, A64_Sha512su1,
A64_Sm3partw1, A64_Sm3partw2, A64_Sm3ss1,
A64_Sm3tt1a, A64_Sm3tt1b, A64_Sm3tt2a, A64_Sm3tt2b,
A64_Sm4e, A64_Sm4ekey,

// Scalar FP16 fixed-point conversions.  These require one architectural
// rounding step; decomposing them into a half conversion plus a half multiply
// overflows and can also double-round.
A64_ScvtfFixed, A64_UcvtfFixed, A64_FcvtzsFixed, A64_FcvtzuFixed,

// Floating-point status register access.  These map to LLVM's architectural
// FPSR get/set intrinsics so reads, explicit writes, and cumulative QC state
// survive recompilation.
A64_GetFPSR, A64_SetFPSR,

// FEAT_MOPS memory-set prologue.  The operation has implementation-defined
// progress, so preserve the architectural instruction and all writebacks.
A64_MopsSetP, A64_MopsSetPN, A64_MopsSetPT, A64_MopsSetPTN,

// FEAT_MOPS forward-copy prologue.  Like SETP, this instruction has
// implementation-defined progress and updates all three operand registers.
A64_MopsCpyFP,

// FEAT_FAMINMAX absolute floating-point vector min/max.  The trailing lifter
// operands carry the active vector width and element width for typed emission.
A64_Famax, A64_Famin,

// FEAT_THE + FEAT_D128 read-check-write pair compare-and-swap.  Keep the
// architectural ordering and checked/strong variant explicit so both LLVM
// and High-C emission can reproduce the original instruction.
A64_Rcwcasp, A64_Rcwcaspa, A64_Rcwcaspal, A64_Rcwcaspl,
A64_Rcwscasp, A64_Rcwscaspa, A64_Rcwscaspal, A64_Rcwscaspl,

// Scalar FP reciprocal exponent.  LLVM and ACLE both provide dedicated
// FRECPX intrinsics; keep it distinct from the reciprocal estimate family.
A64_Frecpx,

// BF16 matrix multiply-accumulate.  The destination is four f32 accumulators;
// both multiplicands are eight packed bfloat16 lanes.
A64_Bfmmla,

// FPCR-directed scalar rounding and control-register access.  FRINTI follows
// the runtime FPCR rounding mode, so it must remain distinct from FRINTN.
A64_Frinti, A64_GetFPCR, A64_SetFPCR,

// FEAT_LSE128 pair bit-clear.  LLVM lowers these to an i128 atomicrmw and
// High-C renders the standard __atomic_fetch_and builtin; the distinct IDs
// retain the architectural acquire/release ordering through textual IR.
A64_Ldclrp, A64_Ldclrpa, A64_Ldclrpal, A64_Ldclrpl,

// Exclusive monitor operations.  These map directly to LLVM's architectural
// AArch64 intrinsics and to the standard Arm compiler builtins in High-C.
A64_Ldxr, A64_Ldaxr, A64_Stxr, A64_Stlxr,
A64_Ldxp, A64_Ldaxp, A64_Stxp, A64_Stlxp,

// Generic pointer authentication code.  Keep this appended so existing
// serialized intrinsic IDs remain stable.
A64_Pacga,

// Runtime SVE element counts.  These map to LLVM's architectural SVE count
// intrinsics and to the standard ACLE svcnt* functions in High-C.
A64_SveCntb, A64_SveCnth, A64_SveCntw, A64_SveCntd,

// Runtime-width SVE register-state operations.
A64_SvePtrue, A64_SveDup, A64_SveLastb, A64_SveSt1, A64_SveStnt1,

// Architectural 32-bit stack-pointer write (zero-extends into SP).
A64_WspWrite,
A64_WspZeroExtend,
