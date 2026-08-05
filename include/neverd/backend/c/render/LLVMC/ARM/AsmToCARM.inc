//===-- AsmToCARM.inc - ARM asm to C intrinsic table ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===-------------------------------------------------------------===//
//
// ARM/AArch64 inline asm string -> C/ACLE intrinsic expression mapping.
// Included by LLVMCIntrinsicRender.cpp.
// Macro: M(asm_string, c_expression_with_parens)

// Data/instruction barrier
M("dmb ish",     "__dmb(0xB)")
M("dmb sy",      "__dmb(0xF)")
M("dmb",         "__dmb(0xF)")
M("dsb ish",     "__dsb(0xB)")
M("dsb sy",      "__dsb(0xF)")
M("dsb",         "__dsb(0xF)")
M("isb",         "__isb(0xF)")
M("isb sy",      "__isb(0xF)")

// Hint
M("yield",       "__yield()")
M("wfe",         "__wfe()")
M("wfi",         "__wfi()")
M("sev",         "__sev()")
M("sevl",        "__sevl()")

// Trap / debug
M("bkpt",        "__builtin_debugtrap()")
M("hlt",         "__builtin_trap()")
M("udf",         "__builtin_trap()")

// Exclusive
M("clrex",       "__clrex()")

// NOP (lifter usually eliminates, but catch residuals)
M("nop",         "__nop()")
