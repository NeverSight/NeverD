//===- SBFSolanaRecovery.cpp - Solana program fact recovery ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Drives Solana fact recovery over an analyzed program: scans read-only data
/// for addresses and discriminators, replays every block through the
/// per-instruction visitors, then resolves the dispatch arms, the returned
/// error codes, and the lint findings.
///
//===----------------------------------------------------------------------===//

#include "neverd/sbf/solana/SBFSolanaRecovery.h"

#include "SBFSolanaRecoveryDetail.h"

#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/SBFIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::sbf {

using namespace solana_recovery_detail;

namespace solana_recovery_detail {

void Recovery::scanReadOnlyData() {
  // Index both tables by their leading word so a region scan costs one hash
  // probe per offset instead of one comparison per table entry.
  llvm::DenseMap<uint64_t, const KnownAddressInfo *> AddressByFirstWord;
  for (const KnownAddressInfo &Info : knownAddressInfos()) {
    // The all-zero System Program address would match every zero run in
    // read-only data, so it is only recognized when code references it.
    if (!Info.Decoded || Info.Key.isZero())
      continue;
    AddressByFirstWord.try_emplace(
        llvm::support::endian::read64le(Info.Key.Bytes.data()), &Info);
  }

  // A discriminator is only eight bytes, so one probe is the whole comparison.
  // Anchor derives them by truncating SHA-256, which makes an accidental match
  // as unlikely as a collision in that hash.
  llvm::DenseSet<uint64_t> NamedWords;
  for (const AnchorNameInfo &Info : anchorNameInfos())
    NamedWords.insert(Info.Discriminator.toWord());
  if (Options.Idl)
    for (const AnchorIdlItem &Item : Options.Idl->Items)
      NamedWords.insert(Item.Discriminator.toWord());

  for (const ProgramRegion &Region : Program.ExecutableImage.regions()) {
    if (!Region.DataVisible || Region.Bytes.size() < kAnchorDiscriminatorLength)
      continue;
    const size_t LastWord = Region.Bytes.size() - kAnchorDiscriminatorLength;
    const bool HoldsKey = Region.Bytes.size() >= kPubkeyByteCount;
    const size_t LastKey =
        HoldsKey ? Region.Bytes.size() - kPubkeyByteCount : 0;

    for (size_t Offset = 0; Offset <= LastWord; ++Offset) {
      const uint64_t Word =
          llvm::support::endian::read64le(Region.Bytes.data() + Offset);

      if (NamedWords.contains(Word)) {
        RecoveredDiscriminator Found;
        Found.Address = Region.Address + Offset;
        Found.Value = AnchorDiscriminator::fromWord(Word);
        std::optional<RecoveryEvidence> Evidence;
        if (nameDiscriminator(Found.Value, Found.Name, Found.Namespace,
                              Evidence)) {
          Found.Evidence = *Evidence;
          Model.Discriminators.push_back(std::move(Found));
        }
      }

      if (!HoldsKey || Offset > LastKey)
        continue;
      auto Address = AddressByFirstWord.find(Word);
      if (Address == AddressByFirstWord.end())
        continue;
      const llvm::ArrayRef<uint8_t> Window =
          llvm::ArrayRef(Region.Bytes).slice(Offset, kPubkeyByteCount);
      if (!llvm::equal(Window, Address->second->Key.Bytes))
        continue;
      noteKey(Region.Address + Offset, Address->second->Key,
              /*ReferencedByCode=*/false);
    }
  }
}

void Recovery::resolveHandlers() {
  std::vector<AnchorHandler> Resolved;
  Resolved.reserve(Candidates.size());
  bool AnyNamed = false;

  for (const Candidate &Entry : Candidates) {
    AnchorHandler Handler;
    Handler.Discriminator = AnchorDiscriminator::fromWord(Entry.Word);
    Handler.CompareSlot = Entry.CompareSlot;
    Handler.TargetSlot = Entry.TargetSlot;

    AnyNamed |= nameDiscriminator(Handler.Discriminator, Handler.Name,
                                  Handler.Namespace, Handler.NameEvidence);
    Resolved.push_back(std::move(Handler));
  }

  // A 64-bit constant comparison only means "discriminator" once at least one
  // of them is a known Anchor name. Without that anchor, these are ordinary
  // constant comparisons and reporting them would be a guess.
  if (!AnyNamed)
    return;
  Model.IsAnchor = true;
  Model.Handlers = std::move(Resolved);
  if (Options.Idl)
    Model.IdlName = Options.Idl->Name;
}

void Recovery::resolveErrors() {
  // A number in a framework band is only a framework error code when the
  // program uses that framework. In anything else the same value is an
  // ordinary constant, and reporting it would invent a diagnosis.
  for (const ReturnedError &Candidate : ErrorCandidates) {
    if (!Candidate.Classification.Known && !Model.IsAnchor)
      continue;
    const bool Duplicate =
        llvm::any_of(Model.Errors, [&](const ReturnedError &Recorded) {
          return Recorded.Code == Candidate.Code;
        });
    if (!Duplicate)
      Model.Errors.push_back(Candidate);
  }
  llvm::stable_sort(Model.Errors,
                    [](const ReturnedError &Left, const ReturnedError &Right) {
                      return Left.Code < Right.Code;
                    });
}

void Recovery::addLints() {
  auto Report = [&](Lint ID, std::optional<size_t> Slot, std::string Detail) {
    Model.Findings.push_back({ID, Slot, std::move(Detail)});
  };

  for (const CPISite &Site : Model.CPISites) {
    if (!Site.ProgramId)
      Report(Lint::UnresolvedCPITarget, Site.Slot,
             "the invoked program id is computed at run time");
    if (Site.Selected && Site.Selected->Status == InstructionStatus::Deprecated)
      Report(Lint::DeprecatedProgramInstruction, Site.Slot,
             (Site.KnownProgram->Name + "::" + Site.Selected->Name).str());
  }

  // The signer and owner checks need at least one resolvable account access;
  // without one there is no evidence either way, and silence is the honest
  // report.
  if (!Model.AccountAccesses.empty()) {
    auto Reads = [&](AccountField Field) {
      return llvm::any_of(
          Model.AccountAccesses,
          [&](const AccountAccess &Access) { return Access.Field == Field; });
    };
    if (!Model.CPISites.empty() && !Reads(AccountField::IsSigner))
      Report(Lint::MissingSignerCheck, std::nullopt,
             "no recovered access reads is_signer");
    if (!Reads(AccountField::Owner))
      Report(Lint::MissingOwnerCheck, std::nullopt,
             "no recovered access reads owner");
  }

  if (Model.ProgramId && !Model.IsAnchor)
    Report(Lint::MissingDiscriminatorCheck, std::nullopt,
           "the entry point compares its own program id but dispatches no "
           "recognized discriminator");

  for (const SyscallUse &Use : Program.High.Syscalls) {
    if (!Use.Info)
      continue;
    // "This name is on its way out" and "this call does not resolve on the
    // chain you asked about" are different warnings, and a reader acts on them
    // differently: one is a rewrite to schedule, the other is a program that
    // will not run.
    if (Use.Info->Lifecycle == SyscallLifecycle::Deprecated)
      Report(Lint::DeprecatedSyscall, Use.Slot, Use.Info->Name.str());
    switch (syscallRegistration(Use.Info->ID, Options.Profile)) {
    case SyscallRegistration::Registered:
      break;
    case SyscallRegistration::GateUnmet:
      Report(Lint::FeatureGatedSyscall, Use.Slot,
             Use.Info->Name.str() + " is not registered on " +
                 clusterName(Options.Profile.OnCluster).str());
      break;
    case SyscallRegistration::EnvironmentExcluded:
      Report(Lint::FeatureGatedSyscall, Use.Slot,
             Use.Info->Name.str() + " is not in the " +
                 runtimePurposeName(Options.Profile.Purpose).str() +
                 " registry");
      break;
    }
  }

  if (Program.Low.TheVersion < Version::V3)
    Report(Lint::LegacyDeploymentVersion, std::nullopt,
           versionDisplayName(Program.Low.TheVersion).str());
}

SolanaModel Recovery::run() {
  scanReadOnlyData();

  for (const MedBlock &Block : Program.Med.Blocks)
    replayBlock(
        Index, Block, Flow.entryState(Block.ID), Program.ExecutableImage,
        [&](const MedInstruction &Instruction, const MachineState &State) {
          if (Instruction.Op == Operation::Load ||
              Instruction.Op == Operation::Store)
            visitMemoryAccess(Instruction, State);
          else if (Instruction.Op == Operation::Eq ||
                   Instruction.Op == Operation::Ne)
            visitComparison(Instruction, State);
          else if (Instruction.Op == Operation::Exit)
            visitExit(Instruction, State);
          if (Instruction.Call == CallKind::Syscall && Instruction.Syscall)
            visitSyscall(Instruction, State);
        });

  resolveHandlers();
  resolveErrors();
  addLints();

  llvm::stable_sort(Model.Pubkeys, [](const RecoveredPubkey &Left,
                                      const RecoveredPubkey &Right) {
    return Left.Address < Right.Address;
  });
  return std::move(Model);
}

} // namespace solana_recovery_detail

SolanaModel recoverSolanaModel(const SBFProgram &Program,
                               const SolanaRecoveryOptions &Options) {
  if (Program.Med.Blocks.empty())
    return {};
  return Recovery(Program, Options).run();
}

} // namespace neverd::sbf
