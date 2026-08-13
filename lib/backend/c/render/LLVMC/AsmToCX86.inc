//===-- AsmToCX86.inc - x86 asm to C intrinsic table ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===-------------------------------------------------------------===//
//
// x86/x64 inline asm string -> C/MSVC intrinsic name mapping.
// Included by LLVMCIntrinsicRenderX86.cpp.
// Macro: M(asm_string, c_function_name)

// System
M("rdmsr",       "__readmsr")
M("wrmsr",       "__writemsr")
M("rdtsc",       "__rdtsc")
M("rdtscp",      "__rdtscp")
M("rdpmc",       "__rdpmc")

// Control / privilege
M("cli",         "_disable")
M("sti",         "_enable")
M("invlpg",      "__invlpg")
M("swapgs",      "__swapgs")
M("wbinvd",      "_wbinvd")
M("vmcall",      "__vmx_vmcall")
M("vmmcall",     "__vmx_vmcall")
M("wrpkru",      "_wrpkru")
M("rdpkru",      "_rdpkru")

// Barriers / hints
M("nop",         "__nop")
M("pause",       "_mm_pause")
M("mfence",      "_mm_mfence")
M("lfence",      "_mm_lfence")
M("sfence",      "_mm_sfence")
M("clflush",     "_mm_clflush")
M("clflushopt",  "_mm_clflushopt")
M("clwb",        "_mm_clwb")

// Save / restore
M("fxsave64",    "_fxsave64")
M("fxrstor64",   "_fxrstor64")
M("ldmxcsr",     "_mm_setcsr")
M("stmxcsr",     "_mm_getcsr")

// Traps / debug
M("int3",        "__debugbreak")
M("ud2",         "__ud2")
M("hlt",         "__halt")

// I/O port
M("inb",         "__inbyte")
M("outb",        "__outbyte")
M("insb",        "__inbytestring")
M("outsb",       "__outbytestring")
M("insw",        "__inwordstring")
M("outsw",       "__outwordstring")
M("insd",        "__indwordstring")
M("outsd",       "__outdwordstring")

// TSX (Transactional Synchronization Extensions)
M("xbegin",      "_xbegin")
M("xend",        "_xend")
M("xtest",       "_xtest")

// xsave / xrstor
M("xsave64",     "_xsave64")
M("xrstor64",    "_xrstor64")
M("xsaves64",    "_xsaves64")
M("xrstors64",   "_xrstors64")
M("xsavec64",    "_xsavec64")
M("xsaveopt64",  "_xsaveopt64")

// CET (Control-flow Enforcement Technology)
M("incsspq",     "_inc_ssp")
M("rdsspq",      "_rdsspq")

// REP string ops
M("rep movsb",   "__movsb")
M("rep stosb",   "__stosb")
M("rep movsw",   "__movsw")
M("rep stosw",   "__stosw")
M("rep movsd",   "__movsd")
M("rep stosd",   "__stosd")
M("rep movsq",   "__movsq")
M("rep stosq",   "__stosq")
