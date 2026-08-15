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
