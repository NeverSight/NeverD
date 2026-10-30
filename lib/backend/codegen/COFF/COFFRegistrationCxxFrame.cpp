//===- COFFRegistrationCxxFrame.cpp - PE32 C++ frame proof ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "COFFRegistrationCxxIRProof.h"
#include "COFFRegistrationFrameProof.h"

#include "neverd/Limits.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/RegistrationCxxGuardedFrame.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

namespace neverd {
llvm::Error validateCOFFRegistrationCxxIR(const llvm::Function &Function,
                                          const ExceptionFunction &Source,
                                          const BinaryImage &Image) {
  auto Proof =
      coff_registration::getCheckedCxxControlIRProof(Function, Source, Image);
  if (!Proof)
    return Proof.takeError();
  if (const auto &Guarded = Proof->Guarded)
    return registration_frame::validateCxxGuardedFrame(
        Image, Guarded->Source, Function, *Proof->Frame.Slot, Guarded->Layout,
        Guarded->Calls, Guarded->Cleanups, Proof->ChainReads);
  const auto &States = *Proof->Source.RegistrationStates;
  coff_registration::RegistrationCxxFrameContract Contract;
  Contract.Image = &Image;
  Contract.SavedStackRestores = Proof->SavedStackRestores;
  Contract.CatchReturns = Proof->CatchReturns;
  std::map<X86RegistrationCatchIdentity, size_t> CatchIndices;
  for (const auto &[Identity, Catch] : Proof->Catches) {
    CatchIndices.emplace(Identity, Contract.Catches.size());
    auto &Invocation = Contract.Catches.emplace_back();
    Invocation.Catch = Catch.Pad;
    if (const auto &Home = Catch.Home) {
      Invocation.HomeOffset = Home->Offset;
      Invocation.ObjectSize = Home->ObjectSize;
      Invocation.Reference = Home->Reference;
    }
    Invocation.CallbackStack = Catch.Stack;
    Invocation.CallbackBlocks = Catch.Blocks;
  }
  Contract.SavedStackOffset = int64_t(Proof->Frame.Establisher) +
                              *Source.Registration->RegistrationOffset - 4;
  auto Ranges = coff_loader::getCheckedX86CxxMetadataRanges(Image, Source);
  auto Runtime = coff_loader::getCheckedX86CxxPersonalityABI(Image, Source);
  if (!Ranges || !Runtime)
    return coff_registration::rejectIR(
        "C++ runtime image contract is incomplete");
  std::vector<ExceptionAddressRange> Immutable = std::move(*Ranges);
  Immutable.insert(Immutable.end(), Runtime->CodeRanges.begin(),
                   Runtime->CodeRanges.end());
  Immutable.push_back({Runtime->IATVA, Runtime->IATVA + 4});
  size_t Work = 0;
  std::map<va_t, bool> TrivialDestruction;
  auto RecordDestruction = [&](const auto &Info) {
    const auto [It, New] =
        TrivialDestruction.emplace(Info.TypeDescriptorVA, !Info.DestructorVA);
    if (!New)
      It->second &= !Info.DestructorVA;
  };
  for (const auto &Segment : Image.Segments) {
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        Segment.VA > UINT32_MAX ||
        Segment.Size > uint64_t(UINT32_MAX) + 1 - Segment.VA)
      return coff_registration::rejectIR(
          "C++ runtime image segment has no bounded PE32 extent");
    if (!Segment.isWritable() || Segment.ReadOnlyAfterRelocations)
      Immutable.push_back({Segment.VA, Segment.VA + Segment.Size});
  }
  for (const auto &[Call, Checked] : Proof->Calls) {
    Contract.ImageWrites.emplace(Call, Checked.Contract.ImageWrites);
    for (va_t Slot : Checked.Contract.RequiredImports) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return coff_registration::rejectIR("C++ import closure exceeds budget");
      Immutable.push_back({Slot, Slot + 4});
    }
    if (Checked.Contract.isCxxParent())
      for (const auto &Info : Checked.Contract.RuntimeThrowInfos) {
        const auto Object =
            getCheckedX86RegistrationThrowObjectABI(Image, Info.Address, &Work);
        if (!Object)
          return coff_registration::rejectIR(
              "C++ parent throw metadata changed");
        RecordDestruction(Object->Info);
        Immutable.insert(Immutable.end(), Object->Info.ReadOnlyRanges.begin(),
                         Object->Info.ReadOnlyRanges.end());
        Immutable.push_back(Object->Info.TypeDescriptorRange);
      }
    if (Checked.RuntimeObject)
      if (auto Error = coff_registration::bindCxxRuntimeObjectBorrow(
              *Proof, *Call, Checked, Contract))
        return Error;
    if (Checked.ObjectFrameOffset) {
      auto [It, Inserted] = Contract.Borrows.emplace(
          Call,
          coff_registration::RegistrationFrameBorrow{
              int64_t(Proof->Frame.Establisher) + *Checked.ObjectFrameOffset,
              Checked.Contract.ECXReads, Checked.Contract.ECXWrites});
      auto &Borrow = It->second;
      Borrow.Initializer = Checked.Contract.isInitializer();
      Borrow.ScalarStackWords = Checked.Contract.StackPopBytes / 4;
      Borrow.Initialized = Checked.Contract.ECXInitialized;
    }
    if (Checked.Contract.isRuntimeThrow()) {
      if (auto Error = coff_registration::bindCxxRuntimeThrow(
              *Proof, *Call, Checked, Image, Contract, Immutable, Work))
        return Error;
      if (Checked.RuntimeThrow && !Checked.RuntimeThrow->isRethrow()) {
        const auto Object = getCheckedX86RegistrationThrowObjectABI(
            Image, Checked.RuntimeThrow->ThrowInfoVA, &Work);
        if (!Object)
          return coff_registration::rejectIR(
              "C++ runtime throw destruction metadata changed");
        RecordDestruction(Object->Info);
      }
    } else if (Checked.Contract.isThrow()) {
      auto Throw = getCheckedX86RegistrationThrowCalleeABI(
          Image, Checked.Contract.Target, &Work);
      if (!Throw || Throw->IsRethrow != Checked.Contract.isRethrow())
        return coff_registration::rejectIR(
            "C++ private throw lost its original runtime ABI");
      Immutable.push_back({Throw->ImportIATVA, Throw->ImportIATVA + 4});
      if (!Throw->IsRethrow) {
        RecordDestruction(Throw->ThrowInfo);
        Immutable.insert(Immutable.end(),
                         Throw->ThrowInfo.ReadOnlyRanges.begin(),
                         Throw->ThrowInfo.ReadOnlyRanges.end());
        Immutable.push_back(Throw->ThrowInfo.TypeDescriptorRange);
      }
    }
  }
  for (const auto &[Identity, Index] : CatchIndices) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return coff_registration::rejectIR(
          "C++ catch destruction closure exceeds budget");
    const auto &Catch =
        Source.Cxx->TryBlocks[Identity.first].Handlers[Identity.second];
    const auto Found = TrivialDestruction.find(Catch.TypeDescriptorVA);
    Contract.Catches[Index].TrivialDestruction =
        Catch.TypeDescriptorVA && Found != TrivialDestruction.end() &&
        Found->second;
  }
  std::map<std::pair<va_t, uint32_t>, const llvm::Instruction *> Operations;
  for (const auto &Block : Function)
    for (const auto &I : Block)
      if (const auto *MD =
              I.getMetadata(windows_eh_md::RegistrationOperationAttachment)) {
        auto Address = coff_registration::metadataInteger(*MD, 1, 64);
        auto Seq = coff_registration::metadataInteger(*MD, 2, 32);
        if (!Address || !Seq ||
            !Operations.emplace(std::make_pair(*Address, uint32_t(*Seq)), &I)
                 .second)
          return coff_registration::rejectIR(
              "C++ frame operation identity is duplicated");
      }
  for (const auto &Access : States.RuntimeObjectAccesses) {
    const auto Found =
        Operations.find({Access.Address, uint32_t(Access.OpSeq)});
    const auto Index = CatchIndices.find({Access.TryIndex, Access.CatchIndex});
    if (Found == Operations.end() || Index == CatchIndices.end() ||
        !Contract.Catches[Index->second].Reference ||
        !Contract.RuntimeAccesses
             .emplace(Found->second,
                      coff_registration::RegistrationRuntimeAccess{
                          Access.Offset, Access.Width, Access.Write,
                          Contract.Catches[Index->second].Catch})
             .second)
      return coff_registration::rejectIR(
          "C++ runtime object lost its exact source access");
  }
  for (const auto &Range : Immutable)
    if (!Range.isValid() || Range.End > uint64_t(UINT32_MAX) + 1)
      return coff_registration::rejectIR(
          "C++ immutable runtime range is malformed");
  return coff_registration::validateFramePrivacy(
      Function, {}, Proof->Frame.Slot, {}, Proof->IncomingAccesses,
      Proof->CallerPCWrites, Proof->ChainReads, 0, Immutable, &Contract);
}
} // namespace neverd

namespace neverd::coff_registration {
llvm::Expected<coff_loader::X86CxxPersonalityABI>
getCheckedCxxRuntimeIR(const llvm::Function &Function,
                       const ExceptionFunction &Source,
                       const BinaryImage &Image) {
  if (auto Runtime = coff_loader::getCheckedX86CxxPersonalityABI(Image, Source))
    return *Runtime;
  const auto Handler = coff_loader::getCheckedX86CxxHandlerABI(Image, Source);
  if (!Handler || Handler->CookieGuards.empty())
    return rejectIR("C++ runtime has no checked original dispatch");
  if (auto Error = validateCOFFRegistrationCxxIR(Function, Source, Image))
    return std::move(Error);
  return Handler->Dispatch;
}
} // namespace neverd::coff_registration
