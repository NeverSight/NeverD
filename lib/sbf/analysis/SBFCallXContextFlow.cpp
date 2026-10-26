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
#include <utility>
#include <vector>

namespace neverd::sbf {
namespace {

using namespace callx_detail;

using MissingRegisters = std::array<CallXMissingFact, kRegisterCount>;

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
  RegisterState Origins{};
  ScratchState OriginScratch;
  MemoryPostconditions Postconditions;

  ContextState() = default;
  ContextState(const MachineState &State) : Scratch(State.Scratch) {
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

  void join(const ContextState &Other) {
    for (unsigned R = 0; R < kRegisterCount; ++R)
      Registers[R].join(Other.Registers[R]);
    for (unsigned R = 0; R < kRegisterCount; ++R)
      if (Origins[R] != Other.Origins[R])
        Origins[R] = {};
    Scratch.Memory.meet(Other.Scratch.Memory);
    Scratch.Values.joinValues(Other.Scratch.Values);
    OriginScratch.meet(Other.OriginScratch);
    Postconditions.meet(Other.Postconditions);
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

void pushAbstractFrame(ContextState &State, const SBFProgram &Program) {
  const auto Values = State.Registers[kFramePointerRegister].values();
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
  State.Registers[kFramePointerRegister] = Frames.value_or(ProvenValueSet{});
}

uint64_t stateBytes(const ContextState &State) {
  uint64_t Bytes = sizeof(State) + State.Scratch.retainedByteEstimate() +
                   State.OriginScratch.retainedByteEstimate() +
                   State.Postconditions.retainedByteEstimate();
  for (const auto &Value : State.Registers)
    Bytes += Value.allocatedBytes();
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

struct FunctionContext : llvm::FoldingSetNode {
  size_t ID;
  size_t EntryBlock;
  ContextState Input;
  std::optional<ContextState> Return;
  std::vector<CallXContextEvidence> Sites;
  std::vector<CallXSummaryEdge> Calls;
  bool IsRoot = false;
  bool Published = false;
  bool Generic;
  bool Complete = false;
  bool PreservesMemory = true;
  bool WritesOnlyOwnAndChildFrames = true;
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
                              ContextState &State, bool Collect);
  llvm::Expected<bool> call(FunctionContext &Context,
                            const MedInstruction &Instruction,
                            ContextState &State, bool Collect);
  void transfer(const MedInstruction &Instruction, ContextState &State,
                MissingRegisters *Missing);
  void transferValues(const MedInstruction &Instruction, ContextState &State);
  void refineOrigins(const FunctionContext &Context, ContextState &State);
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
  std::vector<bool> ReadsMemory;
  std::vector<bool> ReachesCallX;
  size_t ProjectedRegisters = 0;
  size_t ProjectedScratchSnapshots = 0;
  size_t DependencyVisits = 0;
  size_t CacheEvictions = 0;
  size_t GeneralizedPublishedSummaries = 0;
  size_t EquivalentGenericReuses = 0;
  size_t SkippedNonCallXRoots = 0;
  size_t SkippedConstantBranchSuccessors = 0;
  size_t RootGeneration = 0;
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
    const auto Origin = Context.Return->Origins[0].ValueKind;
    if (Origin != RegisterValue::Kind::Unknown &&
        Origin != RegisterValue::Kind::Constant)
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

  using TargetKey = std::pair<size_t, va_t>;
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
    const auto AddSites =
        [&](const FunctionContext &Context) -> llvm::Expected<bool> {
      for (const auto &Site : Context.Sites) {
        // A union of target alternatives is a different proof from separate
        // singleton contexts; do not discard that distinction here.
        if (Site.TargetValueAlternatives.values().size() > 1)
          return false;
        if (Site.TargetAddress &&
            Targets.emplace(Site.CallXSlot, *Site.TargetAddress).second) {
          if (auto Error = retain(TargetBytes))
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
          auto Added = AddSites(Child);
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
  ReadsMemory.resize(Count);
  ReachesCallX.resize(Count);
  if (auto Error = retain(Count * (sizeof(uint16_t) + 2 * sizeof(bool))))
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
  const auto Bit = [](unsigned R) { return uint16_t{1} << R; };
  const uint16_t All = (uint16_t{1} << kRegisterCount) - 1;
  const uint16_t Clobbered = (uint16_t{1} << kFirstCalleeSavedRegister) - 1;
  const uint16_t Arguments = ((uint16_t{1} << kArgumentRegisterCount) - 1)
                             << kFirstArgumentRegister;
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
      if (I->Semantics.WritesDestination)
        Uses &= ~Bit(I->Dst);
      if (I->Call != CallKind::None) {
        Uses &= ~Clobbered;
        if (I->Call == CallKind::Internal && I->CallTarget &&
            ByEntry.contains(*I->CallTarget) &&
            !(I->Dispatch == CallDispatchPolicy::LegacyRuntimeThenFunction &&
              Program.isSyscallRegistered(
                  static_cast<uint32_t>(I->Immediate)))) {
          const size_t Entry = ByEntry.at(*I->CallTarget);
          Uses |= InputRegisters[Entry];
          Memory |= ReadsMemory[Entry];
          CallX |= ReachesCallX[Entry];
        } else if (I->Call == CallKind::Syscall) {
          // The host receives the five argument carriers, not the VM's
          // return/callee-saved/frame registers. Its memory remains opaque.
          Uses |= Arguments;
          Memory = true;
        } else {
          Uses |= All;
          Memory = true;
          CallX |= I->Call != CallKind::Syscall;
        }
        if (I->Call == CallKind::Indirect)
          Uses |= Bit(I->CallRegister);
        continue;
      }
      if (I->Op == Operation::Exit)
        Uses |= Bit(0);
      Memory |= I->Op == Operation::Load;
      switch (I->Form) {
      case OperandForm::Dst:
      case OperandForm::Endian:
      case OperandForm::BranchImm:
      case OperandForm::StoreImm:
        Uses |= Bit(I->Dst);
        break;
      case OperandForm::DstImm:
        if (I->Op != Operation::Mov)
          Uses |= Bit(I->Dst);
        break;
      case OperandForm::DstSrc:
        if (I->Op != Operation::Mov)
          Uses |= Bit(I->Dst);
        Uses |= Bit(I->Src);
        break;
      case OperandForm::Load:
        Uses |= Bit(I->Src);
        break;
      case OperandForm::StoreReg:
      case OperandForm::BranchReg:
        Uses |= Bit(I->Dst) | Bit(I->Src);
        break;
      default:
        break;
      }
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

void ContextFlow::transferValues(const MedInstruction &Instruction,
                                 ContextState &State) {
  const auto OriginAddress = [&](unsigned Register) {
    RegisterState Entry{};
    Entry[0] = State.Origins[Register];
    return substituteOrigin(
        {RegisterValue::Kind::StackAddress, 1, Instruction.Offset}, Entry,
        Program);
  };
  const auto LoadedOrigin =
      Instruction.Op == Operation::Load &&
              Instruction.Semantics.MemoryWidth == kDoubleWordBitWidth
          ? State.Postconditions.read(OriginAddress(Instruction.Src))
          : RegisterValue{};
  MachineState Scalar = State.scalar();
  MachineState OriginMemory = Scalar;
  OriginMemory.Scratch = State.OriginScratch;
  if (Instruction.Op == Operation::Store) {
    State.Postconditions.store(
        OriginAddress(Instruction.Dst), Instruction.Width / kBitsPerByte,
        Instruction.Form == OperandForm::StoreReg
            ? State.Origins[Instruction.Src]
            : RegisterValue{RegisterValue::Kind::Constant,
                            normalizeImmediate(Instruction.Immediate,
                                               Instruction.Semantics.Immediate),
                            0});
    MedInstruction Store = Instruction;
    if (Store.Form == OperandForm::StoreReg) {
      Store.Src = Store.Dst == 0 ? 1 : 0;
      OriginMemory.Registers[Store.Src] = State.Origins[Instruction.Src];
    }
    applyTransfer(Store, OriginMemory, Program.ExecutableImage,
                  ScratchLoadPolicy::ProvenValues);
    State.OriginScratch = std::move(OriginMemory.Scratch);
  } else if (Instruction.Call != CallKind::None) {
    if (Instruction.Call != CallKind::Syscall || !Instruction.Syscall ||
        !preservesCallerMemory(Instruction.Syscall->ID))
      State.Postconditions.clear();
    applyTransfer(Instruction, OriginMemory, Program.ExecutableImage,
                  ScratchLoadPolicy::ProvenValues);
    State.OriginScratch = std::move(OriginMemory.Scratch);
  }
  applyRegisterTransfer(Instruction, State.Origins, Program.ExecutableImage);
  if (Instruction.Op == Operation::Load) {
    applyRegisterTransfer(Instruction, OriginMemory.Registers,
                          Program.ExecutableImage, &State.OriginScratch.Values);
    const auto Origin = OriginMemory.Registers[Instruction.Dst];
    if (Origin.ValueKind != RegisterValue::Kind::Unknown)
      State.Origins[Instruction.Dst] = Origin;
    else if (LoadedOrigin.ValueKind != RegisterValue::Kind::Unknown)
      State.Origins[Instruction.Dst] = LoadedOrigin;
  }
  if (Instruction.Op == Operation::Store) {
    // Lift the canonical store over the complete base/value alternatives.
    // Its alias invalidation, full-width checks and pointer tags stay owned
    // by applyTransfer, including havoc for any unknown address alternative.
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
    return;
  }
  if (Instruction.Call != CallKind::None) {
    applyTransfer(Instruction, Scalar, Program.ExecutableImage,
                  ScratchLoadPolicy::ProvenValues);
    State.Scratch = std::move(Scalar.Scratch);
    for (unsigned R = 0; R < kFirstCalleeSavedRegister; ++R)
      State.Registers[R] = {};
    return;
  }
  if (!Instruction.Semantics.WritesDestination)
    return;

  std::optional<ProvenValueSet> Output;
  const auto Evaluate = [&](MachineState &Variant) {
    applyRegisterTransfer(Instruction, Variant.Registers,
                          Program.ExecutableImage, &Variant.Scratch.Values);
    const ProvenValueSet Value(Variant.Registers[Instruction.Dst]);
    if (!Output)
      Output = Value;
    else
      Output->join(Value);
  };
  if (Instruction.Op == Operation::Load) {
    for (const auto &Base : possibleValues(State.Registers[Instruction.Src])) {
      const auto Address = effectiveAddress(Base, Instruction.Offset);
      const auto Coordinate =
          Base.ValueKind == RegisterValue::Kind::StackAddress
              ? MemoryCoordinate::FrameRelative
              : MemoryCoordinate::Absolute;
      const ProvenValueSet Stored =
          Address && Instruction.Semantics.MemoryWidth == kDoubleWordBitWidth
              ? Scalar.Scratch.Values.readValues(*Address, Coordinate)
              : ProvenValueSet{};
      for (const auto &Value : possibleValues(Stored)) {
        MachineState Variant = Scalar;
        Variant.Registers[Instruction.Src] = Base;
        if (Address && !Stored.unknown())
          Variant.Scratch.Values.write(*Address, Value, Coordinate);
        Evaluate(Variant);
      }
    }
  } else {
    // Lift the canonical scalar transfer over complete operand alternatives;
    // neither numeric opcode semantics nor affine-address rules live here.
    for (const auto &Source : possibleValues(State.Registers[Instruction.Src]))
      for (const auto &Destination :
           possibleValues(State.Registers[Instruction.Dst])) {
        MachineState Variant = Scalar;
        Variant.Registers[Instruction.Dst] = Destination;
        Variant.Registers[Instruction.Src] = Source;
        Evaluate(Variant);
      }
  }
  State.Registers[Instruction.Dst] = std::move(*Output);
}

void ContextFlow::refineOrigins(const FunctionContext &Context,
                                ContextState &State) {
  const auto Frame = Context.Input.Registers[kFramePointerRegister].singleton();
  RegisterState Entry{};
  for (unsigned R = 0; R < kRegisterCount; ++R)
    Entry[R] = Context.Input.Registers[R].singleton();
  for (unsigned R = 0; R < kRegisterCount; ++R) {
    const auto Value = State.Registers[R].singleton();
    if (Value.ValueKind == RegisterValue::Kind::StackAddress &&
        Frame.ValueKind == RegisterValue::Kind::StackAddress &&
        Value.Value == Frame.Value) {
      int64_t Offset;
      if (!llvm::SubOverflow(Value.Offset, Frame.Offset, Offset))
        State.Origins[R] = {RegisterValue::Kind::StackAddress, kRegisterCount,
                            Offset};
    } else if (State.Origins[R].ValueKind == RegisterValue::Kind::Unknown &&
               Value.ValueKind == RegisterValue::Kind::Constant) {
      State.Origins[R] = Value;
    } else if (State.Registers[R].unknown()) {
      const auto Origin = substituteOrigin(State.Origins[R], Entry, Program);
      if (Origin.ValueKind != RegisterValue::Kind::Unknown)
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
  const RegisterValue Target =
      State.Registers[Instruction.CallRegister].singleton();
  if (Target.ValueKind == RegisterValue::Kind::Constant) {
    Evidence.TargetAddress = Target.Value;
    const uint64_t Slot =
        (Target.Value - Program.Low.TextAddress) / kInstructionSize;
    if (Slot < Program.Low.Instructions.size()) {
      Evidence.Result = CallXContextResult::ProvenTarget;
      Evidence.TargetSlot = static_cast<size_t>(Slot);
      Evidence.TargetIsContinuation =
          Program.Low.Instructions[Slot].IsContinuation;
    } else {
      Evidence.Result = CallXContextResult::InvalidTarget;
    }
  } else {
    Evidence.Missing = Missing[Instruction.CallRegister];
    if (State.Registers[Instruction.CallRegister].values().size() > 1) {
      Evidence.TargetValueAlternatives =
          State.Registers[Instruction.CallRegister];
      Evidence.Missing.Kind = CallXMissingFactKind::MultipleTargetValues;
    } else if (Evidence.Missing.Kind == CallXMissingFactKind::None)
      Evidence.Missing.Kind = CallXMissingFactKind::SymbolicTarget;
  }
  return Evidence;
}

llvm::Expected<bool> ContextFlow::call(FunctionContext &Context,
                                       const MedInstruction &Instruction,
                                       ContextState &State, bool Collect) {
  // Resolved runtime calls keep the one canonical memory-effect owner.
  if (Instruction.Call == CallKind::Syscall) {
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
  CallXSummaryEdge Edge;
  Edge.CallerSummaryID = Context.ID;
  Edge.CallerSlot = Instruction.Slot;
  Edge.CalleeEntrySlot = Instruction.CallTarget;
  if (Instruction.Call == CallKind::Indirect) {
    const RegisterValue Target =
        State.Registers[Instruction.CallRegister].singleton();
    if (Target.ValueKind == RegisterValue::Kind::Constant) {
      const uint64_t Slot =
          (Target.Value - Program.Low.TextAddress) / kInstructionSize;
      if (Slot < Program.Low.Instructions.size())
        Edge.CalleeEntrySlot = static_cast<size_t>(Slot);
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
      for (unsigned R = 0; R < kFirstCalleeSavedRegister; ++R)
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
    if (Callee->Generic)
      Edge.InputGeneralization = Generalization;
  }
  Context.PreservesMemory &=
      Callee && Callee->PreservesMemory && !Edge.LegacyRuntime;
  RegisterState OriginEntry = State.Origins;
  pushAbstractFrame(OriginEntry, Program);
  ContextState ValueEntry;
  ValueEntry.Registers = State.Registers;
  pushAbstractFrame(ValueEntry, Program);
  if (Collect) {
    if (!Callee || Edge.LegacyRuntime)
      Context.Effects.Unknown = true;
    else
      Context.Effects.include(Callee->Effects, OriginEntry, Program);
  }
  // This is the canonical caller ABI: r0-r5 are clobbered, r6-r10 survive.
  // A converged callee supplies only its return carrier and memory effects.
  ScratchState Preserved;
  ScratchState PreservedOrigins = State.OriginScratch;
  const bool PreserveInput =
      Callee && Callee->PreservesMemory && !Edge.LegacyRuntime;
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
      PreservedPostconditions.invalidate(Callee->Effects, OriginEntry, Program);
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
    if (Callee->Generic && !EquivalentLeaf) {
      const auto Origin = Callee->Return->Origins[0];
      std::optional<ProvenValueSet> Instantiated;
      if (!Edge.LegacyRuntime &&
          Origin.ValueKind == RegisterValue::Kind::Constant)
        Instantiated = Origin;
      else if (!Edge.LegacyRuntime &&
               Origin.ValueKind == RegisterValue::Kind::StackAddress)
        for (const auto &Value :
             ValueEntry.Registers[Origin.Value - 1].values()) {
          RegisterState Actual{};
          Actual[Origin.Value - 1] = Value;
          auto Result = substituteOrigin(Origin, Actual, Program);
          if (!Instantiated)
            Instantiated = Result;
          else
            Instantiated->join(Result);
        }
      if (Instantiated && !Instantiated->unknown())
        State.Registers[0] = *Instantiated;
      else
        for (const auto &Value : State.Registers[0].values())
          if (Value.ValueKind == RegisterValue::Kind::StackAddress) {
            State.Registers[0] = {};
            break;
          }
    }
    State.Origins[0] = Edge.LegacyRuntime
                           ? RegisterValue{}
                           : substituteOrigin(Callee->Return->Origins[0],
                                              OriginEntry, Program);
    State.OriginScratch = std::move(PreservedOrigins);
    State.Postconditions = std::move(PreservedPostconditions);
  }
  return true;
}

llvm::Expected<bool> ContextFlow::replay(FunctionContext &Context,
                                         size_t BlockID, ContextState &State,
                                         bool Collect) {
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
      const uint64_t Bytes =
          sizeof(Site) + Site.TargetValueAlternatives.allocatedBytes();
      if (auto Error = retain(Bytes))
        return std::move(Error);
      Context.RetainedBytes += Bytes;
      Context.Sites.push_back(std::move(Site));
    }
    if (Instruction->Call != CallKind::None) {
      auto Continues = call(Context, *Instruction, State, Collect);
      if (!Continues)
        return Continues.takeError();
      if (!*Continues)
        return false;
      if (Collect)
        for (unsigned R = 0; R < kFirstCalleeSavedRegister; ++R) {
          Missing[R] = {};
          if (State.Registers[R].unknown()) {
            Missing[R].Kind = CallXMissingFactKind::FunctionReturn;
            Missing[R].Slot = Slot;
            Missing[R].InputRegister = R;
          }
        }
    } else {
      if (Instruction->Op == Operation::Store)
        Context.PreservesMemory = false;
      if (Collect && Instruction->Op == Operation::Store) {
        RegisterState Base{};
        Base[0] = State.Origins[Instruction->Dst];
        const auto Address = substituteOrigin(
            {RegisterValue::Kind::StackAddress, 1, Instruction->Offset}, Base,
            Program);
        Context.Effects.addWrite(Address, Instruction->Width / kBitsPerByte,
                                 Program);
      }
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
  Input.Origins = symbolicInputs();
  Input.OriginScratch.clear();
  Input.Postconditions.clear();
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
        Widened.join(Input);
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
  const auto Enqueue = [&](size_t BlockID,
                           const ContextState &Incoming) -> llvm::Error {
    auto It = States.find(BlockID);
    if (It == States.end()) {
      const uint64_t Bytes = StateCharge(Incoming);
      if (auto Error = retain(Bytes))
        return Error;
      SolverBytes += Bytes;
      States.emplace(BlockID, BlockState{Incoming, true});
      Worklist.push_back(BlockID);
      return llvm::Error::success();
    }
    ContextState Merged = It->second.Entry;
    Merged.join(Incoming);
    if (Merged.Registers == It->second.Entry.Registers &&
        Merged.Scratch == It->second.Entry.Scratch &&
        Merged.Origins == It->second.Entry.Origins &&
        Merged.OriginScratch == It->second.Entry.OriginScratch &&
        Merged.Postconditions == It->second.Entry.Postconditions)
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
    if (!It->second.Queued) {
      Worklist.push_back(BlockID);
      It->second.Queued = true;
    }
    return llvm::Error::success();
  };
  if (auto Error = Enqueue(Entry, Input))
    return std::move(Error);
  while (!Worklist.empty()) {
    const size_t BlockID = Worklist.front();
    Worklist.pop_front();
    BlockState &Block = States.at(BlockID);
    Block.Queued = false;
    ContextState State = Block.Entry;
    auto Continues = replay(Context, BlockID, State, false);
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
        if (auto Error = Enqueue(Successors[E], State))
          return std::move(Error);
      }
    }
  }
  // Publish no provisional call or target: replay only the fixed-point entry
  // states. Cache nodes created while converging are not automatically roots.
  for (const auto &[BlockID, Block] : States) {
    ContextState State = Block.Entry;
    auto Continues = replay(Context, BlockID, State, true);
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
      Context.Return->join(State);
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
  const auto AddSites = [&](const FunctionContext &Context,
                            const CallXSummaryEdge *Edge) {
    for (auto Site : Context.Sites) {
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
    for (unsigned R = 0; R < kRegisterCount; ++R) {
      Summary.Inputs[R] = Context->Input.Registers[R].singleton();
      if (Context->Input.Registers[R].values().size() > 1)
        Summary.InputAlternatives.emplace_back(R, Context->Input.Registers[R]);
    }
    Summary.MayReturn = Context->Return.has_value();
    if (Context->Return) {
      Summary.ReturnValue = Context->Return->Registers[0].singleton();
      Summary.ReturnAlternatives = Context->Return->Registers[0];
      const auto &Origin = Context->Return->Origins[0];
      if (Origin.ValueKind == RegisterValue::Kind::StackAddress)
        Summary.ReturnInputExpression = std::make_pair(
            static_cast<unsigned>(Origin.Value - 1), Origin.Offset);
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
    Report.Summaries.push_back(std::move(Summary));
    if (Context->IsRoot)
      AddSites(*Context, nullptr);
    for (const auto &Edge : Context->Calls) {
      Report.SummaryEdges.push_back(Edge);
      if (Edge.CalleeSummaryID)
        AddSites(*Contexts[*Edge.CalleeSummaryID], &Edge);
    }
    if (Report.UnresolvedCallXSlots.size() + Report.Contexts.size() +
            Report.Summaries.size() + Report.SummaryEdges.size() >
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
  Report.SkippedNonCallXRootSpecializations = SkippedNonCallXRoots;
  Report.SkippedConstantBranchSuccessors = SkippedConstantBranchSuccessors;
  return Report;
}

} // namespace

llvm::Expected<CallXContextReport>
recoverCallXContexts(const SBFProgram &Program) {
  return ContextFlow(Program).run();
}

} // namespace neverd::sbf
