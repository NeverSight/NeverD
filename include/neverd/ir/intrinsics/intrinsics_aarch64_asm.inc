// AArch64 assembly mnemonics — included by intrinsics.cpp.
// Only for intrinsics with NO standard C/ACLE builtin.

A(Intrinsic::Svc,          "svc");
A(Intrinsic::Hvc,          "hvc");
A(Intrinsic::Smc,          "smc");
A(Intrinsic::Brk,          "brk");
A(Intrinsic::Hlt_A64,      "hlt");
A(Intrinsic::Mrs,          "mrs");
A(Intrinsic::Msr,          "msr");
A(Intrinsic::Dc,           "dc");
A(Intrinsic::Ic,           "ic");
A(Intrinsic::Tlbi,         "tlbi");
A(Intrinsic::At,           "at");
A(Intrinsic::Eret,         "eret");
A(Intrinsic::Pacda,        "pacda");
A(Intrinsic::Pacdb,        "pacdb");
A(Intrinsic::Autda,        "autda");
A(Intrinsic::Autdb,        "autdb");
A(Intrinsic::Xpaci,        "xpaci");
A(Intrinsic::Xpacd,        "xpacd");
A(Intrinsic::Irg,          "irg");
A(Intrinsic::Gmi,          "gmi");
A(Intrinsic::Addg,         "addg");
A(Intrinsic::Subg,         "subg");
A(Intrinsic::Stg,          "stg");
A(Intrinsic::Stzg,         "stzg");
A(Intrinsic::St2g,         "st2g");
A(Intrinsic::Stz2g,        "stz2g");
A(Intrinsic::Ldg,          "ldg");
A(Intrinsic::CntRead,      "mrs");

// Advanced system
A(Intrinsic::A64_Brb,      "brb");
A(Intrinsic::A64_Gcssttr,  "gcssttr");
A(Intrinsic::A64_Gcsstr,   "gcsstr");
A(Intrinsic::A64_Genter,   "genter");
A(Intrinsic::A64_Gexit,    "gexit");
A(Intrinsic::A64_Tstart,   "tstart");
A(Intrinsic::A64_Tcommit,  "tcommit");
A(Intrinsic::A64_Tcancel,  "tcancel");
A(Intrinsic::A64_Ttest,    "ttest");
A(Intrinsic::A64_Wkdmc,    "wkdmc");
A(Intrinsic::A64_Wkdmd,    "wkdmd");
A(Intrinsic::A64_Wrffr,    "wrffr");
A(Intrinsic::A64_Setffr,   "setffr");
A(Intrinsic::A64_Xaflag,   "xaflag");
A(Intrinsic::A64_Axflag,   "axflag");
A(Intrinsic::A64_Rmif,     "rmif");
A(Intrinsic::A64_Clrex,    "clrex");
A(Intrinsic::A64_Wfet,     "wfet");
A(Intrinsic::A64_Wfit,     "wfit");
