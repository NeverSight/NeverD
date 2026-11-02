//===- MedSmcccConvention.cpp - SMC Calling Convention --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Arm's SMC Calling Convention (DEN0028) passes an SMC or HVC #0 its function
// identifier in w0/r0 and its arguments in x1-x7/r1-r7, and returns its
// results in x0-x3/r0-r3.  On ARM the procedure call standard preserves
// r4-r7, so a function's own values of them are no arguments it can name;
// a call is taken to read r0-r3, as PSCI's and most others' do.
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedSyscallConvention.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/ARM/ARMRegs.h"

namespace neverd {

namespace {
constexpr uint64_t AArch64Arguments[] = {a64reg::X0, a64reg::X1, a64reg::X2,
                                         a64reg::X3, a64reg::X4, a64reg::X5,
                                         a64reg::X6, a64reg::X7};
constexpr uint64_t ARMArguments[] = {armreg::R0, armreg::R1, armreg::R2,
                                     armreg::R3};
} // namespace

llvm::ArrayRef<uint64_t> smcccArgumentRegisters(Arch TheArch, Intrinsic Lifted,
                                                uint64_t Immediate) {
  if (Immediate != 0)
    return {};
  if (TheArch == Arch::AArch64 &&
      (Lifted == Intrinsic::Smc || Lifted == Intrinsic::Hvc))
    return AArch64Arguments;
  if (TheArch == Arch::ARM &&
      (Lifted == Intrinsic::ArmSmc || Lifted == Intrinsic::ArmHvc))
    return ARMArguments;
  return {};
}

} // namespace neverd
