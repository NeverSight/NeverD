//===- RegistrationEntryABI.cpp - PE32 parent return contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate parent returns independently of the CRT's callback returns.
//===----------------------------------------------------------------------===//
#include "RegistrationABIPrivate.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include <map>

namespace neverd::registration_abi {
std::optional<uint16_t> parentPopBytes(const LowFunc &Function,
                                       const BinaryImage &Image) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || !Function.ExceptionMetadata ||
      !Function.ExceptionMetadata->Cxx ||
      !Function.ExceptionMetadata->Registration ||
      Function.Entry != Function.ExceptionMetadata->CodeRange.Begin ||
      !Function.hasCompleteLiftCoverage() || !Function.RegistrationStates)
    return std::nullopt;
  const auto &States = *Function.RegistrationStates;
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete ||
      !States.CxxContinuationsComplete || !States.ChainOperationsComplete ||
      !States.CallFrameEffectsComplete ||
      States.Blocks.size() != Function.Blocks.size())
    return std::nullopt;
  if (auto Error = validateLowInstructionBoundaries(
          Function, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return std::nullopt;
  }
  size_t Work = 0;
  std::map<int, const RegistrationBlockState *> ByBlock;
  for (const auto &State : States.Blocks)
    if (!chargeCalleeWork(Work, 1) ||
        !ByBlock.emplace(State.BlockId, &State).second)
      return std::nullopt;
  std::optional<uint16_t> Pop;
  for (const auto &Block : Function.Blocks) {
    if (!chargeCalleeWork(Work, Block.Ops.size() +
                                    Block.InstructionBoundaries.size() + 1))
      return std::nullopt;
    const auto It = ByBlock.find(Block.Id);
    if (It == ByBlock.end() || It->second->Range.Begin != Block.StartAddr ||
        It->second->Range.End != Block.EndAddr)
      return std::nullopt;
    const auto &State = *It->second;
    if (!State.Reached)
      continue;
    if (State.Unknown)
      return std::nullopt;
    va_t End = Block.EndAddr;
    for (const auto &Op : Block.Ops)
      if (const auto *Call = States.callFrameEffect(Op.Addr, Op.Seq);
          Call && Call->DoesNotReturn)
        End = std::min(End, Call->EndAddress);
    for (const auto &Boundary : Block.InstructionBoundaries) {
      if (Boundary.Address >= End ||
          Boundary.Control != LowInstructionControl::Return)
        continue;
      if (Boundary.Address + Boundary.Size != Block.EndAddr ||
          !Block.Succs.empty() || Block.Ops.empty() ||
          Block.Ops.back().Opcode != NdOp::RETURN ||
          Block.Ops.back().Addr != Boundary.Address)
        return std::nullopt;
      const auto Bytes =
          readImmutableCodeBytes(Image, Boundary.Address, Boundary.Size);
      if (!Bytes || !((Bytes->size() == 1 && (*Bytes)[0] == 0xc3) ||
                      (Bytes->size() == 3 && (*Bytes)[0] == 0xc2)))
        return std::nullopt;
      const uint16_t Count =
          Bytes->size() == 1 ? 0 : readLE<uint16_t>(Bytes->data() + 1);
      if (Boundary.Immediate.value_or(0) != Count || Count % 4 ||
          (State.CallbackOnly && Count))
        return std::nullopt;
      if (!State.CallbackOnly) {
        if (Pop && *Pop != Count)
          return std::nullopt;
        Pop = Count;
      }
    }
  }
  return Pop;
}
} // namespace neverd::registration_abi
