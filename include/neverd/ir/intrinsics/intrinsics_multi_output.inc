// Multi-output intrinsic metadata — included by intrinsics.cpp.
// Macro: MULTI_OUT(IntrinsicId, output_count)
//
// output_count = number of register-level outputs beyond the primary return.
// Used by cf_structurer to recognize the INTRINSIC + COPY-clobber pattern
// and by emitters to render proper multi-output C code.
// The counts are aligned in a column, which clang-format would undo.
// clang-format off

// x86/x64
MULTI_OUT(Intrinsic::Cpuid,   4);  // eax, ebx, ecx, edx
MULTI_OUT(Intrinsic::Rdtsc,   2);  // eax(lo), edx(hi)
MULTI_OUT(Intrinsic::Rdtscp,  3);  // eax(lo), edx(hi), ecx(aux)
MULTI_OUT(Intrinsic::Xgetbv,  2);  // eax(lo), edx(hi)

// REP CMPS/SCAS return the leftover count as their primary result and one
// auxiliary LAHF/SETO flag snapshot.  The lifter unpacks the latter into the
// architectural arithmetic flags without re-reading the compared memory.
MULTI_OUT(Intrinsic::Cmpsb, 1);
MULTI_OUT(Intrinsic::Cmpsw, 1);
MULTI_OUT(Intrinsic::Cmpsd_str, 1);
MULTI_OUT(Intrinsic::Cmpsq, 1);
MULTI_OUT(Intrinsic::Scasb, 1);
MULTI_OUT(Intrinsic::Scasw, 1);
MULTI_OUT(Intrinsic::Scasd, 1);
MULTI_OUT(Intrinsic::Scasq, 1);

// AArch64 FEAT_MOPS set prologue: destination, remaining count, NZCV.
// SMC and HVC #0 under the SMC Calling Convention: x0-x3 (r0-r3), which
// the lift binds only for that immediate (smcccArgumentRegisters).
MULTI_OUT(Intrinsic::Smc,    4);
MULTI_OUT(Intrinsic::Hvc,    4);
MULTI_OUT(Intrinsic::ArmSmc, 4);
MULTI_OUT(Intrinsic::ArmHvc, 4);

MULTI_OUT(Intrinsic::A64_MopsSetP,   3);
MULTI_OUT(Intrinsic::A64_MopsSetPN,  3);
MULTI_OUT(Intrinsic::A64_MopsSetPT,  3);
MULTI_OUT(Intrinsic::A64_MopsSetPTN, 3);

// AArch64 FEAT_MOPS copy prologue: destination, source, count, NZCV.
MULTI_OUT(Intrinsic::A64_MopsCpyFP, 4);

// SGX: rBX and the two-byte CF/ZF completion; rAX is the primary result.
MULTI_OUT(Intrinsic::X86Encls, 2);
MULTI_OUT(Intrinsic::X86Enclv, 2);

#define NEVERD_X86_REP_STATE(Id, Name, Words, Compare, ...) MULTI_OUT(Intrinsic::Id, Words - 1 + Compare);
#include "neverd/ir/intrinsics/X86/X86RepState.def"
#undef NEVERD_X86_REP_STATE

MULTI_OUT(Intrinsic::X86EncluPending, 4);
MULTI_OUT(Intrinsic::X86GetsecPending, 4);
MULTI_OUT(Intrinsic::X86EncluBounded, 4);
MULTI_OUT(Intrinsic::X86GetsecBounded, 4);

#define NEVERD_A64_WRITEBACK_MULTI_OUTPUT(Id, Count) MULTI_OUT(Intrinsic::Id, Count);
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.def"
