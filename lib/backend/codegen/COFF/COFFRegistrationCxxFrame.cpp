//===- COFFRegistrationCxxFrame.cpp - PE32 C++ frame proof ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "COFFRegistrationCxxIRProof.h"
#include "COFFRegistrationFrameProof.h"

#include "neverd/Limits.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
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
  const auto &States = *Proof->Source.RegistrationStates;
  coff_registration::RegistrationCxxFrameContract Contract;
  Contract.Image = &Image;
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
    if (Checked.ObjectFrameOffset)
      Contract.Borrows.emplace(
          Call,
          coff_registration::RegistrationFrameBorrow{
              int64_t(Proof->Frame.Establisher) + *Checked.ObjectFrameOffset,
              Checked.Contract.ECXReads, Checked.Contract.ECXWrites});
    if (Checked.Contract.isRuntimeThrow()) {
      if (auto Error = coff_registration::bindCxxRuntimeThrow(
              *Proof, *Call, Checked, Image, Contract, Immutable, Work))
        return Error;
    } else if (Checked.Contract.isThrow()) {
      auto Throw = getCheckedX86RegistrationThrowCalleeABI(
          Image, Checked.Contract.Target, &Work);
      if (!Throw || Throw->IsRethrow != Checked.Contract.isRethrow())
        return coff_registration::rejectIR(
            "C++ private throw lost its original runtime ABI");
      Immutable.push_back({Throw->ImportIATVA, Throw->ImportIATVA + 4});
      if (!Throw->IsRethrow) {
        Immutable.insert(Immutable.end(),
                         Throw->ThrowInfo.ReadOnlyRanges.begin(),
                         Throw->ThrowInfo.ReadOnlyRanges.end());
        Immutable.push_back(Throw->ThrowInfo.TypeDescriptorRange);
      }
    }
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
  // A preserved call must obey the same image closure as the edited parent.
  for (const auto &[Call, Checked] : Proof->Calls)
    for (const auto &Write : Checked.Contract.ImageWrites)
      for (const auto &Range : Immutable) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return coff_registration::rejectIR(
              "C++ runtime image proof exhausted its work budget");
        if (Write.overlaps(Range))
          return coff_registration::rejectIR(
              "C++ preserved callee mutates language metadata or runtime "
              "dispatch");
      }
  return coff_registration::validateFramePrivacy(
      Function, {}, Proof->Frame.Slot, {}, Proof->IncomingAccesses,
      Proof->CallerPCWrites, Proof->ChainReads, 0, Immutable, &Contract);
}
} // namespace neverd
