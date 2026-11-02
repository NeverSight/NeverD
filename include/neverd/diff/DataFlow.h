//===- DataFlow.h - Revision-bound SSA dependency evidence ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Immutable SSA/CFG observations for region candidates and impact slices.
/// Physical register names and SSA numbering are not correspondence evidence.
/// Memory aliases, calls, unknown entries and incomplete control flow remain
/// explicit boundaries; these graphs never establish semantic equivalence.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_DATAFLOW_H
#define NEVERD_DIFF_DATAFLOW_H

#include "neverd/diff/Control.h"
#include "neverd/diff/Limits.h"
#include "neverd/diff/Program.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace neverd::diff {

enum FlowGap : uint32_t {
  FlowUnknownInput = 1u << 0,
  FlowMemoryAlias = 1u << 1,
  FlowCallEffects = 1u << 2,
  FlowIndirectControl = 1u << 3,
  FlowExceptionalControl = 1u << 4,
  FlowNativeCoverage = 1u << 5,
  FlowNoExit = 1u << 6,
};
inline constexpr uint32_t AllFlowGaps = (FlowNoExit << 1) - 1;
inline constexpr llvm::StringLiteral
    DataFlowProfile(limits::diff::kMedSSAProfile);
inline constexpr llvm::StringLiteral
    LLVMDataFlowProfile(limits::diff::kLLVMSSAProfile);
inline constexpr llvm::StringLiteral
    LLVMParameterDataFlowProfile(limits::diff::kLLVMParameterSSAProfile);
inline constexpr llvm::StringLiteral
    LLVMProgressDataFlowProfile(limits::diff::kLLVMProgressSSAProfile);
inline constexpr llvm::StringLiteral LLVMMultiBlockProgressDataFlowProfile(
    limits::diff::kLLVMMultiBlockProgressSSAProfile);
inline constexpr llvm::StringLiteral LLVMNestedProgressDataFlowProfile(
    limits::diff::kLLVMNestedProgressSSAProfile);
inline constexpr llvm::StringLiteral LLVMBranchProgressDataFlowProfile(
    limits::diff::kLLVMBranchProgressSSAProfile);
inline constexpr llvm::StringLiteral LLVMModularProgressDataFlowProfile(
    limits::diff::kLLVMModularProgressSSAProfile);
inline constexpr llvm::StringLiteral LLVMParameterBoundProgressDataFlowProfile(
    limits::diff::kLLVMParameterBoundProgressSSAProfile);
inline constexpr llvm::StringLiteral LLVMInvariantBoundProgressDataFlowProfile(
    limits::diff::kLLVMInvariantBoundProgressSSAProfile);
inline bool hasProgressProof(llvm::StringRef Profile) {
  return Profile == LLVMProgressDataFlowProfile ||
         Profile == LLVMMultiBlockProgressDataFlowProfile ||
         Profile == LLVMNestedProgressDataFlowProfile ||
         Profile == LLVMBranchProgressDataFlowProfile ||
         Profile == LLVMModularProgressDataFlowProfile ||
         Profile == LLVMParameterBoundProgressDataFlowProfile ||
         Profile == LLVMInvariantBoundProgressDataFlowProfile;
}
inline bool hasParameterViews(llvm::StringRef Profile) {
  return Profile == LLVMParameterDataFlowProfile || hasProgressProof(Profile);
}
inline bool isLLVMDataFlowProfile(llvm::StringRef Profile) {
  return Profile == LLVMDataFlowProfile || hasParameterViews(Profile);
}
std::vector<llvm::StringRef> flowGapNames(uint32_t Mask);

/// An interrupted projection publishes no graph with missing definitions.
class DataFlowInterrupted : public std::runtime_error {
public:
  ComparisonState State;
  explicit DataFlowInterrupted(ComparisonState State)
      : std::runtime_error("dataflow projection " + stateName(State).str()),
        State(State) {}
};

struct DataFlowOptions {
  uint32_t MaxNodes = limits::diff::kDefaultFlowMaxNodes;
  uint32_t MaxBlocks = limits::diff::kDefaultFlowMaxBlocks;
  uint64_t MaxWork = limits::diff::kDefaultFlowMaxWork;
  unsigned Rounds = limits::diff::kDefaultFlowRounds;
  bool operator==(const DataFlowOptions &) const = default;
  std::optional<std::string> validate() const;
};

struct FlowOperand {
  enum class Kind : uint8_t { Node, Constant, Parameter, Unknown };
  Kind Type = Kind::Unknown;
  uint32_t Node = NoIndex;
  uint16_t Bytes = 0;
  /// Exact scalar bits or parameter position. An unknown's identity is only
  /// meaningful inside this snapshot and is excluded from correspondence keys.
  uint64_t Value = 0;
  /// Address provenance remains distinct from an equal-valued scalar.
  uint8_t Provenance = 0;
  uint64_t AddressOwner = 0;
  /// Incoming predecessor for a PHI operand; otherwise NoIndex.
  uint32_t Predecessor = NoIndex;
  /// Profile-specific exact type and argument attributes. Empty for MedIR.
  std::string Semantics;
  bool operator==(const FlowOperand &) const = default;
};

struct FlowOrigin {
  uint64_t Address = 0;
  int32_t Sequence = -1;
  auto operator<=>(const FlowOrigin &) const = default;
};

struct FlowNode {
  std::string Opcode;
  uint16_t ResultBytes = 0;
  uint8_t MemoryOrdering = 0;
  uint8_t MemoryAddressSpace = 0;
  uint32_t Block = NoIndex;
  uint64_t Address = 0;
  /// -1 is synthetic; a PHI has block provenance, not an invented instruction.
  int32_t OriginSequence = -1;
  bool Phi = false;
  bool Commutative = false;
  uint32_t Gaps = 0;
  /// Recovered call arguments occupy this contiguous part of Inputs, after
  /// the raw operation inputs and before any preserved call-clobber prefix.
  /// NoIndex means no call-argument record; a known zero-argument record has
  /// a real Begin and Count zero. This does not establish callee/formal
  /// binding.
  uint32_t CallArgumentBegin = NoIndex;
  uint32_t CallArgumentCount = 0;
  std::vector<FlowOperand> Inputs;
  /// Exact typed operation contract, distinct from its display opcode.
  std::string Semantics;
  /// Direct retained LLVM observations, sorted and unique. Optimized nodes
  /// never invent one representative Address/OriginSequence from this list.
  std::vector<FlowOrigin> Origins;
  bool hasNativeOrigin() const {
    return OriginSequence >= 0 || !Origins.empty();
  }
  template <class Callback> void forEachOrigin(Callback &&Visit) const {
    if (OriginSequence >= 0)
      Visit(FlowOrigin{Address, OriginSequence});
    for (const auto &Origin : Origins)
      Visit(Origin);
  }
  bool operator==(const FlowNode &) const = default;
};
/// Extension processing charged in addition to the existing node/input work.
uint64_t flowExtraWork(const FlowNode &Node);

struct FlowBlock {
  uint64_t Begin = 0;
  uint64_t End = 0;
  uint32_t Terminator = NoIndex;
  std::vector<uint32_t> Successors;
  uint32_t Gaps = 0;
  bool operator==(const FlowBlock &) const = default;
};

/// Recorded native parameter identity; UINT64_MAX denotes a stack site.
/// This metadata is not an independent ABI or register-validity proof.
struct FlowParameterSite {
  uint64_t Register = UINT64_MAX;
  int64_t Index = -1;
  bool operator==(const FlowParameterSite &) const = default;
};
struct FlowParameterView {
  uint16_t SourceBits = 0, ViewBits = 0;
  std::string Attributes;
  std::optional<FlowParameterSite> NativeSite;
  uint32_t ElidedTruncations = 0;
  bool operator==(const FlowParameterView &) const = default;
};

enum class FlowProgressKind : uint8_t { Unproven, Acyclic, CounterLoops };
llvm::StringRef flowProgressName(FlowProgressKind Kind);
enum class FlowCounterProof : uint8_t {
  None,
  NoWrap,
  Modular,
  ParameterModular,
  InvariantModular
};
llvm::StringRef flowCounterProofName(FlowCounterProof Proof);
struct FlowLoopProgress {
  uint32_t Block = NoIndex, Counter = NoIndex, Update = NoIndex,
           Compare = NoIndex;
  /// v2/v3 name the header separately from Block (the backedge latch). The
  /// published v1 self-loop record retains NoIndex and its four wire fields.
  uint32_t Header = NoIndex;
  /// v4 observes each consuming guard in Block and names its physical latch
  /// separately. Several guards can justify one latch; earlier versions keep
  /// NoIndex and their original wire fields.
  uint32_t Latch = NoIndex;
  /// v5 records the rederived proof basis; v6 also admits parameter-relative
  /// modular exits. None retains the v1-v4 encoding.
  FlowCounterProof Proof = FlowCounterProof::None;
  bool operator==(const FlowLoopProgress &) const = default;
};
struct FlowProgress {
  FlowProgressKind Kind = FlowProgressKind::Unproven;
  /// Full prefixes prevent source string attributes from being mistaken for
  /// operation flags. Fresh source binding authenticates the transformation.
  std::string SourceContract, ObservedContract;
  /// Independently derived totals, never inferred from a partial node page.
  uint32_t LoopCount = 0, BackedgeCount = 0;
  /// One consuming guard per record. Before v4 every guard is its own latch.
  std::vector<FlowLoopProgress> Loops;
  bool operator==(const FlowProgress &) const = default;
};

struct DataFlowGraph {
  std::string Revision;
  std::string Profile = DataFlowProfile.str();
  std::string ABI;
  uint64_t Function = 0;
  uint32_t EntryBlock = NoIndex;
  uint32_t Gaps = 0;
  std::vector<FlowNode> Nodes;
  std::vector<FlowBlock> Blocks;
  /// Source argument order, including unused/unprojected parameters. Only the
  /// parameter-view profiles own these records; they are not operation nodes.
  std::vector<FlowParameterView> ParameterViews;
  /// Only the progress profile owns this independently derived property.
  std::optional<FlowProgress> Progress;
  uint64_t retainedItems() const {
    return uint64_t(Nodes.size()) + ParameterViews.size() +
           (Progress ? 1 + Progress->Loops.size() : 0);
  }
  bool operator==(const DataFlowGraph &) const = default;
  std::optional<std::string>
  validate(const DataFlowOptions &Options,
           const ComparisonControl &Control = {}) const;
};

struct FlowFingerprints {
  ComparisonState State = ComparisonState::Complete;
  uint64_t Work = 0;
  unsigned CompletedRounds = 0;
  /// Heuristic neighborhoods only. Hash equality cannot mark code unchanged.
  std::vector<uint64_t> Keys;
  /// One flag per key: the finite data neighborhood still reaches unvisited
  /// definitions, a cycle or an unknown effect. A closed data neighborhood
  /// still does not establish control/memory/ABI equivalence.
  std::vector<uint8_t> OpenDataFrontiers;
};
FlowFingerprints fingerprintDataFlow(const DataFlowGraph &Graph,
                                     const DataFlowOptions &Options = {},
                                     const ComparisonControl &Control = {});

enum class SliceDirection : uint8_t { Backward, Forward };
struct FlowSlice {
  ComparisonState State = ComparisonState::Complete;
  uint64_t Work = 0;
  uint32_t Gaps = 0;
  bool ControlDependencies = false;
  std::vector<uint32_t> Nodes;
};
/// Traverse explicit SSA dependencies and, when requested, postdominator-based
/// control dependencies. Loops without a path to an exit are a visible gap.
/// Alias-dependent memory edges and interprocedural effects are never guessed.
FlowSlice sliceDataFlow(const DataFlowGraph &Graph,
                        llvm::ArrayRef<uint32_t> Seeds,
                        SliceDirection Direction, bool IncludeControl,
                        const DataFlowOptions &Options = {},
                        const ComparisonControl &Control = {});

} // namespace neverd::diff

#endif // NEVERD_DIFF_DATAFLOW_H
