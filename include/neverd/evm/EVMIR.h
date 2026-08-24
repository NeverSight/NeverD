//===- EVMIR.h - Staged Ethereum Virtual Machine IR ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the lossless EVM LowIR, 256-bit stack-SSA MedIR, recovered HighIR,
/// and the aggregate program passed to backends.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_EVMIR_H
#define NEVERD_EVM_EVMIR_H

#include "neverd/evm/analysis/EVMStorageSlots.h"
#include "neverd/evm/bytecode/EVMOpcodes.h"
#include "neverd/evm/runtime/EVMABI.h"
#include "neverd/evm/runtime/EVMCalls.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd::evm {

struct Diagnostic {
  uint64_t PC = 0;
  std::string Message;
};

enum class OpcodeDecodeStatus : uint8_t {
#define EVM_OPCODE_DECODE_STATUS(NAME, SPELLING) NAME,
#include "neverd/evm/bytecode/EVMDecodeStatuses.def"
};

enum class ImmediateDecodeStatus : uint8_t {
#define EVM_IMMEDIATE_DECODE_STATUS(NAME, SPELLING) NAME,
#include "neverd/evm/bytecode/EVMDecodeStatuses.def"
};

/// A decoded instruction whose metadata records both byte identity and
/// activation in the selected hardfork.
struct LowInstruction {
  uint64_t PC = 0;
  uint64_t NextPC = 0;
  OpcodeInfo Info;
  OpcodeDecodeStatus DecodeStatus = OpcodeDecodeStatus::Unknown;
  llvm::APInt Immediate = llvm::APInt(kWordBits, 0);
  ImmediateDecodeStatus ImmediateStatus = ImmediateDecodeStatus::None;
  std::array<uint16_t, kMaxImmediateStackOperands> StackOperands{};
  uint8_t StackOperandCount = 0;
  std::vector<uint8_t> Encoding;

  [[nodiscard]] Opcode opcode() const { return Info.Op; }
  [[nodiscard]] bool isAssigned() const { return Info.isAssigned(); }
  [[nodiscard]] bool isActive() const {
    return DecodeStatus == OpcodeDecodeStatus::Active;
  }
  [[nodiscard]] bool hasWellFormedImmediate() const {
    switch (Info.Immediate) {
    case ImmediateKind::None:
      return ImmediateStatus == ImmediateDecodeStatus::None &&
             StackOperandCount == 0;
    case ImmediateKind::PushData:
      return (ImmediateStatus == ImmediateDecodeStatus::Complete ||
              ImmediateStatus == ImmediateDecodeStatus::Truncated) &&
             StackOperandCount == 0;
    case ImmediateKind::EIP8024Single:
      return (ImmediateStatus == ImmediateDecodeStatus::Complete ||
              ImmediateStatus == ImmediateDecodeStatus::Truncated) &&
             StackOperandCount == 1;
    case ImmediateKind::EIP8024Pair:
      return (ImmediateStatus == ImmediateDecodeStatus::Complete ||
              ImmediateStatus == ImmediateDecodeStatus::Truncated) &&
             StackOperandCount == 2;
    }
    return false;
  }
  [[nodiscard]] bool isExecutable() const {
    return isActive() && isAssigned() && hasWellFormedImmediate();
  }
  /// Any non-executable instruction faults at runtime and therefore terminates
  /// the semantic block even when its assigned opcode metadata does not.
  [[nodiscard]] bool isTerminator() const {
    return !isExecutable() || Info.IsTerminator;
  }
  [[nodiscard]] uint8_t stackPops() const {
    return isExecutable() ? Info.StackPops : 0;
  }
  [[nodiscard]] uint8_t stackPushes() const {
    return isExecutable() ? Info.StackPushes : 0;
  }
  /// Returns true only when this is an active opcode equal to \p Candidate.
  [[nodiscard]] bool is(Opcode Candidate) const {
    return isExecutable() && opcode() == Candidate;
  }
  /// Opcode-family queries include the hardfork activation check. This keeps
  /// relaxed decoding from treating a future opcode as executable semantics.
  [[nodiscard]] bool isPush() const {
    return isExecutable() && evm::isPush(opcode());
  }
  [[nodiscard]] bool isDup() const {
    return isExecutable() && (evm::isDup(opcode()) || evm::isDeepDup(opcode()));
  }
  [[nodiscard]] bool isSwap() const {
    return isExecutable() &&
           (evm::isSwap(opcode()) || evm::isDeepSwap(opcode()));
  }
  [[nodiscard]] bool isExchange() const {
    return isExecutable() && evm::isExchange(opcode());
  }
  [[nodiscard]] uint16_t dupDepth() const {
    if (!isDup())
      return 0;
    return evm::isDeepDup(opcode()) && StackOperandCount == 1
               ? StackOperands[0]
               : evm::dupDepth(opcode());
  }
  [[nodiscard]] uint16_t swapDepth() const {
    if (!isSwap())
      return 0;
    return evm::isDeepSwap(opcode()) && StackOperandCount == 1
               ? StackOperands[0]
               : evm::swapDepth(opcode());
  }
  [[nodiscard]] std::optional<StackDepthPair> exchangeDepths() const {
    if (!isExchange() || StackOperandCount != 2)
      return std::nullopt;
    return StackDepthPair{StackOperands[0], StackOperands[1]};
  }
  [[nodiscard]] size_t requiredStackHeight() const {
    if (!isExecutable())
      return 0;
    if (isDup())
      return dupDepth();
    if (isSwap())
      return static_cast<size_t>(swapDepth()) + 1;
    if (const auto Depths = exchangeDepths())
      return static_cast<size_t>(std::max(Depths->First, Depths->Second)) + 1;
    return Info.StackPops;
  }
  [[nodiscard]] std::ptrdiff_t stackDelta() const {
    if (!isExecutable())
      return 0;
    return static_cast<std::ptrdiff_t>(Info.StackPushes) -
           static_cast<std::ptrdiff_t>(Info.StackPops);
  }
  [[nodiscard]] bool isLog() const {
    return isExecutable() && evm::isLog(opcode());
  }
  [[nodiscard]] bool isJump() const {
    return isExecutable() && evm::isJump(opcode());
  }
};

enum class EdgeKind : uint8_t {
  Fallthrough,
  Jump,
  ConditionalTrue,
  ConditionalFalse,
  Indirect,
};

/// Whether a CFG fact follows a feasible analyzed path or only a conservative
/// overapproximation introduced while resolving an indirect destination.
enum class Reachability : uint8_t {
  MayReachable,
  Reachable,
};

/// Strong identifiers for immutable, hash-consed abstract values and stack
/// states. Their underlying values are stable table indices within one
/// EVMLowIR instance; lanes use the self-describing identifier below.
enum class LowAbstractValueID : uint32_t {};
enum class LowAbstractStackID : uint32_t {};

inline constexpr LowAbstractValueID kInvalidLowAbstractValueID =
    static_cast<LowAbstractValueID>(std::numeric_limits<uint32_t>::max());
inline constexpr LowAbstractStackID kInvalidLowAbstractStackID =
    static_cast<LowAbstractStackID>(std::numeric_limits<uint32_t>::max());

/// A self-describing block-local lane identifier. Ordinals are deterministic
/// within BlockPC and do not depend on the layout of EVMLowIR::StateLanes.
struct LowStateLaneID {
  uint64_t BlockPC = std::numeric_limits<uint64_t>::max();
  uint32_t Ordinal = std::numeric_limits<uint32_t>::max();

  [[nodiscard]] constexpr bool isValid() const {
    return BlockPC != std::numeric_limits<uint64_t>::max() &&
           Ordinal != std::numeric_limits<uint32_t>::max();
  }

  friend constexpr bool operator==(const LowStateLaneID &,
                                   const LowStateLaneID &) = default;
  friend constexpr bool operator<(const LowStateLaneID &Left,
                                  const LowStateLaneID &Right) {
    return Left.BlockPC < Right.BlockPC ||
           (Left.BlockPC == Right.BlockPC && Left.Ordinal < Right.Ordinal);
  }
};

[[nodiscard]] constexpr uint32_t lowAbstractValueIndex(LowAbstractValueID ID) {
  return static_cast<uint32_t>(ID);
}

[[nodiscard]] constexpr uint32_t lowAbstractStackIndex(LowAbstractStackID ID) {
  return static_cast<uint32_t>(ID);
}

[[nodiscard]] constexpr bool isValidLowAbstractValueID(LowAbstractValueID ID) {
  return ID != kInvalidLowAbstractValueID;
}

[[nodiscard]] constexpr bool isValidLowAbstractStackID(LowAbstractStackID ID) {
  return ID != kInvalidLowAbstractStackID;
}

enum class LowAbstractValueKind : uint8_t {
  Top,
  ConstantSet,
  Symbol,
  Expression,
};

enum class LowAbstractExactness : uint8_t {
  Exact,
  OverApproximation,
};

/// Identity of one opaque EVM value producer transfer. ProducerLane keeps
/// repeated executions of the same PC distinct; OutputOrdinal separates
/// multi-result instructions without equating unrelated environment reads.
struct LowAbstractSymbolKey {
  LowStateLaneID ProducerLane{};
  uint64_t ProducerPC = 0;
  Opcode ProducerOpcode = Opcode::STOP;
  uint8_t OutputOrdinal = 0;

  friend bool operator==(const LowAbstractSymbolKey &,
                         const LowAbstractSymbolKey &) = default;
};

/// Identity of a pure expression over canonical abstract value nodes.
struct LowAbstractExpressionKey {
  Opcode Operator = Opcode::STOP;
  std::vector<LowAbstractValueID> Operands;

  friend bool operator==(const LowAbstractExpressionKey &,
                         const LowAbstractExpressionKey &) = default;
};

/// One immutable abstract operand-stack value. ConstantSet values are sorted
/// and duplicate-free. Top has no identity; exact Symbol and Expression nodes
/// preserve producer correlation across DUP and pure operations.
struct LowAbstractValue {
  LowAbstractValueID ID = kInvalidLowAbstractValueID;
  LowAbstractValueKind Kind = LowAbstractValueKind::Top;
  LowAbstractExactness Exactness = LowAbstractExactness::OverApproximation;
  std::vector<llvm::APInt> Constants;
  std::optional<LowAbstractSymbolKey> Symbol;
  std::optional<LowAbstractExpressionKey> Expression;

  friend bool operator==(const LowAbstractValue &,
                         const LowAbstractValue &) = default;
};

/// An immutable whole-stack state shared by every lane with identical words.
struct LowAbstractStack {
  LowAbstractStackID ID = kInvalidLowAbstractStackID;
  std::vector<LowAbstractValueID> Words;

  friend bool operator==(const LowAbstractStack &,
                         const LowAbstractStack &) = default;
};

/// One indivisible block-entry state. EntryState, rather than stack height,
/// defines lane identity. ExitState is populated after the lane is transferred.
struct LowStateLane {
  LowStateLaneID ID{};
  Reachability Evidence = Reachability::Reachable;
  LowAbstractStackID EntryState = kInvalidLowAbstractStackID;
  std::optional<LowAbstractStackID> ExitState;

  friend bool operator==(const LowStateLane &, const LowStateLane &) = default;
};

/// Path-sensitive transfer between immutable lanes. A missing target preserves
/// an unresolved indirect jump without inventing a destination lane.
struct LowLaneTransition {
  LowStateLaneID Source{};
  std::optional<LowStateLaneID> Target;
  EdgeKind Kind = EdgeKind::Fallthrough;
  std::optional<uint64_t> TargetPC;
  Reachability Evidence = Reachability::Reachable;

  friend bool operator==(const LowLaneTransition &,
                         const LowLaneTransition &) = default;
};

struct LowEdge {
  EdgeKind Kind = EdgeKind::Fallthrough;
  std::optional<uint64_t> Target;
  Reachability Evidence = Reachability::Reachable;
};

/// Runtime fault classes that terminate one analyzed LowIR path prefix.
enum class LowFaultKind : uint8_t {
#define EVM_LOW_FAULT_KIND(ID, CONSUMES_INPUTS) ID,
#include "neverd/evm/analysis/EVMLowFaultKinds.def"
};

/// A typed runtime fault for one current abstract block-entry lane. Evidence is
/// obtained from the referenced lane; only Reachable faults are definite.
///
/// Lane is the path identity. EntryStackHeight remains compatibility metadata
/// for consumers that have not yet adopted lane-sensitive lowering; it must
/// never be used to merge fault prefixes.
struct LowFaultPrefix {
  LowStateLaneID Lane{};
  size_t EntryStackHeight = 0;
  uint64_t PC = 0;
  LowFaultKind Kind = LowFaultKind::NonExecutableInstruction;
};

/// Sorted, duplicate-free concrete operand-stack heights reaching a block.
/// An empty domain means the block is unreachable from the program entry.
class StackHeightDomain {
public:
  bool insert(size_t Height) {
    auto It = llvm::lower_bound(Heights, Height);
    if (It != Heights.end() && *It == Height)
      return false;
    Heights.insert(It, Height);
    return true;
  }

  [[nodiscard]] bool empty() const { return Heights.empty(); }
  [[nodiscard]] llvm::ArrayRef<size_t> values() const { return Heights; }

  [[nodiscard]] std::optional<size_t> singleton() const {
    return Heights.size() == 1 ? std::optional<size_t>(Heights.front())
                               : std::nullopt;
  }

  [[nodiscard]] std::optional<size_t> maximum() const {
    return Heights.empty() ? std::nullopt
                           : std::optional<size_t>(Heights.back());
  }

private:
  std::vector<size_t> Heights;
};

struct LowBlock {
  uint64_t StartPC = 0;
  uint64_t EndPC = 0;
  size_t FirstInstruction = 0;
  size_t InstructionCount = 0;
  std::vector<LowStateLaneID> StateLanes;
  std::vector<LowEdge> Successors;
  /// Predecessors supported by Reachable evidence. Semantic lowering consumes
  /// this compatibility view and therefore cannot ingest speculative PHIs.
  std::vector<uint64_t> Predecessors;
  /// Predecessors supported only by conservative indirect-target expansion.
  std::vector<uint64_t> MayPredecessors;
  /// Compatibility view consumed by semantic recovery: at least one analyzed
  /// path reaches this block without conservative indirect-target expansion.
  bool Reachable = false;
  /// At least one lane reaches this block through conservative target
  /// expansion. A block can be both Reachable and MayReachable.
  bool MayReachable = false;
  bool HasIndirectSuccessor = false;
  /// Stack-height domains supported by Reachable evidence.
  StackHeightDomain EntryStackHeights;
  StackHeightDomain ExitStackHeights;
  /// Stack-height domains supported by conservative target expansion. These
  /// remain visible to CFG clients without becoming semantic-lowering inputs.
  StackHeightDomain MayEntryStackHeights;
  StackHeightDomain MayExitStackHeights;
  std::vector<LowFaultPrefix> FaultPrefixes;
};

/// Lossless bytecode and CFG representation at the decoder boundary.
struct EVMLowIR {
  Hardfork Fork = Hardfork::Latest;
  bool Strict = true;
  std::vector<uint8_t> Code;
  std::vector<LowInstruction> Instructions;
  std::vector<LowBlock> Blocks;
  std::vector<LowAbstractValue> AbstractValues;
  std::vector<LowAbstractStack> AbstractStacks;
  std::vector<LowStateLane> StateLanes;
  std::vector<LowLaneTransition> LaneTransitions;
  std::set<uint64_t> JumpDestinations;
  std::vector<Diagnostic> Diagnostics;

  const LowBlock *findBlock(uint64_t PC) const {
    const auto It = llvm::lower_bound(
        Blocks, PC, [](const LowBlock &Block, uint64_t Address) {
          return Block.StartPC < Address;
        });
    return It != Blocks.end() && It->StartPC == PC ? &*It : nullptr;
  }
  LowBlock *findBlock(uint64_t PC) {
    const auto It = llvm::lower_bound(
        Blocks, PC, [](const LowBlock &Block, uint64_t Address) {
          return Block.StartPC < Address;
        });
    return It != Blocks.end() && It->StartPC == PC ? &*It : nullptr;
  }
  const LowAbstractValue *findAbstractValue(LowAbstractValueID ID) const {
    const size_t Index = lowAbstractValueIndex(ID);
    return Index < AbstractValues.size() ? &AbstractValues[Index] : nullptr;
  }
  const LowAbstractStack *findAbstractStack(LowAbstractStackID ID) const {
    const size_t Index = lowAbstractStackIndex(ID);
    return Index < AbstractStacks.size() ? &AbstractStacks[Index] : nullptr;
  }
  const LowStateLane *findStateLane(LowStateLaneID ID) const {
    const auto It = llvm::lower_bound(
        StateLanes, ID, [](const LowStateLane &Lane, LowStateLaneID Candidate) {
          return Lane.ID < Candidate;
        });
    return It != StateLanes.end() && It->ID == ID ? &*It : nullptr;
  }
  bool hasEdge(uint64_t From, uint64_t To, EdgeKind Kind) const {
    const LowBlock *Block = findBlock(From);
    if (!Block)
      return false;
    for (const auto &Edge : Block->Successors)
      if (Edge.Kind == Kind && Edge.Target && *Edge.Target == To)
        return true;
    return false;
  }
};

using ValueID = uint32_t;
inline constexpr ValueID kInvalidValueID = std::numeric_limits<ValueID>::max();

/// Strong identifier for one lane-sensitive MedIR stack state. The table
/// index is stable within one EVMMedIR instance; LowLane preserves the
/// self-describing control-flow identity across the lowering boundary.
enum class MedStateLaneID : uint32_t {};
inline constexpr MedStateLaneID kInvalidMedStateLaneID =
    static_cast<MedStateLaneID>(std::numeric_limits<uint32_t>::max());

[[nodiscard]] constexpr uint32_t medStateLaneIndex(MedStateLaneID ID) {
  return static_cast<uint32_t>(ID);
}

[[nodiscard]] constexpr bool isValidMedStateLaneID(MedStateLaneID ID) {
  return ID != kInvalidMedStateLaneID;
}

struct MedPhiIncoming {
  MedStateLaneID SourceLane = kInvalidMedStateLaneID;
  ValueID Value = kInvalidValueID;

  friend bool operator==(const MedPhiIncoming &,
                         const MedPhiIncoming &) = default;
  friend bool operator<(const MedPhiIncoming &Left,
                        const MedPhiIncoming &Right) {
    return Left.SourceLane < Right.SourceLane ||
           (Left.SourceLane == Right.SourceLane && Left.Value < Right.Value);
  }
};

enum class ValueKind : uint8_t { Constant, Instruction, Phi, Unknown };

struct MedValue {
  ValueID ID = kInvalidValueID;
  ValueKind Kind = ValueKind::Unknown;
  uint64_t PC = 0;
  std::string Name;
  std::vector<ValueID> Inputs;
  /// Lane-keyed inputs for Phi values. Inputs is the corresponding value-only
  /// compatibility view and must exactly match this vector's Value sequence.
  std::vector<MedPhiIncoming> PhiIncomings;
  std::optional<llvm::APInt> Constant;
};

struct MedOperation {
  uint64_t PC = 0;
  Opcode Op = Opcode::STOP;
  std::string Name;
  std::vector<ValueID> Inputs;
  std::vector<ValueID> Outputs;
  EffectKind Effect = EffectKind::Unknown;
  MemoryAccessKind MemoryAccess = MemoryAccessKind::Unknown;
  StateAccessKind StateAccess = StateAccessKind::Unknown;
  CallValueAccessKind CallValueAccess = CallValueAccessKind::Unknown;
  /// Sorted, duplicate-free lane provenance. A lane that faults before this
  /// opcode cannot also execute it.
  std::vector<MedStateLaneID> ExecutingLanes;
  std::vector<MedStateLaneID> FaultingLanes;

  [[nodiscard]] bool mayExecute() const { return !ExecutingLanes.empty(); }
  [[nodiscard]] bool mayFault() const { return !FaultingLanes.empty(); }
};

struct MedStateLane {
  MedStateLaneID ID = kInvalidMedStateLaneID;
  LowStateLaneID LowLane{};
  Reachability Evidence = Reachability::Reachable;
  std::vector<ValueID> EntryStack;
  std::vector<ValueID> ExitStack;

  friend bool operator==(const MedStateLane &, const MedStateLane &) = default;
};

struct MedBlock {
  uint64_t StartPC = 0;
  std::vector<MedStateLaneID> StateLanes;
  /// Proven-reachable, uniform-height compatibility views. Lane-sensitive
  /// consumers must use StateLanes instead of treating these as path identity.
  std::vector<ValueID> EntryStack;
  std::vector<ValueID> PhiValues;
  std::vector<MedOperation> Operations;
  std::vector<ValueID> ExitStack;
};

/// Stack SSA representation with explicit values, phis, and semantic effects.
struct EVMMedIR {
  std::vector<MedValue> Values;
  std::vector<MedBlock> Blocks;
  std::vector<MedStateLane> StateLanes;
  std::vector<Diagnostic> Diagnostics;

  const MedValue *findValue(ValueID ID) const {
    if (ID >= Values.size())
      return nullptr;
    return &Values[ID];
  }

  const MedStateLane *findStateLane(MedStateLaneID ID) const {
    const size_t Index = medStateLaneIndex(ID);
    return Index < StateLanes.size() ? &StateLanes[Index] : nullptr;
  }
  MedStateLane *findStateLane(MedStateLaneID ID) {
    const size_t Index = medStateLaneIndex(ID);
    return Index < StateLanes.size() ? &StateLanes[Index] : nullptr;
  }
};

enum class Mutability : uint8_t { Pure, View, NonPayable, Payable };

struct RecoveredArgument {
  unsigned Index = 0;
  uint64_t CalldataOffset = 0;
  std::string Type = kDefaultRecoveredWordType.str();
  std::string Name;
  ABITypeSource TypeSource = ABITypeSource::Default;
  /// False when nothing in the function reads this position. A head slot the
  /// body ignores is still an argument, because every later argument's offset
  /// counts from it, so dropping it would renumber the rest.
  bool Read = true;
};

struct RecoveredFunction {
  uint32_t Selector = 0;
  uint64_t EntryPC = 0;
  std::string Name;
  /// The tabulated hash candidate compatible with recovered argument use, null
  /// when the dictionary has none or dataflow contradicts it. Recovery never
  /// synthesizes a signature from inferred types.
  const KnownSignatureInfo *Known = nullptr;
  /// The uniquely recognized standard declaration of Known, when interface
  /// evidence disambiguates it. A canonical selector can have multiple
  /// variants with different returns, so this is never chosen by table order.
  const KnownFunctionVariantInfo *KnownVariant = nullptr;
  std::vector<RecoveredArgument> Arguments;
  std::vector<std::string> Returns;
  ABITypeSource ReturnSource = ABITypeSource::Default;
  Mutability StateMutability = Mutability::Pure;
};

/// How a storage key was formed, which is what separates a variable the source
/// declared from an element the program addressed.
enum class StorageKeyKind : uint8_t {
#define EVM_STORAGE_KEY_KIND(ID, NAME, SUMMARY) ID,
#include "neverd/evm/EVMRecoveredFacts.def"
};

struct StorageKeyKindInfo {
  StorageKeyKind ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<StorageKeyKindInfo> storageKeyKindInfos();
llvm::StringRef storageKeyKindName(StorageKeyKind Kind);

struct StorageFact {
  uint64_t PC = 0;
  bool IsWrite = false;
  bool IsTransient = false;
  StorageKeyKind KeyKind = StorageKeyKind::Unknown;
  std::optional<llvm::APInt> Slot;
  /// The tabulated slot the key equals, null when no specification fixes that
  /// number. A compiler numbers a contract's own variables from zero, so this
  /// is the difference between a slot that means something outside this
  /// binary and one that means nothing without the source.
  const KnownSlotInfo *Known = nullptr;
  std::string SuggestedName;
};

/// One call that runs another contract's code against this contract's storage.
///
/// This is what makes an upgradeable contract readable: the code at this
/// address is a router, and the code that actually runs is wherever the target
/// points. Recovering which slot the target is read from, or which address it
/// is fixed to, is what tells a reader where to look next.
struct ProxyFact {
  uint64_t PC = 0;
  CalleeKind Kind = CalleeKind::Dynamic;
  /// DELEGATECALL, or CALLCODE for the retired form that does the same thing.
  Opcode Op = Opcode::DELEGATECALL;
  /// The slot the target was loaded from, when it is a constant.
  std::optional<llvm::APInt> Slot;
  const KnownSlotInfo *Known = nullptr;
  /// The target itself, when the code fixes it rather than loading it. This is
  /// what a minimal-proxy clone compiles to.
  std::optional<llvm::APInt> Implementation;
};

/// One call this program makes into another program.
///
/// A deployed contract is usually a participant in a system rather than a whole
/// one, so what it calls is as much a part of its interface as what it answers
/// to. This records the outgoing half: which address, established how, and
/// which selector was sent there.
///
/// The recovered signature is never counted towards the standards the program
/// answers to. Sending `transfer(address,uint256)` says the program uses a
/// token, not that it is one, and conflating the two would report every router
/// and vault as an ERC-20.
struct CallFact {
  uint64_t PC = 0;
  Opcode Op = Opcode::CALL;
  CalleeKind TargetKind = CalleeKind::Dynamic;
  /// The callee address, when the code fixes it rather than loading it.
  std::optional<llvm::APInt> Target;
  /// The precompile the analyzed fork reserves that address for, null when it
  /// reserves nothing there.
  const PrecompileInfo *Precompiled = nullptr;
  /// The slot the callee was read from, when it is a constant.
  std::optional<llvm::APInt> Slot;
  /// The tabulated slot that number is, null when no specification fixes it.
  const KnownSlotInfo *NamedSlot = nullptr;
  /// The selector the call places at the start of the callee's calldata, when
  /// a store before the call proves it. A call with no selector is a plain
  /// value transfer, which is how a contract pays an address that may not have
  /// code at all.
  std::optional<uint32_t> Selector;
  /// The tabulated hash candidate for that selector, null when the dictionary
  /// has none. Outgoing calldata is not treated as an interface the current
  /// program implements.
  const KnownSignatureInfo *Known = nullptr;
  /// The value transferred, for the members of the family that carry one and
  /// when it is a constant. A proven zero is what distinguishes a call that
  /// only invokes from one that also pays.
  std::optional<llvm::APInt> Value;
  std::string SuggestedName;
};

struct EventFact {
  uint64_t PC = 0;
  unsigned Topics = 0;
  std::optional<llvm::APInt> Topic0;
  /// The unique tabulated event whose full signature topic and exact indexed
  /// topic arity match this LOG operation.
  const KnownSignatureInfo *Known = nullptr;
  std::string SuggestedName;
};

/// What a revert hands back to its caller.
enum class RevertKind : uint8_t {
#define EVM_REVERT_KIND(ID, NAME, SUMMARY) ID,
#include "neverd/evm/EVMRecoveredFacts.def"
};

struct RevertKindInfo {
  RevertKind ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<RevertKindInfo> revertKindInfos();
llvm::StringRef revertKindName(RevertKind Kind);

struct ErrorFact {
  uint64_t PC = 0;
  RevertKind Kind = RevertKind::Bare;
  std::optional<uint32_t> Selector;
  /// The tabulated custom-error candidate whose signature hashes to the
  /// selector. The four-byte match names the payload but is not standard
  /// evidence.
  const KnownSignatureInfo *Known = nullptr;
  /// Which compiler-inserted check failed, when the payload is a panic whose
  /// code is constant.
  const PanicCodeInfo *Panic = nullptr;
  std::string SuggestedName;
};

enum class RegionKind : uint8_t { Function, CFG };

struct StructuredRegion {
  uint64_t EntryPC = 0;
  RegionKind Kind = RegionKind::CFG;
  std::vector<uint64_t> Blocks;
};

/// Best-effort source-level facts recovered without claiming source identity.
struct EVMHighIR {
  std::vector<RecoveredFunction> Functions;
  std::vector<StorageFact> Storage;
  std::vector<EventFact> Events;
  std::vector<ErrorFact> Errors;
  std::vector<ProxyFact> Proxies;
  /// Every call this program makes into another program, in program order.
  std::vector<CallFact> Calls;
  std::vector<StructuredRegion> Regions;
  /// The standards the program answers to, in table order. Recognition needs
  /// either each standard's declared minimum of distinct ABI-compatible
  /// function selectors or strong full-topic event, storage, or proxy
  /// evidence; a single four-byte function or error selector is insufficient.
  std::vector<KnownStandard> Standards;
  /// True only when the path taken by a call whose selector matched nothing
  /// provably does something other than reject it. A dispatcher that only
  /// reverts has no fallback to reach, and a fallback that only reverts is
  /// indistinguishable from not having one.
  bool HasFallback = false;
  bool HasReceive = false;
  std::vector<Diagnostic> Diagnostics;
};

/// Owns all EVM pipeline stages for one normalized runtime program.
struct EVMProgram {
  EVMLowIR Low;
  EVMMedIR Med;
  EVMHighIR High;
};

} // namespace neverd::evm

#endif // NEVERD_EVM_EVMIR_H
