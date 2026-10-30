//===- RegistrationCalleeABI.cpp - PE32 callee contracts ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"
#include "RegistrationCRTData.h"
#include "RegistrationCRTFrame.h"
#include "RegistrationImportLoad.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include <deque>
#include <set>

namespace neverd::registration_abi {

bool isPlainRegistrationCalleeEntry(const BinaryImage &Image, va_t Target) {
  return Image.Arch == Arch::X86 && Image.Bits == Bitness::Bits32 &&
         Image.Format == BinaryFormat::COFF && Target <= UINT32_MAX &&
         Image.isCodeAddress(Target) &&
         !Image.ExceptionMetadata.findFunction(Target);
}

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

namespace {
const Import *checkedRuntimeImportSlot(const BinaryImage &Image, va_t Target) {
  const auto *Import = checkedImportSlot(Image, Target);
  return Import && isFileBackedPEImportSlot(Image, Target) ? Import : nullptr;
}

bool isRaiseExceptionProvider(const Import &Import) {
  return Import.Name == "RaiseException" &&
         (llvm::StringRef(Import.Module).equals_insensitive("kernel32.dll") ||
          llvm::StringRef(Import.Module).equals_insensitive("kernelbase.dll"));
}

bool isStartupInfoProvider(const Import &Import) {
  return Import.Name == "GetStartupInfoW" &&
         llvm::StringRef(Import.Module).equals_insensitive("kernel32.dll");
}

bool isHeapTerminationProvider(const Import &Import) {
  return Import.Name == "HeapSetInformation" &&
         llvm::StringRef(Import.Module).equals_insensitive("kernel32.dll");
}

bool isInterlockedProvider(const Import &Import) {
  return Import.Name == "InterlockedCompareExchange" &&
         llvm::StringRef(Import.Module).equals_insensitive("kernel32.dll");
}

bool isSleepProvider(const Import &Import) {
  return Import.Name == "Sleep" &&
         llvm::StringRef(Import.Module).equals_insensitive("kernel32.dll");
}

bool checkedDirectRuntimeCall(const BinaryImage &Image, const LowOp &Op,
                              size_t &Work, bool (*Provider)(const Import &)) {
  if (Op.Opcode != NdOp::INDIR_CALL || Op.NumInputs != 1 || Op.Seq < 0 ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None || Op.Addr > UINT32_MAX - 6 ||
      !chargeCalleeWork(Work, 10))
    return false;
  const auto *Import = checkedRuntimeImportSlot(Image, Op.Inputs[0].Offset);
  if (!Import || !Provider(*Import))
    return false;
  const auto Bytes =
      readImmutablePE32CodeBytes(Image, Op.Addr, 6, {Op.Addr + 2});
  return Bytes && (*Bytes)[0] == 0xff && (*Bytes)[1] == 0x15 &&
         readLE<uint32_t>(Bytes->data() + 2) == Op.Inputs[0].Offset;
}
} // namespace

bool isCheckedRegistrationRaiseSlot(const BinaryImage &Image, va_t Target) {
  const auto *Import = checkedRuntimeImportSlot(Image, Target);
  return Import && isRaiseExceptionProvider(*Import);
}

bool isCheckedRegistrationStartupInfoSlot(const BinaryImage &Image,
                                          va_t Target) {
  const auto *Import = checkedRuntimeImportSlot(Image, Target);
  return Import && isStartupInfoProvider(*Import);
}

bool isCheckedRegistrationStartupInfoCall(const BinaryImage &Image,
                                          const LowOp &Op, size_t &Work) {
  return checkedDirectRuntimeCall(Image, Op, Work, isStartupInfoProvider);
}

bool isCheckedRegistrationHeapTerminationSlot(const BinaryImage &Image,
                                              va_t Target) {
  const auto *Import = checkedRuntimeImportSlot(Image, Target);
  return Import && isHeapTerminationProvider(*Import);
}

bool isCheckedRegistrationHeapTerminationCall(const BinaryImage &Image,
                                              const LowOp &Op, size_t &Work) {
  return checkedDirectRuntimeCall(Image, Op, Work, isHeapTerminationProvider);
}

bool isCheckedRegistrationInterlockedSlot(const BinaryImage &Image,
                                          va_t Target) {
  const auto *Import = checkedRuntimeImportSlot(Image, Target);
  return Import && isInterlockedProvider(*Import);
}

bool isCheckedRegistrationInterlockedCall(const BinaryImage &Image,
                                          const LowOp &Op, size_t &Work) {
  return checkedDirectRuntimeCall(Image, Op, Work, isInterlockedProvider);
}

bool isCheckedRegistrationSleepSlot(const BinaryImage &Image, va_t Target) {
  const auto *Import = checkedRuntimeImportSlot(Image, Target);
  return Import && isSleepProvider(*Import);
}

bool isCheckedRegistrationSleepCall(const BinaryImage &Image, const LowOp &Op,
                                    size_t &Work) {
  return checkedDirectRuntimeCall(Image, Op, Work, isSleepProvider);
}

std::optional<uint32_t>
checkedRegistrationImportStackPop(const BinaryImage &Image, va_t Target) {
  const auto *Import = checkedRuntimeImportSlot(Image, Target);
  if (!Import)
    return std::nullopt;
  // These are returning ESP facts only. Dispatch, allocation, memory writes,
  // pointer results, callbacks and FP state need separate effect contracts.
  if (isRaiseExceptionProvider(*Import))
    return 16;
  if (isStartupInfoProvider(*Import))
    return 4;
  if (registration_state::isCheckedControlFPSlot(Image, Target))
    return 0;
  const llvm::StringRef Module = Import->Module;
  const llvm::StringRef Name = Import->Name;
  if (Module.equals_insensitive("kernel32.dll")) {
    // The SDK's PE32 WINAPI declarations use 32-bit stack carriers. Exact
    // exports matter: neither similarly named APIs nor other DLLs inherit it.
    if (Name == "DecodePointer" || Name == "EncodePointer" || Name == "Sleep" ||
        Name == "IsProcessorFeaturePresent" ||
        Name == "SetUnhandledExceptionFilter")
      return 4;
    if (Name == "InterlockedExchange")
      return 8;
    if (Name == "InterlockedCompareExchange")
      return 12;
    if (Name == "HeapSetInformation")
      return 16;
  }
  if ((Module.equals_insensitive("msvcrt.dll") ||
       Module.equals_insensitive("msvcr100.dll")) &&
      (Name == "malloc" || Name == "calloc" || Name == "realloc" ||
       Name == "free" || Name == "memcpy" || Name == "memmove" ||
       Name == "memset" || Name == "_initterm" || Name == "_initterm_e" ||
       Name == "_lock" || Name == "_unlock" || Name == "_onexit" ||
       Name == "__set_app_type" || Name == "__setusermatherr" ||
       Name == "_configthreadlocale" || Name == "??2@YAPAXI@Z" ||
       Name == "??_U@YAPAXI@Z" || Name == "??3@YAXPAX@Z" ||
       Name == "??_V@YAXPAX@Z"))
    return 0;
  if ((Name == "log" || Name == "exp") &&
      (Module.equals_insensitive("api-ms-win-crt-math-l1-1-0.dll") ||
       Module.equals_insensitive("ucrtbase.dll") ||
       Module.equals_insensitive("ucrtbased.dll") ||
       Module.equals_insensitive("msvcrt.dll")))
    return 0;
  return std::nullopt;
}

bool preservesRegistrationImportBindings(const BinaryImage &Image,
                                         va_t Target) {
  const auto *Import = checkedRuntimeImportSlot(Image, Target);
  if (!Import)
    return false;
  const llvm::StringRef Module = Import->Module;
  // The legacy MSVCR100 setters retain an int or encoded callback pointer in
  // CRT state without replacing the caller image's IAT. __setusermatherr does
  // not invoke that callback while setting it. Keep the write and runtime
  // values opaque: this is no frame, callback, result or no-throw contract.
  // Other CRT providers and the modern _set_app_type have separate contracts.
  if (Module.equals_insensitive("msvcr100.dll") &&
      (Import->Name == "__set_app_type" || Import->Name == "__setusermatherr"))
    return true;
  // These APIs transform pointer bits with a process-specific secret, without
  // replacing the caller image's import slots. Keep the runtime result opaque;
  // in particular, decoding does not establish frame-independent provenance.
  return Module.equals_insensitive("kernel32.dll") &&
         (Import->Name == "DecodePointer" || Import->Name == "EncodePointer");
}

std::optional<RegistrationImportLoad>
checkedRegistrationImportLoad(const BinaryImage &Image, const LowBlock &Block,
                              const LowInstructionBoundary &Boundary,
                              size_t &Work,
                              RegistrationImportLoadPolicy Policy) {
  if (!chargeCalleeWork(Work, 1) || Boundary.OpCount != 4 ||
      !chargeCalleeWork(Work, 16))
    return std::nullopt;
  auto Load = registrationImportLoad(Block, Boundary);
  if (!Load || (Boundary.Size != 5 && Boundary.Size != 6) ||
      Boundary.Address > UINT32_MAX - Boundary.Size)
    return std::nullopt;
  if (!checkedRegistrationImportStackPop(Image, Load->Slot)) {
    if (Policy != RegistrationImportLoadPolicy::IncludeCRTData ||
        !isCheckedRegistrationCRTDataSlot(Image, Load->Slot))
      return std::nullopt;
    Load->Storage = RegistrationImportLoad::Kind::CRTData;
  }
  const unsigned Field = Boundary.Size - 4;
  const auto Bytes = readImmutablePE32CodeBytes(
      Image, Boundary.Address, Boundary.Size, {Boundary.Address + Field});
  if (!Bytes || readLE<uint32_t>(Bytes->data() + Field) != Load->Slot)
    return std::nullopt;
  const unsigned Register = Load->Register / x86reg::GeneralRegStride;
  if (Boundary.Size == 5
          ? (*Bytes)[0] != 0xa1 || Register != 0
          : (*Bytes)[0] != 0x8b || (*Bytes)[1] != uint8_t(0x05 + Register * 8))
    return std::nullopt;
  return Load;
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
  if (!registration_abi::isPlainRegistrationCalleeEntry(Image, Target))
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
      // The private-frame interpreter rejects every reached ordinary leaf
      // call. Do not reject a call on a proved infeasible edge before that
      // shared traversal. Object graphs retain their separate closure check.
      if (!Context && !Callbacks)
        continue;
      if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 || Op.Seq < 0 ||
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
  bool ClosedIntegerEffects = false;
  const PrivateFrameOptions Frame{
      .BorrowECX = true,
      .IndependentScalarReturn = &IndependentScalarReturn,
      .DiscardReturnValue = ResultKind == RegistrationLeafResult::Discarded,
      .CopyObjectSize = CopyObjectSize,
      .Initializer = Initializer,
      .Initializers = Initializer ? &Initializers : nullptr,
      .ObjectCallbacks = Callbacks ? &ObjectCallbacks : nullptr,
      .CallbackStackPopBytes = CallbackStackPopBytes,
      .ClosedIntegerEffects = &ClosedIntegerEffects};
  if (!hasPrivateCallerFrame(Callee, Image, Work, Effects, Frame))
    return std::nullopt;
  if (!callerPCIsNotReadBack(Effects))
    return std::nullopt;
  RegistrationLeafCalleeABI Result;
  Result.Target = Target;
  Result.StackPopBytes = Pop;
  Result.HasIndependentScalarReturn = IndependentScalarReturn;
  Result.PreservesDispatchBindings =
      ClosedIntegerEffects && !Initializer && !Callbacks && !CopyObjectSize &&
      Effects.ECXReads.empty() && Effects.ECXWrites.empty();
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
