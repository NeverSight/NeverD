//===- EVMHighAnalysis.cpp - EVM source-level fact recovery -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMHighAnalysis.h"

#include "EVMHighAnalysisDetail.h"
#include "EVMMedIRVerifier.h"
#include "EVMMemoryDataflow.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd::evm {

using detail::analyzeFallback;
using detail::analyzeReceive;
using detail::ArgumentRecovery;
using detail::classifyCall;
using detail::classifyDelegation;
using detail::classifyRevert;
using detail::constantWord;
using detail::DefiniteExecutionIndex;
using detail::discoverSelectorDispatch;
using detail::EVMMemoryDataflow;
using detail::nonPayableGuardReads;
using detail::ProducerIndex;
using detail::reachableFunctionLanes;
using detail::selectorHex;
using detail::SemanticClassifier;
using detail::SemanticKind;
using detail::SemanticValue;
using detail::storageKeyKind;
using detail::wordHexDigits;

namespace {

const LowInstruction *instructionAt(const EVMLowIR &Low, uint64_t PC) {
  const auto It =
      std::lower_bound(Low.Instructions.begin(), Low.Instructions.end(), PC,
                       [](const LowInstruction &Instruction, uint64_t Address) {
                         return Instruction.PC < Address;
                       });
  return It != Low.Instructions.end() && It->PC == PC ? &*It : nullptr;
}

llvm::Expected<std::set<uint64_t>>
blocksFor(const DefiniteExecutionIndex &Execution,
          const std::set<MedStateLaneID> &Lanes,
          llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  std::set<uint64_t> Blocks;
  for (MedStateLaneID ID : Lanes) {
    if (llvm::Error Error = NoteReferenceVisit(kEntryPC))
      return std::move(Error);
    if (const MedStateLane *Lane = Execution.lane(ID))
      Blocks.insert(Lane->LowLane.BlockPC);
  }
  return Blocks;
}

Mutability recoveredMutability(StateAccessKind Access, bool ReadsCallValue) {
  if (ReadsCallValue)
    return Mutability::Payable;
  if (Access == StateAccessKind::Unknown)
    return Mutability::NonPayable;
  switch (Access) {
  case StateAccessKind::None:
    return Mutability::Pure;
  case StateAccessKind::Read:
    return Mutability::View;
  case StateAccessKind::Write:
  case StateAccessKind::Unknown:
    return Mutability::NonPayable;
  }
  return Mutability::NonPayable;
}

llvm::Error highAnalysisError(llvm::StringRef Name, size_t Limit, uint64_t PC) {
  return llvm::make_error<llvm::StringError>(
      (llvm::Twine(kHighIRAnalysisDiagnosticPrefix) + Name +
       kAnalysisLimitSuffix + llvm::Twine(Limit) +
       kAnalysisLimitExceededSuffix + kAnalysisAtPCInfix + llvm::utohexstr(PC))
          .str(),
      llvm::inconvertibleErrorCode());
}

llvm::Error validateHighAnalysisOptions(const AnalyzeOptions &Options) {
#define EVM_ANALYSIS_LIMIT_DECODE(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT_CONTROL_FLOW(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT_MEDIUM_IR(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT_HIGH_IR(NAME, DEFAULT_VALUE)                        \
  if (Options.NAME == 0)                                                       \
    return llvm::make_error<llvm::StringError>(                                \
        (llvm::Twine(kHighIRAnalysisDiagnosticPrefix) + #NAME +                \
         kAnalysisLimitMustBePositiveSuffix)                                   \
            .str(),                                                            \
        llvm::inconvertibleErrorCode());
#define EVM_ANALYSIS_LIMIT(STAGE, NAME, DEFAULT_VALUE)                         \
  EVM_ANALYSIS_LIMIT_##STAGE(NAME, DEFAULT_VALUE)
#include "neverd/evm/analysis/EVMAnalysisLimits.def"
#undef EVM_ANALYSIS_LIMIT_DECODE
#undef EVM_ANALYSIS_LIMIT_CONTROL_FLOW
#undef EVM_ANALYSIS_LIMIT_MEDIUM_IR
#undef EVM_ANALYSIS_LIMIT_HIGH_IR
  return llvm::Error::success();
}

class HighIRBudget {
public:
  explicit HighIRBudget(const AnalyzeOptions &Options) : Options(Options) {}

  llvm::Error copyDiagnostics(EVMHighIR &High,
                              llvm::ArrayRef<Diagnostic> Source) {
    size_t Bytes = 0;
    for (const Diagnostic &Diagnostic : Source) {
      if (!fits(Bytes, Diagnostic.Message.size(),
                Options.MaxHighDiagnosticBytes))
        return highAnalysisError(kMaxHighDiagnosticBytesName,
                                 Options.MaxHighDiagnosticBytes, Diagnostic.PC);
      Bytes += Diagnostic.Message.size();
    }
    const uint64_t PC = Source.empty() ? kEntryPC : Source.front().PC;
    if (llvm::Error Error = noteDiagnostics(Source.size(), Bytes, PC))
      return Error;
    High.Diagnostics.assign(Source.begin(), Source.end());
    return llvm::Error::success();
  }

  template <typename Builder>
  llvm::Error addDiagnostic(EVMHighIR &High, uint64_t PC, size_t MessageBytes,
                            Builder &&Build) {
    if (llvm::Error Error = noteDiagnostics(1, MessageBytes, PC))
      return Error;
    High.Diagnostics.push_back({PC, std::forward<Builder>(Build)()});
    return llvm::Error::success();
  }

  llvm::Error noteFunction(uint64_t PC) {
    return charge(kMaxHighFunctionsName, Options.MaxHighFunctions, Functions, 1,
                  PC);
  }
  llvm::Error noteDispatchCandidate(uint64_t PC) {
    return charge(kMaxHighDispatchCandidatesName,
                  Options.MaxHighDispatchCandidates, DispatchCandidates, 1, PC);
  }
  llvm::Error noteRecoveredArguments(size_t Amount, uint64_t PC) {
    return charge(kMaxHighRecoveredArgumentsName,
                  Options.MaxHighRecoveredArguments, RecoveredArguments, Amount,
                  PC);
  }
  llvm::Error noteLaneVisits(size_t Amount, uint64_t PC) {
    return charge(kMaxHighLaneVisitsName, Options.MaxHighLaneVisits, LaneVisits,
                  Amount, PC);
  }
  llvm::Error noteOperationVisits(size_t Amount, uint64_t PC) {
    return charge(kMaxHighOperationVisitsName, Options.MaxHighOperationVisits,
                  OperationVisits, Amount, PC);
  }
  llvm::Error noteReferenceVisits(size_t Amount, uint64_t PC) {
    return charge(kMaxHighReferenceVisitsName, Options.MaxHighReferenceVisits,
                  ReferenceVisits, Amount, PC);
  }
  llvm::Error noteOperationPasses(size_t Operations, size_t Passes,
                                  uint64_t PC) {
    if (Operations != 0 && Passes > Options.MaxHighOperationVisits / Operations)
      return highAnalysisError(kMaxHighOperationVisitsName,
                               Options.MaxHighOperationVisits, PC);
    return noteOperationVisits(Operations * Passes, PC);
  }
  llvm::Error noteRegionBlocks(size_t Amount, uint64_t PC) {
    return charge(kMaxHighRegionBlockReferencesName,
                  Options.MaxHighRegionBlockReferences, RegionBlocks, Amount,
                  PC);
  }

private:
  llvm::Error noteDiagnostics(size_t Count, size_t Bytes, uint64_t PC) {
    if (!fits(Diagnostics, Count, Options.MaxHighDiagnostics))
      return highAnalysisError(kMaxHighDiagnosticsName,
                               Options.MaxHighDiagnostics, PC);
    if (!fits(DiagnosticBytes, Bytes, Options.MaxHighDiagnosticBytes))
      return highAnalysisError(kMaxHighDiagnosticBytesName,
                               Options.MaxHighDiagnosticBytes, PC);
    Diagnostics += Count;
    DiagnosticBytes += Bytes;
    return llvm::Error::success();
  }

  static bool fits(size_t Used, size_t Amount, size_t Limit) {
    return Used <= Limit && Amount <= Limit - Used;
  }

  static llvm::Error charge(llvm::StringRef Name, size_t Limit, size_t &Used,
                            size_t Amount, uint64_t PC) {
    if (Amount > Limit - std::min(Used, Limit))
      return highAnalysisError(Name, Limit, PC);
    Used += Amount;
    return llvm::Error::success();
  }

  const AnalyzeOptions &Options;
  size_t Diagnostics = 0;
  size_t DiagnosticBytes = 0;
  size_t Functions = 0;
  size_t DispatchCandidates = 0;
  size_t RecoveredArguments = 0;
  size_t LaneVisits = 0;
  size_t OperationVisits = 0;
  size_t ReferenceVisits = 0;
  size_t RegionBlocks = 0;
};

/// Global fact and error recovery each perform one complete operation-table
/// pass. Root dispatcher, receive, fallback, and memory walks charge the work
/// they actually perform before each visit.
inline constexpr size_t kHighGlobalOperationPasses = 2;

/// Whether the bytecode's recovered calldata use remains compatible with a
/// signature that merely shares its four-byte selector.
///
/// A selector exhibits one Keccak prefix, not a unique preimage. Absence of
/// argument evidence therefore leaves the dictionary candidate intact, while
/// an extra head slot or a contradictory observed type disproves it.
bool isABICompatible(const KnownSignatureInfo &Candidate,
                     const ArgumentRecovery &Arguments) {
  const llvm::SmallVector<llvm::StringRef, 8> Declared =
      signatureArgumentTypes(Candidate.Signature);
  if (Arguments.count() > Declared.size())
    return false;
  for (size_t Position = 0; Position < Arguments.count(); ++Position) {
    const ABIConstraint &Constraint = Arguments.constraint(Position);
    if (!Constraint.empty() &&
        Constraint.resolve().spelling() != Declared[Position])
      return false;
  }
  return true;
}

/// Evidence that the program implements an interface, kept separate from the
/// dictionary match that merely names one four-byte selector.
class StandardEvidence {
public:
  StandardEvidence() : ByStandard(kKnownStandardCount) {}

  void noteCompatibleFunction(const RecoveredFunction &Function) {
    if (!Function.Known || Function.Known->Kind != SignatureKind::Function ||
        Function.Known->Selector != Function.Selector)
      return;
    for (const KnownFunctionVariantInfo *Variant :
         knownFunctionVariants(*Function.Known))
      if (Variant->contributesIndependentSelectorEvidence())
        evidence(Variant->Standard)
            .CompatibleFunctionSelectors.insert(Function.Selector);
  }

  void noteExactEvent(const EventFact &Event) {
    if (!Event.Known || Event.Known->Kind != SignatureKind::Event ||
        !Event.Known->Event || !Event.Topic0 ||
        *Event.Topic0 != Event.Known->Topic ||
        Event.Topics != Event.Known->Event->totalTopicCount())
      return;
    evidence(Event.Known->Event->Standard).Strong = true;
  }

  void noteKnownStorage(const StorageFact &Storage) {
    if (Storage.Known)
      evidence(Storage.Known->Standard).Strong = true;
  }

  void noteKnownProxy(const ProxyFact &Proxy) {
    if (Proxy.Known)
      evidence(Proxy.Known->Standard).Strong = true;
  }

  void appendRecognizedStandards(std::vector<KnownStandard> &Standards) const {
    // Iterate the declarative table, not the observations, so output order is
    // stable across CFG layouts and compiler builds.
    for (const KnownStandardInfo &Standard : knownStandardInfos()) {
      const PerStandardEvidence &Observed =
          ByStandard[static_cast<size_t>(Standard.ID)];
      if (Observed.Strong || Observed.CompatibleFunctionSelectors.size() >=
                                 standardSelectorEvidenceCount(
                                     Standard.MinimumIndependentSelectors))
        Standards.push_back(Standard.ID);
    }
  }

private:
  struct PerStandardEvidence {
    std::set<uint32_t> CompatibleFunctionSelectors;
    bool Strong = false;
  };

  PerStandardEvidence &evidence(KnownStandard Standard) {
    return ByStandard[static_cast<size_t>(Standard)];
  }

  std::vector<PerStandardEvidence> ByStandard;
};

std::optional<llvm::StringRef>
unambiguousReturns(const KnownSignatureInfo &Function) {
  const auto Variants = knownFunctionVariants(Function);
  if (Variants.empty())
    return std::nullopt;
  const llvm::StringRef Common = Variants.front()->Returns;
  if (llvm::any_of(Variants, [&](const KnownFunctionVariantInfo *Variant) {
        return Variant->Returns != Common;
      }))
    return std::nullopt;
  return Common;
}

enum class SuccessfulReturnKind : uint8_t {
  NoSuccessfulExit,
  ExactBytes,
  Unknown,
};

struct SuccessfulReturnShape {
  SuccessfulReturnKind Kind = SuccessfulReturnKind::NoSuccessfulExit;
  size_t Bytes = 0;

  void observeExact(size_t ObservedBytes) {
    if (Kind == SuccessfulReturnKind::NoSuccessfulExit) {
      Kind = SuccessfulReturnKind::ExactBytes;
      Bytes = ObservedBytes;
      return;
    }
    if (Kind == SuccessfulReturnKind::ExactBytes && Bytes != ObservedBytes)
      Kind = SuccessfulReturnKind::Unknown;
  }

  void observeUnknown() { Kind = SuccessfulReturnKind::Unknown; }
};

llvm::Expected<SuccessfulReturnShape> observeSuccessfulReturns(
    const EVMLowIR &Low, const EVMMedIR &Med, const ProducerIndex &Index,
    const DefiniteExecutionIndex &Execution,
    const std::set<MedStateLaneID> &FunctionLanes,
    llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  SuccessfulReturnShape Shape;
  for (MedStateLaneID LaneID : FunctionLanes) {
    if (llvm::Error Error = NoteReferenceVisit(kEntryPC))
      return std::move(Error);
    if (!Execution.isReachable(LaneID))
      continue;
    // An unresolved transfer can still enter a successful return path whose
    // byte shape is absent from the definite lane graph. Poison the aggregate
    // claim without treating any speculative target lane as proven.
    if (Execution.hasMayTransitionFrom(LaneID))
      Shape.observeUnknown();
    auto Successors = Execution.successors(LaneID, std::nullopt, std::nullopt,
                                           NoteReferenceVisit);
    if (!Successors)
      return Successors.takeError();
    if (!Successors->empty())
      continue;
    const MedStateLane *Lane = Execution.lane(LaneID);
    const MedBlock *Block = Lane ? Index.block(Lane->LowLane.BlockPC) : nullptr;
    if (!Lane || !Block)
      continue;

    const MedOperation *LastExecuted = nullptr;
    bool Faulted = false;
    for (const MedOperation &Operation : Block->Operations) {
      if (llvm::Error Error = NoteOperationVisit(Operation.PC))
        return std::move(Error);
      if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
        return std::move(Error);
      auto Faults = Execution.faultsIn(Operation, LaneID, NoteReferenceVisit);
      if (!Faults)
        return Faults.takeError();
      if (*Faults) {
        Faulted = true;
        break;
      }
      auto Executes =
          Execution.executesIn(Operation, LaneID, NoteReferenceVisit);
      if (!Executes)
        return Executes.takeError();
      if (*Executes)
        LastExecuted = &Operation;
    }
    if (Faulted || !LastExecuted)
      continue;

    switch (LastExecuted->Op) {
    case Opcode::STOP:
    case Opcode::SELFDESTRUCT:
      Shape.observeExact(0);
      break;
    case Opcode::RETURN: {
      if (LastExecuted->Inputs.size() != 2) {
        Shape.observeUnknown();
        break;
      }
      if (llvm::Error Error = NoteReferenceVisit(LastExecuted->PC))
        return std::move(Error);
      const MedValue *Size = Med.findValue(LastExecuted->Inputs[1]);
      if (!Size || !Size->Constant ||
          Size->Constant->getActiveBits() >
              std::numeric_limits<size_t>::digits) {
        Shape.observeUnknown();
        break;
      }
      Shape.observeExact(Size->Constant->getZExtValue());
      break;
    }
    default:
      // Reaching the end of legacy bytecode is an implicit successful STOP.
      // JUMPI also falls through when its condition is false; every other
      // terminator either has been handled above or is not a successful exit.
      if (const LowInstruction *Instruction =
              instructionAt(Low, LastExecuted->PC);
          Instruction && Instruction->NextPC == Low.Code.size() &&
          (!Instruction->isTerminator() || Instruction->is(Opcode::JUMPI)))
        Shape.observeExact(0);
      break;
    }
  }
  return Shape;
}

std::optional<size_t> staticReturnBytes(llvm::StringRef Returns) {
  const auto Types = splitTypeList(Returns);
  if (Types.empty())
    return size_t{0};

  const llvm::StringRef DynamicBytes =
      getABITypeClassInfo(ABITypeClass::Bytes).Spelling;
  const llvm::StringRef DynamicString =
      getABITypeClassInfo(ABITypeClass::String).Spelling;
  for (llvm::StringRef Type : Types)
    if (Type == DynamicBytes || Type == DynamicString || Type.contains('[') ||
        Type.contains('('))
      return std::nullopt;
  if (Types.size() > std::numeric_limits<size_t>::max() / kWordBytes)
    return std::nullopt;
  return Types.size() * kWordBytes;
}

const KnownFunctionVariantInfo *
recognizedVariant(const KnownSignatureInfo &Function,
                  llvm::ArrayRef<KnownStandard> RecognizedStandards) {
  const KnownFunctionVariantInfo *Selected = nullptr;
  for (const KnownFunctionVariantInfo *Variant :
       knownFunctionVariants(Function)) {
    if (!llvm::is_contained(RecognizedStandards, Variant->Standard))
      continue;
    if (Selected)
      return nullptr;
    Selected = Variant;
  }
  return Selected;
}

} // namespace

namespace {

enum class InputValidation : uint8_t {
  ExternalIR,
  CanonicalIR,
};

llvm::Expected<EVMHighIR> recoverHighIRImpl(const EVMLowIR &Low,
                                            const EVMMedIR &Med,
                                            AnalyzeOptions Options,
                                            InputValidation Validation) {
  if (llvm::Error Error = validateHighAnalysisOptions(Options))
    return std::move(Error);
  EVMHighIR High;
  HighIRBudget Budget(Options);
  const auto Failure =
      Validation == InputValidation::ExternalIR
          ? detail::verifyMedIRForHighAnalysis(Low, Med, Options)
          : detail::verifyIRResourceBoundsForHighAnalysis(Low, Med, Options);
  if (Failure) {
    if (llvm::Error Error = Budget.addDiagnostic(
            High, Failure->PC, kMalformedMedIRDiagnostic.size(),
            [] { return kMalformedMedIRDiagnostic.str(); }))
      return std::move(Error);
    return High;
  }
  if (llvm::Error Error = Budget.copyDiagnostics(High, Med.Diagnostics))
    return std::move(Error);
  const ProducerIndex Index(Med);
  if (!Index.valid()) {
    if (llvm::Error Error = Budget.addDiagnostic(
            High, Index.errorPC(), kMalformedMedIRDiagnostic.size(),
            [] { return kMalformedMedIRDiagnostic.str(); }))
      return std::move(Error);
  }
  const DefiniteExecutionIndex Execution(Low, Med);
  const auto NoteLaneVisit = [&](uint64_t PC) {
    return Budget.noteLaneVisits(1, PC);
  };
  const auto NoteOperationVisit = [&](uint64_t PC) {
    return Budget.noteOperationVisits(1, PC);
  };
  const auto NoteReferenceVisit = [&](uint64_t PC) {
    return Budget.noteReferenceVisits(1, PC);
  };
  const auto NoteDispatchCandidate = [&](uint64_t PC) {
    return Budget.noteDispatchCandidate(PC);
  };
  SemanticClassifier Classifier(Med, Index, Execution, NoteReferenceVisit);
  size_t EligibleOperations = 0;
  for (const MedBlock &Block : Med.Blocks)
    for (const MedOperation &Operation : Block.Operations) {
      auto Eligible = Execution.isEligible(Operation, NoteReferenceVisit);
      if (!Eligible)
        return Eligible.takeError();
      EligibleOperations += *Eligible;
    }
  if (llvm::Error Error = Budget.noteOperationPasses(
          EligibleOperations, kHighGlobalOperationPasses, kEntryPC))
    return std::move(Error);

  auto MemoryResult =
      EVMMemoryDataflow::analyze(Low, Med, Execution, Options, NoteLaneVisit,
                                 NoteOperationVisit, NoteReferenceVisit);
  if (!MemoryResult)
    return MemoryResult.takeError();
  const EVMMemoryDataflow Memory = std::move(*MemoryResult);

  struct FunctionCandidate {
    RecoveredFunction Function;
    std::set<MedStateLaneID> EntryLanes;
  };
  std::map<uint32_t, FunctionCandidate> Functions;
  std::map<uint32_t, SuccessfulReturnShape> FunctionReturnShapes;
  std::set<uint32_t> AmbiguousSelectors;
  if (Index.valid()) {
    auto Dispatch = discoverSelectorDispatch(
        Low, Index, Execution, Classifier, NoteDispatchCandidate, NoteLaneVisit,
        NoteOperationVisit, NoteReferenceVisit);
    if (!Dispatch)
      return Dispatch.takeError();
    for (detail::SelectorDispatchCandidate &Candidate : *Dispatch) {
      const uint32_t Selector = Candidate.Selector;
      if (AmbiguousSelectors.contains(Selector))
        continue;
      auto FunctionIt = Functions.find(Selector);
      if (FunctionIt != Functions.end()) {
        if (FunctionIt->second.Function.EntryPC == Candidate.EntryPC) {
          if (llvm::Error Error = Budget.noteReferenceVisits(
                  Candidate.EntryLanes.size(), Candidate.BranchPC))
            return std::move(Error);
          FunctionIt->second.EntryLanes.insert(Candidate.EntryLanes.begin(),
                                               Candidate.EntryLanes.end());
          continue;
        }
        constexpr size_t kDuplicateSelectorDiagnosticBytes =
            kDuplicateSelectorDiagnosticPrefix.size() + kSelectorHexDigits +
            kDuplicateSelectorDiagnosticSuffix.size();
        if (llvm::Error Error = Budget.addDiagnostic(
                High, Candidate.BranchPC, kDuplicateSelectorDiagnosticBytes,
                [Selector] {
                  return kDuplicateSelectorDiagnosticPrefix.str() +
                         selectorHex(Selector) +
                         kDuplicateSelectorDiagnosticSuffix.str();
                }))
          return std::move(Error);
        Functions.erase(FunctionIt);
        AmbiguousSelectors.insert(Selector);
        continue;
      }
      if (llvm::Error Error = Budget.noteFunction(Candidate.BranchPC))
        return std::move(Error);
      FunctionIt = Functions.try_emplace(Selector).first;
      RecoveredFunction &Function = FunctionIt->second.Function;
      Function.Selector = Selector;
      Function.EntryPC = Candidate.EntryPC;
      if (llvm::Error Error = Budget.noteReferenceVisits(
              Candidate.EntryLanes.size(), Candidate.BranchPC))
        return std::move(Error);
      FunctionIt->second.EntryLanes.insert(Candidate.EntryLanes.begin(),
                                           Candidate.EntryLanes.end());
      // Selector lookup only produces a hash candidate. Argument recovery
      // below keeps it when the bytecode supplies no contradictory ABI use.
      Function.Known = findKnownFunction(Selector);
      Function.Name = Function.Known ? Function.Known->name().str()
                                     : kRecoveredFunctionPrefix.str() +
                                           selectorHex(Selector);
    }
  }

  for (auto &[Selector, Candidate] : Functions) {
    (void)Selector;
    RecoveredFunction &Function = Candidate.Function;
    auto FunctionLanes = reachableFunctionLanes(
        Index, Execution, Classifier, Candidate.EntryLanes, Function.Selector,
        NoteLaneVisit, NoteOperationVisit, NoteReferenceVisit);
    if (!FunctionLanes)
      return FunctionLanes.takeError();
    auto FunctionBlocks =
        blocksFor(Execution, *FunctionLanes, NoteReferenceVisit);
    if (!FunctionBlocks)
      return FunctionBlocks.takeError();
    auto GuardReads =
        nonPayableGuardReads(Index, Execution, Classifier, *FunctionLanes,
                             NoteOperationVisit, NoteReferenceVisit);
    if (!GuardReads)
      return GuardReads.takeError();
    StateAccessKind StateAccess = StateAccessKind::None;
    bool ReadsCallValue = false;
    for (uint64_t BlockPC : *FunctionBlocks) {
      if (llvm::Error Error = NoteReferenceVisit(BlockPC))
        return std::move(Error);
      const LowBlock *LowBlock = Low.findBlock(BlockPC);
      const MedBlock *Block = Index.block(BlockPC);
      if (!LowBlock || !Block) {
        StateAccess = mergeStateAccess(StateAccess, StateAccessKind::Unknown);
        continue;
      }
      for (const MedOperation &Operation : Block->Operations) {
        if (llvm::Error Error = NoteOperationVisit(Operation.PC))
          return std::move(Error);
        if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
          return std::move(Error);
        auto Executes = Execution.executesInAny(Operation, *FunctionLanes,
                                                NoteReferenceVisit);
        if (!Executes)
          return Executes.takeError();
        if (!*Executes)
          continue;
        if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
          return std::move(Error);
        const bool IsGuardRead = GuardReads->contains(Operation.PC);
        StateAccess =
            mergeStateAccess(StateAccess, IsGuardRead ? StateAccessKind::None
                                                      : Operation.StateAccess);
        ReadsCallValue |= !IsGuardRead && Operation.CallValueAccess ==
                                              CallValueAccessKind::Read;
      }
    }
    for (MedStateLaneID Lane : *FunctionLanes) {
      if (llvm::Error Error = NoteReferenceVisit(Function.EntryPC))
        return std::move(Error);
      if (Execution.hasMayTransitionFrom(Lane))
        StateAccess = mergeStateAccess(StateAccess, StateAccessKind::Unknown);
    }

    auto Arguments =
        ArgumentRecovery::create(Med, Index, Execution, *FunctionLanes,
                                 NoteOperationVisit, NoteReferenceVisit);
    if (!Arguments)
      return Arguments.takeError();
    if (Function.Known && !isABICompatible(*Function.Known, *Arguments)) {
      const size_t MessageBytes = kIncompatibleKnownFunctionPrefix.size() +
                                  Function.Known->Signature.size();
      if (llvm::Error Error = Budget.addDiagnostic(
              High, Function.EntryPC, MessageBytes, [&Function] {
                return kIncompatibleKnownFunctionPrefix.str() +
                       Function.Known->Signature.str();
              }))
        return std::move(Error);
      Function.Known = nullptr;
      Function.Name =
          kRecoveredFunctionPrefix.str() + selectorHex(Function.Selector);
    }
    // An accepted dictionary candidate supplies its declared argument list.
    // Otherwise the head slots the body read decide, and every slot below the
    // highest is reported even when nothing read it: dropping a gap would
    // renumber the rest.
    const llvm::SmallVector<llvm::StringRef, 8> Declared =
        Function.Known ? signatureArgumentTypes(Function.Known->Signature)
                       : llvm::SmallVector<llvm::StringRef, 8>{};
    const size_t Count = Function.Known ? Declared.size() : Arguments->count();
    if (llvm::Error Error =
            Budget.noteRecoveredArguments(Count, Function.EntryPC))
      return std::move(Error);
    for (size_t Position = 0; Position < Count; ++Position) {
      RecoveredArgument Argument;
      Argument.Index = static_cast<unsigned>(Position);
      Argument.CalldataOffset = kSelectorBytes + Position * kWordBytes;
      Argument.Name = kRecoveredArgumentPrefix.str() + std::to_string(Position);
      Argument.Read =
          Position < Arguments->count() && Arguments->read(Position);
      if (Function.Known) {
        Argument.Type = Declared[Position].str();
        Argument.TypeSource = ABITypeSource::KnownSignature;
      } else {
        const ABIConstraint &Constraint = Arguments->constraint(Position);
        Argument.Type = Constraint.resolve().spelling();
        Argument.TypeSource = Constraint.source();
      }
      Function.Arguments.push_back(std::move(Argument));
    }

    auto ReturnShape =
        observeSuccessfulReturns(Low, Med, Index, Execution, *FunctionLanes,
                                 NoteOperationVisit, NoteReferenceVisit);
    if (!ReturnShape)
      return ReturnShape.takeError();
    FunctionReturnShapes.emplace(Function.Selector, std::move(*ReturnShape));
    Function.StateMutability = recoveredMutability(StateAccess, ReadsCallValue);
    High.Functions.push_back(Function);
    if (llvm::Error Error =
            Budget.noteRegionBlocks(FunctionBlocks->size(), Function.EntryPC))
      return std::move(Error);
    High.Regions.push_back({Function.EntryPC,
                            RegionKind::Function,
                            {FunctionBlocks->begin(), FunctionBlocks->end()}});
  }

  for (const MedBlock &Block : Med.Blocks) {
    for (const MedOperation &Operation : Block.Operations) {
      const LowInstruction *Instruction = instructionAt(Low, Operation.PC);
      if (!Instruction || !Instruction->isExecutable())
        continue;
      auto Eligible = Execution.isEligible(Operation, NoteReferenceVisit);
      if (!Eligible)
        return Eligible.takeError();
      if (!*Eligible)
        continue;
      if (Operation.Op == Opcode::SLOAD || Operation.Op == Opcode::SSTORE ||
          Operation.Op == Opcode::TLOAD || Operation.Op == Opcode::TSTORE) {
        if (llvm::Error Error = Budget.noteReferenceVisits(
                Operation.Inputs.size(), Operation.PC))
          return std::move(Error);
        StorageFact Fact;
        Fact.PC = Operation.PC;
        Fact.IsWrite =
            Operation.Op == Opcode::SSTORE || Operation.Op == Opcode::TSTORE;
        Fact.IsTransient =
            Operation.Op == Opcode::TLOAD || Operation.Op == Opcode::TSTORE;
        if (Index.valid() && !Operation.Inputs.empty()) {
          Fact.KeyKind = storageKeyKind(Med, Index, Operation.Inputs[0]);
          if (const MedValue *Key = Med.findValue(Operation.Inputs[0]);
              Key && Key->Constant) {
            Fact.Slot = Key->Constant;
            Fact.Known = findKnownSlot(*Key->Constant);
          }
        }
        Fact.SuggestedName = kUnknownStorageName.str();
        // A slot a specification fixes carries its published name; a slot a
        // compiler allocated carries only its number, because nothing outside
        // the source says what it holds.
        if (Fact.Known)
          Fact.SuggestedName = Fact.Known->Name.str();
        else if (Fact.Slot)
          Fact.SuggestedName =
              kStorageSlotPrefix.str() + wordHexDigits(*Fact.Slot);
        else if (Fact.KeyKind == StorageKeyKind::Hashed ||
                 Fact.KeyKind == StorageKeyKind::HashedOffset)
          Fact.SuggestedName =
              kStorageElementPrefix.str() + llvm::utohexstr(Operation.PC);
        High.Storage.push_back(std::move(Fact));
      }
      if (Index.valid())
        if (const CallFamilyInfo *Family = findCallFamily(Operation.Op)) {
          if (llvm::Error Error = Budget.noteReferenceVisits(
                  Operation.Inputs.size(), Operation.PC))
            return std::move(Error);
          High.Calls.push_back(
              classifyCall(Med, Index, Memory, *Family, Operation, Low.Fork));
          // A delegating call is also an outgoing call, but it is the only one
          // whose callee runs against this program's own storage, so it stays
          // reported on its own.
          if (Family->Delegates)
            High.Proxies.push_back(classifyDelegation(Med, Index, Operation));
        }
      if (evm::isLog(Operation.Op)) {
        if (llvm::Error Error = Budget.noteReferenceVisits(
                Operation.Inputs.size(), Operation.PC))
          return std::move(Error);
        EventFact Fact;
        Fact.PC = Operation.PC;
        Fact.Topics = logTopicCount(Operation.Op);
        if (Index.valid() && Fact.Topics != 0 && Operation.Inputs.size() > 2)
          if (const MedValue *Topic = Med.findValue(Operation.Inputs[2]);
              Topic && Topic->Constant) {
            Fact.Topic0 = Topic->Constant;
            Fact.Known = findKnownEvent(*Topic->Constant, Fact.Topics);
          }
        Fact.SuggestedName = Fact.Known ? Fact.Known->name().str()
                                        : kRecoveredEventPrefix.str() +
                                              llvm::utohexstr(Operation.PC);
        High.Events.push_back(std::move(Fact));
      }
    }
  }

  // A decoded REVERT is not itself evidence that the operation can execute:
  // it may sit behind an unresolved indirect jump, after a definite fault, or
  // in dead bytes. Only a successfully indexed MedIR operation can contribute
  // a definite HighIR error fact.
  if (Index.valid())
    for (const MedBlock &Block : Med.Blocks)
      for (const MedOperation &Operation : Block.Operations)
        if (Operation.Op == Opcode::REVERT) {
          auto Eligible = Execution.isEligible(Operation, NoteReferenceVisit);
          if (!Eligible)
            return Eligible.takeError();
          if (*Eligible) {
            if (llvm::Error Error = Budget.noteReferenceVisits(
                    Operation.Inputs.size(), Operation.PC))
              return std::move(Error);
            High.Errors.push_back(classifyRevert(Med, Memory, Operation));
          }
        }

  if (Index.valid()) {
    auto Receive =
        analyzeReceive(Low, Index, Execution, Classifier, NoteLaneVisit,
                       NoteOperationVisit, NoteReferenceVisit);
    if (!Receive)
      return Receive.takeError();
    High.HasReceive = *Receive;
  }

  StandardEvidence Standards;
  for (const RecoveredFunction &Function : High.Functions)
    Standards.noteCompatibleFunction(Function);
  for (const EventFact &Event : High.Events)
    Standards.noteExactEvent(Event);
  // A four-byte custom-error selector has the same collision ambiguity as a
  // function selector, but no recovered argument dataflow to cross-check it.
  // It therefore names the error without contributing interface evidence.
  for (const StorageFact &Storage : High.Storage)
    Standards.noteKnownStorage(Storage);
  for (const ProxyFact &Proxy : High.Proxies)
    Standards.noteKnownProxy(Proxy);
  Standards.appendRecognizedStandards(High.Standards);

  // A selector proves only the canonical name and argument types. Resolve a
  // per-standard return declaration after the whole contract's independent
  // evidence is known, and accept it only when every successful path has the
  // exact static ABI byte count it requires.
  for (RecoveredFunction &Function : High.Functions) {
    const auto ShapeIt = FunctionReturnShapes.find(Function.Selector);
    const SuccessfulReturnShape Shape = ShapeIt == FunctionReturnShapes.end()
                                            ? SuccessfulReturnShape{}
                                            : ShapeIt->second;
    std::optional<llvm::StringRef> DeclaredReturns;
    if (Function.Known) {
      Function.KnownVariant =
          recognizedVariant(*Function.Known, High.Standards);
      DeclaredReturns =
          Function.KnownVariant
              ? std::optional<llvm::StringRef>(Function.KnownVariant->Returns)
              : unambiguousReturns(*Function.Known);
    }
    if (DeclaredReturns) {
      const auto DeclaredBytes = staticReturnBytes(*DeclaredReturns);
      if (DeclaredBytes && Shape.Kind == SuccessfulReturnKind::ExactBytes &&
          *DeclaredBytes == Shape.Bytes) {
        for (llvm::StringRef Type : splitTypeList(*DeclaredReturns))
          Function.Returns.push_back(Type.str());
        Function.ReturnSource = ABITypeSource::KnownSignature;
      } else if (DeclaredBytes &&
                 Shape.Kind == SuccessfulReturnKind::ExactBytes) {
        const size_t MessageBytes = kIncompatibleKnownReturnPrefix.size() +
                                    Function.Known->Signature.size();
        if (llvm::Error Error = Budget.addDiagnostic(
                High, Function.EntryPC, MessageBytes, [&Function] {
                  return kIncompatibleKnownReturnPrefix.str() +
                         Function.Known->Signature.str();
                }))
          return std::move(Error);
      }
    }
    if (Function.Returns.empty() &&
        Shape.Kind == SuccessfulReturnKind::ExactBytes &&
        Shape.Bytes == kWordBytes) {
      Function.Returns.push_back(kDefaultRecoveredWordType.str());
      Function.ReturnSource = ABITypeSource::Default;
    }
  }

  if (Index.valid()) {
    auto Fallback =
        analyzeFallback(Low, Index, Execution, Classifier, NoteLaneVisit,
                        NoteOperationVisit, NoteReferenceVisit);
    if (!Fallback)
      return Fallback.takeError();
    High.HasFallback = *Fallback;
  }
  if (High.Regions.empty()) {
    if (llvm::Error Error =
            Budget.noteRegionBlocks(Low.Blocks.size(), kEntryPC))
      return std::move(Error);
    StructuredRegion Root;
    Root.EntryPC = kEntryPC;
    Root.Kind = RegionKind::CFG;
    Root.Blocks.reserve(Low.Blocks.size());
    for (const LowBlock &Block : Low.Blocks)
      Root.Blocks.push_back(Block.StartPC);
    High.Regions.push_back(std::move(Root));
  }
  return High;
}

} // namespace

llvm::Expected<EVMHighIR> recoverHighIR(const EVMLowIR &Low,
                                        const EVMMedIR &Med,
                                        AnalyzeOptions Options) {
  return recoverHighIRImpl(Low, Med, Options, InputValidation::ExternalIR);
}

llvm::Expected<EVMHighIR>
detail::recoverCanonicalHighIR(const EVMLowIR &Low, const EVMMedIR &Med,
                               AnalyzeOptions Options) {
  return recoverHighIRImpl(Low, Med, Options, InputValidation::CanonicalIR);
}

} // namespace neverd::evm
