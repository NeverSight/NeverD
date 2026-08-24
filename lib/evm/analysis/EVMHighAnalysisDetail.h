//===- EVMHighAnalysisDetail.h - Private HighIR recovery ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the parts of source-level fact recovery that its translation
/// units share: the value-graph index, the semantic classifier the dispatcher
/// is read with, calldata argument recovery, and the per-site classifiers.
///
/// This is an implementation detail of lib/evm/analysis. Nothing outside that
/// directory may include it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_EVM_ANALYSIS_EVMHIGHANALYSISDETAIL_H
#define NEVERD_LIB_EVM_ANALYSIS_EVMHIGHANALYSISDETAIL_H

#include "neverd/evm/analysis/EVMAnalyzer.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd::evm::detail {

class EVMMemoryDataflow;

/// One authority for facts whose source operation and control-flow path are
/// proven to execute. HighIR must not re-derive this predicate from block
/// reachability: one Med block can contain both exact and conservative lanes.
class DefiniteExecutionIndex {
public:
  DefiniteExecutionIndex(const EVMLowIR &Low, const EVMMedIR &Med);

  [[nodiscard]] const MedStateLane *lane(MedStateLaneID ID) const;
  [[nodiscard]] std::optional<MedStateLaneID> medLane(LowStateLaneID ID) const;
  [[nodiscard]] bool isReachable(MedStateLaneID ID) const;
  [[nodiscard]] bool isEligible(const MedOperation &Operation) const;
  [[nodiscard]] llvm::Expected<bool> isEligible(
      const MedOperation &Operation,
      llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) const;
  [[nodiscard]] bool executesIn(const MedOperation &Operation,
                                MedStateLaneID Lane) const;
  [[nodiscard]] llvm::Expected<bool> executesIn(
      const MedOperation &Operation, MedStateLaneID Lane,
      llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) const;
  [[nodiscard]] bool faultsIn(const MedOperation &Operation,
                              MedStateLaneID Lane) const;
  [[nodiscard]] llvm::Expected<bool>
  faultsIn(const MedOperation &Operation, MedStateLaneID Lane,
           llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) const;
  [[nodiscard]] bool executesInAny(const MedOperation &Operation,
                                   const std::set<MedStateLaneID> &Lanes) const;
  [[nodiscard]] llvm::Expected<bool> executesInAny(
      const MedOperation &Operation, const std::set<MedStateLaneID> &Lanes,
      llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) const;

  [[nodiscard]] llvm::SmallVector<MedStateLaneID, 4>
  reachableLanesAt(uint64_t BlockPC) const;
  [[nodiscard]] llvm::SmallVector<MedStateLaneID, 4>
  successors(MedStateLaneID Source, std::optional<EdgeKind> Kind = std::nullopt,
             std::optional<uint64_t> TargetPC = std::nullopt) const;
  [[nodiscard]] llvm::Expected<llvm::SmallVector<MedStateLaneID, 4>> successors(
      MedStateLaneID Source, std::optional<EdgeKind> Kind,
      std::optional<uint64_t> TargetPC,
      llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) const;
  [[nodiscard]] llvm::SmallVector<MedStateLaneID, 4>
  predecessors(MedStateLaneID Target) const;
  [[nodiscard]] llvm::SmallVector<MedStateLaneID, 4>
  operationTargets(const MedOperation &Operation, EdgeKind Kind,
                   std::optional<uint64_t> TargetPC = std::nullopt) const;
  [[nodiscard]] bool hasMayTransitionFrom(MedStateLaneID Source) const;

private:
  struct IndexedTransition {
    MedStateLaneID Target = kInvalidMedStateLaneID;
    EdgeKind Kind = EdgeKind::Fallthrough;

    friend bool operator==(const IndexedTransition &,
                           const IndexedTransition &) = default;
    friend bool operator<(const IndexedTransition &Left,
                          const IndexedTransition &Right) {
      return Left.Kind < Right.Kind ||
             (Left.Kind == Right.Kind && Left.Target < Right.Target);
    }
  };

  const EVMLowIR &Low;
  const EVMMedIR &Med;
  std::map<LowStateLaneID, MedStateLaneID> MedByLowLane;
  std::map<uint64_t, std::vector<MedStateLaneID>> ReachableByBlock;
  std::map<MedStateLaneID, std::vector<IndexedTransition>> Outgoing;
  std::map<MedStateLaneID, std::vector<MedStateLaneID>> Incoming;
  std::set<MedStateLaneID> MayTransitionSources;
};

std::string wordHexDigits(const llvm::APInt &Value, unsigned MinDigits = 1);

std::string selectorHex(uint32_t Selector);

/// The value as a machine-word-sized number, when it is a constant that fits
/// one.
std::optional<uint64_t> constantWord(const MedValue *Value);

class ProducerIndex {
public:
  explicit ProducerIndex(const EVMMedIR &Med);

  [[nodiscard]] bool valid() const { return Valid; }
  [[nodiscard]] uint64_t errorPC() const { return ErrorPC; }

  [[nodiscard]] const MedOperation *producer(ValueID ID) const {
    return ID < Producers.size() ? Producers[ID] : nullptr;
  }

  [[nodiscard]] const MedBlock *block(uint64_t StartPC) const {
    const auto It = Blocks.find(StartPC);
    return It == Blocks.end() ? nullptr : It->second;
  }

  /// The operation at \p PC and the block that contains it, which is the
  /// context a payload built by neighbouring stores has to be read in.
  [[nodiscard]] const MedOperation *operation(uint64_t PC) const {
    const auto It = Operations.find(PC);
    return It == Operations.end() ? nullptr : It->second.second;
  }
  [[nodiscard]] const MedBlock *containingBlock(uint64_t PC) const {
    const auto It = Operations.find(PC);
    return It == Operations.end() ? nullptr : It->second.first;
  }

private:
  void fail(uint64_t PC);
  void build(const EVMMedIR &Med);

  bool Valid = true;
  uint64_t ErrorPC = kEntryPC;
  std::vector<const MedOperation *> Producers;
  std::map<uint64_t, std::pair<const MedBlock *, const MedOperation *>>
      Operations;
  std::map<uint64_t, const MedBlock *> Blocks;
};

enum class SemanticKind : uint8_t {
  Unknown,
  Constant,
  CalldataWordZero,
  SelectorWord,
  SelectorXor,
  SelectorEquality,
  CallValue,
  IsZeroCallValue,
  ExternalOutcome,
  IsZeroExternalOutcome,
  CalldataSize,
  IsZeroCalldataSize,
  CalldataSizeBelowSelector,
  CalldataSizeAtLeastSelector,
};

struct SemanticValue {
  SemanticKind Kind = SemanticKind::Unknown;
  llvm::APInt Word = llvm::APInt(kWordBits, 0);
  uint32_t Selector = 0;
  std::vector<uint64_t> OriginPCs;

  static SemanticValue constant(const llvm::APInt &Value) {
    SemanticValue Result;
    Result.Kind = SemanticKind::Constant;
    Result.Word = Value;
    return Result;
  }

  static SemanticValue simple(SemanticKind Kind) {
    SemanticValue Result;
    Result.Kind = Kind;
    return Result;
  }

  static SemanticValue callValue(SemanticKind Kind, uint64_t PC) {
    SemanticValue Result = simple(Kind);
    Result.OriginPCs.push_back(PC);
    return Result;
  }

  static SemanticValue externalOutcome(SemanticKind Kind, uint64_t PC) {
    SemanticValue Result = simple(Kind);
    Result.OriginPCs.push_back(PC);
    return Result;
  }

  static SemanticValue selectorEquality(uint32_t Selector) {
    SemanticValue Result = simple(SemanticKind::SelectorEquality);
    Result.Selector = Selector;
    return Result;
  }

  static SemanticValue selectorXor(uint32_t Selector) {
    SemanticValue Result = simple(SemanticKind::SelectorXor);
    Result.Selector = Selector;
    return Result;
  }
};

enum class VisitState : uint8_t { Unseen, Active, Complete };

class SemanticClassifier {
public:
  SemanticClassifier(
      const EVMMedIR &Med, const ProducerIndex &Index,
      const DefiniteExecutionIndex &Execution,
      llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit)
      : Med(Med), Index(Index), Execution(Execution), States(Med.Values.size()),
        Results(Med.Values.size()), NoteReferenceVisit(NoteReferenceVisit) {}

  llvm::Expected<SemanticValue> classify(ValueID Root);

private:
  llvm::Expected<llvm::SmallVector<ValueID, kMaxALUStackPops>>
  dependencies(ValueID ID) const;
  llvm::Error noteReferences(size_t Count, uint64_t PC) const;
  const SemanticValue &input(const MedOperation &Operation, size_t Index) const;
  llvm::Expected<SemanticValue> evaluatePhi(const MedValue &Phi) const;
  bool isSelectorMask(const SemanticValue &Value) const;
  llvm::Expected<SemanticValue> evaluateInstruction(ValueID ID) const;
  llvm::Expected<SemanticValue> evaluate(ValueID ID) const;

  const EVMMedIR &Med;
  const ProducerIndex &Index;
  const DefiniteExecutionIndex &Execution;
  std::vector<VisitState> States;
  std::vector<SemanticValue> Results;
  llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit;
};

/// The calldata head slots one function reads, and what its own code says
/// about each of them.
///
/// A loaded word is followed through the operations that only move it, so a
/// mask applied to a duplicate still narrows the argument the duplicate came
/// from. Nothing here decides a type: the observations are handed to
/// \c ABIConstraint::resolve, which is the one place the precedence lives.
class ArgumentRecovery {
public:
  static llvm::Expected<ArgumentRecovery>
  create(const EVMMedIR &Med, const ProducerIndex &Index,
         const DefiniteExecutionIndex &Execution,
         const std::set<MedStateLaneID> &Lanes,
         llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
         llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit);

  /// One past the highest head slot the function read, which is the smallest
  /// argument count consistent with what it does.
  [[nodiscard]] size_t count() const { return Constraints.size(); }
  [[nodiscard]] const ABIConstraint &constraint(size_t Position) const {
    return Constraints[Position];
  }
  [[nodiscard]] bool read(size_t Position) const { return Read[Position]; }

private:
  ArgumentRecovery(const DefiniteExecutionIndex &Execution,
                   const std::set<MedStateLaneID> &Lanes,
                   llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
                   llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit)
      : Execution(Execution), Lanes(Lanes),
        NoteOperationVisit(NoteOperationVisit),
        NoteReferenceVisit(NoteReferenceVisit) {}

  llvm::Error run(const EVMMedIR &Med, const ProducerIndex &Index);
  llvm::Error noteReferences(size_t Count, uint64_t PC) const;
  [[nodiscard]] size_t owner(ValueID Value) const;
  bool adopt(ValueID Value, size_t Position);
  llvm::Error seed(const EVMMedIR &Med);
  llvm::Error propagate(const EVMMedIR &Med);
  llvm::Error observe(const EVMMedIR &Med, const MedOperation &Operation);

  std::vector<const MedBlock *> Ordered;
  const DefiniteExecutionIndex &Execution;
  const std::set<MedStateLaneID> &Lanes;
  llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit;
  llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit;
  std::vector<size_t> Owner;
  std::vector<ABIConstraint> Constraints;
  std::vector<bool> Read;
};

/// How the key of a storage access was formed, which is what separates a
/// declared variable from an element the program addressed.
StorageKeyKind storageKeyKind(const EVMMedIR &Med, const ProducerIndex &Index,
                              ValueID Key);

/// What a revert hands back, to the extent the stores before it prove.
ErrorFact classifyRevert(const EVMMedIR &Med, const EVMMemoryDataflow &Memory,
                         const MedOperation &Revert);

/// The call that runs another contract's code in this contract's storage, and
/// what its target's provenance says about the shape of the proxy.
ProxyFact classifyDelegation(const EVMMedIR &Med, const ProducerIndex &Index,
                             const MedOperation &Call);

/// One call out of this program, and everything the code proved about it.
CallFact classifyCall(const EVMMedIR &Med, const ProducerIndex &Index,
                      const EVMMemoryDataflow &Memory,
                      const CallFamilyInfo &Family, const MedOperation &Call,
                      Hardfork Fork);

/// One selector match reached while the root dispatcher is still in its
/// unmatched state. The true-edge lanes enter the function body; they are not
/// themselves walked as dispatcher lanes.
struct SelectorDispatchCandidate {
  uint32_t Selector = 0;
  uint64_t BranchPC = 0;
  uint64_t EntryPC = 0;
  llvm::SmallVector<MedStateLaneID, 4> EntryLanes;
};

/// Walks the root dispatcher and returns only selector tests reachable while
/// no earlier selector has matched. This prevents a comparison inside one
/// function body from being mistaken for another externally callable entry.
llvm::Expected<llvm::SmallVector<SelectorDispatchCandidate, 8>>
discoverSelectorDispatch(
    const EVMLowIR &Low, const ProducerIndex &Index,
    const DefiniteExecutionIndex &Execution, SemanticClassifier &Classifier,
    llvm::function_ref<llvm::Error(uint64_t)> NoteDispatchCandidate,
    llvm::function_ref<llvm::Error(uint64_t)> NoteLaneVisit,
    llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit);

/// Walks one recovered function from its matched dispatcher entries while
/// preserving the exact calldata selector that selected it.
///
/// A function may jump back into shared dispatcher code. Selector predicates
/// encountered there are refined against \p Selector, so a contradictory edge
/// cannot import another externally callable body. Unrelated or unreadable
/// predicates retain every definite CFG edge conservatively.
llvm::Expected<std::set<MedStateLaneID>> reachableFunctionLanes(
    const ProducerIndex &Index, const DefiniteExecutionIndex &Execution,
    SemanticClassifier &Classifier, const std::set<MedStateLaneID> &Entries,
    uint32_t Selector, llvm::function_ref<llvm::Error(uint64_t)> NoteLaneVisit,
    llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit);

/// Whether the code a call reaches when its selector matched nothing has a
/// successful terminal path.
///
/// The walk starts at the entry and, wherever the dispatcher branches on a
/// selector equality or on calldata being empty, follows only the edge
/// selected by that constrained input. What remains is what a call carrying
/// an unrecognized selector can actually run. An indirect or semantically
/// unreadable conditional branch ends that path rather than widening it, so an
/// unreadable dispatcher reports no fallback instead of claiming one.
llvm::Expected<bool>
analyzeFallback(const EVMLowIR &Low, const ProducerIndex &Index,
                const DefiniteExecutionIndex &Execution,
                SemanticClassifier &Classifier,
                llvm::function_ref<llvm::Error(uint64_t)> NoteLaneVisit,
                llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
                llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit);

/// Whether an empty-calldata, non-zero-value walk from the program entry
/// crosses an explicit empty-calldata guard and then accepts the call. Starting
/// at the root preserves selector constraints that can make an internal guard
/// unreachable for empty calldata.
llvm::Expected<bool>
analyzeReceive(const EVMLowIR &Low, const ProducerIndex &Index,
               const DefiniteExecutionIndex &Execution,
               SemanticClassifier &Classifier,
               llvm::function_ref<llvm::Error(uint64_t)> NoteLaneVisit,
               llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
               llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit);

llvm::Expected<std::set<uint64_t>> nonPayableGuardReads(
    const ProducerIndex &Index, const DefiniteExecutionIndex &Execution,
    SemanticClassifier &Classifier, const std::set<MedStateLaneID> &Lanes,
    llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit);

} // namespace neverd::evm::detail

#endif // NEVERD_LIB_EVM_ANALYSIS_EVMHIGHANALYSISDETAIL_H
