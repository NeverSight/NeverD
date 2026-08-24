//===- SBFIR.h - Staged Solana SBF intermediate representations -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_SBFIR_H
#define NEVERD_SBF_SBFIR_H

#include "neverd/Common.h"
#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/SBFMetadata.h"
#include "neverd/sbf/image/SBFProgramImage.h"
#include "neverd/sbf/runtime/SBFOpcodes.h"
#include "neverd/sbf/runtime/SBFRuntimeEnvironment.h"
#include "neverd/sbf/runtime/SBFSemantics.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"
#include "neverd/sbf/solana/SBFSolanaModel.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::sbf {

enum class ValidationRule : uint8_t {
  None,
#define SBF_VALIDATION_RULE(NAME, STABLE_ID, FAULT_CODE, MESSAGE) NAME,
#include "neverd/sbf/analysis/SBFValidationRules.def"
};

struct ValidationRuleInfo {
  ValidationRule Rule = ValidationRule::None;
  llvm::StringLiteral StableID = "none";
  FaultCode Fault = FaultCode::None;
  llvm::StringLiteral Message = "valid instruction";
};

constexpr ValidationRuleInfo getValidationRuleInfo(ValidationRule Rule) {
  switch (Rule) {
  case ValidationRule::None:
    return {};
#define SBF_VALIDATION_RULE(NAME, STABLE_ID, FAULT_CODE, MESSAGE)              \
  case ValidationRule::NAME:                                                   \
    return {ValidationRule::NAME, STABLE_ID, FaultCode::FAULT_CODE, MESSAGE};
#include "neverd/sbf/analysis/SBFValidationRules.def"
  }
  return {};
}

constexpr FaultCode executionFaultForValidationRule(ValidationRule Rule) {
  const FaultCode Fault = getValidationRuleInfo(Rule).Fault;
  return Fault == FaultCode::None ? FaultCode::InvalidInstruction : Fault;
}

enum class DiagnosticSeverity : uint8_t { Warning, Error };

struct Diagnostic {
  DiagnosticSeverity Severity = DiagnosticSeverity::Error;
  size_t Slot = 0;
  va_t Address = 0;
  std::string Message;
  ValidationRule Rule = ValidationRule::None;
};

enum class VerificationStage : uint8_t { LocalPreflight };

/// Lifecycle of one explicitly named verification stage.
///
/// NotRequested and BlockedByRequisite are deliberately distinct from an
/// empty accepted report: a consumer must never mistake a stage that did not
/// run for one that examined and accepted the program.
enum class VerificationState : uint8_t {
  NotRequested,
  BlockedByRequisite,
  Accepted,
  Rejected,
};

enum class VerifierPayloadKind : uint8_t { SyscallHash, InstructionSlot };

enum class VerifierRule : uint8_t {
#define SBF_VERIFIER_RULE(NAME, STABLE_ID, STAGE, PAYLOAD_KIND, MESSAGE) NAME,
#include "neverd/sbf/analysis/SBFVerifierRules.def"
};

struct VerifierRuleInfo {
  VerifierRule Rule;
  llvm::StringLiteral StableID;
  VerificationStage Stage;
  VerifierPayloadKind PayloadKind;
  llvm::StringLiteral Message;
};

constexpr VerifierRuleInfo getVerifierRuleInfo(VerifierRule Rule) {
  switch (Rule) {
#define SBF_VERIFIER_RULE(NAME, STABLE_ID, STAGE, PAYLOAD_KIND, MESSAGE)       \
  case VerifierRule::NAME:                                                     \
    return {VerifierRule::NAME, STABLE_ID, VerificationStage::STAGE,           \
            VerifierPayloadKind::PAYLOAD_KIND, MESSAGE};
#include "neverd/sbf/analysis/SBFVerifierRules.def"
  }
  llvm_unreachable("unknown SBF verifier rule");
}

struct VerificationIssue {
  VerifierRule Rule = VerifierRule::InvalidSyscall;
  size_t Slot = 0;
  va_t Address = 0;
  uint64_t Payload = 0;
  std::string Message;
};

struct VerificationReport {
  VerificationStage Stage = VerificationStage::LocalPreflight;
  VerificationState State = VerificationState::NotRequested;
  std::vector<VerificationIssue> Issues;

  bool accepted() const { return State == VerificationState::Accepted; }
};

/// The current Agave pipeline uses requisite verification only.  The latest
/// sbpf local preflight is opt-in until a runtime explicitly adopts it.
enum class VerificationPolicy : uint8_t {
  Requisite,
  RequisiteAndLocalPreflight,
};

enum class CallKind : uint8_t {
  None,
  Syscall,
  Internal,
  Indirect,
  Unsupported,
  Unresolved,
};

/// Dispatch sequencing is independent of the identity recovered for a call.
/// Legacy SBF first invokes a matching runtime syscall and then independently
/// invokes a matching executable function. A successful syscall is therefore
/// not an interception when both registries contain the key; only a handled
/// syscall fault terminates before the function lookup.
enum class CallDispatchPolicy : uint8_t {
  Direct,
  LegacyRuntimeThenFunction,
  /// Source-compatible spelling retained for clients of the earlier model.
  LegacyRuntimeRegistryFirst = LegacyRuntimeThenFunction,
};

/// Whether execution consults the runtime syscall registry for this call.
/// Legacy dispatch can do so before independently invoking an internal body,
/// so CallKind alone is not an authority for this question.
constexpr bool dispatchesRuntimeSyscall(CallKind Call,
                                        CallDispatchPolicy Dispatch) {
  return Call == CallKind::Syscall ||
         Dispatch == CallDispatchPolicy::LegacyRuntimeThenFunction;
}

struct LowInstruction {
  size_t Slot = 0;
  va_t Address = 0;
  std::array<uint8_t, kInstructionSize> Encoding{};
  uint8_t RawOpcode = 0;
  uint8_t Dst = 0;
  uint8_t Src = 0;
  int16_t Offset = 0;
  int32_t RawImmediate = 0;
  uint64_t Immediate = 0;
  const OpcodeInfo *Info = nullptr;
  ValidationRule InvalidReason = ValidationRule::None;
  bool IsContinuation = false;
  uint8_t SlotWidth = 1;
  std::optional<size_t> BranchTarget;
  CallKind Call = CallKind::None;
  CallDispatchPolicy Dispatch = CallDispatchPolicy::Direct;
  std::optional<size_t> CallTarget;
  uint8_t CallRegister = 0;
  uint32_t SyscallHash = 0;
  const SyscallInfo *Syscall = nullptr;
  std::string ResolvedName;

  bool isInvalid() const { return InvalidReason != ValidationRule::None; }
};

enum class EdgeKind : uint8_t {
#define SBF_EDGE_KIND(NAME, IR_NAME, API_NAME, CONDITIONAL_API_NAME,           \
                      IS_INTRAPROCEDURAL)                                      \
  NAME,
#include "neverd/sbf/analysis/SBFEdgeKinds.def"
};

struct EdgeKindInfo {
  EdgeKind Kind = EdgeKind::Invalid;
  llvm::StringLiteral IRName;
  llvm::StringLiteral APIName;
  llvm::StringLiteral ConditionalAPIName;
  bool IsIntraprocedural = false;
};

inline constexpr size_t kEdgeKindCount = 0
#define SBF_EDGE_KIND(NAME, IR_NAME, API_NAME, CONDITIONAL_API_NAME,           \
                      IS_INTRAPROCEDURAL)                                      \
  +1
#include "neverd/sbf/analysis/SBFEdgeKinds.def"
    ;

inline constexpr std::array<EdgeKindInfo, kEdgeKindCount> kEdgeKindInfos = {{
#define SBF_EDGE_KIND(NAME, IR_NAME, API_NAME, CONDITIONAL_API_NAME,           \
                      IS_INTRAPROCEDURAL)                                      \
  {EdgeKind::NAME, IR_NAME, API_NAME, CONDITIONAL_API_NAME, IS_INTRAPROCEDURAL},
#include "neverd/sbf/analysis/SBFEdgeKinds.def"
}};

constexpr EdgeKindInfo getEdgeKindInfo(EdgeKind Kind) {
  const size_t Index = static_cast<size_t>(Kind);
  return Index < kEdgeKindInfos.size()
             ? kEdgeKindInfos[Index]
             : kEdgeKindInfos[static_cast<size_t>(EdgeKind::Invalid)];
}

inline constexpr EdgeKindInfo kUnknownEdgeKindInfo =
    getEdgeKindInfo(static_cast<EdgeKind>(std::numeric_limits<uint8_t>::max()));
static_assert(kUnknownEdgeKindInfo.Kind == EdgeKind::Invalid);
static_assert(std::string_view(kUnknownEdgeKindInfo.IRName) ==
              std::string_view(getEdgeKindInfo(EdgeKind::Invalid).IRName));
static_assert(std::string_view(kUnknownEdgeKindInfo.APIName) ==
              std::string_view(getEdgeKindInfo(EdgeKind::Invalid).APIName));
static_assert(
    std::string_view(kUnknownEdgeKindInfo.ConditionalAPIName) ==
    std::string_view(getEdgeKindInfo(EdgeKind::Invalid).ConditionalAPIName));

static_assert(std::string_view(getEdgeKindInfo(EdgeKind::Fallthrough)
                                   .ConditionalAPIName) == "false");
static_assert(std::string_view(getEdgeKindInfo(EdgeKind::BranchTaken)
                                   .ConditionalAPIName) == "true");

struct CFGEdge {
  size_t From = 0;
  std::optional<size_t> To;
  EdgeKind Kind = EdgeKind::Fallthrough;
};

struct BasicBlock {
  size_t ID = 0;
  size_t StartSlot = 0;
  size_t EndSlot = 0;
  std::vector<size_t> Predecessors;
  std::vector<size_t> Successors;
  bool Reachable = false;
};

struct LowIR {
  Version TheVersion = Version::Reserved;
  va_t TextAddress = 0;
  size_t EntrySlot = 0;
  std::vector<LowInstruction> Instructions;
  std::vector<BasicBlock> Blocks;
  std::vector<CFGEdge> Edges;
  std::vector<Diagnostic> Diagnostics;
};

struct MedInstruction {
  size_t Slot = 0;
  va_t Address = 0;
  Opcode SourceOpcode = Opcode::Unknown;
  Operation Op = Operation::Invalid;
  ValidationRule InvalidReason = ValidationRule::None;
  OperandForm Form = OperandForm::None;
  uint8_t Width = 0;
  uint8_t Dst = 0;
  uint8_t Src = 0;
  uint8_t SlotWidth = 1;
  int16_t Offset = 0;
  uint64_t Immediate = 0;
  SemanticTraits Semantics;
  std::optional<size_t> BranchTarget;
  CallKind Call = CallKind::None;
  CallDispatchPolicy Dispatch = CallDispatchPolicy::Direct;
  std::optional<size_t> CallTarget;
  uint8_t CallRegister = 0;
  uint32_t SyscallHash = 0;
  const SyscallInfo *Syscall = nullptr;
};

struct RegisterValue {
  enum class Kind : uint8_t {
    Unknown,
    Constant,
    StackAddress,
    RodataAddress,
    /// An address inside the input buffer whose distance from the buffer's
    /// base the serialized accounts decide, which is a runtime quantity. It is
    /// a source of its own rather than a constant because the only honest
    /// constant to invent for it would be wrong, and a wrong one would let a
    /// load through it be reported as a named account field.
    InstructionDataAddress,
  };
  Kind ValueKind = Kind::Unknown;
  uint64_t Value = 0;
  int64_t Offset = 0;

  bool operator==(const RegisterValue &) const = default;
};

struct MedBlock {
  size_t ID = 0;
  size_t StartSlot = 0;
  size_t EndSlot = 0;
  std::array<RegisterValue, kRegisterCount> Inputs{};
  std::array<RegisterValue, kRegisterCount> Outputs{};
};

struct MedIR {
  Version TheVersion = Version::Reserved;
  std::vector<MedInstruction> Instructions;
  std::vector<MedBlock> Blocks;
};

struct Function {
  size_t EntrySlot = 0;
  va_t Address = 0;
  std::string Name;
  /// Slice in HighIR::FunctionBlocks.  Functions do not own per-function
  /// vectors: every LowIR block has at most one HighIR function owner.
  size_t BlockOffset = 0;
  size_t BlockCount = 0;
};

struct CallEdge {
  size_t SourceSlot = 0;
  std::optional<size_t> TargetSlot;
  CallKind Kind = CallKind::Unresolved;
  std::string Name;
};

struct SyscallUse {
  size_t Slot = 0;
  uint32_t Hash = 0;
  const SyscallInfo *Info = nullptr;
};

struct RecoveredString {
  va_t Address = 0;
  std::string Value;
};

enum class RegionKind : uint8_t { If, Loop, Irreducible };

struct Region {
  RegionKind Kind = RegionKind::Irreducible;
  /// Index into HighIR::Functions.  Regions are function-local even though
  /// they are stored in one compact program-level vector.
  std::optional<size_t> FunctionIndex;
  size_t HeaderBlock = 0;
  std::optional<size_t> ExitBlock;
  /// Parent loop region, absent for a top-level loop or non-loop region.
  std::optional<size_t> ParentRegion;
  /// Preorder interval in the loop forest. A block belongs to this loop when
  /// its innermost loop's preorder lies in this half-open interval.
  size_t LoopPreorder = 0;
  size_t LoopSubtreeEnd = 0;
  /// Inclusive number of blocks in a loop, including nested subloops.
  size_t BlockCount = 0;
  /// Slice in HighIR::LoopLatches for a loop's backedge sources.
  size_t LatchOffset = 0;
  size_t LatchCount = 0;
  /// Sorted, unique explicit membership for Irreducible regions only. If and
  /// Loop regions use compact CFG-boundary and loop-forest metadata instead;
  /// materializing every nested closure would require quadratic storage.
  std::vector<size_t> Blocks;
};

struct HighIR {
  static constexpr size_t NoFunction = std::numeric_limits<size_t>::max();
  static constexpr size_t AmbiguousFunction = NoFunction - 1;
  static constexpr size_t NoRegion = std::numeric_limits<size_t>::max();

  std::vector<Function> Functions;
  /// Concatenated block-ID slices for Functions, indexed by BlockOffset and
  /// BlockCount.  Its size is bounded by the number of LowIR blocks.
  std::vector<size_t> FunctionBlocks;
  /// LowIR block ID to Functions index, NoFunction for an unreachable block,
  /// or AmbiguousFunction when multiple function entries reach a shared tail.
  std::vector<size_t> BlockOwners;
  std::vector<CallEdge> Calls;
  std::vector<SyscallUse> Syscalls;
  std::vector<RecoveredString> Strings;
  std::vector<Region> Regions;
  /// Concatenated loop-latch block IDs, sliced by Region::LatchOffset/Count.
  std::vector<size_t> LoopLatches;
  /// LowIR block ID to its innermost loop Region, or NoRegion.
  std::vector<size_t> BlockLoops;
  bool UsesCPI = false;
  bool UsesAccounts = false;
  /// What the program means as a Solana program rather than as SBF bytecode.
  SolanaModel Solana;

  /// Return the block IDs uniquely owned by F. Shared tails have an ambiguous
  /// owner and are intentionally excluded; use FunctionBodyIndex for semantic
  /// function bodies. F must belong to this HighIR, and the view is invalidated
  /// when FunctionBlocks is mutated.
  llvm::ArrayRef<size_t> ownedBlocks(const Function &F) const {
    if (F.BlockOffset > FunctionBlocks.size() ||
        F.BlockCount > FunctionBlocks.size() - F.BlockOffset)
      return {};
    return llvm::ArrayRef<size_t>(FunctionBlocks)
        .slice(F.BlockOffset, F.BlockCount);
  }

  /// Return the unique owner of BlockID. The pointer is invalidated when
  /// Functions is mutated.
  const Function *functionForBlock(size_t BlockID) const {
    if (BlockID >= BlockOwners.size())
      return nullptr;
    const size_t FunctionID = BlockOwners[BlockID];
    return FunctionID < Functions.size() ? &Functions[FunctionID] : nullptr;
  }

  llvm::ArrayRef<size_t> latches(const Region &Loop) const {
    if (Loop.Kind != RegionKind::Loop ||
        Loop.LatchOffset > LoopLatches.size() ||
        Loop.LatchCount > LoopLatches.size() - Loop.LatchOffset)
      return {};
    return llvm::ArrayRef<size_t>(LoopLatches)
        .slice(Loop.LatchOffset, Loop.LatchCount);
  }

  bool loopContains(const Region &Loop, size_t BlockID) const {
    if (Loop.Kind != RegionKind::Loop || !Loop.FunctionIndex ||
        Loop.LoopPreorder >= Loop.LoopSubtreeEnd ||
        BlockID >= BlockLoops.size())
      return false;
    const size_t Innermost = BlockLoops[BlockID];
    if (Innermost >= Regions.size() ||
        Regions[Innermost].Kind != RegionKind::Loop ||
        Regions[Innermost].FunctionIndex != Loop.FunctionIndex)
      return false;
    const size_t Preorder = Regions[Innermost].LoopPreorder;
    return Loop.LoopPreorder <= Preorder && Preorder < Loop.LoopSubtreeEnd;
  }
};

struct SBFProgram {
  Metadata Image;
  SBFVMConfig Config;
  RuntimeEnvironmentOrigin EnvironmentOrigin = RuntimeEnvironmentOrigin::Agave;
  RuntimeVersionPolicy VersionPolicy = RuntimeVersionPolicy::ChainProfile;
  Version MinimumRuntimeVersion = Version::Auto;
  Version MaximumRuntimeVersion = Version::Auto;
  /// The named chain evidence from which the resolved facts came. A custom
  /// environment deliberately has no named profile, so consumers cannot
  /// accidentally recompute part of it from Agave tables.
  std::optional<RuntimeProfile> Profile;
  /// Immutable resolved facts used by every feature-, loader-, and
  /// registry-sensitive analysis. These are snapshots rather than hints:
  /// downstream passes must not reinterpret Profile to replace them.
  RuntimeFeature ActiveRuntimeFeatures = RuntimeFeature::None;
  AccountABI RuntimeAccountABI = AccountABI::V1;
  std::vector<uint32_t> RegisteredSyscallHashes;
  ProgramImage ExecutableImage;
  VerificationReport Verification;
  LowIR Low;
  MedIR Med;
  HighIR High;

  llvm::ArrayRef<uint8_t> text() const { return ExecutableImage.text(); }
  bool isSyscallRegistered(uint32_t Hash) const {
    return std::binary_search(RegisteredSyscallHashes.begin(),
                              RegisteredSyscallHashes.end(), Hash);
  }
};

} // namespace neverd::sbf

#endif // NEVERD_SBF_SBFIR_H
