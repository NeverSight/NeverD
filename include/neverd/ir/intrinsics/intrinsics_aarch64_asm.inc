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

// FEAT_MOPS memory-set prologues (multi-output; rendered specially).
A(Intrinsic::A64_MopsSetP,   "setp");
A(Intrinsic::A64_MopsSetPN,  "setpn");
A(Intrinsic::A64_MopsSetPT,  "setpt");
A(Intrinsic::A64_MopsSetPTN, "setptn");
A(Intrinsic::A64_MopsCpyFP,  "cpyfp");

// FEAT_THE + FEAT_D128 read-check-write pair compare-and-swap.
A(Intrinsic::A64_Rcwcasp,    "rcwcasp");
A(Intrinsic::A64_Rcwcaspa,   "rcwcaspa");
A(Intrinsic::A64_Rcwcaspal,  "rcwcaspal");
A(Intrinsic::A64_Rcwcaspl,   "rcwcaspl");
A(Intrinsic::A64_Rcwscasp,   "rcwscasp");
A(Intrinsic::A64_Rcwscaspa,  "rcwscaspa");
A(Intrinsic::A64_Rcwscaspal, "rcwscaspal");
A(Intrinsic::A64_Rcwscaspl,  "rcwscaspl");

// FEAT_LSE128 pair bit-clear (rendered as standard atomics in High-C).
A(Intrinsic::A64_Ldclrp,      "ldclrp");
A(Intrinsic::A64_Ldclrpa,     "ldclrpa");
A(Intrinsic::A64_Ldclrpal,    "ldclrpal");
A(Intrinsic::A64_Ldclrpl,     "ldclrpl");

// Exclusive monitor operations (rendered as compiler builtins in High-C).
A(Intrinsic::A64_Ldxr,         "ldxr");
A(Intrinsic::A64_Ldaxr,        "ldaxr");
A(Intrinsic::A64_Stxr,         "stxr");
A(Intrinsic::A64_Stlxr,        "stlxr");
A(Intrinsic::A64_Ldxp,         "ldxp");
A(Intrinsic::A64_Ldaxp,        "ldaxp");
A(Intrinsic::A64_Stxp,         "stxp");
A(Intrinsic::A64_Stlxp,        "stlxp");
A(Intrinsic::A64_Pacga,        "pacga");
A(Intrinsic::A64_SveCntb,      "cntb");
A(Intrinsic::A64_SveCnth,      "cnth");
A(Intrinsic::A64_SveCntw,      "cntw");
A(Intrinsic::A64_SveCntd,      "cntd");
A(Intrinsic::A64_SvePtrue,     "ptrue");
A(Intrinsic::A64_SveDup,       "dup");
A(Intrinsic::A64_SveLastb,     "lastb");
