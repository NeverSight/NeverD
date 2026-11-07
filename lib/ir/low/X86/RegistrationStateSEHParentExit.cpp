//===- RegistrationStateSEHParentExit.cpp - Abandoned CRT guard lifetime -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Requires source chain repair before reentering exception dispatch.

#include "RegistrationStateSolver.h"

#include "neverd/lift/X86/X86Regs.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::canReconnectSEHParentChain(
    const Domain &State) const {
  return !KnownCxx && !Chain.RealignedFrame && State.Parent &&
         !State.Callback && !State.Unknown && State.Installed &&
         !State.Uninstalled && !State.Levels.empty() &&
         State.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset ==
             0 &&
         State.Frame.load(*Chain.RegistrationOffset, 4).PreviousChain;
}

bool RegistrationStateSolver::validateSEHParentChainUse(
    const Domain &State, const LowOp &Op, const FrameTransfer &Transfer) const {
  if (!State.AbandonedSEHGuard)
    return true;
  switch (Op.Opcode) {
  case NdOp::CALL:
    return CookieChecks.count({Op.Addr, Op.Seq}) != 0;
  case NdOp::INDIR_CALL:
  case NdOp::INTRINSIC:
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
    return false;
  default:
    break;
  }
  const auto Memory = lowMemoryOperands(Op);
  if (!Memory.Address)
    return true;
  if (!Memory.Complete)
    return false;
  if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS)
    return Op.Opcode == NdOp::STORE;
  const auto Address = Transfer.read(*Memory.Address);
  if (Address.preferredAddress())
    return *Address.preferredAddress() != 0 &&
           uint64_t(*Address.preferredAddress()) + Memory.AccessSize <=
               uint64_t(UINT32_MAX) + 1;
  const auto SP =
      State.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
  return Address.Offset && SP && *Address.Offset >= *SP &&
         int64_t(*Address.Offset) + Memory.AccessSize <= 4;
}

} // namespace neverd::registration_state
