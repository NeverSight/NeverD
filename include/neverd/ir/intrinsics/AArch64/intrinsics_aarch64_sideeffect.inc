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
SIDEEFFECT(Intrinsic::Udf_A64);
SIDEEFFECT(Intrinsic::Svc);
SIDEEFFECT(Intrinsic::Hvc);
SIDEEFFECT(Intrinsic::Smc);
SIDEEFFECT(Intrinsic::Eret);
SIDEEFFECT(Intrinsic::A64_Clrex);
SIDEEFFECT(Intrinsic::A64_SetFPSR);
SIDEEFFECT(Intrinsic::A64_SetFPCR);
// Predicated SVE stores mutate only their active memory lanes.
SIDEEFFECT(Intrinsic::A64_SveSt1);
SIDEEFFECT(Intrinsic::A64_SveStnt1);
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

// LSE128 pair bit-set mutates the addressed 16-byte object even if the old
// pair returned in the two source registers is unused.
SIDEEFFECT(Intrinsic::A64_Ldsetp);
SIDEEFFECT(Intrinsic::A64_Ldsetpa);
SIDEEFFECT(Intrinsic::A64_Ldsetpal);
SIDEEFFECT(Intrinsic::A64_Ldsetpl);

// Scalar LSE RMWs mutate memory even when the returned old value is discarded
// by an ST* alias or dead in the surrounding function.
SIDEEFFECT(Intrinsic::A64_AtomicAnd);
SIDEEFFECT(Intrinsic::A64_AtomicOr);
SIDEEFFECT(Intrinsic::A64_AtomicXor);
SIDEEFFECT(Intrinsic::A64_AtomicSmax);
SIDEEFFECT(Intrinsic::A64_AtomicSmin);
SIDEEFFECT(Intrinsic::A64_AtomicUmax);
SIDEEFFECT(Intrinsic::A64_AtomicUmin);
SIDEEFFECT(Intrinsic::A64LinuxSyscall);
// Authentication may fault even when no data-flow consumer uses its result.
#define NEVERD_A64_PAUTH_EFFECT_true(Id) SIDEEFFECT(Intrinsic::Id);
#define NEVERD_A64_PAUTH_EFFECT_false(Id)
#define NEVERD_A64_PAUTH_OPERATION(Id, Insn, Mnemonic, Key, Strip, Auth)       \
  NEVERD_A64_PAUTH_EFFECT_##Auth(Id)
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.def"
#undef NEVERD_A64_PAUTH_EFFECT_true
#undef NEVERD_A64_PAUTH_EFFECT_false
SIDEEFFECT(Intrinsic::A64_AuthBranch);
// Checked arithmetic observes architectural enable controls.
#define NEVERD_A64_CPA(Id, ...) SIDEEFFECT(Intrinsic::Id);
#include "neverd/ir/intrinsics/AArch64/AArch64CPA.def"

#define NEVERD_A64_PAUTH_LR_OPERATION(Id, Mnemonic, ...)                       \
  SIDEEFFECT(Intrinsic::Id);
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLR.def"

#define NEVERD_A64_PAUTH_M_OPERATION(Id, Mnemonic, ...)                        \
  SIDEEFFECT(Intrinsic::Id);
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthM.def"
SIDEEFFECT(Intrinsic::A64_PACMPending);

#define NEVERD_A64_PAUTH_LOAD_OPERATION(Id, ...) SIDEEFFECT(Intrinsic::Id);
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLoad.def"

#define NEVERD_A64_ADDRESS_MEMORY_OPERATION(Id, ...) SIDEEFFECT(Intrinsic::Id);
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.def"

// Pointer-check controls are observable even when the result is discarded.
#define NEVERD_A64_SVE_CPA_OPERATION(Id, ...) SIDEEFFECT(Intrinsic::Id);
#include "neverd/ir/intrinsics/AArch64/AArch64SVECPA.def"

#define NEVERD_A64_WRITEBACK_OPERATION(Id, Mnemonic) SIDEEFFECT(Intrinsic::Id);
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.def"
