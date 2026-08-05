// ARM32 assembly mnemonics — included by intrinsics.cpp.

A(Intrinsic::ArmSvc,       "svc");
A(Intrinsic::ArmHvc,       "hvc");
A(Intrinsic::ArmSmc,       "smc");
A(Intrinsic::ArmBkpt,      "bkpt");
A(Intrinsic::ArmHlt,       "hlt");
A(Intrinsic::ArmUdf,       "udf");
A(Intrinsic::ArmMsr,       "msr");
A(Intrinsic::ArmMrs,       "mrs");
A(Intrinsic::ArmLdrex,     "ldrex");
A(Intrinsic::ArmStrex,     "strex");
A(Intrinsic::ArmClrex,     "clrex");
A(Intrinsic::ArmSwp,       "swp");
A(Intrinsic::ArmMcr,       "mcr");
A(Intrinsic::ArmMrc,       "mrc");
A(Intrinsic::ArmCdp,       "cdp");
A(Intrinsic::ArmLdc,       "ldc");
A(Intrinsic::ArmStc,       "stc");
A(Intrinsic::ArmVmrs,      "vmrs");
A(Intrinsic::ArmVmsr,      "vmsr");
A(Intrinsic::ArmSsat,      "ssat");
A(Intrinsic::ArmUsat,      "usat");
A(Intrinsic::ArmSel,       "sel");

// PAC
A(Intrinsic::ArmAut,       "aut");
A(Intrinsic::ArmAutg,      "autg");
A(Intrinsic::ArmBxaut,     "bxaut");

// CDE (Custom Datapath Extension)
A(Intrinsic::ArmCx1,       "cx1");
A(Intrinsic::ArmCx1a,      "cx1a");
A(Intrinsic::ArmCx1d,      "cx1d");
A(Intrinsic::ArmCx1da,     "cx1da");
A(Intrinsic::ArmCx2,       "cx2");
A(Intrinsic::ArmCx2a,      "cx2a");
A(Intrinsic::ArmCx2d,      "cx2d");
A(Intrinsic::ArmCx2da,     "cx2da");
A(Intrinsic::ArmCx3,       "cx3");
A(Intrinsic::ArmCx3a,      "cx3a");
A(Intrinsic::ArmCx3d,      "cx3d");
A(Intrinsic::ArmCx3da,     "cx3da");

// CDE vector
A(Intrinsic::ArmVcx1,      "vcx1");
A(Intrinsic::ArmVcx1a,     "vcx1a");
A(Intrinsic::ArmVcx2,      "vcx2");
A(Intrinsic::ArmVcx2a,     "vcx2a");
A(Intrinsic::ArmVcx3,      "vcx3");
A(Intrinsic::ArmVcx3a,     "vcx3a");

// MVE / Helium
A(Intrinsic::ArmVscclrm,   "vscclrm");
A(Intrinsic::ArmVpst,      "vpst");
A(Intrinsic::ArmVpt,       "vpt");
A(Intrinsic::ArmLe,        "le");
A(Intrinsic::ArmLetp,      "letp");
A(Intrinsic::ArmBfcsel,    "bfcsel");

// Exclusive access variants
A(Intrinsic::ArmLdrexb,    "ldrexb");
A(Intrinsic::ArmLdrexh,    "ldrexh");
A(Intrinsic::ArmLdrexd,    "ldrexd");
A(Intrinsic::ArmStrexb,    "strexb");
A(Intrinsic::ArmStrexh,    "strexh");
A(Intrinsic::ArmStrexd,    "strexd");
A(Intrinsic::ArmLda,       "lda");
A(Intrinsic::ArmLdab,      "ldab");
A(Intrinsic::ArmLdah,      "ldah");
A(Intrinsic::ArmLdaex,     "ldaex");
A(Intrinsic::ArmLdaexb,    "ldaexb");
A(Intrinsic::ArmLdaexh,    "ldaexh");
A(Intrinsic::ArmLdaexd,    "ldaexd");
A(Intrinsic::ArmStl,       "stl");
A(Intrinsic::ArmStlb,      "stlb");
A(Intrinsic::ArmStlh,      "stlh");
A(Intrinsic::ArmStlex,     "stlex");
A(Intrinsic::ArmStlexb,    "stlexb");
A(Intrinsic::ArmStlexh,    "stlexh");
A(Intrinsic::ArmStlexd,    "stlexd");
A(Intrinsic::ArmSwpb,      "swpb");

// Coprocessor variants
A(Intrinsic::ArmMcr2,      "mcr2");
A(Intrinsic::ArmMcrr,      "mcrr");
A(Intrinsic::ArmMcrr2,     "mcrr2");
A(Intrinsic::ArmMrc2,      "mrc2");
A(Intrinsic::ArmMrrc,      "mrrc");
A(Intrinsic::ArmMrrc2,     "mrrc2");
A(Intrinsic::ArmCdp2,      "cdp2");
A(Intrinsic::ArmLdc2,      "ldc2");
A(Intrinsic::ArmLdc2l,     "ldc2l");
A(Intrinsic::ArmStc2,      "stc2");
A(Intrinsic::ArmStc2l,     "stc2l");
