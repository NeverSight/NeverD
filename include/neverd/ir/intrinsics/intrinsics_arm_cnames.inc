// ARM32 standard C intrinsic names — included by intrinsics.cpp.

// NEON crypto
C(Intrinsic::ArmAesd,          "vaesdq_u8");
C(Intrinsic::ArmAese,          "vaeseq_u8");
C(Intrinsic::ArmAesmc,         "vaesmcq_u8");
C(Intrinsic::ArmAesimc,        "vaesimcq_u8");
C(Intrinsic::ArmSha1c,         "vsha1cq_u32");
C(Intrinsic::ArmSha1h,         "vsha1h_u32");
C(Intrinsic::ArmSha1m,         "vsha1mq_u32");
C(Intrinsic::ArmSha1p,         "vsha1pq_u32");
C(Intrinsic::ArmSha1su0,       "vsha1su0q_u32");
C(Intrinsic::ArmSha1su1,       "vsha1su1q_u32");
C(Intrinsic::ArmSha256h,       "vsha256hq_u32");
C(Intrinsic::ArmSha256h2,      "vsha256h2q_u32");
C(Intrinsic::ArmSha256su0,     "vsha256su0q_u32");
C(Intrinsic::ArmSha256su1,     "vsha256su1q_u32");

// NEON misc
C(Intrinsic::ArmVrecpe,        "vrecpeq");
C(Intrinsic::ArmVrecps,        "vrecpsq");
C(Intrinsic::ArmVrsqrte,       "vrsqrteq");
C(Intrinsic::ArmVrsqrts,       "vrsqrtsq");
C(Intrinsic::ArmVext,          "vextq");
C(Intrinsic::ArmVtbl,          "vtbl1");
C(Intrinsic::ArmVtbx,          "vtbx1");
C(Intrinsic::ArmVdup,          "vdupq");

// NEON narrow / saturating
C(Intrinsic::ArmVqmovn,        "vqmovnq");
C(Intrinsic::ArmVqmovun,       "vqmovunq");
C(Intrinsic::ArmVqadd,         "vqaddq");
C(Intrinsic::ArmVqsub,         "vqsubq");
C(Intrinsic::ArmVmulp,         "vmul_p8");
C(Intrinsic::ArmVmullp,        "vmull_p8");
C(Intrinsic::ArmVswp,          "vswpq");
C(Intrinsic::ArmVtrn,          "vtrnq");
C(Intrinsic::ArmVuzp,          "vuzpq");
C(Intrinsic::ArmVzip,          "vzipq");

// Trap / debug
C(Intrinsic::ArmBkpt,          "__builtin_debugtrap");
C(Intrinsic::ArmHlt,           "__builtin_trap");
C(Intrinsic::ArmClrex,         "__clrex");

// Barriers (ACLE)
C(Intrinsic::ArmDmb,           "__dmb");
C(Intrinsic::ArmDsb,           "__dsb");
C(Intrinsic::ArmIsb,           "__isb");

// Byte select (ACLE)
C(Intrinsic::ArmSel,           "__sel");

// Saturating (ACLE)
C(Intrinsic::ArmSsat,          "__ssat");
C(Intrinsic::ArmUsat,          "__usat");

// CRC
C(Intrinsic::ArmCrc32b,        "__crc32b");
C(Intrinsic::ArmCrc32h,        "__crc32h");
C(Intrinsic::ArmCrc32w,        "__crc32w");
C(Intrinsic::ArmCrc32cb,       "__crc32cb");
C(Intrinsic::ArmCrc32ch,       "__crc32ch");
C(Intrinsic::ArmCrc32cw,       "__crc32cw");
C(Intrinsic::ArmRbit,          "__rbit");
