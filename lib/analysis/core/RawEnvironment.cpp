//===- RawEnvironment.cpp - Raw recovery integrity ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "RawEnvironment.h"

#include "RecoveryArchitecture.h"
#include "RecoveryDigest.h"

#include "neverd/ir/low/LowRawEnvironment.h"
#include "neverd/ir/low/LowUndefinedEffects.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Errc.h"

namespace neverd::analysis {
namespace {
llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s", Message);
}

bool validDigest(llvm::StringRef Text) {
  return Text.size() == 64 && llvm::all_of(Text, [](char C) {
           return (C >= '0' && C <= '9') || (C >= 'A' && C <= 'F');
         });
}

std::string domainDigest(const SpecializationRawEnvironment &D,
                         llvm::StringRef OptionsDigest) {
  RecoveryDigest H("neverd-raw-environment-domain-v1");
  H.number(D.Version);
  H.text(OptionsDigest);
  H.number(D.PrimaryImageBegin);
  H.number(D.PrimaryImageEnd);
  H.text(D.PrimaryImageDigest);
  return H.finish();
}

llvm::Error validatePrimaryExtent(const SpecializationRawEnvironment &D) {
  if (D.Version != 1 || !D.Declaration.hasRawEnvironment() ||
      !validDigest(D.PrimaryImageDigest) ||
      D.PrimaryImageBegin >= D.PrimaryImageEnd)
    return invalid("invalid raw environment primary image evidence");
  auto Architecture = recoveryArchitecture(D.Declaration.Architecture);
  if (!Architecture)
    return Architecture.takeError();
  return (*Architecture)->validateRawExtent(D);
}

llvm::Error preflightResult(const SpecializationResult &R,
                            const SpecializationOptions &Expected) {
  // Independent expected limits bound all later graph/receipt walks, including
  // the older external-exit validator. Stored limits are never consulted.
  auto Architecture = recoveryArchitecture(Expected.Architecture);
  if (!Architecture)
    return Architecture.takeError();
  RecoveryValidationBudget Budget{Expected.MaxRawEnvironmentResultBytes / 8,
                                  Expected.MaxRawEnvironmentResultRecords / 8};
  const auto AddBytes = [&](uint64_t N) { return Budget.bytes(N); };
  const auto AddRecords = [&](uint64_t N, uint64_t Width) {
    return Budget.records(N, Width);
  };
  const auto Exhausted = [] { return RecoveryValidationBudget::exhausted(); };
  const auto &F = R.Residual;
  if (!AddBytes(2048) || !AddBytes(F.Name.size()) ||
      !AddBytes(F.DebugName.size()) || !AddBytes(F.SourceFile.size()) ||
      !AddBytes(R.Diagnostic.size()) || !AddRecords(F.Blocks.size(), 128) ||
      !AddRecords(F.ModuleAnalysisRoots.size(), 8) ||
      !AddRecords(F.OrdinaryModuleAnalysisRoots.size(), 8) ||
      !AddRecords(F.FunctionTemporaries.size(), 32) ||
      !AddRecords(R.Origins.size(), 160) || !AddRecords(R.Reads.size(), 128) ||
      !AddRecords(R.ExternalExits.size(), 2048))
    return Exhausted();
  if (auto Error =
          (*Architecture)->reserveExternalExitValidation(R, Expected, Budget))
    return Error;
  for (const auto &B : F.Blocks)
    if (!AddRecords(B.Ops.size(), 512) ||
        !AddRecords(B.InstructionBoundaries.size(), 128) ||
        !AddRecords(B.Preds.size(), 8) || !AddRecords(B.Succs.size(), 8))
      return Exhausted();
  for (const auto &Read : R.Reads)
    if (!AddBytes(Read.Bytes.size()) || !AddBytes(Read.Evidence.size()))
      return Exhausted();
  for (const auto &E : R.ExternalExits) {
    const auto &I = E.Original;
    if (!AddRecords(I.Ops.size(), 512) || !AddBytes(I.NativeBytes.size()) ||
        !AddRecords(I.UndefinedEffects.Effects.size(), 256) ||
        !AddBytes(I.UndefinedEffects.OperationDigest.size()) ||
        !AddBytes(I.UndefinedEffects.Diagnostic.size()) ||
        !AddBytes(I.PreservedState.NativeBytesDigest.size()) ||
        !AddBytes(I.PreservedState.OperationDigest.size()) ||
        !AddBytes(E.IdentityDigest.size()) ||
        !AddBytes(E.OriginalOperationDigest.size()) ||
        !AddBytes(E.ExpandedOperationDigest.size()) ||
        !AddBytes(E.ResidualOperationDigest.size()))
      return Exhausted();
    // Validate before legacy helpers index the fixed operand storage.
    for (const auto &Op : I.Ops)
      if (Op.NumInputs > std::size(Op.Inputs))
        return invalid("raw environment exit has an invalid operand count");
  }
  return llvm::Error::success();
}

llvm::Expected<std::string> graphDigest(const LowFunc &F) {
  // The raw publisher creates ordinary blocks and explicit roots, not stale
  // loader/EH/address-occurrence certificates. Refuse unfamiliar sidecars
  // instead of silently leaving them outside the integrity boundary.
  if (!F.JumpTables.empty() || !F.CxxContinuationExits.empty() ||
      !F.CxxContinuationEntries.empty() ||
      !F.SEHLocalUnwindContinuations.empty() ||
      F.CxxContinuationExitAnalysisComplete ||
      !F.RelocatedInstructionAddressOccurrences.empty() ||
      !F.I386GetPcOccurrences.empty() ||
      !F.RelocatedInstructionScalarModelOccurrences.empty() ||
      !F.EverPublishedJumpTableBranchAddresses.empty() ||
      !F.UnsafeIndirectBranchAddresses.empty() || F.ExceptionMetadata ||
      F.RegistrationStates || !F.DecodeFailureAddresses.empty() ||
      !F.UnsupportedInstructionAddresses.empty() ||
      !F.UnprovenReturnAddresses.empty() || !F.TruncatedPathAddresses.empty() ||
      F.CalleePopBytes || !F.CodeRefTargets.empty())
    return invalid("raw environment graph carries unsupported sidecars");
  // Graphs whose operations fit six stored input slots keep the v1 bytes.
  // Extended operands or stored extension bits select v2, binding every slot.
  const bool V1 = llvm::all_of(F.Blocks, [](const auto &B) {
    return llvm::all_of(B.Ops, lowOperationFitsV1InputSlots);
  });
  const size_t InputSlots =
      V1 ? kLowUndefinedV1InputSlots : std::size(LowOp{}.Inputs);
  RecoveryDigest H(V1 ? "neverd-raw-environment-graph-v1"
                      : "neverd-raw-environment-graph-v2");
  H.number(F.Entry);
  H.number(F.OriginalSize);
  H.text(F.Name);
  H.text(F.DebugName);
  H.text(F.SourceFile);
  H.number(F.SourceLine);
  H.number(F.DecodedInstructionCount);
  H.number(F.LiftedInstructionCount);
  H.numbers(F.ModuleAnalysisRoots);
  H.numbers(F.OrdinaryModuleAnalysisRoots);
  H.number(F.FunctionTemporaries.size());
  for (const auto &T : F.FunctionTemporaries) {
    H.number(T.Offset);
    H.number(T.Bytes);
  }
  H.number(F.Blocks.size());
  for (const auto &B : F.Blocks) {
    if (!B.ExceptionalSuccs.empty() || !B.ExceptionalPreds.empty())
      return invalid("raw environment graph carries exceptional edges");
    H.number(B.Id);
    H.number(B.StartAddr);
    H.number(B.EndAddr);
    H.numbers(B.Preds);
    H.numbers(B.Succs);
    H.number(B.Ops.size());
    for (const auto &Op : B.Ops) {
      if (Op.NumInputs > std::size(Op.Inputs))
        return invalid("raw environment graph has an invalid operand count");
      H.operation(Op, InputSlots);
    }
    H.number(B.InstructionBoundaries.size());
    for (const auto &Boundary : B.InstructionBoundaries)
      H.boundary(Boundary);
  }
  if (auto Error = validateLowInstructionBoundaries(
          F, LowInstructionBoundaryRequirement::Required))
    return std::move(Error);
  return H.finish();
}

std::string evidenceDigest(const SpecializationResult &R) {
  // As for the graph: exits whose original operations fit six stored input
  // slots keep the v1 bytes, and any other exit selects v2.
  const bool V1 = llvm::all_of(R.ExternalExits, [](const auto &E) {
    return llvm::all_of(E.Original.Ops, lowOperationFitsV1InputSlots);
  });
  const size_t InputSlots =
      V1 ? kLowUndefinedV1InputSlots : std::size(LowOp{}.Inputs);
  RecoveryDigest H(V1 ? "neverd-raw-environment-evidence-v1"
                      : "neverd-raw-environment-evidence-v2");
  H.number(R.Origins.size());
  for (const auto &O : R.Origins) {
    H.number(O.ResidualAddress);
    H.boundary(O.NativeInstruction);
  }
  H.number(R.Reads.size());
  for (const auto &Read : R.Reads) {
    H.number(Read.InstructionAddress);
    H.number(Read.OpSeq);
    H.number(Read.Address);
    H.bytes(Read.Bytes);
    H.text(Read.Evidence);
  }
  H.number(R.ExternalExits.size());
  for (const auto &E : R.ExternalExits) {
    // The external-exit validator independently recomputes this identity from
    // the complete original instruction, physical expansion and occurrence.
    H.text(E.IdentityDigest);
    H.text(E.Original.UndefinedEffects.Diagnostic);
    // Also bind inactive fixed operand slots, which the older semantic
    // operation digest deliberately ignores. Raw receipt integrity is exact.
    H.number(E.Original.Ops.size());
    for (const auto &Op : E.Original.Ops)
      H.operation(Op, InputSlots);
  }
  H.number(static_cast<unsigned>(R.Status));
  H.text(R.Diagnostic);
  H.number(R.EvaluatedOperations);
  H.number(R.NodeEvaluations);
  H.number(R.Contexts);
  H.number(R.SolverQueries);
  H.number(R.RelationalWidenings);
  H.number(R.DiscoveredControlFields);
  H.number(R.DiscoveredContextFields);
  H.number(R.ControlRefinements);
  H.number(R.DiscoveryVisits);
  return H.finish();
}

std::string receiptDigest(const SpecializationRawEnvironmentReceipt &R) {
  RecoveryDigest H("neverd-raw-environment-receipt-v1");
  H.number(R.Version);
  H.text(R.Domain->DomainDigest);
  H.text(R.GraphDigest);
  H.text(R.EvidenceDigest);
  return H.finish();
}
} // namespace

llvm::Expected<std::shared_ptr<const SpecializationRawEnvironment>>
prepareRawEnvironment(const SpecializationOptions &Options, va_t PrimaryBegin,
                      va_t PrimaryEnd, llvm::StringRef PrimaryDigest) {
  auto Architecture = recoveryArchitecture(Options.Architecture);
  if (!Architecture)
    return Architecture.takeError();
  auto Cost = (*Architecture)->validateRawDeclarations(Options);
  if (!Cost)
    return Cost.takeError();
  auto D = std::make_shared<SpecializationRawEnvironment>();
  D->Declaration = Options;
  D->PrimaryImageBegin = PrimaryBegin;
  D->PrimaryImageEnd = PrimaryEnd;
  D->PrimaryImageDigest = PrimaryDigest;
  if (auto Error = validatePrimaryExtent(*D))
    return std::move(Error);
  D->DomainDigest =
      domainDigest(*D, (*Architecture)->rawOptionsDigest(Options));
  return D;
}

llvm::Error validatePreparedRawEnvironment(
    const std::shared_ptr<const SpecializationRawEnvironment> &Domain,
    const SpecializationOptions &Options) {
  if (!Options.hasRawEnvironment())
    return Domain ? invalid("unexpected prepared raw environment")
                  : llvm::Error::success();
  auto Architecture = recoveryArchitecture(Options.Architecture);
  if (!Architecture)
    return Architecture.takeError();
  auto ExpectedCost = (*Architecture)->validateRawDeclarations(Options);
  if (!ExpectedCost)
    return ExpectedCost.takeError();
  if (!Domain)
    return invalid("raw environment declarations require a prepared provider");
  if (Domain->Declaration.Architecture != Options.Architecture)
    return invalid("raw environment differs from its expected architecture");
  // Never let a stored declaration authorize its own larger traversal budget.
  // Compare these scalars before examining any stored collection or payload.
  if (Domain->Declaration.MaxImagePreparationBytes !=
          Options.MaxImagePreparationBytes ||
      Domain->Declaration.MaxImagePreparationRecords !=
          Options.MaxImagePreparationRecords ||
      Domain->Declaration.MaxRawEnvironmentResultBytes !=
          Options.MaxRawEnvironmentResultBytes ||
      Domain->Declaration.MaxRawEnvironmentResultRecords !=
          Options.MaxRawEnvironmentResultRecords)
    return invalid("raw environment differs from its expected budgets");
  auto StoredCost =
      (*Architecture)->validateRawDeclarations(Domain->Declaration);
  if (!StoredCost)
    return StoredCost.takeError();
  if (auto Error = validatePrimaryExtent(*Domain))
    return Error;
  const auto Expected = (*Architecture)->rawOptionsDigest(Options);
  const auto Stored = (*Architecture)->rawOptionsDigest(Domain->Declaration);
  if (Expected != Stored ||
      Domain->DomainDigest != domainDigest(*Domain, Stored))
    return invalid("raw environment differs from its expected declaration");
  return llvm::Error::success();
}

void appendRawEnvironmentPolicy(RecoveryDigest &H,
                                const SpecializationOptions &O) {
  H.number(O.DiscoverControlState);
  H.number(O.ControlRegisters.size());
  for (const auto &R : O.ControlRegisters) {
    H.number(R.Offset);
    H.number(R.Bytes);
  }
  H.number(O.FrameBaseRegister.has_value());
  if (O.FrameBaseRegister) {
    H.number(O.FrameBaseRegister->Offset);
    H.number(O.FrameBaseRegister->Bytes);
  }
  H.number(O.EntryFrameBounds.has_value());
  if (O.EntryFrameBounds) {
    H.number(O.EntryFrameBounds->Begin);
    H.number(O.EntryFrameBounds->End);
    if (!O.EntryFrameBounds->ExcludedAddressRanges.empty()) {
      H.text("entry-frame-exclusions-v1");
      H.number(O.EntryFrameBounds->ExcludedAddressRanges.size());
      for (const auto &Range : O.EntryFrameBounds->ExcludedAddressRanges) {
        H.number(Range.Begin);
        H.number(Range.End);
      }
    }
  }
  if (!O.EntryFrameExcludedObjects.empty()) {
    H.text("entry-frame-excluded-objects-v1");
    H.number(O.EntryFrameExcludedObjects.size());
    for (const auto &Object : O.EntryFrameExcludedObjects) {
      H.number(Object.Register.Offset);
      H.number(Object.Register.Bytes);
      H.number(Object.Begin);
      H.number(Object.End);
    }
  }
  H.number(O.EntryFrameAlignment.has_value());
  if (O.EntryFrameAlignment) {
    H.number(O.EntryFrameAlignment->Alignment);
    H.number(O.EntryFrameAlignment->Residue);
  }
  H.number(O.ControlFrameSlots.size());
  for (const auto &R : O.ControlFrameSlots) {
    H.number(R.Offset);
    H.number(R.Bytes);
  }
  H.number(O.RequireRestoredFrameAtReturn);
  H.number(O.ProveStoreReturnSlotSeparation);
  if (O.ProveStoreFrameSeparation)
    H.text("proved-store-frame-separation-v1");
  H.number(O.ExternalStoresPreserveEntryReturnSlot);
  H.number(O.ExternalStoresDisjointEntryFrame);
  H.number(O.EntryConstants.size());
  for (const auto &C : O.EntryConstants) {
    H.variable(C.Location);
    H.number(C.Value);
  }
  H.number(static_cast<unsigned>(O.ByteOrder));
  H.number(O.MaxImagePreparationBytes);
  H.number(O.MaxImagePreparationRecords);
  H.number(O.MaxRawEnvironmentResultBytes);
  H.number(O.MaxRawEnvironmentResultRecords);
  H.number(O.MaxNodes);
  H.number(O.MaxContextsPerAddress);
  H.number(O.MaxOperations);
  H.number(O.MaxNodeEvaluations);
  H.number(O.MaxIndirectTargets);
  H.number(O.MaxChainedTransfers);
  H.number(O.StopChainingAtRepeatedDestination);
  H.number(O.MaxNativeReturnSlots);
  H.number(O.MaxImmutableReadAddresses);
  H.number(O.MaxControlTuples);
  H.number(O.MaxControlFields);
  H.number(O.MaxControlRefinements);
  H.number(O.MaxDiscoveryVisits);
  H.number(O.MaxSolverQueries);
  H.number(O.MaxSolverGates);
  H.number(O.MaxSolverConflicts);
  H.number(O.MaxSolverPropagations);
  H.number(O.MaxSolverWatchVisits);
  H.number(O.MaxSymbolicNodes);
  H.number(O.MaxChainedVisitsPerDestination);
  H.number(O.MaxFrameOffsetNormalizationWorkPerProof);
}

std::array<uint64_t, 4>
rawEnvironmentDigestWords(const SpecializationRawEnvironment &Domain) {
  assert(validDigest(Domain.DomainDigest));
  const auto Bytes = llvm::fromHex(Domain.DomainDigest);
  std::array<uint64_t, 4> Words;
  for (size_t I = 0; I != Words.size(); ++I)
    Words[I] = llvm::support::endian::read64le(Bytes.data() + I * 8);
  return Words;
}

llvm::Error
validateSpecializationRawEnvironment(const SpecializationResult &Result,
                                     const SpecializationOptions &Options) {
  if (!Options.hasRawEnvironment()) {
    if (Result.RawEnvironment ||
        hasLowRawEnvironmentRequirement(Result.Residual))
      return invalid("raw environment evidence lacks its expected declaration");
    return llvm::Error::success();
  }
  if (!Result.complete() || !Result.RawEnvironment ||
      Result.RawEnvironment->Version != 1)
    return invalid("raw environment requires a complete versioned receipt");
  if (auto Error = preflightResult(Result, Options))
    return Error;
  const auto &Receipt = *Result.RawEnvironment;
  if (auto Error = validatePreparedRawEnvironment(Receipt.Domain, Options))
    return Error;
  const auto &F = Result.Residual;
  if (F.Blocks.empty() || F.Blocks.front().Id != 0 ||
      F.Blocks.front().StartAddr != F.Entry ||
      F.Blocks.front().EndAddr != F.Entry + 1 ||
      !F.Blocks.front().Preds.empty() || F.Blocks.front().Ops.size() != 2 ||
      F.Blocks.front().Ops.back().Opcode != NdOp::BRANCH)
    return invalid("raw environment requires its original entry anchor");
  const auto &Marker = F.Blocks.front().Ops.front();
  if (!lowRawEnvironmentRequirementShapeIsValid(Marker) ||
      Marker.Addr != F.Entry || Marker.Seq != 0)
    return invalid("raw environment entry requirement is missing or malformed");
  const auto Words = rawEnvironmentDigestWords(*Receipt.Domain);
  for (size_t I = 0; I != Words.size(); ++I)
    if (Marker.Inputs[I + 1].Offset != Words[I])
      return invalid("raw environment entry requirement has a stale digest");
  size_t Occurrences = 0;
  for (const auto &B : F.Blocks)
    for (const auto &Op : B.Ops)
      Occurrences += isLowRawEnvironmentRequirement(Op);
  if (Occurrences != 1)
    return invalid("raw environment requirement must occur exactly once");
  auto Graph = graphDigest(F);
  if (!Graph)
    return Graph.takeError();
  if (*Graph != Receipt.GraphDigest)
    return invalid("raw environment graph differs from its receipt");
  if (auto Error = validateSpecializationExternalExits(Result, Options))
    return Error;
  if (Receipt.EvidenceDigest != evidenceDigest(Result) ||
      Receipt.IdentityDigest != receiptDigest(Receipt))
    return invalid("raw environment evidence differs from its receipt");
  return llvm::Error::success();
}

llvm::Error sealRawEnvironment(
    SpecializationResult &Result,
    const std::shared_ptr<const SpecializationRawEnvironment> &Domain,
    const SpecializationOptions &Options) {
  if (auto Error = preflightResult(Result, Options))
    return Error;
  auto Graph = graphDigest(Result.Residual);
  if (!Graph)
    return Graph.takeError();
  SpecializationRawEnvironmentReceipt Receipt;
  Receipt.Domain = Domain;
  Receipt.GraphDigest = std::move(*Graph);
  Receipt.EvidenceDigest = evidenceDigest(Result);
  Receipt.IdentityDigest = receiptDigest(Receipt);
  Result.RawEnvironment = std::move(Receipt);
  return validateSpecializationRawEnvironment(Result, Options);
}
} // namespace neverd::analysis
