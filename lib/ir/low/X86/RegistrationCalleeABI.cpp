//===- RegistrationCalleeABI.cpp - PE32 callee contracts ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include <deque>
#include <set>

namespace neverd::registration_abi {

const Import *checkedImportSlot(const BinaryImage &Image, va_t Target) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF)
    return nullptr;
  const auto *Import = Image.findImportAt(Target);
  if (!Import || Import->IATAddr != Target ||
      !Image.isValidImportStorageSlot(Import->IATAddr, Import->Name))
    return nullptr;
  for (const auto &Other : Image.Imports)
    if (Other.IATAddr == Import->IATAddr &&
        (Other.Name != Import->Name ||
         !llvm::StringRef(Other.Module).equals_insensitive(Import->Module)))
      return nullptr;
  const auto Storage = Image.collectImportStorageSlot(Import->IATAddr);
  const auto Slot = Storage.Slots.find(Import->IATAddr);
  if (Storage.Conflicts.count(Import->IATAddr) || Slot == Storage.Slots.end() ||
      Slot->second.Name != Import->Name || Slot->second.Addend)
    return nullptr;
  return Import;
}

std::optional<uint32_t>
checkedRegistrationImportStackPop(const BinaryImage &Image, va_t Target) {
  const auto *Named = Image.findImportAt(Target);
  const auto *Import =
      Named ? checkedImportSlot(Image, Named->IATAddr) : nullptr;
  // These authenticated C-runtime math entries use caller cleanup on PE32.
  // This is a returning ESP fact only: callbacks, errno and floating-point
  // effects still prevent a private-memory or native replacement contract.
  if (Import && !Import->IsDelayImport &&
      (Import->Name == "log" || Import->Name == "exp") &&
      isFileBackedPEImportSlot(Image, Import->IATAddr)) {
    const llvm::StringRef Module = Import->Module;
    if (Module.equals_insensitive("api-ms-win-crt-math-l1-1-0.dll") ||
        Module.equals_insensitive("ucrtbase.dll") ||
        Module.equals_insensitive("ucrtbased.dll") ||
        Module.equals_insensitive("msvcrt.dll"))
      return 0;
  }
  if (!Import || Import->Name != "RaiseException" ||
      (!llvm::StringRef(Import->Module).equals_insensitive("kernel32.dll") &&
       !llvm::StringRef(Import->Module).equals_insensitive("kernelbase.dll")))
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

} // namespace neverd::registration_abi

namespace neverd {

namespace {
using registration_abi::callerPCIsNotReadBack;
using registration_abi::chargeCalleeWork;
using registration_abi::chargeDecodedCallee;
using registration_abi::collectCalleeCodeRanges;
using registration_abi::copyExtents;
using registration_abi::hasPrivateCallerFrame;
using registration_abi::ImageFrameEffects;

bool retainLeafEntryBlocks(LowFunc &Function, size_t &Work,
                           uint32_t ExpectedPop) {
  std::map<int, const LowBlock *> Blocks;
  std::deque<int> Pending;
  for (const auto &Block : Function.Blocks) {
    if (!chargeCalleeWork(Work, 1) || !Blocks.emplace(Block.Id, &Block).second)
      return false;
    if (Block.StartAddr == Function.Entry)
      Pending.push_back(Block.Id);
  }
  if (Pending.size() != 1)
    return false;
  std::set<int> Reached;
  while (!Pending.empty()) {
    const int Id = Pending.front();
    Pending.pop_front();
    if (!chargeCalleeWork(Work, 1) || !Blocks.count(Id))
      return false;
    if (!Reached.insert(Id).second)
      continue;
    const auto &Block = *Blocks.at(Id);
    if (!chargeCalleeWork(Work, Block.InstructionBoundaries.size() +
                                    Block.Succs.size()))
      return false;
    for (const auto &Boundary : Block.InstructionBoundaries)
      if (Boundary.Control == LowInstructionControl::Return &&
          Boundary.Immediate.value_or(0) != ExpectedPop)
        return false;
    Pending.insert(Pending.end(), Block.Succs.begin(), Block.Succs.end());
  }
  // Only this physical entry is called. Address-taken roots without an
  // ordinary path from it do not contribute calls, effects or return cleanup.
  // Every retained edge and machine return was checked above; this projection
  // grants no ownership or rewrite permission for the discarded roots.
  llvm::erase_if(Function.Blocks,
                 [&](const auto &Block) { return !Reached.count(Block.Id); });
  for (auto &Block : Function.Blocks)
    llvm::erase_if(Block.Preds, [&](int Predecessor) {
      return !Reached.count(Predecessor);
    });
  Function.CalleePopBytes = ExpectedPop;
  return true;
}

} // namespace

std::optional<RegistrationLeafCalleeABI> registration_abi::checkedLeafCalleeABI(
    const BinaryImage &Image, va_t Target, size_t &Work,
    RegistrationLeafResult ResultKind, uint32_t CopyObjectSize,
    std::vector<RegistrationObjectExtent> *CopySource,
    InitializerFrame *Initializer, InitializerContext *Context,
    ObjectCallbackContext *Callbacks, uint32_t CallbackStackPopBytes) {
  if ((Initializer && CopyObjectSize) || (bool(Initializer) != bool(Context)) ||
      (Callbacks && (Initializer || CopyObjectSize)) ||
      (!Callbacks && CallbackStackPopBytes))
    return std::nullopt;
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Target > UINT32_MAX ||
      !Image.isCodeAddress(Target) ||
      Image.ExceptionMetadata.findFunction(Target))
    return std::nullopt;
  if (!chargeCalleeWork(Work, 1))
    return std::nullopt;
  Decoder Decoder;
  if (!Decoder.init(Image))
    return std::nullopt;
  CFGBuilder Builder;
  LowFunc Callee =
      Builder.buildIntegerFrameProof(Image, Decoder, Target, "abi-leaf");
  const uint32_t Pop = Initializer      ? Initializer->StackPopBytes
                       : CopyObjectSize ? 4
                       : Callbacks      ? CallbackStackPopBytes
                                        : 0;
  if (!chargeDecodedCallee(Work, Callee) || !Callee.hasCompleteLiftCoverage() ||
      Callee.Blocks.empty() || Callee.ExceptionMetadata ||
      !retainLeafEntryBlocks(Callee, Work, Pop))
    return std::nullopt;
  InitializerCalls Initializers;
  ObjectCallbackCalls ObjectCallbacks;
  for (const auto &Block : Callee.Blocks) {
    if (Block.Succs.empty() &&
        (Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::RETURN))
      return std::nullopt;
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;
      if ((!Context && !Callbacks) || Op.Opcode != NdOp::CALL ||
          Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
          Op.Inputs[0].Size != 4 || Op.Seq < 0 ||
          Initializers.size() + ObjectCallbacks.size() >=
              kMaxObjectCallbackNodes)
        return std::nullopt;
      if (Context) {
        auto Nested = checkedInitializerCalleeABI(Image, Op.Inputs[0].Offset,
                                                  Work, *Context);
        if (!Nested ||
            !Initializers
                 .emplace(std::make_pair(Op.Addr, Op.Seq), std::move(*Nested))
                 .second)
          return std::nullopt;
      } else {
        auto Nested = checkedObjectCallbackCalleeABI(Image, Op.Inputs[0].Offset,
                                                     Work, *Callbacks);
        if (!Nested ||
            !ObjectCallbacks
                 .emplace(std::make_pair(Op.Addr, Op.Seq), std::move(*Nested))
                 .second)
          return std::nullopt;
      }
    }
  }
  ImageFrameEffects Effects;
  bool IndependentScalarReturn = false;
  const PrivateFrameOptions Frame{
      .BorrowECX = true,
      .IndependentScalarReturn = &IndependentScalarReturn,
      .DiscardReturnValue = ResultKind == RegistrationLeafResult::Discarded,
      .CopyObjectSize = CopyObjectSize,
      .Initializer = Initializer,
      .Initializers = Initializer ? &Initializers : nullptr,
      .ObjectCallbacks = Callbacks ? &ObjectCallbacks : nullptr,
      .CallbackStackPopBytes = CallbackStackPopBytes};
  if (!hasPrivateCallerFrame(Callee, Image, Work, Effects, Frame))
    return std::nullopt;
  if (!callerPCIsNotReadBack(Effects))
    return std::nullopt;
  RegistrationLeafCalleeABI Result;
  Result.Target = Target;
  Result.StackPopBytes = Pop;
  Result.HasIndependentScalarReturn = IndependentScalarReturn;
  if (!collectCalleeCodeRanges(Callee, Image, Work, Result.CodeRanges))
    return std::nullopt;
  std::set<std::pair<va_t, va_t>> Code;
  for (const auto &Range : Result.CodeRanges)
    Code.emplace(Range.Begin, Range.End);
  for (const auto &[Site, Nested] : Initializers) {
    if (!chargeCalleeWork(Work, Nested.Object.CodeRanges.size()))
      return std::nullopt;
    for (const auto &Range : Nested.Object.CodeRanges)
      Code.emplace(Range.Begin, Range.End);
  }
  for (const auto &[Site, Nested] : ObjectCallbacks) {
    if (!chargeCalleeWork(Work, Nested.CodeRanges.size()))
      return std::nullopt;
    for (const auto &Range : Nested.CodeRanges)
      Code.emplace(Range.Begin, Range.End);
  }
  Result.CodeRanges.clear();
  copyExtents(Code, Result.CodeRanges);
  copyExtents(Effects.ECXReads, Result.ECXReads);
  copyExtents(Effects.ECXWrites, Result.ECXWrites);
  copyExtents(Effects.Reads, Result.ImageReads);
  copyExtents(Effects.Writes, Result.ImageWrites);
  copyExtents(Effects.CallerPCWrites, Result.CallerPCWrites);
  if (CopySource)
    copyExtents(Effects.CopySourceReads, *CopySource);
  return Result;
}

std::optional<RegistrationLeafCalleeABI>
getCheckedX86RegistrationLeafCalleeABI(const BinaryImage &Image, va_t Target,
                                       size_t *CumulativeWork,
                                       RegistrationLeafResult ResultKind) {
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  return registration_abi::checkedLeafCalleeABI(Image, Target, Work,
                                                ResultKind);
}

} // namespace neverd
