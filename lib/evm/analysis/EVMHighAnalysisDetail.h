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
  SelectorEquality,
  CallValue,
  IsZeroCallValue,
  CalldataSize,
  IsZeroCalldataSize,
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

  static SemanticValue selectorEquality(uint32_t Selector) {
    SemanticValue Result = simple(SemanticKind::SelectorEquality);
    Result.Selector = Selector;
    return Result;
  }
};

enum class VisitState : uint8_t { Unseen, Active, Complete };

class SemanticClassifier {
public:
  SemanticClassifier(const EVMMedIR &Med, const ProducerIndex &Index)
      : Med(Med), Index(Index), States(Med.Values.size()),
        Results(Med.Values.size()) {}

  SemanticValue classify(ValueID Root);

private:
  llvm::SmallVector<ValueID, kMaxALUStackPops> dependencies(ValueID ID) const;
  const SemanticValue &input(const MedOperation &Operation, size_t Index) const;
  SemanticValue evaluatePhi(const MedValue &Phi) const;
  bool isSelectorMask(const SemanticValue &Value) const;
  SemanticValue evaluateInstruction(ValueID ID) const;
  SemanticValue evaluate(ValueID ID) const;

  const EVMMedIR &Med;
  const ProducerIndex &Index;
  std::vector<VisitState> States;
  std::vector<SemanticValue> Results;
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
  ArgumentRecovery(const EVMMedIR &Med, const ProducerIndex &Index,
                   const std::set<uint64_t> &Blocks);

  /// One past the highest head slot the function read, which is the smallest
  /// argument count consistent with what it does.
  [[nodiscard]] size_t count() const { return Constraints.size(); }
  [[nodiscard]] const ABIConstraint &constraint(size_t Position) const {
    return Constraints[Position];
  }
  [[nodiscard]] bool read(size_t Position) const { return Read[Position]; }

private:
  [[nodiscard]] size_t owner(ValueID Value) const;
  bool adopt(ValueID Value, size_t Position);
  void seed(const EVMMedIR &Med);
  void propagate(const EVMMedIR &Med);
  void observe(const EVMMedIR &Med, const MedOperation &Operation);

  std::vector<const MedBlock *> Ordered;
  std::vector<size_t> Owner;
  std::vector<ABIConstraint> Constraints;
  std::vector<bool> Read;
};

/// How the key of a storage access was formed, which is what separates a
/// declared variable from an element the program addressed.
StorageKeyKind storageKeyKind(const EVMMedIR &Med, const ProducerIndex &Index,
                              ValueID Key);

/// What a revert hands back, to the extent the stores before it prove.
ErrorFact classifyRevert(const EVMMedIR &Med, const MedBlock &Block,
                         const MedOperation &Revert);

/// The call that runs another contract's code in this contract's storage, and
/// what its target's provenance says about the shape of the proxy.
ProxyFact classifyDelegation(const EVMMedIR &Med, const ProducerIndex &Index,
                             const MedOperation &Call);

/// One call out of this program, and everything the code proved about it.
CallFact classifyCall(const EVMMedIR &Med, const ProducerIndex &Index,
                      const MedBlock &Block, const CallFamilyInfo &Family,
                      const MedOperation &Call, Hardfork Fork);

/// Whether the code a call reaches when its selector matched nothing does more
/// than reject it.
///
/// The walk starts at the entry and, wherever the dispatcher branches on a
/// selector equality or on calldata being empty, follows only the edge that
/// says the test failed. What remains is what a call carrying an unrecognized
/// selector can actually run. An indirect branch ends that path rather than
/// widening it, so an unreadable dispatcher reports no fallback instead of
/// claiming one.
bool reachesFallback(const EVMLowIR &Low, const ProducerIndex &Index,
                     SemanticClassifier &Classifier);

std::set<uint64_t> nonPayableGuardReads(const EVMLowIR &Low,
                                        const ProducerIndex &Index,
                                        SemanticClassifier &Classifier,
                                        const std::set<uint64_t> &Blocks);

} // namespace neverd::evm::detail

#endif // NEVERD_LIB_EVM_ANALYSIS_EVMHIGHANALYSISDETAIL_H
