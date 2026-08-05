// ARM32 side-effect intrinsics — included by Intrinsics.cpp.
// Macro: SIDEEFFECT(IntrinsicId)
//
// These intrinsics have observable side effects and must not be
// eliminated as dead even when their result is unused.

// barriers
SIDEEFFECT(Intrinsic::ArmDmb);
SIDEEFFECT(Intrinsic::ArmDsb);
SIDEEFFECT(Intrinsic::ArmIsb);
// traps
SIDEEFFECT(Intrinsic::ArmSvc);
SIDEEFFECT(Intrinsic::ArmHvc);
SIDEEFFECT(Intrinsic::ArmSmc);
SIDEEFFECT(Intrinsic::ArmBkpt);
SIDEEFFECT(Intrinsic::ArmHlt);
SIDEEFFECT(Intrinsic::ArmUdf);
// exclusive / atomic
SIDEEFFECT(Intrinsic::ArmClrex);
