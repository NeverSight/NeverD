//===- RegistrationThrowABI.cpp - PE32 throw callees ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove preserved private scalar-throw and active-exception rethrow helpers.
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/loader/BinaryImage.h"

namespace neverd {
namespace {
using registration_abi::callerPCIsNotReadBack;
using registration_abi::chargeCalleeWork;
using registration_abi::chargeDecodedCallee;
using registration_abi::collectCalleeCodeRanges;
using registration_abi::copyExtents;
using registration_abi::hasPrivateCallerFrame;
using registration_abi::ImageFrameEffects;
} // namespace

std::optional<RegistrationThrowCalleeABI>
getCheckedX86RegistrationThrowCalleeABI(const BinaryImage &Image, va_t Target,
                                        size_t *CumulativeWork) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Target > UINT32_MAX ||
      !Image.isCodeAddress(Target) ||
      Image.ExceptionMetadata.findFunction(Target))
    return std::nullopt;
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  if (!chargeCalleeWork(Work, 1))
    return std::nullopt;
  Decoder Decoder;
  if (!Decoder.init(Image))
    return std::nullopt;
  CFGBuilder Builder;
  const LowFunc Callee = Builder.build(Image, Decoder, Target, "abi-throw");
  if (!chargeDecodedCallee(Work, Callee) || Callee.CalleePopBytes ||
      !Callee.hasCompleteLiftCoverage() || Callee.Blocks.empty() ||
      Callee.ExceptionMetadata || !lowFunctionNeverReturns(Callee, Arch::X86))
    return std::nullopt;
  RegistrationThrowCalleeABI Result;
  Result.Target = Target;
  unsigned Calls = 0;
  for (const auto &Block : Callee.Blocks) {
    bool ThrowAtExit = false;
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;
      if (++Calls != 1 || Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 || Op.Seq < 0)
        return std::nullopt;
      const auto Import = getCheckedX86RegistrationThrowImportABI(
          Image, Op.Inputs[0].Offset, &Work);
      if (!Import)
        return std::nullopt;
      const auto Boundary =
          llvm::find_if(Block.InstructionBoundaries,
                        [&](const auto &B) { return B.Address == Op.Addr; });
      if (Boundary == Block.InstructionBoundaries.end() ||
          Boundary->Control != LowInstructionControl::Call ||
          !hasLowInstructionControlFlag(Boundary->ControlFlags,
                                        LowInstructionControlFlag::NoReturn) ||
          hasLowInstructionControlFlag(
              Boundary->ControlFlags, LowInstructionControlFlag::Conditional) ||
          Op.Addr + Boundary->Size != Block.EndAddr)
        return std::nullopt;
      Result.ImportVA = Op.Inputs[0].Offset;
      Result.ImportIATVA = Import->IATVA;
      Result.ThrowCallVA = Op.Addr;
      Result.ThrowCallEndVA = Op.Addr + Boundary->Size;
      Result.ThrowOpSeq = Op.Seq;
      ThrowAtExit = true;
    }
    if (Block.Succs.empty() && !ThrowAtExit)
      return std::nullopt;
  }
  if (Calls != 1)
    return std::nullopt;
  ImageFrameEffects Effects;
  if (!hasPrivateCallerFrame(Callee, Image, Work, Effects, false, &Result) ||
      (Result.IsRethrow ? Result.ThrowInfo.Address != InvalidVA
                        : Result.ThrowInfo.Address == InvalidVA) ||
      !callerPCIsNotReadBack(Effects))
    return std::nullopt;
  if (!Result.IsRethrow) {
    const auto &Info = Result.ThrowInfo;
    if (Effects.Writes.size() > (limits::kMaxRegistrationEHStateWork - Work) /
                                    (Info.ReadOnlyRanges.size() + 1))
      return std::nullopt;
    Work += Effects.Writes.size() * (Info.ReadOnlyRanges.size() + 1);
    for (const auto &[Begin, End] : Effects.Writes) {
      const ExceptionAddressRange Write{Begin, End};
      if (Write.overlaps(Info.TypeDescriptorRange))
        return std::nullopt;
      for (const auto &Range : Info.ReadOnlyRanges)
        if (Write.overlaps(Range))
          return std::nullopt;
    }
  }
  if (!collectCalleeCodeRanges(Callee, Image, Work, Result.CodeRanges) ||
      !chargeCalleeWork(Work, 1))
    return std::nullopt;
  std::set<std::pair<va_t, va_t>> Code;
  for (const auto &Range : Result.CodeRanges)
    Code.emplace(Range.Begin, Range.End);
  Code.emplace(Result.ImportVA, Result.ImportVA + 6);
  Result.CodeRanges.clear();
  copyExtents(Code, Result.CodeRanges);
  copyExtents(Effects.Reads, Result.ImageReads);
  copyExtents(Effects.Writes, Result.ImageWrites);
  copyExtents(Effects.CallerPCWrites, Result.CallerPCWrites);
  return Result;
}

} // namespace neverd
