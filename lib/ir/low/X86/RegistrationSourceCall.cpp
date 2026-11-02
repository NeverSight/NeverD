//===- RegistrationSourceCall.cpp - PE32 source call binding --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationSourceCall.h"

#include "../../../loader/ReadOnlyBytesIndex.h"
#include "RegistrationABIPrivate.h"
#include "RegistrationFrame.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/lift/X86/X86Regs.h"
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

std::optional<uint32_t> getCheckedX86IndependentIndexedCallStackPop(
    const BinaryImage &Image, const LowBlock &Prefix,
    const std::set<va_t> &Roots, size_t &Work, size_t Limit) {
  using registration_state::FrameValue;
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      !Image.isCOFF() || Prefix.Ops.empty() ||
      Prefix.InstructionBoundaries.empty() || Prefix.StartAddr > UINT32_MAX ||
      Prefix.EndAddr <= Prefix.StartAddr || Prefix.EndAddr > UINT32_MAX)
    return std::nullopt;
  const auto Charge = [&](size_t Amount) {
    if (Amount > Limit - std::min(Work, Limit)) {
      Work = Limit;
      return false;
    }
    Work += Amount;
    return true;
  };
  if (!Charge(Roots.size() + 1))
    return std::nullopt;
  const auto Root = Roots.upper_bound(Prefix.StartAddr);
  if (Root != Roots.end() && *Root < Prefix.EndAddr)
    return std::nullopt;
  const auto &Call = Prefix.Ops.back();
  if (Call.Opcode != NdOp::CALL || Call.NumInputs != 1 || Call.Seq != 0 ||
      Call.Output != NdVar::reg(x86reg::RAX, 4) || !Call.Inputs[0].isConst() ||
      Call.Inputs[0].Size != 4 || Call.Addr + 5 != Prefix.EndAddr ||
      !hasCheckedX86RegistrationDirectCallBytes(
          Image, Call.Addr, Call.Inputs[0].Offset, Work, Limit))
    return std::nullopt;
  const va_t Entry = Call.Inputs[0].Offset;
  if (Entry > UINT32_MAX - 7)
    return std::nullopt;
  // The unprefixed encoding has exactly one memory read and one tail jump.
  // No base register, ESP index, extra instruction or guessed table capacity.
  const auto Jump =
      readRelocatedPE32CodeBytes(Image, Entry, 7, {Entry + 3}, Work, Limit);
  if (!Jump || (*Jump)[0] != 0xff || (*Jump)[1] != 0x24 ||
      ((*Jump)[2] & 0xc7) != 0x85 || ((*Jump)[2] & 0x38) == 0x20)
    return std::nullopt;
  const unsigned IndexRegister = x86reg::generalReg(((*Jump)[2] >> 3) & 7);
  const uint32_t Table = readLE<uint32_t>(Jump->data() + 3);

  Decoder Dec;
  if (!Dec.init(Image))
    return std::nullopt;
  Dec.setStrict(true);
  registration_state::FrameState State;
  registration_state::FrameTransfer Transfer(State, 0);
  size_t FirstOp = 0;
  va_t Address = Prefix.StartAddr;
  for (const auto &Boundary : Prefix.InstructionBoundaries) {
    const bool Last = Address == Call.Addr;
    if (!Charge(Boundary.OpCount + 1) || !Boundary.Size ||
        Boundary.Address != Address || Boundary.FirstOp != FirstOp ||
        FirstOp > Prefix.Ops.size() ||
        Boundary.OpCount > Prefix.Ops.size() - FirstOp ||
        Boundary.Size > Prefix.EndAddr - Address ||
        Boundary.Mode != InstructionMode::Default ||
        Boundary.TargetMode != LowInstructionTargetMode::Preserve ||
        Boundary.Control != (Last ? LowInstructionControl::Call
                                  : LowInstructionControl::None) ||
        Boundary.ControlFlags != (Last ? LowInstructionControlFlag::Call
                                       : LowInstructionControlFlag::None) ||
        (Last ? Boundary.Immediate != Entry : Boundary.Immediate.has_value()))
      return std::nullopt;
    // Scalar source facts require relocation-free current bytes. Relift them
    // rather than trusting a stale instruction/value cache after an image edit.
    const auto Bytes = readImmutablePE32CodeBytes(Image, Address, Boundary.Size,
                                                  {}, Work, Limit);
    DecodedInsn Insn;
    if (!Bytes || !Dec.decodeOne(Bytes->data(), Bytes->size(), Address, Insn) ||
        Insn.Size != Boundary.Size)
      return std::nullopt;
    std::vector<LowOp> Expected;
    try {
      Dec.liftToLow(Insn, Expected);
    } catch (const UnliftedInstruction &) {
      return std::nullopt;
    }
    if (!Charge(Expected.size() + 1) || Expected.size() != Boundary.OpCount ||
        !std::equal(Expected.begin(), Expected.end(),
                    Prefix.Ops.begin() + FirstOp))
      return std::nullopt;
    Transfer.beginInstruction(Address);
    if (!Last)
      for (const auto &Op : Expected) {
        size_t Width = Op.Output.Size;
        for (unsigned I = 0; I != Op.NumInputs; ++I)
          Width += Op.Inputs[I].Size;
        if (!Charge(Width + State.OtherRegisterBytes.size() + 1) ||
            Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
            Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
            Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::RETURN ||
            Op.Opcode == NdOp::INTRINSIC ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            Op.MemoryOrdering != NdMemoryOrdering::None)
          return std::nullopt;
        // No cells are stored: loads, saved registers and incoming values
        // cannot supply a scalar index. Integer transfer has one shared owner.
        Transfer.write(Op, Transfer.evaluate(Op, false));
      }
    FirstOp += Expected.size();
    Address += Insn.Size;
  }
  if (Address != Prefix.EndAddr || FirstOp != Prefix.Ops.size())
    return std::nullopt;
  const auto Index = Transfer.read(NdVar::reg(IndexRegister, 4));
  if (!Index.Constant || Index != FrameValue::constant(*Index.Constant) ||
      Table > UINT32_MAX - 3 ||
      *Index.Constant > (UINT32_MAX - 3 - uint64_t(Table)) / 4)
    return std::nullopt;
  const auto Target = readImmutablePE32JumpTableTarget(
      Image, uint64_t(Table) + uint64_t(*Index.Constant) * 4, Work, Limit);
  return Target ? getCheckedX86IndependentCalleeStackPop(Image, *Target, Work,
                                                         Limit)
                : std::nullopt;
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
