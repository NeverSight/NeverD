// ARM 32-bit intrinsic enum entries — included by intrinsics.h.
// Range: 11000 – 15999.  New entries go at the bottom.

// Supervisor / debug.
ArmSvc = 11000,
ArmHvc, ArmSmc, ArmBkpt, ArmHlt, ArmUdf,

// System register.
ArmMsr, ArmMrs,
ArmAut, ArmAutg, ArmBxaut,

// CDE (Custom Datapath Extension).
ArmCx1, ArmCx1a, ArmCx1d, ArmCx1da,
ArmCx2, ArmCx2a, ArmCx2d, ArmCx2da,
ArmCx3, ArmCx3a, ArmCx3d, ArmCx3da,

// CDE vector.
ArmVcx1, ArmVcx1a, ArmVcx2, ArmVcx2a, ArmVcx3, ArmVcx3a,

// FP status.
ArmVmrs, ArmVmsr,

// MVE / Helium.
ArmVscclrm, ArmVpst, ArmVpt, ArmLe, ArmLetp, ArmBfcsel,

// Bit manipulation.
ArmRbit,

// CRC32.
ArmCrc32b, ArmCrc32h, ArmCrc32w,
ArmCrc32cb, ArmCrc32ch, ArmCrc32cw,

// Saturating arithmetic.
ArmSsat, ArmUsat,

// Byte select.
ArmSel,

// NEON intrinsics.
ArmVrecpe, ArmVrecps, ArmVrsqrte, ArmVrsqrts,
ArmVswp, ArmVtrn, ArmVuzp, ArmVzip,
ArmVdup, ArmVtbl, ArmVtbx, ArmVext,
ArmVqmovn, ArmVqmovun, ArmVqadd, ArmVqsub,

// NEON saturating doubling multiply + saturating/rounding shifts (mapped to
// LLVM ARM NEON intrinsics).
ArmVqdmulh, ArmVqrdmulh, ArmVqdmull, ArmVmulp, ArmVmullp,
ArmVqshifts, ArmVqshiftu, ArmVqshiftsu, ArmVqrshifts, ArmVqrshiftu,
ArmVqshiftns, ArmVqshiftnu, ArmVqshiftnsu,
ArmVqrshiftns, ArmVqrshiftnu, ArmVqrshiftnsu,
// Non-saturating rounding variable shift (VRSHL register form).
ArmVrshifts, ArmVrshiftu,

// Exclusive / atomic access.
ArmLdrex, ArmLdrexb, ArmLdrexh, ArmLdrexd,
ArmStrex, ArmStrexb, ArmStrexh, ArmStrexd,
ArmClrex,
ArmLda, ArmLdab, ArmLdah,
ArmLdaex, ArmLdaexb, ArmLdaexh, ArmLdaexd,
ArmStlex, ArmStlexb, ArmStlexh, ArmStlexd,
ArmStl, ArmStlb, ArmStlh,
ArmSwp, ArmSwpb,

// Crypto.
ArmAesd, ArmAese, ArmAesmc, ArmAesimc,
ArmSha1c, ArmSha1h, ArmSha1m, ArmSha1p, ArmSha1su0, ArmSha1su1,
ArmSha256h, ArmSha256h2, ArmSha256su0, ArmSha256su1,

// Barriers.
ArmDmb, ArmDsb, ArmIsb,

// Coprocessor.
ArmMcr, ArmMcr2, ArmMcrr, ArmMcrr2,
ArmMrc, ArmMrc2, ArmMrrc, ArmMrrc2,
ArmCdp, ArmCdp2,
ArmLdc, ArmLdc2, ArmLdc2l,
ArmStc, ArmStc2, ArmStc2l,
