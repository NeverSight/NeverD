//===- RegistrationSourceCall.cpp - PE32 source call binding --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationSourceCall.h"

#include "RegistrationABIPrivate.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <limits>

namespace neverd {

bool hasCheckedX86RegistrationDirectCallBytes(const BinaryImage &Image,
                                              va_t Address, va_t Target) {
  size_t Work = 0;
  return hasCheckedX86RegistrationDirectCallBytes(
      Image, Address, Target, Work, std::numeric_limits<size_t>::max());
}

bool hasCheckedX86RegistrationDirectCallBytes(const BinaryImage &Image,
                                              va_t Address, va_t Target,
                                              size_t &Work, size_t Limit) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Address > UINT32_MAX - 5 ||
      Target > UINT32_MAX)
    return false;
  // The byte reader separately owns mapping and fixup charges. Pay the
  // executable-owner classification before entering its inventory queries.
  const auto Charge = [&](size_t Count) {
    if (Count > Limit - std::min(Work, Limit)) {
      Work = Limit;
      return false;
    }
    Work += Count;
    return true;
  };
  for (unsigned I = 0; I != 8; ++I)
    if (!Charge(Image.Segments.size()) || !Charge(Image.Sections.size()))
      return false;
  for (size_t Count : {size_t{1}, Image.ImportStubRanges.size(),
                       Image.KnownCodeRanges.size(), Image.Symbols.size()})
    if (!Charge(Count))
      return false;
  if (!Image.hasExecutableCodeOwnerRange(Address, 5))
    return false;
  // E8 has no absolute operand: every overlapping fixup remains forbidden.
  const auto Bytes =
      readImmutablePE32CodeBytes(Image, Address, 5, {}, Work, Limit);
  return Bytes && (*Bytes)[0] == 0xe8 &&
         uint32_t(Address + 5 + int64_t(readLE<int32_t>(Bytes->data() + 1))) ==
             Target;
}

namespace registration_state {

const LowInstructionBoundary *
checkedRegistrationDirectCall(const BinaryImage &Image, const LowFunc &Function,
                              const LowOp &Op, size_t &Work) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Op.Opcode != NdOp::CALL ||
      Op.NumInputs != 1 || Op.Seq < 0 || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 4 || Op.Inputs[0].Offset > UINT32_MAX ||
      Op.Addr > UINT32_MAX - 5 || Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !registration_abi::chargeCalleeWork(Work, 16))
    return nullptr;
  for (const auto &Block : Function.Blocks) {
    if (!registration_abi::chargeCalleeWork(Work, Block.Ops.size() + 1))
      return nullptr;
    for (size_t I = 0; I != Block.Ops.size(); ++I) {
      if (&Block.Ops[I] != &Op)
        continue;
      if (auto Error = validateLowInstructionBoundaries(
              Block, LowInstructionBoundaryRequirement::Required)) {
        llvm::consumeError(std::move(Error));
        return nullptr;
      }
      for (const auto &Insn : Block.InstructionBoundaries) {
        if (!registration_abi::chargeCalleeWork(Work, 1))
          return nullptr;
        if (Insn.Address != Op.Addr || I < Insn.FirstOp ||
            I - Insn.FirstOp >= Insn.OpCount)
          continue;
        if (Insn.Size != 5 || Insn.Control != LowInstructionControl::Call ||
            Insn.Immediate != Op.Inputs[0].Offset ||
            Insn.Mode != InstructionMode::Default ||
            Insn.TargetMode != LowInstructionTargetMode::Preserve ||
            hasLowInstructionControlFlag(
                Insn.ControlFlags, LowInstructionControlFlag::Conditional) ||
            hasLowInstructionControlFlag(
                Insn.ControlFlags, LowInstructionControlFlag::InstructionGuard))
          return nullptr;
        if (!hasCheckedX86RegistrationDirectCallBytes(Image, Op.Addr,
                                                      Op.Inputs[0].Offset))
          return nullptr;
        return &Insn;
      }
      return nullptr;
    }
  }
  return nullptr;
}

} // namespace registration_state
} // namespace neverd
