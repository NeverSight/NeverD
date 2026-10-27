//===- SBFCallXContextFlow.cpp - Interprocedural CALLX must-facts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Solves whole-function CFGs with input-keyed argument, return and scratch
/// summaries. Only the final summary dependency graph supplies CALLX evidence.
///
//===----------------------------------------------------------------------===//

#include "SBFCallXMemoryEffects.h"

#include "neverd/sbf/analysis/SBFAnalysisLimits.h"
#include "neverd/sbf/analysis/SBFCallXContexts.h"
#include "neverd/sbf/runtime/SBFSemantics.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"
#include "neverd/sbf/runtime/SBFVMConfig.h"

#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::sbf {
namespace {

using namespace callx_detail;

using MissingRegisters = std::array<CallXMissingFact, kRegisterCount>;

constexpr uint16_t registerBit(unsigned Register) {
  return uint16_t{1} << Register;
}

constexpr uint16_t AllRegisters = (uint16_t{1} << kRegisterCount) - 1;

struct CallInputs {
  uint16_t Registers = AllRegisters;
  bool Memory = true;
};

struct InstructionDependencies {
  uint16_t Reads = 0;
  uint16_t Writes = 0;
  bool Memory = false;
};

InstructionDependencies instructionDependencies(const MedInstruction &I,
                                                CallInputs Callee = {}) {
  InstructionDependencies Result;
  if (I.Semantics.WritesDestination)
    Result.Writes = registerBit(I.Dst);
  if (I.Call != CallKind::None) {
    Result.Writes |= (uint16_t{1} << callClobberedRegisterCount(I.Call)) - 1;
    if (I.Call == CallKind::Syscall) {
      // The host sees only its five argument carriers. Its memory is opaque.
      Result.Reads = ((uint16_t{1} << kArgumentRegisterCount) - 1)
                     << kFirstArgumentRegister;
      Result.Memory = true;
    } else {
      Result.Reads = Callee.Registers;
      Result.Memory = Callee.Memory;
    }
    if (I.Op == Operation::CallX)
      Result.Reads |= registerBit(I.CallRegister);
    return Result;
  }
  if (I.Op == Operation::Exit)
    Result.Reads |= registerBit(kReturnRegister);
  Result.Memory = I.Op == Operation::Load;
  switch (I.Form) {
  case OperandForm::Dst:
  case OperandForm::Endian:
  case OperandForm::BranchImm:
  case OperandForm::StoreImm:
    Result.Reads |= registerBit(I.Dst);
    break;
  case OperandForm::DstImm:
    if (I.Op != Operation::Mov)
      Result.Reads |= registerBit(I.Dst);
    break;
  case OperandForm::DstSrc:
    if (I.Op != Operation::Mov)
      Result.Reads |= registerBit(I.Dst);
    Result.Reads |= registerBit(I.Src);
    break;
  case OperandForm::Load:
    Result.Reads |= registerBit(I.Src);
    break;
  case OperandForm::StoreReg:
  case OperandForm::BranchReg:
    Result.Reads |= registerBit(I.Dst) | registerBit(I.Src);
    break;
  default:
    break;
  }
  return Result;
}

void pushAbstractFrame(RegisterState &State, const SBFProgram &Program) {
  if (versionHasFeature(Program.Low.TheVersion,
                        VersionFeature::ManualStackFrames))
    return;
  RegisterValue &Frame = State[kFramePointerRegister];
  const uint64_t Stride =
      automaticFrameStride(Program.Low.TheVersion, Program.Config);
  if (Frame.ValueKind == RegisterValue::Kind::Constant) {
    Frame.Value += Stride;
  } else if (Frame.ValueKind == RegisterValue::Kind::StackAddress) {
    int64_t Offset;
    if (llvm::AddOverflow(Frame.Offset, static_cast<int64_t>(Stride), Offset))
      Frame = {};
    else
      Frame.Offset = Offset;
  } else {
    Frame = {};
  }
}

struct ContextState {
  std::array<ProvenValueSet, kRegisterCount> Registers{};
  ScratchState Scratch;
  std::array<ProvenValueSet, kRegisterCount> Origins{};
  ScratchState OriginScratch;
  MemoryPostconditions Postconditions;
  std::array<std::optional<size_t>, kRegisterCount> InputExpressions{};
  // Private expression IDs, never VM values. The canonical typed-cell owner
  // supplies full-width store, overlap and coordinate invalidation semantics.
  StoredRegisterMemory InputSpills;
  MemoryEffects InputWrites;

  ContextState() = default;
  ContextState(const MachineState &State) : Scratch(State.Scratch) {
    Scratch.Memory.clear();
    for (unsigned R = 0; R < kRegisterCount; ++R)
      Registers[R] = State.Registers[R];
  }

  MachineState scalar() const {
    MachineState State;
    for (unsigned R = 0; R < kRegisterCount; ++R)
      State.Registers[R] = Registers[R].singleton();
    State.Scratch = Scratch;
    return State;
  }

  void join(const ContextState &Other, const SBFProgram &Program) {
    for (unsigned R = 0; R < kRegisterCount; ++R)
      Registers[R].join(Other.Registers[R]);
    for (unsigned R = 0; R < kRegisterCount; ++R)
      Origins[R].join(Other.Origins[R]);
    Scratch.Memory.meet(Other.Scratch.Memory);
    Scratch.Values.joinValues(Other.Scratch.Values);
    OriginScratch.meet(Other.OriginScratch);
    Postconditions.meet(Other.Postconditions);
    InputSpills.meet(Other.InputSpills);
    for (unsigned R = 0; R < kRegisterCount; ++R)
      if (InputExpressions[R] != Other.InputExpressions[R])
        InputExpressions[R].reset();
    InputWrites.include(Other.InputWrites, symbolicInputs(), Program);
  }
};

std::optional<size_t> knownBranchSuccessor(const MedInstruction *Last,
                                           const ContextState &State) {
  if (!Last || Last->Semantics.Terminator != TerminatorKind::ConditionalJump)
    return std::nullopt;
  const ProvenValueSet Immediate(RegisterValue{
      RegisterValue::Kind::Constant,
      normalizeImmediate(Last->Immediate, Last->Semantics.Immediate), 0});
  const auto &Right =
      Last->Semantics.Source == OperandSourceKind::SourceRegister
          ? State.Registers[Last->Src]
          : Immediate;
  std::optional<bool> Taken;
  for (const auto &Left : State.Registers[Last->Dst].values()) {
    if (Left.ValueKind != RegisterValue::Kind::Constant)
      return std::nullopt;
    for (const auto &RHS : Right.values()) {
      if (RHS.ValueKind != RegisterValue::Kind::Constant)
        return std::nullopt;
      const bool Outcome =
          evaluateBranchCondition(Last->Op, Last->Width, Left.Value, RHS.Value);
      if (Taken && *Taken != Outcome)
        return std::nullopt;
      Taken = Outcome;
    }
  }
  if (!Taken)
    return std::nullopt;
  return *Taken ? *Last->BranchTarget : Last->Slot + Last->SlotWidth;
}

void pushAbstractFrame(std::array<ProvenValueSet, kRegisterCount> &State,
                       const SBFProgram &Program) {
  const auto Values = State[kFramePointerRegister].values();
  std::optional<ProvenValueSet> Frames;
  for (const auto &Value : Values) {
    RegisterState Registers{};
    Registers[kFramePointerRegister] = Value;
    pushAbstractFrame(Registers, Program);
    if (!Frames)
      Frames = Registers[kFramePointerRegister];
    else
      Frames->join(Registers[kFramePointerRegister]);
  }
  State[kFramePointerRegister] = Frames.value_or(ProvenValueSet{});
}

void pushAbstractFrame(ContextState &State, const SBFProgram &Program) {
  pushAbstractFrame(State.Registers, Program);
}

uint64_t stateBytes(const ContextState &State) {
  uint64_t Bytes = sizeof(State) + State.Scratch.retainedByteEstimate() +
                   State.OriginScratch.retainedByteEstimate() +
                   State.Postconditions.retainedByteEstimate() +
                   State.InputSpills.retainedByteEstimate() +
                   State.InputWrites.Writes.capacity() * sizeof(MemoryWrite);
  for (const auto &Value : State.Registers)
    Bytes += Value.allocatedBytes();
  for (const auto &Origin : State.Origins)
    Bytes += Origin.allocatedBytes();
  return Bytes;
}

void profileInput(llvm::FoldingSetNodeID &ID, size_t Block, bool Generic,
                  const ContextState &Input) {
  ID.AddInteger(Block);
  ID.AddBoolean(Generic);
  for (const auto &Value : Input.Registers)
    Value.profile(ID);
  Input.Scratch.Memory.profile(ID);
  Input.Scratch.Values.profile(ID);
}

struct ContextCall : CallXSummaryEdge {
  std::vector<CallXContextEvidence> InputExpressionTargets;
};

struct FunctionContext : llvm::FoldingSetNode {
  size_t ID;
  size_t EntryBlock;
  ContextState Input;
  std::optional<ContextState> Return;
  std::vector<CallXContextEvidence> Sites;
  std::vector<ContextCall> Calls;
  bool IsRoot = false;
  bool Published = false;
  bool Generic;
  bool Complete = false;
  bool PreservesMemory = true;
  bool WritesOnlyOwnAndChildFrames = true;
  // The symbolic lane can express an argument using its relation to the entry
  // frame even when the body never reads r10. Preserve that anchor on reuse.
  uint16_t ReuseInputRegisters = registerBit(kFramePointerRegister);
  bool ReuseReadsMemory = false;
  MemoryEffects Effects;
  size_t RootGeneration = 0;
  size_t IncomingReferences = 0;
  uint64_t RetainedBytes = 0;

  FunctionContext(size_t ID, size_t EntryBlock, const ContextState &Input,
                  bool Generic)
      : ID(ID), EntryBlock(EntryBlock), Input(Input), Generic(Generic) {}

  void Profile(llvm::FoldingSetNodeID &ID) const {
    profileInput(ID, EntryBlock, Generic, Input);
  }

  bool isUnreferencedSolverNode() const {
    return !Generic && Complete && !IsRoot && !Published &&
           IncomingReferences == 0;
  }
};

struct BlockState {
  ContextState Entry;
  bool Queued = false;
  uint16_t DefinitelyWritten = 0;
};

class ContextFlow {
public:
  explicit ContextFlow(const SBFProgram &Program)
      : Program(Program), Index(Program.Med),
        Offsets(Program.Med.Blocks.size() + 1) {
    for (const MedBlock &Block : Program.Med.Blocks)
      ByEntry.emplace(Block.StartSlot, Block.ID);
    for (const CFGEdge &Edge : Program.Low.Edges)
      if (Edge.To && getEdgeKindInfo(Edge.Kind).IsIntraprocedural)
        ++Offsets[Edge.From + 1];
    std::partial_sum(Offsets.begin(), Offsets.end(), Offsets.begin());
    Successors.resize(Offsets.back());
    auto Next = Offsets;
    for (const CFGEdge &Edge : Program.Low.Edges)
      if (Edge.To && getEdgeKindInfo(Edge.Kind).IsIntraprocedural)
        Successors[Next[Edge.From]++] = *Edge.To;
  }

  llvm::Expected<CallXContextReport> run();

private:
  llvm::Expected<FunctionContext *>
  summarize(size_t Entry, const ContextState &Input, bool Generic = false,
            CallXInputGeneralization *Generalization = nullptr);
  llvm::Error computeInputDependencies();
  ContextState genericInput() const;
  llvm::Error collectEntryPrefixes(CallXContextReport &Report);
  void reclaimContext(FunctionContext &Context,
                      std::vector<size_t> *Unreferenced = nullptr,
                      bool ReleasePublishedChildren = false);
  void publishRoot(FunctionContext &Context);
  void reclaimOrphanedContexts(size_t Entry);
  llvm::Expected<bool>
  generalizeRedundantPublishedContext(size_t Entry, const ContextState &Input);
  llvm::Expected<bool> replay(FunctionContext &Context, size_t BlockID,
                              ContextState &State, uint16_t &Defined,
                              bool Collect);
  llvm::Expected<bool> call(FunctionContext &Context,
                            const MedInstruction &Instruction,
                            ContextState &State, bool Collect);
  llvm::Expected<bool> callTarget(FunctionContext &Context,
                                  const MedInstruction &Instruction,
                                  ContextState &State, bool Collect);
  std::optional<size_t> rawTargetSlot(va_t Address) const {
    // Raw CALLX subtracts in uint64_t and floors to a slot. An unaligned
    // address or an LDDW continuation is not rejected by address resolution.
    const uint64_t Slot =
        (Address - Program.Low.TextAddress) / kInstructionSize;
    if (Slot < Program.Low.Instructions.size())
      return static_cast<size_t>(Slot);
    return std::nullopt;
  }
  void transfer(const MedInstruction &Instruction, ContextState &State,
                MissingRegisters *Missing);
  void transferValues(const MedInstruction &Instruction, ContextState &State);
  void refineOrigins(const FunctionContext &Context, ContextState &State);
  llvm::Expected<size_t> inputExpression(CallXInputExpression Expression);
  llvm::Expected<std::optional<size_t>> inputMemoryGuard(MemoryEffects Effects);
  RegisterValue storeOrigin(const MedInstruction &Instruction,
                            const ContextState &State) const;
  llvm::Expected<std::optional<size_t>>
  inputOriginExpression(const ProvenValueSet &Origins);
  llvm::Expected<std::optional<size_t>> composeReturnExpression(
      size_t Expression, const ContextState &State,
      const std::array<ProvenValueSet, kRegisterCount> &OriginEntry);
  llvm::Error transferInputExpression(const MedInstruction &Instruction,
                                      ContextState &State);
  llvm::Error instantiateTargetInputs(FunctionContext &Caller,
                                      const FunctionContext &Callee,
                                      const ContextState &Input,
                                      ContextCall &Edge);
  ProvenValueSet loadFullWord(const MedInstruction &Instruction,
                              const ProvenValueSet &Bases,
                              const StoredRegisterMemory &Memory) const;
  ProvenValueSet loadOrigin(const MedInstruction &Instruction,
                            const ContextState &State) const;
  void classifyTarget(CallXContextEvidence &Evidence,
                      const ProvenValueSet &Values,
                      const CallXMissingFact &Missing) const;
  CallXContextEvidence evidence(const FunctionContext &Context,
                                const MedInstruction &Instruction,
                                const ContextState &State,
                                const MissingRegisters &Missing,
                                bool BeyondEntry) const;
  llvm::Error retain(uint64_t Bytes) {
    if (Bytes > kCallXContextRetainedByteBudget - RetainedBytes) {
      SpecializationBudgetExhausted = true;
      return llvm::make_error<llvm::StringError>(
          kCallXContextMemoryBudgetDiagnostic, llvm::inconvertibleErrorCode());
    }
    RetainedBytes += Bytes;
    PeakRetainedBytes = std::max(PeakRetainedBytes, RetainedBytes);
    return llvm::Error::success();
  }
  llvm::Error visit() {
    if (Visits == kCallXContextInstructionVisitBudget) {
      SpecializationBudgetExhausted = true;
      return llvm::make_error<llvm::StringError>(
          kCallXContextWorkBudgetDiagnostic, llvm::inconvertibleErrorCode());
    }
    ++Visits;
    return llvm::Error::success();
  }

  const SBFProgram &Program;
  MedInstructionIndex Index;
  std::map<size_t, size_t> ByEntry;
  std::vector<size_t> Offsets;
  std::vector<size_t> Successors;
  llvm::FoldingSet<FunctionContext> Cache;
  std::vector<std::unique_ptr<FunctionContext>> Contexts;
  std::set<size_t> ActiveEntries;
  std::map<size_t, FunctionContext *> GenericContexts;
  // Live variants in creation order. Stable context IDs outlive reclaimed
  // nodes; scanning that history for each replacement becomes quadratic.
  std::map<size_t, llvm::SmallVector<FunctionContext *,
                                     kCallXContextSpecializationsPerEntry>>
      Specializations;
  std::vector<uint16_t> InputRegisters;
  std::vector<bool> LiveCallReturns;
  std::vector<bool> ReadsMemory;
  std::vector<bool> ReachesCallX;
  size_t ProjectedRegisters = 0;
  size_t ProjectedScratchSnapshots = 0;
  size_t DependencyVisits = 0;
  size_t CacheEvictions = 0;
  size_t GeneralizedPublishedSummaries = 0;
  size_t EquivalentGenericReuses = 0;
  size_t EquivalentSpecializedReuses = 0;
  size_t SkippedNonCallXRoots = 0;
  size_t SkippedConstantBranchSuccessors = 0;
  size_t RootGeneration = 0;
  using ExpressionKey =
      std::tuple<CallXInputExpressionKind, size_t, uint64_t, int64_t, Opcode,
                 std::vector<size_t>, std::optional<size_t>>;
  std::map<ExpressionKey, size_t> ExpressionIndex;
  std::vector<CallXInputExpression> InputExpressions;
  std::vector<size_t> InputExpressionRoots;
  using MemoryGuardKey =
      std::tuple<bool, std::vector<std::tuple<RegisterValue::Kind, uint64_t,
                                              int64_t, uint64_t>>>;
  std::map<MemoryGuardKey, size_t> MemoryGuardIndex;
  std::vector<MemoryEffects> InputMemoryGuards;
  size_t InstantiatedTargetInputExpressions = 0;
  bool SpecializationBudgetExhausted = false;
  size_t Visits = 0;
  size_t CacheHits = 0;
  uint64_t RetainedBytes = 0;
  uint64_t PeakRetainedBytes = 0;
};

void ContextFlow::reclaimContext(FunctionContext &Context,
                                 std::vector<size_t> *Unreferenced,
                                 bool ReleasePublishedChildren) {
  Cache.RemoveNode(&Context);
  for (const auto &Edge : Context.Calls)
    if (Edge.CalleeSummaryID) {
      auto &Child = *Contexts[*Edge.CalleeSummaryID];
      --Child.IncomingReferences;
      if (ReleasePublishedChildren && Child.IncomingReferences == 0 &&
          !Child.Generic && !Child.IsRoot && Child.Complete &&
          Child.RootGeneration < RootGeneration &&
          !ActiveEntries.contains(
              Program.Med.Blocks[Child.EntryBlock].StartSlot))
        Child.Published = false;
      if (Unreferenced && Child.isUnreferencedSolverNode())
        Unreferenced->push_back(Child.ID);
    }
  RetainedBytes -= Context.RetainedBytes;
  auto &Variants = Specializations.at(Context.EntryBlock);
  Variants.erase(std::find(Variants.begin(), Variants.end(), &Context));
  Contexts[Context.ID].reset();
  ++CacheEvictions;
}

void ContextFlow::publishRoot(FunctionContext &Context) {
  Context.IsRoot = true;
  if (Context.Published)
    return;
  std::vector<size_t> Pending;
  Pending.reserve(Contexts.size());
  Context.Published = true;
  Pending.push_back(Context.ID);
  for (size_t I = 0; I < Pending.size(); ++I)
    for (const auto &Edge : Contexts[Pending[I]]->Calls)
      if (Edge.CalleeSummaryID) {
        auto &Child = *Contexts[*Edge.CalleeSummaryID];
        if (!Child.Published) {
          Child.Published = true;
          Pending.push_back(Child.ID);
        }
      }
}

void ContextFlow::reclaimOrphanedContexts(size_t Entry) {
  // Avoid discarding reusable inputs when the four variants are all pinned.
  const auto CouldReleaseEntry = [&](const auto &Context) {
    return Context && Context->EntryBlock == Entry && !Context->Generic &&
           !Context->Published && Context->Complete &&
           Context->RootGeneration < RootGeneration &&
           !ActiveEntries.contains(Program.Med.Blocks[Entry].StartSlot);
  };
  const auto &Variants = Specializations.at(Entry);
  if (std::none_of(Variants.begin(), Variants.end(), CouldReleaseEntry))
    return;

  std::vector<bool> Live(Contexts.size());
  std::vector<size_t> Pending;
  Pending.reserve(Contexts.size());
  for (const auto &Context : Contexts) {
    if (!Context)
      continue;
    Live[Context->ID] = Context->Published;
    if (!Context->Published &&
        (Context->Generic || !Context->Complete ||
         Context->RootGeneration == RootGeneration ||
         ActiveEntries.contains(
             Program.Med.Blocks[Context->EntryBlock].StartSlot))) {
      Live[Context->ID] = true;
      Pending.push_back(Context->ID);
    }
  }
  for (size_t I = 0; I < Pending.size(); ++I)
    for (const auto &Edge : Contexts[Pending[I]]->Calls)
      if (Edge.CalleeSummaryID && !Live[*Edge.CalleeSummaryID]) {
        Live[*Edge.CalleeSummaryID] = true;
        Pending.push_back(*Edge.CalleeSummaryID);
      }
  if (std::none_of(Variants.begin(), Variants.end(), [&](const auto &Context) {
        return CouldReleaseEntry(Context) && !Live[Context->ID];
      }))
    return;

  // Release old orphan parents and their newly unreferenced children only
  // when that can supply a variant. Current-root and published graphs stay
  // live.
  Pending.clear();
  for (const auto &Context : Contexts)
    if (Context && !Live[Context->ID] && Context->isUnreferencedSolverNode())
      Pending.push_back(Context->ID);
  for (size_t I = 0; I < Pending.size(); ++I)
    if (!Live[Pending[I]])
      reclaimContext(*Contexts[Pending[I]], &Pending);
}

llvm::Expected<bool>
ContextFlow::generalizeRedundantPublishedContext(size_t Entry,
                                                 const ContextState &Input) {
  if (ActiveEntries.contains(Program.Med.Blocks[Entry].StartSlot))
    return false;
  // Favor a new image-backed argument when all four variants repeat the same
  // one. Replacing variants for every scalar/frame change would recompute old
  // inputs across roots until the work budget is exhausted. This selects work;
  // an image address by itself supplies no target or alias proof.
  const auto &Variants = Specializations.at(Entry);
  bool NewImageArgument = false;
  for (unsigned R = 0; R < kRegisterCount; ++R) {
    const auto Value = Input.Registers[R].singleton();
    const auto Previous = Variants.front()->Input.Registers[R].singleton();
    if (Value.ValueKind != RegisterValue::Kind::Constant || Value == Previous ||
        Previous.ValueKind != RegisterValue::Kind::Constant ||
        !Program.ExecutableImage.findRegion(Value.Value, 1, true) ||
        !Program.ExecutableImage.findRegion(Previous.Value, 1, true))
      continue;
    NewImageArgument |=
        std::all_of(Variants.begin(), Variants.end(), [&](const auto *Variant) {
          return Variant->Input.Registers[R].singleton() == Previous;
        });
  }
  if (!NewImageArgument)
    return false;
  const auto &Generic = *GenericContexts.at(Entry);
  // Replacing an old incoming edge must leave the caller transfer unchanged.
  // Start with the opaque-memory case: no returned cells or formal return
  // expression can be lost, and the scalar result must agree exactly.
  const auto OpaqueReturn = [](const FunctionContext &Context) {
    if (!Context.Complete || !Context.Return || !Context.Effects.Unknown ||
        !Context.Return->Scratch.empty() ||
        Context.Return->Postconditions.size())
      return false;
    for (const auto &Origin : Context.Return->Origins[0].values())
      if (Origin.ValueKind != RegisterValue::Kind::Constant)
        return false;
    return std::none_of(Context.Return->Registers[0].values().begin(),
                        Context.Return->Registers[0].values().end(),
                        [](const RegisterValue &Value) {
                          return Value.ValueKind ==
                                 RegisterValue::Kind::StackAddress;
                        });
  };
  if (!OpaqueReturn(Generic))
    return false;

  using TargetKey = std::tuple<size_t, size_t, size_t, va_t>;
  std::set<TargetKey> Required, Covered;
  constexpr uint64_t TargetBytes = sizeof(TargetKey) + 4 * sizeof(void *);
  uint64_t TemporaryBytes = 0;
  const auto Release =
      llvm::scope_exit([&] { RetainedBytes -= TemporaryBytes; });
  const auto ClearTargets = [&](auto &Targets) {
    const uint64_t Bytes = Targets.size() * TargetBytes;
    RetainedBytes -= Bytes;
    TemporaryBytes -= Bytes;
    Targets.clear();
  };
  // Stable-ID traversal storage is charged with each context ID, as for root
  // publication and orphan reclamation. Target keys are charged separately.
  std::vector<bool> Seen(Contexts.size());
  std::vector<size_t> Pending;
  Pending.reserve(Contexts.size());
  const auto CollectTargets = [&](const FunctionContext &Start, size_t Excluded,
                                  auto &Targets) -> llvm::Expected<bool> {
    std::fill(Seen.begin(), Seen.end(), false);
    Pending.clear();
    Pending.push_back(Start.ID);
    Seen[Start.ID] = true;
    const auto AddSites = [&](const FunctionContext &Context,
                              const ContextCall *Edge =
                                  nullptr) -> llvm::Expected<bool> {
      size_t Instantiated = 0;
      for (const auto &Template : Context.Sites) {
        const auto &Site =
            Edge && Instantiated < Edge->InputExpressionTargets.size() &&
                    Edge->InputExpressionTargets[Instantiated].CallXSlot ==
                        Template.CallXSlot
                ? Edge->InputExpressionTargets[Instantiated++]
                : Template;
        // A union of target alternatives is a different proof from separate
        // singleton contexts; do not discard that distinction here.
        if (Site.TargetValueAlternatives.values().size() > 1)
          return false;
        const auto &Caller = Edge ? *Contexts[Edge->CallerSummaryID] : Context;
        const size_t CallerEntry =
            Program.Med.Blocks[Caller.EntryBlock].StartSlot;
        const size_t CallerSlot = Edge ? Edge->CallerSlot : CallerEntry;
        if (Site.TargetAddress &&
            Targets
                .emplace(CallerEntry, CallerSlot, Site.CallXSlot,
                         *Site.TargetAddress)
                .second) {
          if (auto Error = this->retain(TargetBytes))
            return std::move(Error);
          TemporaryBytes += TargetBytes;
        }
      }
      return true;
    };
    auto Added = AddSites(Start);
    if (!Added || !*Added)
      return Added;
    for (size_t I = 0; I < Pending.size(); ++I)
      for (const auto &Edge : Contexts[Pending[I]]->Calls) {
        if (!Edge.CalleeSummaryID)
          continue;
        if (*Edge.CalleeSummaryID == Excluded)
          return false;
        const auto &Child = *Contexts[*Edge.CalleeSummaryID];
        // Legacy dispatch suppresses the immediate child's target evidence;
        // its subsequent summary edges are still traversed by the report.
        if (!Edge.LegacyRuntime) {
          auto Added = AddSites(Child, &Edge);
          if (!Added || !*Added)
            return Added;
        }
        if (!Seen[Child.ID]) {
          Seen[Child.ID] = true;
          Pending.push_back(Child.ID);
        }
      }
    return true;
  };
  const auto IndependentlyPublished = [&](const FunctionContext &Sibling,
                                          size_t Excluded) {
    if (Sibling.IsRoot)
      return true;
    std::fill(Seen.begin(), Seen.end(), false);
    Pending.clear();
    for (const auto &Context : Contexts)
      if (Context && Context->IsRoot) {
        Seen[Context->ID] = true;
        Pending.push_back(Context->ID);
      }
    for (size_t I = 0; I < Pending.size(); ++I)
      for (const auto &Edge : Contexts[Pending[I]]->Calls)
        if (Edge.CalleeSummaryID && *Edge.CalleeSummaryID != Excluded) {
          if (*Edge.CalleeSummaryID == Sibling.ID && !Edge.LegacyRuntime)
            return true;
          if (!Seen[*Edge.CalleeSummaryID]) {
            Seen[*Edge.CalleeSummaryID] = true;
            Pending.push_back(*Edge.CalleeSummaryID);
          }
        }
    return false;
  };

  for (auto *Candidate : Specializations.at(Entry)) {
    if (!Candidate->Published || Candidate->IsRoot ||
        Candidate->RootGeneration == RootGeneration ||
        !OpaqueReturn(*Candidate) ||
        Candidate->Return->Registers[0] != Generic.Return->Registers[0] ||
        Candidate->Return->Origins[0] != Generic.Return->Origins[0])
      continue;
    // Replacing this node also rewrites its incoming edges. Only the generic
    // node's own targets can preserve those incoming caller proofs. Descendant
    // proofs instead retain their actual caller entries and call instructions.
    if (std::any_of(Candidate->Sites.begin(), Candidate->Sites.end(),
                    [&](const auto &Site) {
                      return Site.TargetAddress &&
                             std::none_of(Generic.Sites.begin(),
                                          Generic.Sites.end(),
                                          [&](const auto &Replacement) {
                                            return Replacement.CallXSlot ==
                                                       Site.CallXSlot &&
                                                   Replacement.TargetAddress ==
                                                       Site.TargetAddress;
                                          });
                    }))
      continue;
    ClearTargets(Required);
    auto Collected = CollectTargets(*Candidate, Candidate->ID, Required);
    if (!Collected)
      return Collected.takeError();
    if (!*Collected)
      continue;
    bool Redundant = Required.empty();
    for (auto *Sibling : Specializations.at(Entry)) {
      if (Redundant)
        break;
      if (Sibling == Candidate || !Sibling->Complete || !Sibling->Published)
        continue;
      ClearTargets(Covered);
      auto Collected = CollectTargets(*Sibling, Candidate->ID, Covered);
      if (!Collected)
        return Collected.takeError();
      if (!*Collected ||
          !std::includes(Covered.begin(), Covered.end(), Required.begin(),
                         Required.end()) ||
          !IndependentlyPublished(*Sibling, Candidate->ID))
        continue;
      Redundant = true;
    }
    if (!Redundant)
      continue;
    // Do not rewrite an edge in a graph whose fixed point is still being
    // computed. Completed old callers receive the same ABI facts above.
    bool ActiveCaller = false;
    for (const auto &Context : Contexts)
      if (Context && !Context->Complete)
        for (const auto &Edge : Context->Calls)
          ActiveCaller |= Edge.CalleeSummaryID == Candidate->ID;
    if (ActiveCaller)
      continue;
    for (const auto &Context : Contexts)
      if (Context)
        for (auto &Edge : Context->Calls)
          if (Edge.CalleeSummaryID == Candidate->ID) {
            Edge.CalleeSummaryID = Generic.ID;
            Edge.GeneralizedInputs = true;
            Edge.InputGeneralization =
                CallXInputGeneralization::SpecializationLimit;
            Edge.PreservedOlderFrames = false;
            Edge.AppliedMemoryEffects = false;
            Edge.AppliedStoredValues = 0;
            ++GenericContexts.at(Entry)->IncomingReferences;
            --Candidate->IncomingReferences;
          }
    Candidate->Published = false;
    Pending.clear();
    Pending.push_back(Candidate->ID);
    for (size_t I = 0; I < Pending.size(); ++I)
      reclaimContext(*Contexts[Pending[I]], &Pending, true);
    ++GeneralizedPublishedSummaries;
    return true;
  }
  return false;
}

ContextState ContextFlow::genericInput() const {
  ContextState Input;
  Input.Scratch.Values.setFrameTranslationLimit(
      frameTranslationLimit(Program.Low.TheVersion, Program.Config));
  if (!versionHasFeature(Program.Low.TheVersion,
                         VersionFeature::ManualStackFrames))
    Input.Registers[kFramePointerRegister] = RegisterValue{
        RegisterValue::Kind::StackAddress,
        initialFramePointer(Program.Low.TheVersion, Program.Config), 0};
  return Input;
}

llvm::Error ContextFlow::computeInputDependencies() {
  const size_t Count = Program.Med.Blocks.size();
  InputRegisters.resize(Count);
  LiveCallReturns.resize(Program.Low.Instructions.size(), true);
  ReadsMemory.resize(Count);
  ReachesCallX.resize(Count);
  if (auto Error = retain(Count * (sizeof(uint16_t) + 2 * sizeof(bool)) +
                          (LiveCallReturns.capacity() + kBitsPerByte - 1) /
                              kBitsPerByte))
    return Error;
  // Direct call dependencies participate in the same monotone fixed point as
  // CFG successors. Unknown calls conservatively read every register/memory.
  std::vector<size_t> ReverseOffsets(Count + 1);
  for (size_t ID = 0; ID < Count; ++ID) {
    for (size_t E = Offsets[ID]; E < Offsets[ID + 1]; ++E)
      ++ReverseOffsets[Successors[E] + 1];
    const auto &Block = Program.Med.Blocks[ID];
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot)
      if (const auto *I = Index.find(Slot);
          I && I->Call == CallKind::Internal && I->CallTarget &&
          ByEntry.contains(*I->CallTarget))
        ++ReverseOffsets[ByEntry.at(*I->CallTarget) + 1];
  }
  std::partial_sum(ReverseOffsets.begin(), ReverseOffsets.end(),
                   ReverseOffsets.begin());
  std::vector<size_t> Reverse(ReverseOffsets.back());
  auto Next = ReverseOffsets;
  for (size_t ID = 0; ID < Count; ++ID) {
    for (size_t E = Offsets[ID]; E < Offsets[ID + 1]; ++E)
      Reverse[Next[Successors[E]]++] = ID;
    const auto &Block = Program.Med.Blocks[ID];
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot)
      if (const auto *I = Index.find(Slot);
          I && I->Call == CallKind::Internal && I->CallTarget &&
          ByEntry.contains(*I->CallTarget))
        Reverse[Next[ByEntry.at(*I->CallTarget)]++] = ID;
  }
  const uint64_t Bytes =
      (Reverse.size() + 2 * ReverseOffsets.size() + Count) * sizeof(size_t) +
      Count;
  if (auto Error = retain(Bytes))
    return Error;
  const auto Release = llvm::scope_exit([&] { RetainedBytes -= Bytes; });
  std::vector<bool> Queued(Count, true);
  std::deque<size_t> Worklist;
  for (size_t ID = Count; ID > 0; --ID)
    Worklist.push_back(ID - 1);
  while (!Worklist.empty()) {
    const size_t ID = Worklist.front();
    Worklist.pop_front();
    Queued[ID] = false;
    uint16_t Uses = 0;
    bool Memory = false;
    bool CallX = false;
    for (size_t E = Offsets[ID]; E < Offsets[ID + 1]; ++E) {
      Uses |= InputRegisters[Successors[E]];
      Memory |= ReadsMemory[Successors[E]];
      CallX |= ReachesCallX[Successors[E]];
    }
    const auto &Block = Program.Med.Blocks[ID];
    for (size_t Slot = Block.EndSlot; Slot > Block.StartSlot;) {
      const auto *I = Index.find(--Slot);
      if (!I)
        continue;
      if (auto Error = visit())
        return Error;
      ++DependencyVisits;
      CallInputs Callee;
      if (I->Call != CallKind::None) {
        // Uses is the live-out mask at this call. The same fixed point includes
        // CFG successors and direct callees that consume r0 as an input.
        LiveCallReturns[I->Slot] = Uses & registerBit(kReturnRegister);
        if (I->Call == CallKind::Internal && I->CallTarget &&
            ByEntry.contains(*I->CallTarget) &&
            !(I->Dispatch == CallDispatchPolicy::LegacyRuntimeThenFunction &&
              Program.isSyscallRegistered(
                  static_cast<uint32_t>(I->Immediate)))) {
          const size_t Entry = ByEntry.at(*I->CallTarget);
          Callee = {InputRegisters[Entry], ReadsMemory[Entry]};
          CallX |= ReachesCallX[Entry];
        } else {
          CallX |= I->Call != CallKind::Syscall;
        }
      }
      const auto Dependencies = instructionDependencies(*I, Callee);
      Uses = (Uses & ~Dependencies.Writes) | Dependencies.Reads;
      Memory |= Dependencies.Memory;
    }
    if (Uses == InputRegisters[ID] && Memory == ReadsMemory[ID] &&
        CallX == ReachesCallX[ID])
      continue;
    InputRegisters[ID] = Uses;
    ReadsMemory[ID] = Memory;
    ReachesCallX[ID] = CallX;
    for (size_t E = ReverseOffsets[ID]; E < ReverseOffsets[ID + 1]; ++E)
      if (!Queued[Reverse[E]]) {
        Queued[Reverse[E]] = true;
        Worklist.push_back(Reverse[E]);
      }
  }
  return llvm::Error::success();
}

llvm::ArrayRef<RegisterValue> possibleValues(const ProvenValueSet &Values) {
  static const RegisterValue Unknown;
  return Values.unknown() ? llvm::ArrayRef<RegisterValue>(Unknown)
                          : Values.values();
}

ProvenValueSet
registerResult(const MedInstruction &Instruction,
               const std::array<ProvenValueSet, kRegisterCount> &Input,
               const ProgramImage &Image,
               const StoredRegisterMemory *Memory = nullptr) {
  std::optional<ProvenValueSet> Output;
  for (const auto &Source : possibleValues(Input[Instruction.Src]))
    for (const auto &Destination : possibleValues(Input[Instruction.Dst])) {
      RegisterState Registers{};
      Registers[Instruction.Dst] = Destination;
      Registers[Instruction.Src] = Source;
      applyRegisterTransfer(Instruction, Registers, Image, Memory);
      const ProvenValueSet Value(Registers[Instruction.Dst]);
      if (!Output)
        Output = Value;
      else
        Output->join(Value);
    }
  return *Output;
}

RegisterValue ContextFlow::storeOrigin(const MedInstruction &Instruction,
                                       const ContextState &State) const {
  RegisterState Base{};
  Base[0] = State.Origins[Instruction.Dst].singleton();
  return substituteOrigin(
      {RegisterValue::Kind::StackAddress, 1, Instruction.Offset}, Base,
      Program);
}

llvm::Expected<std::optional<size_t>>
ContextFlow::inputMemoryGuard(MemoryEffects Effects) {
  if (!Effects.WritesFrames && Effects.Writes.empty())
    return std::optional<size_t>{};
  MemoryGuardKey Key{Effects.WritesFrames, {}};
  auto &Writes = std::get<1>(Key);
  for (const auto &Write : Effects.Writes)
    Writes.emplace_back(Write.Address.ValueKind, Write.Address.Value,
                        Write.Address.Offset, Write.Size);
  std::sort(Writes.begin(), Writes.end());
  if (const auto It = MemoryGuardIndex.find(Key); It != MemoryGuardIndex.end())
    return std::optional<size_t>{It->second};
  const uint64_t Bytes =
      2 * sizeof(MemoryEffects) + 2 * sizeof(CallXInputMemoryGuard) +
      sizeof(decltype(MemoryGuardIndex)::value_type) + 4 * sizeof(void *) +
      Effects.Writes.capacity() * sizeof(MemoryWrite) +
      Writes.capacity() *
          sizeof(std::tuple<RegisterValue::Kind, uint64_t, int64_t, uint64_t>) +
      2 * Effects.Writes.size() * sizeof(CallXMemoryWrite);
  if (auto Error = retain(Bytes))
    return std::move(Error);
  const size_t ID = InputMemoryGuards.size();
  MemoryGuardIndex.emplace(std::move(Key), ID);
  InputMemoryGuards.push_back(std::move(Effects));
  return std::optional<size_t>{ID};
}

llvm::Expected<size_t>
ContextFlow::inputExpression(CallXInputExpression Expression) {
  ExpressionKey Key;
  switch (Expression.Kind) {
  case CallXInputExpressionKind::InputRegister:
    Key = {Expression.Kind,
           Expression.Register,
           0,
           Expression.Offset,
           Opcode::Unknown,
           {},
           {}};
    break;
  case CallXInputExpressionKind::Constant:
    Key = {Expression.Kind,
           0,
           Expression.Constant.Value,
           0,
           Opcode::Unknown,
           {},
           {}};
    break;
  case CallXInputExpressionKind::Instruction: {
    const auto &Instruction = *Index.find(Expression.InstructionSlot);
    // These unary nodes bind the value independently of its physical carrier.
    // The fixed program version gives SourceOpcode one normalized transfer;
    // a PC or register rename does not change that transfer. Loads refer only
    // to the preserved entry-memory snapshot, never to a later memory epoch.
    const bool Load = Instruction.Op == Operation::Load;
    Key = {Expression.Kind,
           *Expression.Input,
           Load ? 0
                : normalizeImmediate(Instruction.Immediate,
                                     Instruction.Semantics.Immediate),
           Load ? Instruction.Offset : 0,
           Instruction.SourceOpcode,
           {},
           {}};
    Expression.Depth = InputExpressions[*Expression.Input].Depth + 1;
    break;
  }
  case CallXInputExpressionKind::InputAlternatives:
    Key = {Expression.Kind,         0, 0, 0, Opcode::Unknown,
           Expression.Alternatives, {}};
    Expression.Depth = Expression.Alternatives.size() + 1;
    break;
  }
  std::get<6>(Key) = Expression.MemoryGuard;
  if (const auto It = ExpressionIndex.find(Key); It != ExpressionIndex.end())
    return It->second;
  // Include vector growth and the index node in the same retained estimate.
  constexpr uint64_t Bytes = 2 * sizeof(CallXInputExpression) +
                             sizeof(decltype(ExpressionIndex)::value_type) +
                             4 * sizeof(void *) + 2 * sizeof(size_t);
  // Both the index and the node own their alternative leaf references.
  if (auto Error = retain(Bytes + 2 * Expression.Alternatives.capacity() *
                                      sizeof(size_t)))
    return std::move(Error);
  const size_t ID = InputExpressions.size();
  ExpressionIndex.emplace(Key, ID);
  InputExpressionRoots.push_back(
      Expression.Input ? InputExpressionRoots[*Expression.Input] : ID);
  InputExpressions.push_back(std::move(Expression));
  return ID;
}

llvm::Expected<std::optional<size_t>>
ContextFlow::inputOriginExpression(const ProvenValueSet &Origins) {
  if (Origins.unknown() ||
      std::any_of(Origins.values().begin(), Origins.values().end(),
                  [](const auto &Origin) {
                    return Origin.ValueKind != RegisterValue::Kind::Constant &&
                           Origin.ValueKind !=
                               RegisterValue::Kind::StackAddress;
                  }))
    return std::optional<size_t>{};
  std::vector<size_t> Leaves;
  for (const auto &Origin : Origins.values()) {
    CallXInputExpression Leaf;
    if (Origin.ValueKind == RegisterValue::Kind::Constant) {
      Leaf.Kind = CallXInputExpressionKind::Constant;
      Leaf.Constant = Origin;
    } else {
      Leaf.Register = Origin.Value - 1;
      Leaf.Offset = Origin.Offset;
    }
    auto ID = inputExpression(Leaf);
    if (!ID)
      return ID.takeError();
    Leaves.push_back(*ID);
  }
  std::sort(Leaves.begin(), Leaves.end());
  Leaves.erase(std::unique(Leaves.begin(), Leaves.end()), Leaves.end());
  if (Leaves.size() == 1)
    return std::optional<size_t>{Leaves.front()};
  CallXInputExpression Expression;
  Expression.Kind = CallXInputExpressionKind::InputAlternatives;
  Expression.Alternatives = std::move(Leaves);
  auto ID = inputExpression(std::move(Expression));
  if (!ID)
    return ID.takeError();
  return std::optional<size_t>{*ID};
}

llvm::Expected<std::optional<size_t>> ContextFlow::composeReturnExpression(
    size_t Expression, const ContextState &State,
    const std::array<ProvenValueSet, kRegisterCount> &OriginEntry) {
  const size_t Depth = InputExpressions[Expression].Depth;
  const uint64_t Bytes = Depth * sizeof(size_t) +
                         3 * kCallXContextValueAlternatives *
                             kCallXContextValueAlternatives * sizeof(size_t);
  if (auto Error = retain(Bytes))
    return std::move(Error);
  const auto Release = llvm::scope_exit([&] { RetainedBytes -= Bytes; });
  std::vector<size_t> Chain;
  Chain.reserve(Depth);
  while (InputExpressions[Expression].Input) {
    if (auto Error = visit())
      return std::move(Error);
    const auto &Node = InputExpressions[Expression];
    if (Index.find(Node.InstructionSlot)->Op == Operation::Load &&
        State.InputWrites.Unknown)
      return std::optional<size_t>{};
    Chain.push_back(Expression);
    Expression = *Node.Input;
  }
  const auto RebaseLeaf =
      [&](size_t ID) -> llvm::Expected<std::optional<size_t>> {
    if (auto Error = visit())
      return std::move(Error);
    const auto &Node = InputExpressions[ID];
    if (Node.Kind == CallXInputExpressionKind::Constant)
      return std::optional<size_t>{ID};
    if (Node.Offset == 0 && Node.Register != kFramePointerRegister &&
        State.InputExpressions[Node.Register])
      return State.InputExpressions[Node.Register];
    const auto Origins = substituteOrigins(
        ProvenValueSet(RegisterValue{RegisterValue::Kind::StackAddress,
                                     Node.Register + 1, Node.Offset}),
        OriginEntry, Program);
    return inputOriginExpression(Origins);
  };
  std::optional<size_t> Result;
  // The union format still contains only affine leaves. A substitution may
  // collapse to one arbitrary expression, but differing loaded roots do not
  // become a new, unbounded expression graph.
  if (InputExpressions[Expression].Kind ==
      CallXInputExpressionKind::InputAlternatives) {
    const auto Leaves = InputExpressions[Expression].Alternatives;
    std::vector<size_t> Rebased;
    for (size_t Leaf : Leaves) {
      auto ID = RebaseLeaf(Leaf);
      if (!ID)
        return ID.takeError();
      if (!*ID)
        return std::optional<size_t>{};
      Rebased.push_back(**ID);
    }
    std::sort(Rebased.begin(), Rebased.end());
    Rebased.erase(std::unique(Rebased.begin(), Rebased.end()), Rebased.end());
    if (Rebased.size() == 1) {
      Result = Rebased.front();
    } else {
      std::vector<size_t> Affine;
      for (size_t ID : Rebased) {
        const auto &Node = InputExpressions[ID];
        if (Node.Kind == CallXInputExpressionKind::Instruction)
          return std::optional<size_t>{};
        if (Node.Kind == CallXInputExpressionKind::InputAlternatives)
          Affine.insert(Affine.end(), Node.Alternatives.begin(),
                        Node.Alternatives.end());
        else
          Affine.push_back(ID);
      }
      std::sort(Affine.begin(), Affine.end());
      Affine.erase(std::unique(Affine.begin(), Affine.end()), Affine.end());
      if (Affine.size() > kCallXContextValueAlternatives)
        return std::optional<size_t>{};
      CallXInputExpression Union;
      Union.Kind = CallXInputExpressionKind::InputAlternatives;
      Union.Alternatives = std::move(Affine);
      auto ID = inputExpression(std::move(Union));
      if (!ID)
        return ID.takeError();
      Result = *ID;
    }
  } else {
    auto ID = RebaseLeaf(Expression);
    if (!ID)
      return ID.takeError();
    Result = *ID;
  }
  if (!Result)
    return Result;
  for (auto It = Chain.rbegin(); It != Chain.rend(); ++It) {
    auto Node = InputExpressions[*It];
    if (Index.find(Node.InstructionSlot)->Op == Operation::Load) {
      MemoryEffects Effects = State.InputWrites;
      if (Node.MemoryGuard) {
        RegisterState Entry{};
        for (unsigned R = 0; R < kRegisterCount; ++R)
          Entry[R] = OriginEntry[R].singleton();
        Effects.include(InputMemoryGuards[*Node.MemoryGuard], Entry, Program);
      }
      if (Effects.Unknown)
        return std::optional<size_t>{};
      auto Guard = inputMemoryGuard(std::move(Effects));
      if (!Guard)
        return Guard.takeError();
      Node.MemoryGuard = *Guard;
    }
    Node.Input = Result;
    auto ID = inputExpression(std::move(Node));
    if (!ID)
      return ID.takeError();
    Result = *ID;
  }
  return Result;
}

llvm::Error
ContextFlow::transferInputExpression(const MedInstruction &Instruction,
                                     ContextState &State) {
  if (Instruction.Op == Operation::Store) {
    State.InputWrites.addWrite(storeOrigin(Instruction, State),
                               Instruction.Width / kBitsPerByte, Program);
    MedInstruction Store = Instruction;
    Store.Form = OperandForm::StoreReg;
    Store.Src = Store.Dst == 0 ? 1 : 0;
    RegisterValue Value;
    if (Instruction.Form == OperandForm::StoreReg &&
        Instruction.Width == kDoubleWordBitWidth &&
        State.Registers[Instruction.Src].unknown() &&
        State.Origins[Instruction.Src].unknown() &&
        State.InputExpressions[Instruction.Src])
      Value = {RegisterValue::Kind::Constant,
               *State.InputExpressions[Instruction.Src], 0};
    if (State.InputSpills.empty() &&
        Value.ValueKind == RegisterValue::Kind::Unknown)
      return llvm::Error::success();
    std::optional<StoredRegisterMemory> Output;
    for (const auto &Base : possibleValues(State.Registers[Instruction.Dst])) {
      MachineState Spill;
      Spill.Registers[Store.Dst] = Base;
      Spill.Registers[Store.Src] = Value;
      Spill.Scratch.Values = State.InputSpills;
      applyTransfer(Store, Spill, Program.ExecutableImage,
                    ScratchLoadPolicy::ProvenValues);
      if (!Output)
        Output = std::move(Spill.Scratch.Values);
      else
        Output->meet(Spill.Scratch.Values);
    }
    State.InputSpills = std::move(*Output);
  }
  if (!Instruction.Semantics.WritesDestination)
    return llvm::Error::success();
  if (Instruction.Op == Operation::Load &&
      Instruction.Semantics.MemoryWidth == kDoubleWordBitWidth) {
    std::optional<size_t> Stored;
    for (const auto &Base : possibleValues(State.Registers[Instruction.Src])) {
      const auto Address = effectiveAddress(Base, Instruction.Offset);
      const auto Value =
          Address ? State.InputSpills.read(
                        *Address,
                        Base.ValueKind == RegisterValue::Kind::StackAddress
                            ? MemoryCoordinate::FrameRelative
                            : MemoryCoordinate::Absolute)
                  : RegisterValue{};
      if (Value.ValueKind != RegisterValue::Kind::Constant ||
          (Stored && *Stored != Value.Value)) {
        Stored.reset();
        break;
      }
      Stored = Value.Value;
    }
    if (Stored) {
      // The load happened before the spill. Later writes to its original
      // input cell cannot change the value already captured in this cell.
      State.InputExpressions[Instruction.Dst] = Stored;
      return llvm::Error::success();
    }
  }
  const bool Copy = Instruction.Op == Operation::Mov &&
                    Instruction.Form == OperandForm::DstSrc &&
                    Instruction.Width == kDoubleWordBitWidth;
  const bool Load = Instruction.Op == Operation::Load &&
                    Instruction.Semantics.MemoryWidth == kDoubleWordBitWidth &&
                    !State.InputWrites.Unknown;
  const bool Affine =
      (Instruction.Op == Operation::Add || Instruction.Op == Operation::Sub) &&
      Instruction.Form == OperandForm::DstImm &&
      Instruction.Width == kDoubleWordBitWidth;
  // Capture the source before killing Dst, including LD r2, [r2+40].
  const unsigned Source = Load || Copy ? Instruction.Src : Instruction.Dst;
  auto Input = State.InputExpressions[Source];
  if (Copy) {
    State.InputExpressions[Instruction.Dst] = Input;
    return llvm::Error::success();
  }
  State.InputExpressions[Instruction.Dst].reset();
  if (!Load && !Affine)
    return llvm::Error::success();
  // Reuse a complete canonical origin instead of allocating another guarded
  // expression for a field already known after local stores or helper returns.
  if (Load &&
      (State.InputWrites.WritesFrames || !State.InputWrites.Writes.empty()) &&
      !loadOrigin(Instruction, State).unknown())
    return llvm::Error::success();
  if (Load) {
    const auto Address =
        substituteOrigins(RegisterValue{RegisterValue::Kind::StackAddress,
                                        Source + 1, Instruction.Offset},
                          State.Origins, Program);
    // A guard that discards this very entry-frame cell cannot recover its
    // value from an actual caller. Local stored facts were handled above.
    if (llvm::any_of(Address.values(), [&](const auto &Value) {
          return State.InputWrites.invalidatesEntryFrame(Value, Program);
        }))
      return llvm::Error::success();
  }
  if (!Input && Load) {
    auto Origin = inputOriginExpression(State.Origins[Source]);
    if (!Origin)
      return Origin.takeError();
    Input = *Origin;
  }
  if (!Input) {
    const auto Value = State.Registers[Source].singleton();
    if (!Load || Value.ValueKind != RegisterValue::Kind::Constant)
      return llvm::Error::success();
    CallXInputExpression Constant;
    Constant.Kind = CallXInputExpressionKind::Constant;
    Constant.Constant = Value;
    auto ID = inputExpression(Constant);
    if (!ID)
      return ID.takeError();
    Input = *ID;
  }
  CallXInputExpression Step;
  Step.Kind = CallXInputExpressionKind::Instruction;
  Step.Input = Input;
  Step.InstructionSlot = Instruction.Slot;
  if (Load) {
    auto Guard = inputMemoryGuard(State.InputWrites);
    if (!Guard)
      return Guard.takeError();
    Step.MemoryGuard = *Guard;
  }
  auto ID = inputExpression(Step);
  if (!ID)
    return ID.takeError();
  State.InputExpressions[Instruction.Dst] = *ID;
  return llvm::Error::success();
}

ProvenValueSet
ContextFlow::loadFullWord(const MedInstruction &Instruction,
                          const ProvenValueSet &Bases,
                          const StoredRegisterMemory &Memory) const {
  std::optional<ProvenValueSet> Output;
  for (const auto &Base : possibleValues(Bases)) {
    const auto Address = effectiveAddress(Base, Instruction.Offset);
    const auto Coordinate = Base.ValueKind == RegisterValue::Kind::StackAddress
                                ? MemoryCoordinate::FrameRelative
                                : MemoryCoordinate::Absolute;
    const auto Stored =
        Address ? Memory.readValues(*Address, Coordinate) : ProvenValueSet{};
    for (const auto &Value : possibleValues(Stored)) {
      StoredRegisterMemory Cell;
      if (Address && !Stored.unknown())
        Cell.write(*Address, Value, Coordinate);
      RegisterState Registers{};
      Registers[Instruction.Src] = Base;
      applyRegisterTransfer(Instruction, Registers, Program.ExecutableImage,
                            &Cell);
      const ProvenValueSet Loaded(Registers[Instruction.Dst]);
      if (!Output)
        Output = Loaded;
      else
        Output->join(Loaded);
    }
  }
  return *Output;
}

ProvenValueSet ContextFlow::loadOrigin(const MedInstruction &Instruction,
                                       const ContextState &State) const {
  auto Origin = loadFullWord(Instruction, State.Registers[Instruction.Src],
                             State.OriginScratch.Values);
  if (!Origin.unknown())
    return Origin;
  RegisterState Entry{};
  Entry[0] = State.Origins[Instruction.Src].singleton();
  return State.Postconditions.read(substituteOrigin(
      {RegisterValue::Kind::StackAddress, 1, Instruction.Offset}, Entry,
      Program));
}

void ContextFlow::transferValues(const MedInstruction &Instruction,
                                 ContextState &State) {
  const auto OriginAddress = [&](unsigned Register) {
    RegisterState Entry{};
    Entry[0] = State.Origins[Register].singleton();
    return substituteOrigin(
        {RegisterValue::Kind::StackAddress, 1, Instruction.Offset}, Entry,
        Program);
  };
  const bool FullWordLoad =
      Instruction.Op == Operation::Load &&
      Instruction.Semantics.MemoryWidth == kDoubleWordBitWidth;
  const auto LoadedOrigin =
      FullWordLoad ? State.Postconditions.read(OriginAddress(Instruction.Src))
                   : ProvenValueSet{};
  MachineState Scalar = State.scalar();
  if (Instruction.Op == Operation::Store) {
    ProvenValueSet StoredOrigin =
        Instruction.Form == OperandForm::StoreReg
            ? State.Origins[Instruction.Src]
            : ProvenValueSet(RegisterValue{
                  RegisterValue::Kind::Constant,
                  normalizeImmediate(Instruction.Immediate,
                                     Instruction.Semantics.Immediate),
                  0});
    // Only literal actual values can replace lost formal origins. A frame
    // coordinate must remain relative to an entry input during composition.
    if (StoredOrigin.unknown() &&
        std::all_of(State.Registers[Instruction.Src].values().begin(),
                    State.Registers[Instruction.Src].values().end(),
                    [](const auto &Value) {
                      return Value.ValueKind == RegisterValue::Kind::Constant;
                    }))
      StoredOrigin = State.Registers[Instruction.Src];
    State.Postconditions.store(OriginAddress(Instruction.Dst),
                               Instruction.Width / kBitsPerByte, StoredOrigin);
    MedInstruction Store = Instruction;
    if (Store.Form == OperandForm::StoreReg)
      Store.Src = Store.Dst == 0 ? 1 : 0;
    std::optional<ScratchState> OriginOutput;
    for (const auto &Base : possibleValues(State.Registers[Instruction.Dst]))
      for (const auto &Origin : possibleValues(StoredOrigin)) {
        MachineState Variant = Scalar;
        Variant.Scratch = State.OriginScratch;
        Variant.Registers[Store.Dst] = Base;
        if (Store.Form == OperandForm::StoreReg)
          Variant.Registers[Store.Src] = Origin;
        applyTransfer(Store, Variant, Program.ExecutableImage,
                      ScratchLoadPolicy::ProvenValues);
        if (!OriginOutput)
          OriginOutput = std::move(Variant.Scratch);
        else {
          OriginOutput->Memory.meet(Variant.Scratch.Memory);
          OriginOutput->Values.joinValues(Variant.Scratch.Values);
        }
      }
    State.OriginScratch = std::move(*OriginOutput);
    State.OriginScratch.Memory.clear();
  } else if (Instruction.Call != CallKind::None) {
    if (Instruction.Call != CallKind::Syscall || !Instruction.Syscall ||
        !preservesCallerMemory(Instruction.Syscall->ID))
      State.Postconditions.clear();
    MachineState OriginMemory = Scalar;
    OriginMemory.Scratch = State.OriginScratch;
    applyTransfer(Instruction, OriginMemory, Program.ExecutableImage,
                  ScratchLoadPolicy::ProvenValues);
    State.OriginScratch = std::move(OriginMemory.Scratch);
    State.OriginScratch.Memory.clear();
  }
  if (Instruction.Call != CallKind::None) {
    for (unsigned R = 0; R < callClobberedRegisterCount(Instruction.Call);
         ++R) {
      State.Origins[R] = {};
      State.InputExpressions[R].reset();
    }
  } else if (Instruction.Semantics.WritesDestination) {
    if (Instruction.Op == Operation::Load) {
      State.Origins[Instruction.Dst] =
          FullWordLoad ? loadOrigin(Instruction, State)
                       : registerResult(Instruction, State.Registers,
                                        Program.ExecutableImage,
                                        &State.OriginScratch.Values);
      if (State.Origins[Instruction.Dst].unknown())
        State.Origins[Instruction.Dst] = LoadedOrigin;
    } else {
      State.Origins[Instruction.Dst] =
          registerResult(Instruction, State.Origins, Program.ExecutableImage);
    }
  }
  if (Instruction.Op == Operation::Store) {
    // Both lanes use the canonical store owner over complete alternatives.
    // Alias invalidation and full-width pointer tags are not reimplemented.
    std::optional<ScratchState> Output;
    for (const auto &Base : possibleValues(State.Registers[Instruction.Dst]))
      for (const auto &Value :
           possibleValues(State.Registers[Instruction.Src])) {
        MachineState Variant = Scalar;
        Variant.Registers[Instruction.Dst] = Base;
        if (Instruction.Form == OperandForm::StoreReg)
          Variant.Registers[Instruction.Src] = Value;
        applyTransfer(Instruction, Variant, Program.ExecutableImage,
                      ScratchLoadPolicy::ProvenValues);
        if (!Output)
          Output = std::move(Variant.Scratch);
        else {
          Output->Memory.meet(Variant.Scratch.Memory);
          Output->Values.joinValues(Variant.Scratch.Values);
        }
      }
    State.Scratch = std::move(*Output);
    State.Scratch.Memory.clear();
    return;
  }
  if (Instruction.Call != CallKind::None) {
    applyTransfer(Instruction, Scalar, Program.ExecutableImage,
                  ScratchLoadPolicy::ProvenValues);
    State.Scratch = std::move(Scalar.Scratch);
    State.Scratch.Memory.clear();
    for (unsigned R = 0; R < callClobberedRegisterCount(Instruction.Call); ++R)
      State.Registers[R] = {};
    return;
  }
  if (!Instruction.Semantics.WritesDestination)
    return;
  if (FullWordLoad) {
    State.Registers[Instruction.Dst] = loadFullWord(
        Instruction, State.Registers[Instruction.Src], State.Scratch.Values);
    if (State.Registers[Instruction.Dst].unknown() &&
        std::all_of(LoadedOrigin.values().begin(), LoadedOrigin.values().end(),
                    [](const auto &Value) {
                      return Value.ValueKind == RegisterValue::Kind::Constant;
                    }))
      State.Registers[Instruction.Dst] = LoadedOrigin;
  } else {
    State.Registers[Instruction.Dst] =
        registerResult(Instruction, State.Registers, Program.ExecutableImage,
                       &Scalar.Scratch.Values);
  }
}

void ContextFlow::refineOrigins(const FunctionContext &Context,
                                ContextState &State) {
  const auto Frame = Context.Input.Registers[kFramePointerRegister].singleton();
  for (unsigned R = 0; R < kRegisterCount; ++R) {
    const auto Value = State.Registers[R].singleton();
    if (Value.ValueKind == RegisterValue::Kind::StackAddress &&
        Frame.ValueKind == RegisterValue::Kind::StackAddress &&
        Value.Value == Frame.Value) {
      int64_t Offset;
      if (!llvm::SubOverflow(Value.Offset, Frame.Offset, Offset))
        State.Origins[R] = RegisterValue{RegisterValue::Kind::StackAddress,
                                         kRegisterCount, Offset};
    } else if (State.Origins[R].unknown() &&
               Value.ValueKind == RegisterValue::Kind::Constant) {
      State.Origins[R] = Value;
    } else if (State.Registers[R].unknown()) {
      const auto Origin =
          substituteOrigins(State.Origins[R], Context.Input.Registers, Program);
      if (!Origin.unknown())
        State.Registers[R] = Origin;
    }
  }
}

void ContextFlow::transfer(const MedInstruction &Instruction,
                           ContextState &State, MissingRegisters *Missing) {
  const RegisterValue Base = State.Registers[Instruction.Src].singleton();
  const auto Address = Instruction.Op == Operation::Load
                           ? effectiveAddress(Base, Instruction.Offset)
                           : std::nullopt;
  const CallXMissingFact Source =
      Missing ? (*Missing)[Instruction.Src] : CallXMissingFact{};
  const CallXMissingFact Destination =
      Missing ? (*Missing)[Instruction.Dst] : CallXMissingFact{};
  transferValues(Instruction, State);
  if (!Missing || !Instruction.Semantics.WritesDestination)
    return;
  CallXMissingFact &Why = (*Missing)[Instruction.Dst];
  Why = {};
  if (!State.Registers[Instruction.Dst].unknown())
    return;
  if (Instruction.Op == Operation::Load) {
    if (!Address && Source.Kind != CallXMissingFactKind::None) {
      Why = Source;
    } else {
      Why.Slot = Instruction.Slot;
      Why.Address = Address;
      Why.Coordinate = Base.ValueKind == RegisterValue::Kind::StackAddress
                           ? MemoryCoordinate::FrameRelative
                           : MemoryCoordinate::Absolute;
      Why.Kind =
          Why.Coordinate == MemoryCoordinate::FrameRelative ||
                  (Address && isScratchAddress(*Address))
              ? (Instruction.Semantics.MemoryWidth == kDoubleWordBitWidth
                     ? CallXMissingFactKind::ScratchWord
                     : CallXMissingFactKind::ScratchLoadWidth)
          : Base.ValueKind == RegisterValue::Kind::InstructionDataAddress ||
                  (Address && *Address >= kInputStart &&
                   *Address - kInputStart < kMemoryRegionSize)
              ? CallXMissingFactKind::RuntimeMemory
              : CallXMissingFactKind::UnmappedLoad;
    }
  } else if (Instruction.Op == Operation::Mov &&
             Instruction.Form == OperandForm::DstSrc &&
             Instruction.Width == kDoubleWordBitWidth &&
             Source.Kind != CallXMissingFactKind::None) {
    Why = Source;
  } else if ((Instruction.Op == Operation::Add ||
              Instruction.Op == Operation::Sub) &&
             Instruction.Form == OperandForm::DstImm &&
             Destination.Kind != CallXMissingFactKind::None) {
    Why = Destination;
  } else {
    Why.Kind = CallXMissingFactKind::RegisterWrite;
    Why.Slot = Instruction.Slot;
  }
}

CallXContextEvidence ContextFlow::evidence(const FunctionContext &Context,
                                           const MedInstruction &Instruction,
                                           const ContextState &State,
                                           const MissingRegisters &Missing,
                                           bool BeyondEntry) const {
  CallXContextEvidence Evidence;
  Evidence.SummaryID = Context.ID;
  Evidence.GeneralizedInputs = Context.Generic;
  Evidence.InputGeneralization = Context.Generic
                                     ? CallXInputGeneralization::UnknownEntry
                                     : CallXInputGeneralization::None;
  Evidence.CalleeEntrySlot = Program.Med.Blocks[Context.EntryBlock].StartSlot;
  Evidence.CallXSlot = Instruction.Slot;
  Evidence.BeyondEntryBlock = BeyondEntry;
  Evidence.InputExpression = State.InputExpressions[Instruction.CallRegister];
  classifyTarget(Evidence, State.Registers[Instruction.CallRegister],
                 Missing[Instruction.CallRegister]);
  return Evidence;
}

void ContextFlow::classifyTarget(CallXContextEvidence &Evidence,
                                 const ProvenValueSet &Values,
                                 const CallXMissingFact &Missing) const {
  Evidence.Result = CallXContextResult::UnknownTarget;
  Evidence.TargetAddress.reset();
  Evidence.TargetSlot.reset();
  Evidence.TargetValueAlternatives = {};
  Evidence.Missing = {};
  const RegisterValue Target = Values.singleton();
  if (Target.ValueKind == RegisterValue::Kind::Constant) {
    Evidence.TargetAddress = Target.Value;
    if (const auto Slot = rawTargetSlot(Target.Value)) {
      Evidence.Result = CallXContextResult::ProvenTarget;
      Evidence.TargetSlot = Slot;
      Evidence.TargetIsContinuation =
          Program.Low.Instructions[*Slot].IsContinuation;
    } else {
      Evidence.Result = CallXContextResult::InvalidTarget;
    }
  } else {
    Evidence.Missing = Missing;
    if (Values.values().size() > 1) {
      Evidence.TargetValueAlternatives = Values;
      const bool KnownRawTargets = std::all_of(
          Evidence.TargetValueAlternatives.values().begin(),
          Evidence.TargetValueAlternatives.values().end(),
          [&](const auto &Value) {
            return Value.ValueKind == RegisterValue::Kind::Constant &&
                   rawTargetSlot(Value.Value).has_value();
          });
      if (KnownRawTargets) {
        Evidence.Result = CallXContextResult::ProvenTargetSet;
        Evidence.Missing = {};
      } else {
        Evidence.Missing.Kind = CallXMissingFactKind::MultipleTargetValues;
      }
    } else if (Evidence.Missing.Kind == CallXMissingFactKind::None)
      Evidence.Missing.Kind = CallXMissingFactKind::SymbolicTarget;
  }
}

llvm::Error ContextFlow::instantiateTargetInputs(FunctionContext &Caller,
                                                 const FunctionContext &Callee,
                                                 const ContextState &Input,
                                                 ContextCall &Edge) {
  if (Callee.Sites.empty())
    return llvm::Error::success();
  bool CheckedPath = false;
  std::optional<size_t> FirstCall;
  const auto SelectFirstCall = [&]() -> llvm::Expected<std::optional<size_t>> {
    // Local caller facts cannot authorize a callee's conditional site unless
    // they select its path. Follow only known successors, stop at every call,
    // and never turn an unknown branch or a repeated block into reachability.
    uint64_t Bytes = stateBytes(Input);
    if (auto Error = retain(Bytes))
      return std::move(Error);
    const auto Release = llvm::scope_exit([&] { RetainedBytes -= Bytes; });
    ContextState Prefix = Input;
    std::set<size_t> Seen;
    uint64_t SeenBytes = 0;
    size_t BlockID = Callee.EntryBlock;
    while (!Seen.contains(BlockID)) {
      constexpr uint64_t NodeBytes = sizeof(size_t) + 4 * sizeof(void *);
      if (auto Error = retain(NodeBytes))
        return std::move(Error);
      SeenBytes += NodeBytes;
      Bytes += NodeBytes;
      Seen.insert(BlockID);
      const auto &Block = Program.Med.Blocks[BlockID];
      for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
        const auto *Instruction = Index.find(Slot);
        if (!Instruction)
          continue;
        if (auto Error = visit())
          return std::move(Error);
        if (Instruction->Call != CallKind::None)
          return std::optional<size_t>(Slot);
        transferValues(*Instruction, Prefix);
        const uint64_t NewBytes = stateBytes(Prefix) + SeenBytes;
        if (NewBytes > Bytes) {
          if (auto Error = retain(NewBytes - Bytes))
            return std::move(Error);
        } else {
          RetainedBytes -= Bytes - NewBytes;
        }
        Bytes = NewBytes;
      }
      const auto *Last = Index.find(Block.EndSlot - 1);
      const auto Selected = knownBranchSuccessor(Last, Prefix);
      if (!Selected && Last &&
          Last->Semantics.Terminator == TerminatorKind::ConditionalJump)
        break;
      std::optional<size_t> Next;
      for (size_t E = Offsets[BlockID]; E < Offsets[BlockID + 1]; ++E) {
        if (Selected &&
            Program.Med.Blocks[Successors[E]].StartSlot != *Selected)
          continue;
        if (Next)
          return std::optional<size_t>{};
        Next = Successors[E];
      }
      if (!Next)
        break;
      BlockID = *Next;
    }
    return std::optional<size_t>{};
  };
  for (const auto &Template : Callee.Sites) {
    if (!Template.InputExpression || Template.TargetAddress ||
        Template.Result == CallXContextResult::ProvenTargetSet)
      continue;
    const size_t Root = *Template.InputExpression;
    const auto Unbound = [&](size_t ID) {
      const auto &Leaf = InputExpressions[ID];
      return Leaf.Kind == CallXInputExpressionKind::InputRegister &&
             Input.Registers[Leaf.Register].unknown();
    };
    // Unary loads/affine transfers and their complete input union cannot
    // supply a target when an entry binding is unknown. Avoid building and
    // evaluating their proof chain with that already known result.
    const size_t Leaf = InputExpressionRoots[Root];
    if (Unbound(Leaf) ||
        (InputExpressions[Leaf].Kind ==
             CallXInputExpressionKind::InputAlternatives &&
         std::any_of(InputExpressions[Leaf].Alternatives.begin(),
                     InputExpressions[Leaf].Alternatives.end(), Unbound)))
      continue;
    if (Caller.Generic) {
      if (!CheckedPath) {
        auto Selected = SelectFirstCall();
        if (!Selected)
          return Selected.takeError();
        FirstCall = *Selected;
        CheckedPath = true;
      }
      if (FirstCall != Template.CallXSlot)
        continue;
    }
    const size_t Depth = InputExpressions[Root].Depth;
    const uint64_t TemporaryBytes =
        Depth * (sizeof(size_t) + sizeof(CallXInputExpressionValue) +
                 2 * kCallXContextValueAlternatives * sizeof(RegisterValue));
    if (auto Error = retain(TemporaryBytes))
      return Error;
    const auto Release =
        llvm::scope_exit([&] { RetainedBytes -= TemporaryBytes; });
    std::vector<size_t> Chain;
    Chain.reserve(Depth);
    Chain.push_back(Root);
    for (size_t I = 0; I < Chain.size(); ++I) {
      const auto &Node = InputExpressions[Chain[I]];
      if (Node.Kind == CallXInputExpressionKind::InputAlternatives)
        Chain.insert(Chain.end(), Node.Alternatives.rbegin(),
                     Node.Alternatives.rend());
      else if (Node.Input)
        Chain.push_back(*Node.Input);
    }
    std::vector<CallXInputExpressionValue> Proof;
    Proof.reserve(Depth);
    ProvenValueSet Values;
    for (auto It = Chain.rbegin(); It != Chain.rend(); ++It) {
      if (auto Error = visit())
        return Error;
      const auto &Node = InputExpressions[*It];
      CallXInputExpressionValue Witness;
      Witness.Expression = *It;
      if (Node.Kind == CallXInputExpressionKind::InputRegister) {
        Witness.Inputs = Input.Registers[Node.Register];
        std::optional<ProvenValueSet> Output;
        for (const auto &Value : possibleValues(Witness.Inputs)) {
          RegisterState Actual{};
          Actual[Node.Register] = Value;
          const ProvenValueSet Result(
              substituteOrigin({RegisterValue::Kind::StackAddress,
                                Node.Register + 1, Node.Offset},
                               Actual, Program));
          if (!Output)
            Output = Result;
          else
            Output->join(Result);
        }
        Values = std::move(*Output);
      } else if (Node.Kind == CallXInputExpressionKind::Constant) {
        Values = Node.Constant;
      } else if (Node.Kind == CallXInputExpressionKind::InputAlternatives) {
        std::optional<ProvenValueSet> Complete;
        for (const auto &Leaf : Proof) {
          if (!Complete)
            Complete = Leaf.Values;
          else
            Complete->join(Leaf.Values);
        }
        Values = std::move(*Complete);
        Witness.Inputs = Values;
      } else {
        Witness.Inputs = Values;
        const auto &Instruction = *Index.find(Node.InstructionSlot);
        if (Instruction.Op == Operation::Load) {
          if (Node.MemoryGuard) {
            const auto &Effects = InputMemoryGuards[*Node.MemoryGuard];
            const uint64_t Bytes = Input.Scratch.Values.retainedByteEstimate();
            if (auto Error = retain(Bytes))
              return Error;
            const auto ReleaseMemory =
                llvm::scope_exit([&] { RetainedBytes -= Bytes; });
            for (size_t I = 0;
                 I < Input.Scratch.Values.size() + Effects.Writes.size() + 1;
                 ++I)
              if (auto Error = visit())
                return Error;
            ScratchState Memory;
            Memory.Values = Input.Scratch.Values;
            Effects.invalidate(Memory, Input.Registers, Program);
            Values = loadFullWord(Instruction, Values, Memory.Values);
          } else {
            Values = loadFullWord(Instruction, Values, Input.Scratch.Values);
          }
        } else {
          std::optional<ProvenValueSet> Output;
          for (const auto &Value : possibleValues(Values)) {
            RegisterState Actual{};
            Actual[Instruction.Dst] = Value;
            applyRegisterTransfer(Instruction, Actual, Program.ExecutableImage);
            const ProvenValueSet Result(Actual[Instruction.Dst]);
            if (!Output)
              Output = Result;
            else
              Output->join(Result);
          }
          Values = std::move(*Output);
        }
      }
      Witness.Values = Values;
      Proof.push_back(std::move(Witness));
      if (Values.unknown())
        break;
    }
    if (Values.unknown())
      continue;
    CallXContextEvidence Site = Template;
    classifyTarget(Site, Values, {});
    if (Site.Result == CallXContextResult::UnknownTarget)
      continue;
    Site.InputExpressionValues = std::move(Proof);
    uint64_t Bytes = 2 * sizeof(Site) +
                     Site.TargetValueAlternatives.allocatedBytes() +
                     Site.InputExpressionValues.capacity() *
                         sizeof(CallXInputExpressionValue);
    for (const auto &Value : Site.InputExpressionValues)
      Bytes += Value.Inputs.allocatedBytes() + Value.Values.allocatedBytes();
    if (auto Error = retain(Bytes))
      return Error;
    Caller.RetainedBytes += Bytes;
    Edge.InputExpressionTargets.push_back(std::move(Site));
    ++InstantiatedTargetInputExpressions;
  }
  return llvm::Error::success();
}

llvm::Expected<bool> ContextFlow::call(FunctionContext &Context,
                                       const MedInstruction &Instruction,
                                       ContextState &State, bool Collect) {
  if (Instruction.Call != CallKind::Indirect)
    return callTarget(Context, Instruction, State, Collect);
  const auto Targets = State.Registers[Instruction.CallRegister].values();
  if (Targets.size() < 2 ||
      !std::all_of(Targets.begin(), Targets.end(), [](const auto &Value) {
        return Value.ValueKind == RegisterValue::Kind::Constant;
      }))
    return callTarget(Context, Instruction, State, Collect);

  std::optional<ContextState> Joined;
  uint64_t JoinedBytes = 0;
  const auto Release = llvm::scope_exit([&] { RetainedBytes -= JoinedBytes; });
  for (const auto &Target : Targets) {
    ContextState Alternative = State;
    Alternative.Registers[Instruction.CallRegister] = Target;
    const uint64_t AlternativeBytes = stateBytes(Alternative);
    if (auto Error = retain(AlternativeBytes))
      return std::move(Error);
    const auto ReleaseAlternative =
        llvm::scope_exit([&] { RetainedBytes -= AlternativeBytes; });
    // Apply each output and record its dependency before requesting another
    // specialization: an unreferenced callee cache node may be reclaimed.
    auto Continues = callTarget(Context, Instruction, Alternative, Collect);
    if (!Continues)
      return Continues.takeError();
    if (!*Continues)
      continue;
    if (Joined)
      Joined->join(Alternative, Program);
    else
      Joined = std::move(Alternative);
    const uint64_t Bytes = stateBytes(*Joined);
    if (Bytes > JoinedBytes) {
      if (auto Error = retain(Bytes - JoinedBytes))
        return std::move(Error);
    } else {
      RetainedBytes -= JoinedBytes - Bytes;
    }
    JoinedBytes = Bytes;
  }
  if (!Joined)
    return false;
  State = std::move(*Joined);
  return true;
}

llvm::Expected<bool> ContextFlow::callTarget(FunctionContext &Context,
                                             const MedInstruction &Instruction,
                                             ContextState &State,
                                             bool Collect) {
  // Resolved runtime calls keep the one canonical memory-effect owner.
  if (Instruction.Call == CallKind::Syscall) {
    if (Context.Generic && (!Instruction.Syscall ||
                            !preservesCallerMemory(Instruction.Syscall->ID)))
      State.InputWrites.Unknown = true;
    if (!Instruction.Syscall || !preservesCallerMemory(Instruction.Syscall->ID))
      State.InputSpills.clear();
    if (!Instruction.Syscall) {
      Context.PreservesMemory = false;
      if (Collect)
        Context.Effects.Unknown = true;
    } else
      for (const auto &Window : getSyscallMemory(Instruction.Syscall->ID))
        if (Window.Access == SyscallMemoryAccess::Write) {
          Context.PreservesMemory = false;
          if (Collect)
            Context.Effects.Unknown = true;
        }
    transfer(Instruction, State, nullptr);
    return true;
  }
  ContextCall Edge;
  Edge.CallerSummaryID = Context.ID;
  Edge.CallerSlot = Instruction.Slot;
  Edge.CalleeEntrySlot = Instruction.CallTarget;
  if (Instruction.Call == CallKind::Indirect) {
    const RegisterValue Target =
        State.Registers[Instruction.CallRegister].singleton();
    if (Target.ValueKind == RegisterValue::Kind::Constant) {
      Edge.CallXTargetAddress = Target.Value;
      Edge.CalleeEntrySlot = rawTargetSlot(Target.Value);
    }
  }
  Edge.LegacyRuntime =
      Instruction.Dispatch == CallDispatchPolicy::LegacyRuntimeThenFunction &&
      Program.isSyscallRegistered(static_cast<uint32_t>(Instruction.Immediate));
  if (!Edge.CalleeEntrySlot)
    Edge.Barrier = CallXSummaryBarrier::UnknownTarget;
  else if (!ByEntry.contains(*Edge.CalleeEntrySlot) ||
           !Index.find(*Edge.CalleeEntrySlot))
    Edge.Barrier = CallXSummaryBarrier::UnsupportedRawEntry;
  else if (ActiveEntries.contains(*Edge.CalleeEntrySlot))
    Edge.Barrier = CallXSummaryBarrier::RecursiveCall;
  else if (ActiveEntries.size() >= Program.Config.MaxCallDepth - 1)
    Edge.Barrier = CallXSummaryBarrier::CallDepth;

  FunctionContext *Callee = nullptr;
  if (Edge.Barrier == CallXSummaryBarrier::None) {
    const size_t EntryBlock = ByEntry.at(*Edge.CalleeEntrySlot);
    // A literal CALLX may discover a block that was not a recognized function
    // root. Establish its generic summary before requesting a specialization.
    if (!Context.Generic && !GenericContexts.contains(EntryBlock) &&
        !SpecializationBudgetExhausted) {
      auto Generic = summarize(EntryBlock, ContextState{}, true);
      if (!Generic) {
        if (!SpecializationBudgetExhausted)
          return Generic.takeError();
        llvm::consumeError(Generic.takeError());
      }
    }
    const auto Generic = GenericContexts.find(EntryBlock);
    if (!Context.Generic &&
        (Generic == GenericContexts.end() || !Generic->second->Complete))
      Edge.Barrier = CallXSummaryBarrier::SummaryBudget;
  }
  if (Edge.Barrier == CallXSummaryBarrier::None) {
    ContextState Input = Context.Generic ? ContextState{} : State;
    if (Edge.LegacyRuntime) {
      // Legacy runtime dispatch precedes the function. Its effects cannot be
      // bypassed using the file-local target or the original arguments.
      Input.Scratch.clear();
      for (unsigned R = 0; R < callClobberedRegisterCount(Instruction.Call);
           ++R)
        Input.Registers[R] = {};
    }
    pushAbstractFrame(Input, Program);
    const size_t EntryBlock = ByEntry.at(*Edge.CalleeEntrySlot);
    CallXInputGeneralization Generalization = CallXInputGeneralization::None;
    auto Result =
        summarize(EntryBlock, Input, Context.Generic, &Generalization);
    if (!Result) {
      if (Context.Generic || !SpecializationBudgetExhausted)
        return Result.takeError();
      llvm::consumeError(Result.takeError());
      Result = GenericContexts.at(EntryBlock);
      Generalization = CallXInputGeneralization::ResourceBudget;
    }
    Callee = *Result;
    Edge.CalleeSummaryID = Callee->ID;
    Edge.GeneralizedInputs = Callee->Generic;
    Edge.InputGeneralization = Generalization;
    if (Collect && Callee->Generic && !Callee->Sites.empty() &&
        !Edge.LegacyRuntime && !SpecializationBudgetExhausted) {
      uint64_t InputBytes = 0;
      if (Context.Generic) {
        InputBytes = sizeof(Input.Registers) +
                     State.Scratch.Values.retainedByteEstimate();
        for (const auto &Value : State.Registers)
          InputBytes += Value.allocatedBytes();
        if (auto Error = retain(InputBytes))
          return std::move(Error);
      }
      const auto ReleaseInput =
          llvm::scope_exit([&] { RetainedBytes -= InputBytes; });
      if (Context.Generic) {
        Input.Registers = State.Registers;
        Input.Scratch.Values = State.Scratch.Values;
        pushAbstractFrame(Input, Program);
      }
      if (auto Error = instantiateTargetInputs(Context, *Callee, Input, Edge)) {
        if (!SpecializationBudgetExhausted)
          return std::move(Error);
        llvm::consumeError(std::move(Error));
      }
    }
  }
  Context.PreservesMemory &=
      Callee && Callee->PreservesMemory && !Edge.LegacyRuntime;
  auto OriginEntry = State.Origins;
  pushAbstractFrame(OriginEntry, Program);
  RegisterState ScalarOriginEntry{};
  for (unsigned R = 0; R < kRegisterCount; ++R)
    ScalarOriginEntry[R] = OriginEntry[R].singleton();
  ContextState ValueEntry;
  ValueEntry.Registers = State.Registers;
  pushAbstractFrame(ValueEntry, Program);
  const auto ReturnOrigins =
      Callee && Callee->Return && !Edge.LegacyRuntime
          ? substituteOrigins(Callee->Return->Origins[0], OriginEntry, Program)
          : ProvenValueSet{};
  std::optional<size_t> ReturnExpression;
  const bool NeedsReturnExpression = LiveCallReturns[Instruction.Slot];
  if (Context.Generic && Callee && Callee->Return && !Edge.LegacyRuntime &&
      NeedsReturnExpression && ReturnOrigins.unknown() &&
      Callee->Return->InputExpressions[kReturnRegister]) {
    // Complete substituted origins already describe this return. Rebuilding
    // their instruction chain repeats work at every solver replay and can
    // consume the fixed budget before unrelated caller proofs are reached.
    auto Expression = composeReturnExpression(
        *Callee->Return->InputExpressions[kReturnRegister], State, OriginEntry);
    if (!Expression)
      return Expression.takeError();
    ReturnExpression = *Expression;
  }
  if (Collect) {
    if (!Callee || Edge.LegacyRuntime)
      Context.Effects.Unknown = true;
    else
      Context.Effects.include(Callee->Effects, ScalarOriginEntry, Program);
  }
  // This is the canonical caller ABI: r0-r5 are clobbered, r6-r10 survive.
  // A converged callee supplies only its return carrier and memory effects.
  ScratchState Preserved;
  ScratchState PreservedOrigins = State.OriginScratch;
  const bool PreserveInput =
      Callee && Callee->PreservesMemory && !Edge.LegacyRuntime;
  if (Context.Generic && !PreserveInput) {
    if (Callee && !Edge.LegacyRuntime)
      State.InputWrites.include(Callee->Effects, ScalarOriginEntry, Program);
    else
      State.InputWrites.Unknown = true;
  }
  const bool EquivalentLeaf = Edge.InputGeneralization ==
                              CallXInputGeneralization::EquivalentGenericInputs;
  const bool ApplyEffects = Callee && Callee->Generic && !EquivalentLeaf &&
                            !Callee->Effects.Unknown && !Edge.LegacyRuntime;
  if (PreserveInput || ApplyEffects)
    Preserved = State.Scratch;
  if (!PreserveInput && ApplyEffects) {
    Edge.AppliedMemoryEffects = true;
    Edge.PreservedOlderFrames =
        Callee->Effects.invalidate(Preserved, ValueEntry.Registers, Program) &&
        Callee->Effects.WritesFrames;
  }
  if (!PreserveInput) {
    ScratchState Spills;
    Spills.Values = std::move(State.InputSpills);
    if (Callee && !Edge.LegacyRuntime)
      Callee->Effects.invalidate(Spills, ValueEntry.Registers, Program);
    else
      Spills.clear();
    State.InputSpills = std::move(Spills.Values);
    if (Callee && !Edge.LegacyRuntime)
      Callee->Effects.invalidate(PreservedOrigins, ValueEntry.Registers,
                                 Program);
    else
      PreservedOrigins.clear();
  }
  MemoryPostconditions PreservedPostconditions = State.Postconditions;
  ScratchState ReturnScratch;
  if (Callee && Callee->Return) {
    ReturnScratch = PreserveInput || ApplyEffects ? std::move(Preserved)
                                                  : Callee->Return->Scratch;
    if (Callee->Generic && !EquivalentLeaf && !PreserveInput && !ApplyEffects) {
      // Generic frame coordinates are a private symbolic anchor. Only
      // explicitly substituted postconditions can restore caller-frame cells.
      ReturnScratch.Memory.clear();
      ReturnScratch.Values.discardFrameCoordinates();
    }
    if (!Edge.LegacyRuntime) {
      PreservedPostconditions.invalidate(Callee->Effects, ScalarOriginEntry,
                                         Program);
      PreservedPostconditions.include(Callee->Return->Postconditions,
                                      OriginEntry, Program);
      Edge.AppliedStoredValues = Callee->Return->Postconditions.apply(
          ReturnScratch, ValueEntry.Registers, Program);
    } else {
      PreservedPostconditions.clear();
    }
  } else {
    PreservedPostconditions.clear();
  }
  if (Collect) {
    if (auto Error = retain(sizeof(Edge)))
      return std::move(Error);
    Context.RetainedBytes += sizeof(Edge);
    Context.Calls.push_back(Edge);
    if (Edge.CalleeSummaryID)
      ++Callee->IncomingReferences;
  }
  transfer(Instruction, State, nullptr);
  if (Callee) {
    if (!Callee->Return)
      return false;
    State.Registers[0] = Callee->Return->Registers[0];
    State.Scratch = std::move(ReturnScratch);
    State.Scratch.Memory.clear();
    if (Callee->Generic && !EquivalentLeaf) {
      const auto Instantiated =
          Edge.LegacyRuntime ? ProvenValueSet{}
                             : substituteOrigins(Callee->Return->Origins[0],
                                                 ValueEntry.Registers, Program);
      if (!Instantiated.unknown())
        State.Registers[0] = Instantiated;
      else
        for (const auto &Value : State.Registers[0].values())
          if (Value.ValueKind == RegisterValue::Kind::StackAddress) {
            State.Registers[0] = {};
            break;
          }
    }
    State.Origins[0] = ReturnOrigins;
    State.OriginScratch = std::move(PreservedOrigins);
    State.OriginScratch.Memory.clear();
    State.Postconditions = std::move(PreservedPostconditions);
  }
  if (Context.Generic) {
    for (unsigned R = 0; R < callClobberedRegisterCount(Instruction.Call); ++R)
      State.InputExpressions[R].reset();
    if (NeedsReturnExpression && !ReturnExpression && !Edge.LegacyRuntime) {
      auto Expression = inputOriginExpression(State.Origins[kReturnRegister]);
      if (!Expression)
        return Expression.takeError();
      ReturnExpression = *Expression;
    }
    State.InputExpressions[kReturnRegister] = ReturnExpression;
  }
  return true;
}

llvm::Expected<bool> ContextFlow::replay(FunctionContext &Context,
                                         size_t BlockID, ContextState &State,
                                         uint16_t &Defined, bool Collect) {
  const MedBlock &Block = Program.Med.Blocks[BlockID];
  MissingRegisters Missing;
  if (Collect)
    for (unsigned R = 0; R < kRegisterCount; ++R)
      if (State.Registers[R].unknown()) {
        Missing[R].Kind = BlockID == Context.EntryBlock
                              ? CallXMissingFactKind::IncomingRegister
                              : CallXMissingFactKind::BlockEntry;
        Missing[R].InputRegister = R;
        if (BlockID != Context.EntryBlock)
          Missing[R].Slot = Block.StartSlot;
      }
  for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
    const MedInstruction *Instruction = Index.find(Slot);
    if (!Instruction)
      continue;
    if (auto Error = visit())
      return std::move(Error);
    refineOrigins(Context, State);
    if (Collect && Instruction->Op == Operation::CallX &&
        Instruction->Call == CallKind::Indirect) {
      auto Site = evidence(Context, *Instruction, State, Missing,
                           BlockID != Context.EntryBlock);
      if (Context.Generic && !Site.InputExpression &&
          Site.Result == CallXContextResult::UnknownTarget) {
        auto Expression =
            inputOriginExpression(State.Origins[Instruction->CallRegister]);
        if (!Expression)
          return Expression.takeError();
        Site.InputExpression = *Expression;
      }
      const uint64_t Bytes =
          sizeof(Site) + Site.TargetValueAlternatives.allocatedBytes();
      if (auto Error = retain(Bytes))
        return std::move(Error);
      Context.RetainedBytes += Bytes;
      Context.Sites.push_back(std::move(Site));
    }
    if (Instruction->Call != CallKind::None) {
      const size_t FirstEdge = Context.Calls.size();
      auto Continues = call(Context, *Instruction, State, Collect);
      if (!Continues)
        return Continues.takeError();
      CallInputs Callee;
      if (Collect) {
        Callee = {0, false};
        for (size_t I = FirstEdge; I < Context.Calls.size(); ++I) {
          const auto &Edge = Context.Calls[I];
          if (Edge.Barrier != CallXSummaryBarrier::None || Edge.LegacyRuntime) {
            Callee = {};
            break;
          }
          const auto &Summary = *Contexts[*Edge.CalleeSummaryID];
          if (Summary.Generic) {
            Callee.Registers |= InputRegisters[Summary.EntryBlock];
            Callee.Memory |= ReadsMemory[Summary.EntryBlock];
          } else {
            Callee.Registers |= Summary.ReuseInputRegisters;
            Callee.Memory |= Summary.ReuseReadsMemory;
          }
        }
      }
      const auto Dependencies = instructionDependencies(*Instruction, Callee);
      if (Collect) {
        Context.ReuseInputRegisters |= Dependencies.Reads & ~Defined;
        Context.ReuseReadsMemory |= Dependencies.Memory;
      }
      Defined |= Dependencies.Writes;
      if (!*Continues)
        return false;
      if (Collect)
        for (unsigned R = 0; R < callClobberedRegisterCount(Instruction->Call);
             ++R) {
          Missing[R] = {};
          if (State.Registers[R].unknown()) {
            Missing[R].Kind = CallXMissingFactKind::FunctionReturn;
            Missing[R].Slot = Slot;
            Missing[R].InputRegister = R;
          }
        }
    } else {
      const auto Dependencies = instructionDependencies(*Instruction);
      if (Collect) {
        Context.ReuseInputRegisters |= Dependencies.Reads & ~Defined;
        Context.ReuseReadsMemory |= Dependencies.Memory;
      }
      Defined |= Dependencies.Writes;
      if (Instruction->Op == Operation::Store)
        Context.PreservesMemory = false;
      if (Collect && Instruction->Op == Operation::Store) {
        Context.Effects.addWrite(storeOrigin(*Instruction, State),
                                 Instruction->Width / kBitsPerByte, Program);
      }
      if (Context.Generic)
        if (auto Error = transferInputExpression(*Instruction, State))
          return std::move(Error);
      transfer(*Instruction, State, Collect ? &Missing : nullptr);
    }
  }
  return true;
}

llvm::Expected<FunctionContext *>
ContextFlow::summarize(size_t Entry, const ContextState &Incoming, bool Generic,
                       CallXInputGeneralization *Generalization) {
  const auto Generalized = [&](CallXInputGeneralization Reason) {
    if (Generalization)
      *Generalization = Reason;
    return GenericContexts.at(Entry);
  };
  if (Generalization)
    *Generalization = Generic ? CallXInputGeneralization::UnknownCaller
                              : CallXInputGeneralization::None;
  ContextState Input = Generic ? genericInput() : Incoming;
  // CALLX proofs consume typed full-width cells and immutable image bytes.
  // Descriptor-byte buffers have no proof reader and must not split otherwise
  // equivalent caller inputs or consume their retained-state budget.
  Input.Scratch.Memory.clear();
  const auto Origins = symbolicInputs();
  for (unsigned R = 0; R < kRegisterCount; ++R)
    Input.Origins[R] = Origins[R];
  Input.OriginScratch.clear();
  Input.Postconditions.clear();
  Input.InputExpressions = {};
  Input.InputSpills.clear();
  Input.InputWrites = {};
  const auto Translation =
      frameTranslationLimit(Program.Low.TheVersion, Program.Config);
  Input.Scratch.Values.setFrameTranslationLimit(Translation);
  Input.OriginScratch.Values.setFrameTranslationLimit(Translation);
  Input.InputSpills.setFrameTranslationLimit(Translation);
  if (Generic)
    for (unsigned R = 0; R < kRegisterCount; ++R) {
      CallXInputExpression Expression;
      Expression.Register = R;
      auto ID = inputExpression(Expression);
      if (!ID)
        return ID.takeError();
      Input.InputExpressions[R] = *ID;
    }
  for (unsigned R = 0; R < kRegisterCount; ++R)
    if (!(InputRegisters[Entry] & (uint16_t{1} << R))) {
      ProjectedRegisters += !Generic && !Input.Registers[R].unknown();
      Input.Registers[R] = {};
    }
  if (!Generic) {
    if (!ReadsMemory[Entry] && GenericContexts.contains(Entry) &&
        GenericContexts.at(Entry)->PreservesMemory) {
      ProjectedScratchSnapshots += !Input.Scratch.empty();
      Input.Scratch.clear();
    }
    const auto It = GenericContexts.find(Entry);
    // Generic mode drops locally known inputs at function calls. Equal entry
    // facts therefore authorize direct reuse only for a function-call-free
    // body; its scalar/syscall transfers are identical in both modes.
    if (It != GenericContexts.end() && It->second->Complete &&
        It->second->Calls.empty() &&
        Input.Registers == It->second->Input.Registers &&
        Input.Scratch == It->second->Input.Scratch) {
      ++EquivalentGenericReuses;
      ++CacheHits;
      return Generalized(CallXInputGeneralization::EquivalentGenericInputs);
    }
  }
  llvm::FoldingSetNodeID Key;
  profileInput(Key, Entry, Generic, Input);
  void *InsertPosition = nullptr;
  if (auto *Found = Cache.FindNodeOrInsertPos(Key, InsertPosition)) {
    if (!Found->Complete) {
      if (!Generic && SpecializationBudgetExhausted)
        return Generalized(CallXInputGeneralization::ResourceBudget);
      return llvm::make_error<llvm::StringError>(
          kCallXContextMemoryBudgetDiagnostic, llvm::inconvertibleErrorCode());
    }
    ++CacheHits;
    return Found;
  }
  // Only real caller edges use this conditional projection. Standalone roots
  // retain their own entry assumptions, and generic mode still drops locally
  // known inputs at nested calls, so it is not interchangeable with this mode.
  if (!Generic && Generalization && Specializations.contains(Entry))
    for (auto *Candidate : Specializations.at(Entry)) {
      if (!Candidate->Complete)
        continue;
      bool Same = true;
      for (unsigned R = 0; R < kRegisterCount; ++R)
        if ((Candidate->ReuseInputRegisters & registerBit(R)) &&
            Candidate->Input.Registers[R] != Input.Registers[R]) {
          Same = false;
          break;
        }
      if (!Same ||
          ((Candidate->ReuseReadsMemory || !Candidate->PreservesMemory) &&
           Candidate->Input.Scratch != Input.Scratch))
        continue;
      ++CacheHits;
      ++EquivalentSpecializedReuses;
      *Generalization = CallXInputGeneralization::EquivalentSpecializedInputs;
      return Candidate;
    }
  if (!Generic && SpecializationBudgetExhausted)
    return Generalized(CallXInputGeneralization::ResourceBudget);
  if (!Generic && Specializations.contains(Entry) &&
      Specializations.at(Entry).size() ==
          kCallXContextSpecializationsPerEntry) {
    // A loop widening can supersede its earlier precise input. Previous
    // roots may also leave unused variants or orphan parents. Preserve
    // unrelated alternatives within this root; reclaim older graphs only
    // when a new specialization actually needs their space.
    const auto FindVictim = [&]() -> FunctionContext * {
      if (ActiveEntries.contains(Program.Med.Blocks[Entry].StartSlot))
        return nullptr;
      for (auto *Candidate : Specializations.at(Entry)) {
        if (!Candidate->isUnreferencedSolverNode())
          continue;
        if (Candidate->RootGeneration < RootGeneration)
          return Candidate;
        ContextState Widened = Candidate->Input;
        Widened.join(Input, Program);
        if (Widened.Registers == Input.Registers &&
            Widened.Scratch == Input.Scratch)
          return Candidate;
      }
      return nullptr;
    };
    auto Victim = FindVictim();
    if (!Victim) {
      reclaimOrphanedContexts(Entry);
      Victim = FindVictim();
    }
    if (Specializations.at(Entry).size() ==
        kCallXContextSpecializationsPerEntry) {
      if (Victim)
        reclaimContext(*Victim);
      else {
        auto Reclaimed = generalizeRedundantPublishedContext(Entry, Input);
        if (!Reclaimed)
          return Reclaimed.takeError();
        if (!*Reclaimed)
          return Generalized(CallXInputGeneralization::SpecializationLimit);
      }
    }
    // Removing a FoldingSet node invalidates the earlier insertion position.
    Cache.FindNodeOrInsertPos(Key, InsertPosition);
  }
  const uint64_t ContextBytes = sizeof(FunctionContext) + stateBytes(Input);
  // Stable IDs retain vector slots and reclamation/graph traversal metadata
  // even after an unreferenced solver-only node has been reclaimed.
  constexpr uint64_t StableIDBytes = 3 * sizeof(size_t);
  const uint64_t VariantIndexBytes =
      !Generic && !Specializations.contains(Entry)
          ? sizeof(decltype(Specializations)::value_type) + 4 * sizeof(void *)
          : 0;
  if (auto Error = retain(ContextBytes + StableIDBytes + VariantIndexBytes))
    return std::move(Error);
  auto Owned =
      std::make_unique<FunctionContext>(Contexts.size(), Entry, Input, Generic);
  Owned->RootGeneration = RootGeneration;
  FunctionContext &Context = *Owned;
  Context.RetainedBytes = ContextBytes;
  Cache.InsertNode(&Context, InsertPosition);
  Contexts.push_back(std::move(Owned));
  if (Generic)
    GenericContexts.emplace(Entry, &Context);
  else
    Specializations[Entry].push_back(&Context);
  const size_t EntrySlot = Program.Med.Blocks[Entry].StartSlot;
  ActiveEntries.insert(EntrySlot);
  const auto Leave = llvm::scope_exit([&] { ActiveEntries.erase(EntrySlot); });

  std::map<size_t, BlockState> States;
  std::deque<size_t> Worklist;
  uint64_t SolverBytes = 0;
  const auto Release = llvm::scope_exit([&] { RetainedBytes -= SolverBytes; });
  const auto StateCharge = [](const ContextState &State) {
    return stateBytes(State) + sizeof(decltype(States)::value_type) +
           6 * sizeof(void *);
  };
  const auto Enqueue = [&](size_t BlockID, const ContextState &Incoming,
                           uint16_t Defined) -> llvm::Error {
    const auto ProjectInputs = [&](ContextState &State) {
      const uint16_t Inputs =
          InputRegisters[BlockID] | registerBit(kFramePointerRegister);
      for (unsigned R = 0; R < kRegisterCount; ++R)
        if (!(Inputs & registerBit(R))) {
          State.Registers[R] = {};
          State.Origins[R] = {};
          State.InputExpressions[R].reset();
        }
      if (!ReadsMemory[BlockID])
        State.InputWrites = {};
    };
    auto It = States.find(BlockID);
    if (It == States.end()) {
      ContextState Input = Incoming;
      ProjectInputs(Input);
      const uint64_t Bytes = StateCharge(Input);
      if (auto Error = retain(Bytes))
        return Error;
      SolverBytes += Bytes;
      States.emplace(BlockID, BlockState{std::move(Input), true, Defined});
      Worklist.push_back(BlockID);
      return llvm::Error::success();
    }
    ContextState Merged = It->second.Entry;
    Merged.join(Incoming, Program);
    ProjectInputs(Merged);
    const uint16_t MergedDefinitions = It->second.DefinitelyWritten & Defined;
    if (Merged.Registers == It->second.Entry.Registers &&
        Merged.Scratch == It->second.Entry.Scratch &&
        Merged.Origins == It->second.Entry.Origins &&
        Merged.OriginScratch == It->second.Entry.OriginScratch &&
        Merged.Postconditions == It->second.Entry.Postconditions &&
        Merged.InputExpressions == It->second.Entry.InputExpressions &&
        Merged.InputSpills == It->second.Entry.InputSpills &&
        Merged.InputWrites == It->second.Entry.InputWrites &&
        MergedDefinitions == It->second.DefinitelyWritten)
      return llvm::Error::success();
    const uint64_t OldCharge = StateCharge(It->second.Entry);
    const uint64_t NewCharge = StateCharge(Merged);
    if (NewCharge > OldCharge) {
      if (auto Error = retain(NewCharge - OldCharge))
        return Error;
      SolverBytes += NewCharge - OldCharge;
    } else {
      RetainedBytes -= OldCharge - NewCharge;
      SolverBytes -= OldCharge - NewCharge;
    }
    It->second.Entry = std::move(Merged);
    It->second.DefinitelyWritten = MergedDefinitions;
    if (!It->second.Queued) {
      Worklist.push_back(BlockID);
      It->second.Queued = true;
    }
    return llvm::Error::success();
  };
  if (auto Error = Enqueue(Entry, Input, 0))
    return std::move(Error);
  while (!Worklist.empty()) {
    const size_t BlockID = Worklist.front();
    Worklist.pop_front();
    BlockState &Block = States.at(BlockID);
    Block.Queued = false;
    ContextState State = Block.Entry;
    uint16_t Defined = Block.DefinitelyWritten;
    auto Continues = replay(Context, BlockID, State, Defined, false);
    if (!Continues)
      return Continues.takeError();
    if (*Continues) {
      const auto &MedBlock = Program.Med.Blocks[BlockID];
      const auto Selected =
          knownBranchSuccessor(Index.find(MedBlock.EndSlot - 1), State);
      for (size_t E = Offsets[BlockID]; E < Offsets[BlockID + 1]; ++E) {
        if (Selected &&
            Program.Med.Blocks[Successors[E]].StartSlot != *Selected) {
          ++SkippedConstantBranchSuccessors;
          continue;
        }
        if (auto Error = Enqueue(Successors[E], State, Defined))
          return std::move(Error);
      }
    }
  }
  // Publish no provisional call or target: replay only the fixed-point entry
  // states. Cache nodes created while converging are not automatically roots.
  for (const auto &[BlockID, Block] : States) {
    ContextState State = Block.Entry;
    uint16_t Defined = Block.DefinitelyWritten;
    auto Continues = replay(Context, BlockID, State, Defined, true);
    if (!Continues)
      return Continues.takeError();
    if (!*Continues)
      continue;
    const MedBlock &MedBlock = Program.Med.Blocks[BlockID];
    const MedInstruction *Last = Index.find(MedBlock.EndSlot - 1);
    if (!Last || Last->Op != Operation::Exit)
      continue;
    if (!Context.Return) {
      Context.Return = std::move(State);
    } else {
      Context.Return->join(State, Program);
    }
  }
  if (Context.Return) {
    if (auto Error = retain(stateBytes(*Context.Return)))
      return std::move(Error);
    Context.RetainedBytes += stateBytes(*Context.Return);
  }
  if (auto Error =
          retain(Context.Effects.Writes.capacity() * sizeof(MemoryWrite)))
    return std::move(Error);
  Context.RetainedBytes +=
      Context.Effects.Writes.capacity() * sizeof(MemoryWrite);
  Context.WritesOnlyOwnAndChildFrames =
      !Context.Effects.Unknown && Context.Effects.Writes.empty();
  Context.PreservesMemory =
      Context.WritesOnlyOwnAndChildFrames && !Context.Effects.WritesFrames;
  Context.Complete = true;
  return &Context;
}

llvm::Error ContextFlow::collectEntryPrefixes(CallXContextReport &Report) {
  std::map<size_t, size_t> Prefixes;
  for (const MedInstruction &Instruction : Program.Med.Instructions) {
    if (Instruction.Call != CallKind::Internal || !Instruction.CallTarget)
      continue;
    const auto It = ByEntry.find(*Instruction.CallTarget);
    if (It == ByEntry.end())
      continue;
    const auto *Last = Index.find(Program.Med.Blocks[It->second].EndSlot - 1);
    if (Last && Last->Op == Operation::CallX &&
        Last->Call == CallKind::Indirect)
      Prefixes.emplace(*Instruction.CallTarget, It->second);
  }
  Report.EntryPrefixCallXCount = Prefixes.size();
  if (Prefixes.empty())
    return llvm::Error::success();
  // Preserve the independently caller-seeded prefix proofs when unrelated
  // return paths consume a whole-function summary's variant capacity.
  const ScratchFlow Flow(Program, Index, ScratchLoadPolicy::ProvenValues);
  Report.PrefixScratchPrecision = Flow.statistics().Precision;
  const uint64_t FlowBytes = Flow.statistics().PeakRetainedByteEstimate;
  if (auto Error = retain(FlowBytes))
    return Error;
  const auto Release = llvm::scope_exit([&] { RetainedBytes -= FlowBytes; });
  for (const MedBlock &Block : Program.Med.Blocks) {
    ContextState Caller;
    const RegisterState *Registers = Flow.entryRegisters(Block.ID);
    for (unsigned R = 0; R < kRegisterCount; ++R)
      Caller.Registers[R] = Registers ? (*Registers)[R] : Block.Inputs[R];
    Caller.Scratch = Flow.entryState(Block.ID);
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
      const MedInstruction *Instruction = Index.find(Slot);
      if (!Instruction)
        continue;
      if (auto Error = visit())
        return Error;
      if (Instruction->Call == CallKind::Internal && Instruction->CallTarget &&
          Prefixes.contains(*Instruction->CallTarget)) {
        const size_t Entry = Prefixes.at(*Instruction->CallTarget);
        ContextState Callee = Caller;
        pushAbstractFrame(Callee, Program);
        FunctionContext Context(0, Entry, Callee, false);
        MissingRegisters Missing;
        for (unsigned R = 0; R < kRegisterCount; ++R)
          if (Callee.Registers[R].unknown()) {
            Missing[R].Kind = CallXMissingFactKind::IncomingRegister;
            Missing[R].InputRegister = R;
          }
        const MedBlock &Prefix = Program.Med.Blocks[Entry];
        CallXContextEvidence Evidence;
        for (size_t S = Prefix.StartSlot; S < Prefix.EndSlot; ++S) {
          const auto *Current = Index.find(S);
          if (!Current)
            continue;
          if (auto Error = visit())
            return Error;
          if (S == Prefix.EndSlot - 1)
            Evidence = evidence(Context, *Current, Callee, Missing, false);
          else
            transfer(*Current, Callee, &Missing);
        }
        Evidence.EntryPrefixOnly = true;
        Evidence.CallerSlot = Slot;
        if (Instruction->Dispatch ==
                CallDispatchPolicy::LegacyRuntimeThenFunction &&
            Program.isSyscallRegistered(
                static_cast<uint32_t>(Instruction->Immediate))) {
          Evidence.Result = CallXContextResult::LegacyRuntimeMayMutateArguments;
          Evidence.TargetAddress.reset();
          Evidence.TargetSlot.reset();
          Evidence.TargetValueAlternatives = {};
          Evidence.Missing = {};
        }
        if (Report.Contexts.size() + Report.UnresolvedCallXSlots.size() ==
            kCallXContextOutputElementBudget)
          return llvm::make_error<llvm::StringError>(
              kCallXContextOutputBudgetDiagnostic,
              llvm::inconvertibleErrorCode());
        if (auto Error = retain(sizeof(Evidence)))
          return Error;
        Report.Contexts.push_back(std::move(Evidence));
      }
      transfer(*Instruction, Caller, nullptr);
    }
  }
  return llvm::Error::success();
}

llvm::Expected<CallXContextReport> ContextFlow::run() {
  CallXContextReport Report;
  std::set<size_t> Roots;
  std::vector<bool> HasPredecessor(Program.Med.Blocks.size());
  for (size_t Successor : Successors)
    HasPredecessor[Successor] = true;
  for (const Function &Function : Program.High.Functions)
    Roots.insert(Function.EntrySlot);
  Roots.insert(Program.Low.EntrySlot);
  for (const MedBlock &Block : Program.Med.Blocks)
    if (!HasPredecessor[Block.ID])
      Roots.insert(Block.StartSlot);
  for (const MedInstruction &Instruction : Program.Med.Instructions) {
    if (Instruction.Call == CallKind::Internal && Instruction.CallTarget) {
      Roots.insert(*Instruction.CallTarget);
    }
    if (Instruction.Op == Operation::CallX &&
        Instruction.Call == CallKind::Indirect) {
      if (Report.UnresolvedCallXSlots.size() ==
          kCallXContextOutputElementBudget)
        return llvm::make_error<llvm::StringError>(
            kCallXContextOutputBudgetDiagnostic,
            llvm::inconvertibleErrorCode());
      Report.UnresolvedCallXSlots.push_back(Instruction.Slot);
    }
  }
  if (Report.UnresolvedCallXSlots.empty())
    return Report;
  if (auto Error = computeInputDependencies())
    return std::move(Error);
  if (auto Error = collectEntryPrefixes(Report))
    return std::move(Error);
  // Build sound all-unknown-input summaries first. They are reusable across
  // arbitrary caller facts and remain available if specialization widens.
  for (size_t Slot : Roots) {
    const auto It = ByEntry.find(Slot);
    if (It == ByEntry.end())
      continue;
    auto Root = summarize(It->second, ContextState{}, true);
    if (!Root)
      return Root.takeError();
    publishRoot(**Root);
  }
  // Preserve entry order: eagerly specializing small unknown-input wrappers
  // can pin all variants before their concrete callers are analyzed.
  for (size_t Slot : Roots) {
    const auto It = ByEntry.find(Slot);
    if (It == ByEntry.end())
      continue;
    if (!ReachesCallX[It->second]) {
      // Generic roots remain published. A root with no statically reachable
      // unresolved CALLX cannot add site evidence; any caller can still demand
      // its specialized return/memory summary through summarize().
      ++SkippedNonCallXRoots;
      continue;
    }
    ++RootGeneration;
    ContextState Input;
    for (unsigned R = 0; R < kRegisterCount; ++R)
      Input.Registers[R] = Program.Med.Blocks[It->second].Inputs[R];
    auto Root = summarize(It->second, Input);
    if (!Root) {
      if (!SpecializationBudgetExhausted)
        return Root.takeError();
      llvm::consumeError(Root.takeError());
      continue;
    }
    publishRoot(**Root);
  }

  const size_t Unreached = std::numeric_limits<size_t>::max();
  std::vector<size_t> Depth(Contexts.size(), Unreached);
  std::deque<size_t> Worklist;
  for (const auto &Context : Contexts)
    if (Context && Context->IsRoot) {
      Depth[Context->ID] = 0;
      Worklist.push_back(Context->ID);
    }
  while (!Worklist.empty()) {
    const size_t ID = Worklist.front();
    Worklist.pop_front();
    for (const auto &Call : Contexts[ID]->Calls)
      if (Call.CalleeSummaryID &&
          Depth[*Call.CalleeSummaryID] > Depth[ID] + 1) {
        Depth[*Call.CalleeSummaryID] = Depth[ID] + 1;
        Worklist.push_back(*Call.CalleeSummaryID);
      }
  }
  size_t InputExpressionProofElements = 0;
  size_t ReturnInputElements = 0;
  size_t MemoryGuardElements = InputMemoryGuards.size();
  for (const auto &Guard : InputMemoryGuards)
    MemoryGuardElements += Guard.Writes.size();
  const auto AddSites = [&](const FunctionContext &Context,
                            const ContextCall *Edge) {
    size_t Instantiated = 0;
    for (auto Site : Context.Sites) {
      if (Edge && Instantiated < Edge->InputExpressionTargets.size() &&
          Edge->InputExpressionTargets[Instantiated].CallXSlot ==
              Site.CallXSlot)
        Site = Edge->InputExpressionTargets[Instantiated++];
      Site.IsRootContext = !Edge;
      if (Edge) {
        Site.CallerSlot = Edge->CallerSlot;
        Site.CallerSummaryID = Edge->CallerSummaryID;
        Site.InputGeneralization = Edge->InputGeneralization;
        if (Edge->LegacyRuntime) {
          Site.Result = CallXContextResult::LegacyRuntimeMayMutateArguments;
          Site.TargetAddress.reset();
          Site.TargetSlot.reset();
          Site.TargetValueAlternatives = {};
          Site.Missing = {};
        }
      }
      InputExpressionProofElements += Site.InputExpressionValues.size();
      Report.Contexts.push_back(std::move(Site));
    }
  };
  for (const auto &Context : Contexts) {
    if (!Context)
      continue;
    if (Depth[Context->ID] == Unreached)
      continue;
    CallXFunctionSummary Summary;
    Summary.ID = Context->ID;
    Summary.EntrySlot = Program.Med.Blocks[Context->EntryBlock].StartSlot;
    Summary.InputRegisterMask = InputRegisters[Context->EntryBlock];
    Summary.ReadsInputMemory = ReadsMemory[Context->EntryBlock];
    Summary.ReuseInputRegisterMask = Context->ReuseInputRegisters;
    Summary.ReuseReadsInputMemory = Context->ReuseReadsMemory;
    for (unsigned R = 0; R < kRegisterCount; ++R) {
      Summary.Inputs[R] = Context->Input.Registers[R].singleton();
      if (Context->Input.Registers[R].values().size() > 1)
        Summary.InputAlternatives.emplace_back(R, Context->Input.Registers[R]);
    }
    Summary.MayReturn = Context->Return.has_value();
    if (Context->Return) {
      Summary.ReturnValue = Context->Return->Registers[0].singleton();
      Summary.ReturnAlternatives = Context->Return->Registers[0];
      Summary.ReturnValueExpression = Context->Return->InputExpressions[0];
      const auto Origin = Context->Return->Origins[0].singleton();
      if (Origin.ValueKind == RegisterValue::Kind::StackAddress)
        Summary.ReturnInputExpression = std::make_pair(
            static_cast<unsigned>(Origin.Value - 1), Origin.Offset);
      if (Context->Return->Origins[0].values().size() > 1)
        for (const auto &Value : Context->Return->Origins[0].values()) {
          CallXInputExpression Expression;
          if (Value.ValueKind == RegisterValue::Kind::Constant) {
            Expression.Kind = CallXInputExpressionKind::Constant;
            Expression.Constant = Value;
          } else if (Value.ValueKind == RegisterValue::Kind::StackAddress) {
            Expression.Register = Value.Value - 1;
            Expression.Offset = Value.Offset;
          } else {
            Summary.ReturnInputAlternatives.clear();
            break;
          }
          Summary.ReturnInputAlternatives.push_back(Expression);
        }
      Summary.OutputScratchWords = Context->Return->Scratch.Values.size();
      Summary.OutputParameterizedWords = Context->Return->Postconditions.size();
    }
    Summary.InputScratchWords = Context->Input.Scratch.Values.size();
    Summary.IsRoot = Context->IsRoot;
    Summary.GeneralizedInputs = Context->Generic;
    Summary.PreservesCallerMemory = Context->PreservesMemory;
    Summary.WritesOnlyOwnAndChildFrames = Context->WritesOnlyOwnAndChildFrames;
    Summary.MayWriteUnknownMemory = Context->Effects.Unknown;
    Summary.MayWriteCalleeFrames = Context->Effects.WritesFrames;
    for (const auto &Write : Context->Effects.Writes) {
      CallXMemoryWrite Effect;
      Effect.Size = Write.Size;
      Effect.Address = Write.Address.Value;
      Effect.Offset = Write.Address.Offset;
      if (Write.Address.ValueKind == RegisterValue::Kind::Constant)
        Effect.Base = CallXMemoryWriteBase::Absolute;
      else if (Write.Address.Value == kRegisterCount)
        Effect.Base = CallXMemoryWriteBase::FramePointer;
      else {
        Effect.Base = CallXMemoryWriteBase::InputRegister;
        Effect.Register = Write.Address.Value - 1;
      }
      Summary.MemoryWrites.push_back(Effect);
    }
    Summary.MinimumDepth = Depth[Context->ID];
    ReturnInputElements += Summary.ReturnInputAlternatives.size();
    // Charge the added public metadata while the private summaries still live.
    if (auto Error = retain(sizeof(Summary.ReturnValueExpression) +
                            sizeof(Summary.ReturnInputAlternatives) +
                            Summary.ReturnInputAlternatives.capacity() *
                                sizeof(CallXInputExpression)))
      return std::move(Error);
    Report.Summaries.push_back(std::move(Summary));
    if (Context->IsRoot)
      AddSites(*Context, nullptr);
    for (const auto &Edge : Context->Calls) {
      Report.SummaryEdges.push_back(Edge);
      if (Edge.CalleeSummaryID)
        AddSites(*Contexts[*Edge.CalleeSummaryID], &Edge);
    }
    if (Report.UnresolvedCallXSlots.size() + Report.Contexts.size() +
            Report.Summaries.size() + Report.SummaryEdges.size() +
            InputExpressions.size() + InputExpressionProofElements +
            ReturnInputElements + MemoryGuardElements >
        kCallXContextOutputElementBudget)
      return llvm::make_error<llvm::StringError>(
          kCallXContextOutputBudgetDiagnostic, llvm::inconvertibleErrorCode());
  }
  Report.InstructionVisits = Visits;
  Report.SummaryCacheHits = CacheHits;
  Report.PeakRetainedBytes = PeakRetainedBytes;
  Report.SpecializationBudgetExhausted = SpecializationBudgetExhausted;
  Report.ProjectedRegisters = ProjectedRegisters;
  Report.ProjectedScratchSnapshots = ProjectedScratchSnapshots;
  Report.InputDependencyVisits = DependencyVisits;
  Report.SummaryCacheEvictions = CacheEvictions;
  Report.GeneralizedPublishedSummaries = GeneralizedPublishedSummaries;
  Report.EquivalentGenericReuses = EquivalentGenericReuses;
  Report.EquivalentSpecializedReuses = EquivalentSpecializedReuses;
  Report.SkippedNonCallXRootSpecializations = SkippedNonCallXRoots;
  Report.SkippedConstantBranchSuccessors = SkippedConstantBranchSuccessors;
  Report.InputExpressions = std::move(InputExpressions);
  for (const auto &Guard : InputMemoryGuards) {
    CallXInputMemoryGuard Output;
    Output.WritesCalleeFrames = Guard.WritesFrames;
    for (const auto &Write : Guard.Writes) {
      CallXMemoryWrite Effect;
      Effect.Size = Write.Size;
      Effect.Address = Write.Address.Value;
      Effect.Offset = Write.Address.Offset;
      if (Write.Address.ValueKind == RegisterValue::Kind::Constant)
        Effect.Base = CallXMemoryWriteBase::Absolute;
      else if (Write.Address.Value == kRegisterCount)
        Effect.Base = CallXMemoryWriteBase::FramePointer;
      else {
        Effect.Base = CallXMemoryWriteBase::InputRegister;
        Effect.Register = Write.Address.Value - 1;
      }
      Output.Writes.push_back(Effect);
    }
    Report.InputMemoryGuards.push_back(std::move(Output));
  }
  Report.InstantiatedTargetInputExpressions =
      InstantiatedTargetInputExpressions;
  return Report;
}

} // namespace

llvm::Expected<CallXContextReport>
recoverCallXContexts(const SBFProgram &Program) {
  return ContextFlow(Program).run();
}

} // namespace neverd::sbf
