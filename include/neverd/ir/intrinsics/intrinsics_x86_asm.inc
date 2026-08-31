// x86 assembly mnemonics — included by intrinsics.cpp.
// Only for intrinsics with NO standard C builtin.

A(Intrinsic::Syscall,      "syscall");
A(Intrinsic::Int1,         "int1");
A(Intrinsic::IntN,         "int");
A(Intrinsic::Into,         "into");
A(Intrinsic::Rdmsr,        "rdmsr");
A(Intrinsic::Wrmsr,        "wrmsr");
A(Intrinsic::Clflushopt,   "clflushopt");
A(Intrinsic::Clwb,         "clwb");
A(Intrinsic::In,           "inb");
A(Intrinsic::Out,          "outb");
A(Intrinsic::Insb,         "insb");
A(Intrinsic::Insw,         "insw");
A(Intrinsic::Insd,         "insd");
A(Intrinsic::Outsb,        "outsb");
A(Intrinsic::Outsw,        "outsw");
A(Intrinsic::Outsd,        "outsd");
A(Intrinsic::Cli,          "cli");
A(Intrinsic::Sti,          "sti");
A(Intrinsic::Invd,         "invd");
A(Intrinsic::Invlpg,       "invlpg");
A(Intrinsic::Swapgs,       "swapgs");
A(Intrinsic::Vmcall,       "vmcall");
A(Intrinsic::Nop,          "nop");
A(Intrinsic::Ud2,          "ud2");
A(Intrinsic::Wrpkru,       "wrpkru");
A(Intrinsic::Rdpkru,       "rdpkru");
A(Intrinsic::Wbinvd,       "wbinvd");
A(Intrinsic::Fxsave,       "fxsave");
A(Intrinsic::Fxrstor,      "fxrstor");
A(Intrinsic::Lgdt,         "lgdt");
A(Intrinsic::Lidt,         "lidt");
A(Intrinsic::Lldt,         "lldt");
A(Intrinsic::Sgdt,         "sgdt");
A(Intrinsic::Sidt,         "sidt");
A(Intrinsic::Sldt,         "sldt");
A(Intrinsic::Lmsw,         "lmsw");
A(Intrinsic::Smsw,         "smsw");
A(Intrinsic::Ltr,          "ltr");
A(Intrinsic::Str,          "str");
A(Intrinsic::Vmmcall,      "vmmcall");
A(Intrinsic::Rsm,          "rsm");
A(Intrinsic::Lodsb,        "lodsb");
A(Intrinsic::Lodsw,        "lodsw");
A(Intrinsic::Lodsd,        "lodsd");
A(Intrinsic::Lodsq,        "lodsq");
A(Intrinsic::Cmpsb,        "cmpsb");
A(Intrinsic::Cmpsw,        "cmpsw");
A(Intrinsic::Cmpsd_str,    "cmpsd");
A(Intrinsic::Cmpsq,        "cmpsq");
A(Intrinsic::Scasb,        "scasb");
A(Intrinsic::Scasw,        "scasw");
A(Intrinsic::Scasd,        "scasd");
A(Intrinsic::Scasq,        "scasq");
A(Intrinsic::Xlatb,        "xlatb");
A(Intrinsic::X87Op,        "x87");
A(Intrinsic::X87Fninit,    "fninit");
A(Intrinsic::X87Fnclex,    "fnclex");
A(Intrinsic::Xbegin,       "xbegin");
A(Intrinsic::Xend,         "xend");
A(Intrinsic::Xabort,       "xabort");
A(Intrinsic::Xtest,        "xtest");
A(Intrinsic::Lcall,        "lcall");
A(Intrinsic::Ljmp,         "ljmp");
A(Intrinsic::Retf,         "retf");
A(Intrinsic::Iret,         "iretq");
A(Intrinsic::Pushf,        "pushfq");
A(Intrinsic::Popf,         "popfq");
A(Intrinsic::Enter,        "enter");
A(Intrinsic::Leave,        "leave");
A(Intrinsic::Ud0,          "ud0");
A(Intrinsic::Ud1,          "ud1");
A(Intrinsic::Clac,         "clac");
A(Intrinsic::Stac,         "stac");
A(Intrinsic::Clzero,       "clzero");
A(Intrinsic::Wbnoinvd,     "wbnoinvd");
A(Intrinsic::Aaa,          "aaa");
A(Intrinsic::Aas,          "aas");
A(Intrinsic::Aad,          "aad");
A(Intrinsic::Aam,          "aam");
A(Intrinsic::Daa,          "daa");
A(Intrinsic::Das,          "das");
A(Intrinsic::Bound,        "bound");

// MPX (deprecated — all no-op)
A(Intrinsic::BndMk,        "bndmk");
A(Intrinsic::BndMov,       "bndmov");
A(Intrinsic::BndCl,        "bndcl");
A(Intrinsic::BndCu,        "bndcu");
A(Intrinsic::BndCn,        "bndcn");
A(Intrinsic::BndLdx,       "bndldx");
A(Intrinsic::BndStx,       "bndstx");

// TSX prefix (no-op)
A(Intrinsic::Xacquire,     "xacquire");
A(Intrinsic::Xrelease,     "xrelease");

// SHA catch-all
A(Intrinsic::ShaGeneric,   "sha");

// CET
A(Intrinsic::CetIncSsp,    "incsspq");
A(Intrinsic::CetRdSsp,     "rdsspq");
A(Intrinsic::CetSaveprevssp, "saveprevssp");
A(Intrinsic::CetRstorssp,  "rstorssp");
A(Intrinsic::CetWrss,      "wrssq");
A(Intrinsic::CetWruss,     "wrussq");
A(Intrinsic::CetSetssbsy,  "setssbsy");
A(Intrinsic::CetClrssbsy,  "clrssbsy");
A(Intrinsic::Enqcmd,       "enqcmd");
A(Intrinsic::Enqcmds,      "enqcmds");

// LWP
A(Intrinsic::Llwpcb,       "llwpcb");
A(Intrinsic::Slwpcb,       "slwpcb");
A(Intrinsic::Lwpins,       "lwpins");
A(Intrinsic::Lwpval,       "lwpval");

// VIA PadLock
A(Intrinsic::Ptwrite,      "ptwrite");
A(Intrinsic::Xcryptecb,    "xcryptecb");
A(Intrinsic::Xcryptcbc,    "xcryptcbc");
A(Intrinsic::Xcryptctr,    "xcryptctr");
A(Intrinsic::Xcryptcfb,    "xcryptcfb");
A(Intrinsic::Xcryptofb,    "xcryptofb");
A(Intrinsic::Xsha1,        "xsha1");
A(Intrinsic::Xsha256,      "xsha256");
A(Intrinsic::Montmul,      "montmul");
A(Intrinsic::Xstore,       "xstore");

// xsave/xrstor
A(Intrinsic::Xsave64,      "xsave64");
A(Intrinsic::Xrstor64,     "xrstor64");
A(Intrinsic::Xsaves64,     "xsaves64");
A(Intrinsic::Xrstors64,    "xrstors64");
A(Intrinsic::Xsavec64,     "xsavec64");
A(Intrinsic::Xsaveopt64,   "xsaveopt64");

// SMAP
A(Intrinsic::Cldemote,     "cldemote");
A(Intrinsic::Pconfig,      "pconfig");
A(Intrinsic::Sysexitq,     "sysexitq");
A(Intrinsic::Sysretq,      "sysretq");
A(Intrinsic::Extrq,        "extrq");
A(Intrinsic::Insertq,      "insertq");
A(Intrinsic::Invlpga,      "invlpga");

// Operand-carrying memory instructions (append-only intrinsic IDs).
A(Intrinsic::PrefetchT0,    "prefetcht0");
A(Intrinsic::PrefetchT1,    "prefetcht1");
A(Intrinsic::PrefetchT2,    "prefetcht2");
A(Intrinsic::PrefetchNta,   "prefetchnta");
A(Intrinsic::PrefetchW,     "prefetchw");
A(Intrinsic::PrefetchWT1,   "prefetchwt1");
A(Intrinsic::Xsave,         "xsave");
A(Intrinsic::Xsavec,        "xsavec");
A(Intrinsic::Xsaves,        "xsaves");
A(Intrinsic::Xsaveopt,      "xsaveopt");
A(Intrinsic::Xrstor,        "xrstor");
A(Intrinsic::Xrstors,       "xrstors");
A(Intrinsic::Fxsave64Mem,   "fxsave64");
A(Intrinsic::Fxrstor64Mem,  "fxrstor64");
A(Intrinsic::X87Fldenv,     "fldenv");
A(Intrinsic::X87Fnstenv,    "fnstenv");
A(Intrinsic::X87Frstor,     "frstor");
A(Intrinsic::X87Fnsave,     "fnsave");
