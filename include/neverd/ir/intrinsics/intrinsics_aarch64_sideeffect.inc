// AArch64 side-effect intrinsics — included by Intrinsics.cpp.
// Macro: SIDEEFFECT(IntrinsicId)
//
// These intrinsics have observable side effects and must not be
// eliminated as dead even when their result is unused.

// barriers
SIDEEFFECT(Intrinsic::Dmb);
SIDEEFFECT(Intrinsic::Dsb);
SIDEEFFECT(Intrinsic::Isb);
// hints
SIDEEFFECT(Intrinsic::Yield_A64);
SIDEEFFECT(Intrinsic::Wfe);
SIDEEFFECT(Intrinsic::Wfi);
SIDEEFFECT(Intrinsic::Sev);
SIDEEFFECT(Intrinsic::Sevl);
// traps / system
SIDEEFFECT(Intrinsic::Brk);
SIDEEFFECT(Intrinsic::Hlt_A64);
SIDEEFFECT(Intrinsic::Svc);
SIDEEFFECT(Intrinsic::Hvc);
SIDEEFFECT(Intrinsic::Smc);
SIDEEFFECT(Intrinsic::Eret);
SIDEEFFECT(Intrinsic::A64_Clrex);
SIDEEFFECT(Intrinsic::A64_SetFPSR);
SIDEEFFECT(Intrinsic::A64_SetFPCR);
// WSP writes directly mutate the architectural stack pointer.
SIDEEFFECT(Intrinsic::A64_WspWrite);
SIDEEFFECT(Intrinsic::A64_WspZeroExtend);
// MTE allocation-tag stores mutate tag memory.
SIDEEFFECT(Intrinsic::Stg);
// cache / TLB
SIDEEFFECT(Intrinsic::Dc);
SIDEEFFECT(Intrinsic::Ic);
SIDEEFFECT(Intrinsic::Tlbi);
SIDEEFFECT(Intrinsic::At);

// FEAT_MOPS mutates memory and carries implementation-defined progress state.
SIDEEFFECT(Intrinsic::A64_MopsSetP);
SIDEEFFECT(Intrinsic::A64_MopsSetPN);
SIDEEFFECT(Intrinsic::A64_MopsSetPT);
SIDEEFFECT(Intrinsic::A64_MopsSetPTN);
SIDEEFFECT(Intrinsic::A64_MopsCpyFP);

// Atomic read-check-write operations mutate memory even when their returned
// old pair is unused.
SIDEEFFECT(Intrinsic::A64_Rcwcasp);
SIDEEFFECT(Intrinsic::A64_Rcwcaspa);
SIDEEFFECT(Intrinsic::A64_Rcwcaspal);
SIDEEFFECT(Intrinsic::A64_Rcwcaspl);
SIDEEFFECT(Intrinsic::A64_Rcwscasp);
SIDEEFFECT(Intrinsic::A64_Rcwscaspa);
SIDEEFFECT(Intrinsic::A64_Rcwscaspal);
SIDEEFFECT(Intrinsic::A64_Rcwscaspl);

// LSE128 pair bit-clear mutates the addressed 16-byte object even if the old
// pair returned in the two source registers is unused.
SIDEEFFECT(Intrinsic::A64_Ldclrp);
SIDEEFFECT(Intrinsic::A64_Ldclrpa);
SIDEEFFECT(Intrinsic::A64_Ldclrpal);
SIDEEFFECT(Intrinsic::A64_Ldclrpl);

// Exclusive loads establish the local monitor; exclusive stores consume it
// and may mutate memory.  Both effects are observable even if outputs die.
SIDEEFFECT(Intrinsic::A64_Ldxr);
SIDEEFFECT(Intrinsic::A64_Ldaxr);
SIDEEFFECT(Intrinsic::A64_Stxr);
SIDEEFFECT(Intrinsic::A64_Stlxr);
SIDEEFFECT(Intrinsic::A64_Ldxp);
SIDEEFFECT(Intrinsic::A64_Ldaxp);
SIDEEFFECT(Intrinsic::A64_Stxp);
SIDEEFFECT(Intrinsic::A64_Stlxp);
