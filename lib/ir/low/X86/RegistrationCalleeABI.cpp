//===- RegistrationCalleeABI.cpp - PE32 callee contracts ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include <deque>
#include <set>

namespace neverd::registration_abi {

std::optional<uint32_t>
checkedRegistrationImportStackPop(const BinaryImage &Image, va_t Target) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF)
    return std::nullopt;
  const auto *Import = Image.findImportAt(Target);
  if (!Import || Import->Name != "RaiseException" ||
      (!llvm::StringRef(Import->Module).equals_insensitive("kernel32.dll") &&
       !llvm::StringRef(Import->Module).equals_insensitive("kernelbase.dll")) ||
      !Image.isValidImportStorageSlot(Import->IATAddr, Import->Name))
    return std::nullopt;
  for (const auto &Other : Image.Imports)
    if (Other.IATAddr == Import->IATAddr &&
        (Other.Name != Import->Name ||
         !llvm::StringRef(Other.Module).equals_insensitive(Import->Module)))
      return std::nullopt;
  const auto Storage = Image.collectImportStorageSlot(Import->IATAddr);
  const auto Slot = Storage.Slots.find(Import->IATAddr);
  if (Storage.Conflicts.count(Import->IATAddr) || Slot == Storage.Slots.end() ||
      Slot->second.Name != Import->Name || Slot->second.Addend)
    return std::nullopt;
  return 16;
}

bool callerPCIsNotReadBack(const ImageFrameEffects &Effects) {
  auto Read = Effects.Reads.begin();
  for (const auto &[Begin, End] : Effects.CallerPCWrites) {
    while (Read != Effects.Reads.end() && Read->second <= Begin)
      ++Read;
    if (Read != Effects.Reads.end() && Read->first < End)
      return false;
  }
  return true;
}

bool chargeCalleeWork(size_t &Work, size_t Amount) {
  if (Work > limits::kMaxRegistrationEHStateWork ||
      Amount > limits::kMaxRegistrationEHStateWork - Work) {
    Work = limits::kMaxRegistrationEHStateWork;
    return false;
  }
  Work += Amount;
  return true;
}

} // namespace neverd::registration_abi

namespace neverd {
namespace {
using registration_abi::callerPCIsNotReadBack;
using registration_abi::chargeCalleeWork;
using registration_abi::hasPrivateCallerFrame;
using registration_abi::ImageFrameEffects;

template <typename Set, typename Vector>
void copyExtents(const Set &From, Vector &To) {
  for (const auto &[Begin, End] : From) {
    if (!To.empty() && Begin <= To.back().End)
      To.back().End = std::max(To.back().End, End);
    else
      To.push_back({Begin, End});
  }
}

bool chargeDecodedCallee(size_t &Work, const LowFunc &Function) {
  for (const auto &Block : Function.Blocks)
    if (!chargeCalleeWork(Work, Block.Ops.size() +
                                    Block.InstructionBoundaries.size() + 1))
      return false;
  return true;
}

bool collectCalleeCodeRanges(const LowFunc &Function, const BinaryImage &Image,
                             size_t &Work,
                             std::vector<ExceptionAddressRange> &Ranges) {
  if (auto Error = validateLowInstructionBoundaries(
          Function, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return false;
  }
  std::set<std::pair<va_t, va_t>> Instructions;
  for (const auto &Block : Function.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries) {
      if (!chargeCalleeWork(Work, 1) || !Boundary.Size ||
          Boundary.Address > UINT32_MAX ||
          Boundary.Size > uint64_t(UINT32_MAX) + 1 - Boundary.Address ||
          !Image.isCodeAddress(Boundary.Address) ||
          !Image.readVA(Boundary.Address, Boundary.Size))
        return false;
      Instructions.emplace(Boundary.Address, Boundary.Address + Boundary.Size);
    }
  copyExtents(Instructions, Ranges);
  return !Ranges.empty() && llvm::any_of(Ranges, [&](const auto &Range) {
    return Range.contains(Function.Entry);
  });
}

} // namespace

std::optional<RegistrationLeafCalleeABI>
getCheckedX86RegistrationLeafCalleeABI(const BinaryImage &Image, va_t Target,
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
  const LowFunc Callee = Builder.build(Image, Decoder, Target, "abi-leaf");
  if (!chargeDecodedCallee(Work, Callee) || Callee.CalleePopBytes ||
      !Callee.hasCompleteLiftCoverage() || Callee.Blocks.empty() ||
      Callee.ExceptionMetadata)
    return std::nullopt;
  for (const auto &Block : Callee.Blocks) {
    if (Block.Succs.empty() &&
        (Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::RETURN))
      return std::nullopt;
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        return std::nullopt;
  }
  ImageFrameEffects Effects;
  bool IndependentScalarReturn = false;
  if (!hasPrivateCallerFrame(Callee, Image, Work, Effects, true, nullptr,
                             &IndependentScalarReturn))
    return std::nullopt;
  if (!callerPCIsNotReadBack(Effects))
    return std::nullopt;
  RegistrationLeafCalleeABI Result;
  Result.Target = Target;
  Result.HasIndependentScalarReturn = IndependentScalarReturn;
  if (!collectCalleeCodeRanges(Callee, Image, Work, Result.CodeRanges))
    return std::nullopt;
  copyExtents(Effects.ECXReads, Result.ECXReads);
  copyExtents(Effects.ECXWrites, Result.ECXWrites);
  copyExtents(Effects.Reads, Result.ImageReads);
  copyExtents(Effects.Writes, Result.ImageWrites);
  copyExtents(Effects.CallerPCWrites, Result.CallerPCWrites);
  return Result;
}

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
      const auto *Import = Image.findImportStubAt(Op.Inputs[0].Offset);
      if (!Import || Import->Name != "_CxxThrowException" ||
          (!llvm::StringRef(Import->Module)
                .equals_insensitive("vcruntime140.dll") &&
           !llvm::StringRef(Import->Module)
                .equals_insensitive("vcruntime140d.dll")) ||
          !Import->IATAddr || !Image.readVA(Import->IATAddr, 4))
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
      Result.ImportIATVA = Import->IATAddr;
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
      Result.ThrowInfo.Address == InvalidVA || !callerPCIsNotReadBack(Effects))
    return std::nullopt;
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
  if (!collectCalleeCodeRanges(Callee, Image, Work, Result.CodeRanges) ||
      !chargeCalleeWork(Work, 1))
    return std::nullopt;
  // The original helper calls this exact EAX-preserving import stub. Keeping
  // the helper alone cannot authorize an entry patch that changes its tail.
  const auto *Stub = Image.readVA(Result.ImportVA, 6);
  if (Result.ImportVA > uint64_t(UINT32_MAX) - 5 ||
      !Image.isCodeAddress(Result.ImportVA) || !Stub || Stub[0] != 0xff ||
      Stub[1] != 0x25 || readLE<uint32_t>(Stub + 2) != Result.ImportIATVA)
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

std::optional<RegistrationCleanupRelayABI>
getCheckedX86RegistrationCleanupRelayABI(const BinaryImage &Image, va_t Target,
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
  auto Header = readImmutableCodeBytes(Image, Target, 3);
  if (!Header || (*Header)[0] != 0x8d ||
      ((*Header)[1] != 0x4d && (*Header)[1] != 0x8d))
    return std::nullopt;
  const unsigned JumpOffset = (*Header)[1] == 0x4d ? 3 : 6;
  const unsigned Size = JumpOffset + 5;
  const va_t End = Target + Size;
  if (End > uint64_t(UINT32_MAX) + 1 || !chargeCalleeWork(Work, Size))
    return std::nullopt;
  auto Bytes = readImmutableCodeBytes(Image, Target, Size);
  if (!Bytes || (*Bytes)[JumpOffset] != 0xe9)
    return std::nullopt;
  const int32_t Offset = JumpOffset == 3 ? int8_t((*Bytes)[2])
                                         : readLE<int32_t>(Bytes->data() + 2);
  const uint32_t LeafTarget =
      uint32_t(End) + readLE<uint32_t>(Bytes->data() + JumpOffset + 1);
  auto Leaf = getCheckedX86RegistrationLeafCalleeABI(Image, LeafTarget, &Work);
  if (!Leaf)
    return std::nullopt;
  return RegistrationCleanupRelayABI{Target, End, Offset, std::move(*Leaf)};
}

} // namespace neverd
