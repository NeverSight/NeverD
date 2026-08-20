//===- SymExplore.cpp - Walking every path a function can take ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the depth-first walk described in SymExplore.h.
///
/// A path is a state, a list of what has been assumed, and where to carry on
/// from; forking one is copying it.  The walk keeps them on a stack and takes
/// the most recent, so it follows one route to its end before starting the
/// next — which is what makes the first result the deepest rather than the
/// shallowest, and keeps the number of live states proportional to the depth
/// of the graph instead of its width.
///
/// Two things bend that order deliberately.  A concrete shadow replaces the
/// fork at a branch with a decision, so there is one route and no stack to
/// speak of.  And path merging pauses a route at a join instead of running it
/// through, because two routes that never coexist can never be found to be the
/// same route.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymExplore.h"

#include "neverd/symbolic/SymExec.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace neverd::symbolic {

const char *pathOutcomeName(PathOutcome Outcome) {
  switch (Outcome) {
  case PathOutcome::Returned:
    return "returned";
  case PathOutcome::LeftFunction:
    return "left_function";
  case PathOutcome::UnresolvedBranch:
    return "unresolved_branch";
  case PathOutcome::InvalidControlTarget:
    return "invalid_control_target";
  case PathOutcome::LoopBudget:
    return "loop_budget";
  case PathOutcome::StepBudget:
    return "step_budget";
  case PathOutcome::Infeasible:
    return "infeasible";
  }
  llvm_unreachable("unhandled symbolic path outcome");
}

SymRef SymPath::predicate(SymContext &Ctx) const {
  if (Constraints.empty())
    return Ctx.mkTrue();
  return Ctx.mkAnd(Constraints);
}

namespace {

/// A path still being walked.  Forking one is copying it.
struct Frontier {
  Frontier(SymState State, int BlockId)
      : State(std::move(State)), BlockId(BlockId) {}

  SymState State;
  std::vector<SymRef> Constraints;
  std::vector<int> Blocks;
  std::vector<SymCallResult> CallResults;
  /// How often this path has entered each block, which is what the loop bound
  /// is counted against.  Per path, because two paths through a loop have
  /// nothing to say about each other.
  llvm::DenseMap<int, unsigned> Visits;
  int BlockId;
  /// Operation to execute next when a guest instruction contains an internal
  /// predicate guard.  Zero means the ordinary entry of BlockId.  ARM lowers a
  /// predicated control transfer as `COND_BR next, !predicate` followed by the
  /// transfer's effects in the same instruction boundary; the false side must
  /// therefore resume here instead of pretending it reached another CFG block.
  size_t NextOp = 0;
  /// Decode mode required by an ordinary fallthrough edge.  Direct targets are
  /// checked while their address is resolved; successor IDs have no address
  /// lookup, so their preserved mode is checked when this frontier enters the
  /// destination block.  Legacy boundary-free edges leave this absent.
  std::optional<InstructionMode> IncomingMode;
  bool Infeasible = false;
  /// Set while this path is being taken up again after having been paused at
  /// a join, so that it executes that block instead of pausing there forever.
  bool Resumed = false;
  unsigned Steps = 0;
  unsigned UnmodelledOps = 0;
  unsigned OpaqueOps = 0;
  unsigned CallHavocs = 0;
  unsigned MemoryHavocs = 0;
  unsigned ConcreteBranches = 0;
  unsigned MergedPaths = 0;
};

bool changesControlFlow(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::BRANCH:
  case NdOp::INDIR_BR:
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
  case NdOp::RETURN:
    return true;
  default:
    return false;
  }
}

const LowInstructionBoundary *instructionBoundaryForOp(const LowBlock &Block,
                                                       size_t OpIndex) {
  auto It = std::upper_bound(
      Block.InstructionBoundaries.begin(), Block.InstructionBoundaries.end(),
      OpIndex, [](size_t Index, const LowInstructionBoundary &Boundary) {
        return Index < Boundary.FirstOp;
      });
  if (It == Block.InstructionBoundaries.begin())
    return nullptr;
  --It;
  const uint64_t First = It->FirstOp;
  if (First > Block.Ops.size() || It->OpCount > Block.Ops.size() - First)
    return nullptr;
  const uint64_t End = First + It->OpCount;
  return OpIndex >= First && OpIndex < End ? &*It : nullptr;
}

/// Return the first operation after a predicate guard when the guard and its
/// observable effects are two parts of one decoded instruction.
///
/// InstructionGuard is authoritative when validated boundary metadata is
/// available.  The address-based fallback is only for boundary-free hand-built
/// LowIR and remains control-only: two controls must share a non-zero source
/// address, which ordinary block-ending conditional branches do not satisfy.
std::optional<size_t> predicatedInstructionContinuation(const LowBlock &Block,
                                                        size_t GuardIndex) {
  if (GuardIndex >= Block.Ops.size() ||
      Block.Ops[GuardIndex].Opcode != NdOp::COND_BR)
    return std::nullopt;

  size_t End = GuardIndex + 1;
  bool IsConditionalInstruction = false;
  if (!Block.InstructionBoundaries.empty()) {
    const LowInstructionBoundary *Boundary =
        instructionBoundaryForOp(Block, GuardIndex);
    if (!Boundary || (Boundary->Mode != InstructionMode::ARM &&
                      Boundary->Mode != InstructionMode::Thumb))
      return std::nullopt;
    const bool IsTaggedGuard = hasLowInstructionControlFlag(
        Boundary->ControlFlags, LowInstructionControlFlag::InstructionGuard);
    if (!IsTaggedGuard)
      return std::nullopt;
    End = static_cast<size_t>(Boundary->FirstOp + Boundary->OpCount);
    return GuardIndex + 1 < End ? std::optional<size_t>(GuardIndex + 1)
                                : std::nullopt;
  } else {
    const va_t Address = Block.Ops[GuardIndex].Addr;
    if (Address == 0)
      return std::nullopt;
    while (End < Block.Ops.size() && Block.Ops[End].Addr == Address)
      ++End;
    IsConditionalInstruction = End > GuardIndex + 1;
  }

  if (!IsConditionalInstruction || End > Block.Ops.size())
    return std::nullopt;
  for (size_t I = GuardIndex + 1; I < End; ++I)
    if (changesControlFlow(Block.Ops[I].Opcode))
      return GuardIndex + 1;
  return std::nullopt;
}

/// Add one side of a branch and decide every contradiction the expression
/// builders can prove on their own.  This deliberately stays solver-free: a
/// non-constant conjunction is unknown, never guessed to be impossible.
bool addConstraint(SymContext &Ctx, Frontier &F, SymRef Condition) {
  F.Constraints.push_back(Condition);
  SymRef Predicate =
      F.Constraints.size() == 1 ? Condition : Ctx.mkAnd(F.Constraints);
  std::optional<llvm::APInt> Decided = Ctx.asConst(Predicate);
  return !Decided || !Decided->isZero();
}

/// Take the larger of each count, so a path standing for two is bounded by
/// whichever of them was closest to its loop bound.
void mergeVisits(llvm::DenseMap<int, unsigned> &Into,
                 const llvm::DenseMap<int, unsigned> &From) {
  for (const auto &Entry : From) {
    unsigned &Count = Into[Entry.first];
    Count = std::max(Count, Entry.second);
  }
}

/// What two paths that met again jointly assume.
///
/// They agree on everything up to the branch where they parted, so the answer
/// is that prefix and one disjunction of the rest.  Two arms of a single
/// choice therefore come back as the prefix alone, which is the whole point:
/// the choice is no longer being recorded as though it mattered.
std::vector<SymRef> unionOfConstraints(SymContext &Ctx,
                                       const std::vector<SymRef> &A,
                                       const std::vector<SymRef> &B) {
  size_t Common = 0;
  while (Common < A.size() && Common < B.size() && A[Common] == B[Common])
    ++Common;

  std::vector<SymRef> Merged;
  Merged.reserve(Common + 1);
  for (size_t I = 0; I < Common; ++I)
    Merged.push_back(A[I]);
  // One of them assumed nothing the other did not, so the disjunction is
  // exactly what the shared prefix already says.
  if (Common == A.size() || Common == B.size())
    return Merged;

  auto rest = [&](const std::vector<SymRef> &Of) {
    llvm::SmallVector<SymRef, 8> Tail;
    for (size_t I = Common; I < Of.size(); ++I)
      Tail.push_back(Of[I]);
    return Tail.size() == 1 ? Tail.front() : Ctx.mkAnd(Tail);
  };
  Merged.push_back(Ctx.mkOr(rest(A), rest(B)));
  return Merged;
}

/// Everything the walk needs to know about the shape of the function, worked
/// out once.
class BlockIndex {
public:
  explicit BlockIndex(const LowFunc &Func) : Func(Func) {
    const size_t Count = Func.Blocks.size();
    MetadataValid.assign(Count, true);
    for (size_t I = 0; I < Count; ++I) {
      if (!Func.Blocks[I].InstructionBoundaries.empty()) {
        if (llvm::Error Error = validateLowInstructionBoundaries(
                Func.Blocks[I], LowInstructionBoundaryRequirement::Required)) {
          llvm::consumeError(std::move(Error));
          MetadataValid[I] = false;
        }
      }
      const uint64_t Address = Func.Blocks[I].StartAddr;
      auto It = ByAddress.find(Address);
      if (It == ByAddress.end())
        ByAddress.insert({Address, static_cast<int>(I)});
      else
        // LowFunc does not yet key block identity by decode mode.  Executing
        // an arbitrary one of two same-address variants would be unsound.
        It->second = -1;
    }

    ArrivingEdges.assign(Count, 0u);
    for (const LowBlock &B : Func.Blocks)
      for (int S : B.Succs)
        if (S >= 0 && static_cast<size_t>(S) < Count)
          ++ArrivingEdges[static_cast<size_t>(S)];

    findLoopHeaders();
  }

  const LowBlock *block(int Id) const {
    return Id >= 0 && Id < static_cast<int>(Func.Blocks.size())
               ? &Func.Blocks[Id]
               : nullptr;
  }

  /// The block starting at \p Addr in the requested mode, or -1 when the
  /// function has none.  Legacy blocks without boundaries remain address-only
  /// so hand-built LowIR keeps working.
  int at(uint64_t Addr,
         std::optional<InstructionMode> DestinationMode = std::nullopt) const {
    auto It = ByAddress.find(Addr);
    if (It == ByAddress.end() || It->second < 0)
      return -1;
    const int Id = It->second;
    if (!DestinationMode)
      return Id;

    const LowBlock &Candidate = Func.Blocks[static_cast<size_t>(Id)];
    if (Candidate.InstructionBoundaries.empty())
      return Id;
    const LowInstructionBoundary &Entry =
        Candidate.InstructionBoundaries.front();
    if (Entry.Address != Candidate.StartAddr || Entry.Mode != *DestinationMode)
      return -1;

    // The metadata can reject a wrong-mode block and can route an odd Thumb
    // pointer to an already lifted Thumb block.  CFG construction still owns
    // decoding and currently materializes one block variant per address; this
    // lookup must not be mistaken for mixed-mode recursive descent.
    return Id;
  }

  bool containsAddress(uint64_t Addr) const {
    return ByAddress.find(Addr) != ByAddress.end();
  }

  bool hasValidMetadata(int Id) const {
    return Id >= 0 && static_cast<size_t>(Id) < MetadataValid.size() &&
           MetadataValid[static_cast<size_t>(Id)];
  }

  std::optional<InstructionMode> sourceMode(int Id) const {
    const LowBlock *B = block(Id);
    if (!B || B->InstructionBoundaries.empty())
      return std::nullopt;
    return B->InstructionBoundaries.front().Mode;
  }

  bool modeMatches(int Id, std::optional<InstructionMode> RequiredMode) const {
    if (!RequiredMode)
      return true;
    const LowBlock *B = block(Id);
    return B && (B->InstructionBoundaries.empty() ||
                 B->InstructionBoundaries.front().Mode == *RequiredMode);
  }

  int entry() const { return Func.Blocks.empty() ? -1 : 0; }

  /// The successor that is not \p Taken, which is where a conditional branch
  /// falls through to.  Blocks carry their successors, so the fallthrough does
  /// not have to be guessed from an address.
  int fallthroughOf(const LowBlock &B, int Taken) const {
    for (int S : B.Succs)
      if (S != Taken)
        return S;
    return -1;
  }

  /// A block some edge returns to.  Every cycle in a graph passes through one
  /// of these, so bounding how often a path may re-enter them bounds how long
  /// a path can be — which is what lets every other budget be lifted without
  /// the walk running away.
  bool isLoopHeader(int Id) const {
    return Id >= 0 && static_cast<size_t>(Id) < LoopHeaders.size() &&
           LoopHeaders[static_cast<size_t>(Id)];
  }

  /// A block more than one edge arrives at.  Two paths can only ever meet at
  /// one of these, so they are the only places worth pausing a walk to look.
  bool isJoin(int Id) const {
    return Id >= 0 && static_cast<size_t>(Id) < ArrivingEdges.size() &&
           ArrivingEdges[static_cast<size_t>(Id)] > 1;
  }

private:
  void findLoopHeaders() {
    const size_t Count = Func.Blocks.size();
    LoopHeaders.assign(Count, false);

    // Grey is "on the route currently being walked", so an edge to a grey
    // block is an edge back into that route: a cycle, and the block it lands
    // on is a header.  Iterative because a lifted graph is as deep as the
    // function is long.
    enum class Colour : uint8_t { White, Grey, Black };
    std::vector<Colour> Marks(Count, Colour::White);
    struct Visit {
      size_t Id;
      size_t NextSucc;
    };
    llvm::SmallVector<Visit, 32> Stack;

    for (size_t Root = 0; Root < Count; ++Root) {
      if (Marks[Root] != Colour::White)
        continue;
      Marks[Root] = Colour::Grey;
      Stack.push_back({Root, 0});
      while (!Stack.empty()) {
        Visit &Top = Stack.back();
        const LowBlock &B = Func.Blocks[Top.Id];
        if (Top.NextSucc == B.Succs.size()) {
          Marks[Top.Id] = Colour::Black;
          Stack.pop_back();
          continue;
        }
        const int Succ = B.Succs[Top.NextSucc++];
        if (Succ < 0 || static_cast<size_t>(Succ) >= Count)
          continue;
        const size_t Next = static_cast<size_t>(Succ);
        if (Marks[Next] == Colour::Grey) {
          LoopHeaders[Next] = true;
          continue;
        }
        if (Marks[Next] == Colour::White) {
          Marks[Next] = Colour::Grey;
          Stack.push_back({Next, 0});
        }
      }
    }
  }

  const LowFunc &Func;
  llvm::DenseMap<uint64_t, int> ByAddress;
  std::vector<unsigned> ArrivingEdges;
  std::vector<bool> LoopHeaders;
  std::vector<bool> MetadataValid;
};

/// How often this path may still enter one block.  A loop header answers to
/// whichever of the two bounds is tighter; anything else answers only to the
/// general one.
unsigned visitBudget(const ExploreOptions &Opts, bool LoopHeader) {
  unsigned Allowance = Opts.MaxBlockVisits;
  if (LoopHeader && Opts.MaxLoopIterations != kUnbounded &&
      (Allowance == kUnbounded || Opts.MaxLoopIterations < Allowance))
    Allowance = Opts.MaxLoopIterations;
  return Allowance;
}

struct ControlTargetResolution {
  SymRef Target;
  int BlockId = -1;
  bool IsConcrete = false;
  bool Invalid = false;
  bool InvalidMetadata = false;
  std::optional<InstructionMode> SourceMode;
  std::optional<InstructionMode> DestinationMode;
};

/// Resolve a control expression in the mode contract of the instruction that
/// owns it.  Boundary-free LowIR retains its historical address-only lookup.
ControlTargetResolution resolveControlTarget(
    SymContext &Ctx, const BlockIndex &Index, const LowBlock &Block,
    size_t OpIndex, SymRef Target,
    std::optional<LowInstructionTargetMode> TargetModeOverride = std::nullopt) {
  ControlTargetResolution Result;
  Result.Target = Target;

  const LowInstructionBoundary *Boundary =
      instructionBoundaryForOp(Block, OpIndex);
  if (!Boundary) {
    if (!Block.InstructionBoundaries.empty()) {
      Result.Invalid = true;
      Result.InvalidMetadata = true;
      return Result;
    }
    if (!Target.isValid())
      return Result;
    std::optional<llvm::APInt> Address = Ctx.asConst(Target);
    if (Address && Address->getActiveBits() <= 64) {
      Result.IsConcrete = true;
      Result.BlockId = Index.at(Address->getZExtValue());
    }
    return Result;
  }

  Result.SourceMode = Boundary->Mode;
  const LowInstructionTargetMode TargetMode =
      TargetModeOverride.value_or(Boundary->TargetMode);

  if (Target.isValid()) {
    if (std::optional<llvm::APInt> Address = Ctx.asConst(Target)) {
      Result.IsConcrete = true;
      if (Address->getActiveBits() > 64) {
        Result.Invalid = true;
        return Result;
      }

      llvm::Expected<LowControlTarget> Canonical = canonicalizeLowControlTarget(
          Address->getZExtValue(), Boundary->Mode, TargetMode);
      if (!Canonical) {
        llvm::consumeError(Canonical.takeError());
        const uint64_t ModeProbeAddress =
            TargetMode == LowInstructionTargetMode::FromTargetBit0
                ? Address->getZExtValue() & 1
                : 0;
        llvm::Expected<LowControlTarget> ModeProbe =
            canonicalizeLowControlTarget(ModeProbeAddress, Boundary->Mode,
                                         TargetMode);
        if (ModeProbe)
          Result.DestinationMode = ModeProbe->Mode;
        else {
          llvm::consumeError(ModeProbe.takeError());
          Result.InvalidMetadata = true;
        }
        Result.Invalid = true;
        return Result;
      }

      Result.DestinationMode = Canonical->Mode;
      Result.Target = Ctx.mkConst(Ctx.width(Target), Canonical->Address);
      Result.BlockId = Index.at(Canonical->Address, Canonical->Mode);
      if (Result.BlockId < 0 && Index.containsAddress(Canonical->Address))
        Result.Invalid = true;
      return Result;
    }
  }

  // A symbolic or absent target cannot be canonicalized yet.  Probe with an
  // aligned address to validate its static contract and expose modes that do
  // not depend on target bit zero.
  llvm::Expected<LowControlTarget> ModeProbe =
      canonicalizeLowControlTarget(0, Boundary->Mode, TargetMode);
  if (!ModeProbe) {
    llvm::consumeError(ModeProbe.takeError());
    Result.Invalid = true;
    Result.InvalidMetadata = true;
    return Result;
  }
  if (TargetMode != LowInstructionTargetMode::FromTargetBit0)
    Result.DestinationMode = ModeProbe->Mode;
  return Result;
}

SymPath finish(Frontier &&F, PathOutcome Outcome, SymRef Target = SymRef(),
               std::optional<InstructionMode> SourceMode = std::nullopt,
               std::optional<InstructionMode> DestinationMode = std::nullopt) {
  return SymPath{Outcome,
                 F.BlockId,
                 std::move(F.Constraints),
                 std::move(F.Blocks),
                 std::move(F.State),
                 std::move(F.CallResults),
                 Target,
                 SourceMode,
                 DestinationMode,
                 F.UnmodelledOps,
                 F.OpaqueOps,
                 F.CallHavocs,
                 F.MemoryHavocs,
                 F.ConcreteBranches,
                 F.MergedPaths};
}

} // namespace

SymExploration explorePathsDetailed(SymContext &Ctx, const LowFunc &Func,
                                    const ExploreOptions &Opts) {
  std::vector<SymPath> Finished;
  BlockIndex Index(Func);
  if (Index.entry() < 0)
    return SymExploration{std::move(Finished), true, 0, 0, 0, 0};

  // There is one concrete run, so the shadow can only be following one path.
  // It is consulted until it first fails to answer and never afterwards, which
  // is what makes it safe for the walk to go back to forking from that point:
  // by then nothing will ask the shadow about a path it is not on.
  ConcreteShadow *Shadow = Opts.Concolic;
  bool ShadowTrusted = Shadow != nullptr;
  if (Shadow) {
    Shadow->reset();
    for (const SymConcreteRegister &Seed : Opts.ConcolicSeed)
      Shadow->setRegister(Seed.Offset, Seed.Value);
  }

  llvm::SmallVector<Frontier, 8> Pending;
  // Paths paused at a join, waiting to see whether another one arrives there.
  llvm::SmallVector<Frontier, 8> Waiting;
  Pending.push_back(Frontier(SymState(Ctx, Opts.ByteOrder), Index.entry()));

  unsigned ReachablePaths = 0;
  size_t ExecutedSteps = 0;
  unsigned UnmodelledOps = 0;
  unsigned MergedPaths = 0;
  bool HitIncompleteOutcome = false;
  auto Record = [&](Frontier &&F, PathOutcome Outcome, SymRef Target = SymRef(),
                    std::optional<InstructionMode> SourceMode = std::nullopt,
                    std::optional<InstructionMode> DestinationMode =
                        std::nullopt) {
    ReachablePaths += Outcome != PathOutcome::Infeasible;
    HitIncompleteOutcome |= Outcome == PathOutcome::UnresolvedBranch ||
                            Outcome == PathOutcome::InvalidControlTarget ||
                            Outcome == PathOutcome::LoopBudget ||
                            Outcome == PathOutcome::StepBudget;
    Finished.push_back(
        finish(std::move(F), Outcome, Target, SourceMode, DestinationMode));
  };

  // Pausing a path at a join is what gives another one the chance to arrive
  // there while the first still exists.  Without it the depth-first walk has
  // always run one route to its end before the other has started, and the two
  // never coexist to be recognised as the same.
  auto pauseAtJoin = [&](Frontier &&F) {
    for (Frontier &Waiter : Waiting) {
      if (Waiter.BlockId != F.BlockId || Waiter.NextOp != F.NextOp ||
          !Waiter.State.mergeIdentical(F.State))
        continue;
      Waiter.Constraints =
          unionOfConstraints(Ctx, Waiter.Constraints, F.Constraints);
      mergeVisits(Waiter.Visits, F.Visits);
      Waiter.Steps = std::max(Waiter.Steps, F.Steps);
      Waiter.UnmodelledOps = std::max(Waiter.UnmodelledOps, F.UnmodelledOps);
      Waiter.OpaqueOps = std::max(Waiter.OpaqueOps, F.OpaqueOps);
      Waiter.CallHavocs = std::max(Waiter.CallHavocs, F.CallHavocs);
      Waiter.MemoryHavocs = std::max(Waiter.MemoryHavocs, F.MemoryHavocs);
      // Everything the arriving path already stood for is now stood for here.
      MergedPaths += F.MergedPaths + 1;
      Waiter.MergedPaths += F.MergedPaths + 1;
      return;
    }
    Waiting.push_back(std::move(F));
  };

  while (Opts.MaxPaths == kUnbounded || ReachablePaths < Opts.MaxPaths) {
    if (Pending.empty()) {
      if (Waiting.empty())
        break;
      // Nothing further can arrive at those joins, so let what is there go on.
      for (Frontier &Paused : Waiting) {
        Paused.Resumed = true;
        Pending.push_back(std::move(Paused));
      }
      Waiting.clear();
    }

    Frontier Current = std::move(Pending.back());
    Pending.pop_back();
    if (Current.Infeasible) {
      Record(std::move(Current), PathOutcome::Infeasible);
      continue;
    }

    // Follow this path through as many blocks as it takes, forking only where
    // there is a genuine choice.
    for (;;) {
      const LowBlock *Block = Index.block(Current.BlockId);
      if (!Block) {
        Record(std::move(Current), PathOutcome::LeftFunction);
        break;
      }
      if (!Index.hasValidMetadata(Current.BlockId)) {
        Record(std::move(Current), PathOutcome::InvalidControlTarget, SymRef(),
               Index.sourceMode(Current.BlockId));
        break;
      }
      if (!Index.modeMatches(Current.BlockId, Current.IncomingMode)) {
        const std::optional<InstructionMode> RequiredMode =
            Current.IncomingMode;
        Record(std::move(Current), PathOutcome::InvalidControlTarget, SymRef(),
               RequiredMode, RequiredMode);
        break;
      }
      Current.IncomingMode.reset();
      const bool AtBlockEntry = Current.NextOp == 0;
      if (AtBlockEntry && Opts.MergeEquivalentPaths && !Current.Resumed &&
          Index.isJoin(Current.BlockId)) {
        pauseAtJoin(std::move(Current));
        break;
      }
      Current.Resumed = false;

      if (AtBlockEntry) {
        const unsigned Allowance =
            visitBudget(Opts, Index.isLoopHeader(Current.BlockId));
        const unsigned Visits = ++Current.Visits[Current.BlockId];
        if (Allowance != kUnbounded && Visits > Allowance) {
          Record(std::move(Current), PathOutcome::LoopBudget);
          break;
        }
        Current.Blocks.push_back(Current.BlockId);
      }
      const size_t FirstOp = std::exchange(Current.NextOp, size_t(0));
      if (FirstOp > Block->Ops.size()) {
        Record(std::move(Current), PathOutcome::LeftFunction);
        break;
      }

      SymExec Exec(Ctx, Current.State);
      Exec.setCallPreservedRegisters(Opts.CallPreservedRegisters);
      StepResult Result = StepResult::Continue;
      const LowOp *Terminator = nullptr;
      size_t TerminatorIndex = Block->Ops.size();
      bool OutOfSteps = false;
      bool ReachedNoReturnCall = false;
      for (size_t OpIndex = FirstOp; OpIndex < Block->Ops.size(); ++OpIndex) {
        const LowOp &Op = Block->Ops[OpIndex];
        ++Current.Steps;
        if (Opts.MaxSteps != kUnbounded && Current.Steps > Opts.MaxSteps) {
          OutOfSteps = true;
          break;
        }
        ++ExecutedSteps;
        Result = Exec.step(Op);
        if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
            Opts.TrackedCallResultRegister) {
          auto Snapshot = [&](SymSpace Space, uint64_t Offset, uint16_t Bytes) {
            // The first read may materialise an untouched word as bytes.  Read
            // it again so the snapshot has the same canonical shape as later
            // uses of that register or temporary.
            Current.State.read(Space, Offset, Bytes);
            return Current.State.read(Space, Offset, Bytes);
          };
          SymRef Value;
          if (Op.Output.isReg() && Op.Output.Size)
            Value =
                Snapshot(SymSpace::Register, Op.Output.Offset, Op.Output.Size);
          else if (Op.Output.isTemp() && Op.Output.Size)
            Value =
                Snapshot(SymSpace::Temporary, Op.Output.Offset, Op.Output.Size);
          else if (Opts.TrackedCallResultRegister->Bytes)
            Value = Snapshot(SymSpace::Register,
                             Opts.TrackedCallResultRegister->Offset,
                             Opts.TrackedCallResultRegister->Bytes);
          if (Value.isValid())
            Current.CallResults.push_back(
                {Op.Addr, Current.BlockId, OpIndex, Value});
        }
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
          const LowInstructionBoundary *Boundary =
              instructionBoundaryForOp(*Block, OpIndex);
          if (Boundary && hasLowInstructionControlFlag(
                              Boundary->ControlFlags,
                              LowInstructionControlFlag::NoReturn)) {
            Terminator = &Op;
            TerminatorIndex = OpIndex;
            ReachedNoReturnCall = true;
            break;
          }
        }
        // An operation the engine declined to model still wrote a named
        // unknown to its destination, so the path carries on around it.
        if (Result != StepResult::Continue &&
            Result != StepResult::Unmodelled) {
          Terminator = &Op;
          TerminatorIndex = OpIndex;
          break;
        }
        // A terminator is not handed to the shadow: what the walk wants from
        // it there is the value of an operand, and a concrete engine modelling
        // one straight line has nowhere to go from a branch.
        if (ShadowTrusted)
          ShadowTrusted = Shadow->step(Op);
      }
      Current.UnmodelledOps += Exec.unmodelledCount();
      Current.OpaqueOps += Exec.opaqueOperationCount();
      Current.CallHavocs += Exec.callHavocCount();
      Current.MemoryHavocs += Exec.memoryHavocCount();
      UnmodelledOps += Exec.unmodelledCount();
      if (OutOfSteps) {
        Record(std::move(Current), PathOutcome::StepBudget);
        break;
      }

      if (ReachedNoReturnCall) {
        ControlTargetResolution Resolution = resolveControlTarget(
            Ctx, Index, *Block, TerminatorIndex, Exec.branchTarget());
        Record(std::move(Current),
               Resolution.Invalid ? PathOutcome::InvalidControlTarget
                                  : PathOutcome::LeftFunction,
               Resolution.Target, Resolution.SourceMode,
               Resolution.DestinationMode);
        break;
      }

      if (Result == StepResult::Return) {
        const LowInstructionBoundary *Boundary =
            instructionBoundaryForOp(*Block, TerminatorIndex);
        // AArch32 LowIR makes the architectural return address explicit.  A
        // legacy/default-mode RETURN operand may instead be the semantic value
        // being returned, so only ARM/Thumb boundaries interpret it as a
        // control target.
        const bool HasArchitecturalTarget =
            Boundary && (Boundary->Mode == InstructionMode::ARM ||
                         Boundary->Mode == InstructionMode::Thumb);
        ControlTargetResolution Resolution = resolveControlTarget(
            Ctx, Index, *Block, TerminatorIndex,
            HasArchitecturalTarget ? Exec.branchTarget() : SymRef());
        if (Resolution.Invalid) {
          Record(std::move(Current), PathOutcome::InvalidControlTarget,
                 Resolution.Target, Resolution.SourceMode,
                 Resolution.DestinationMode);
        } else {
          Record(std::move(Current), PathOutcome::Returned, Resolution.Target,
                 Resolution.SourceMode, Resolution.DestinationMode);
        }
        break;
      }

      if (Result == StepResult::Continue || Result == StepResult::Unmodelled) {
        // Ran off the end without a terminator, which the lifters produce for
        // a block that simply falls into the next one.
        int Next = Block->Succs.size() == 1 ? Block->Succs.front() : -1;
        if (Next < 0) {
          Record(std::move(Current), PathOutcome::LeftFunction);
          break;
        }
        Current.IncomingMode = Index.sourceMode(Current.BlockId);
        Current.BlockId = Next;
        continue;
      }

      if (Result == StepResult::Branch ||
          Result == StepResult::IndirectBranch) {
        ControlTargetResolution Resolution = resolveControlTarget(
            Ctx, Index, *Block, TerminatorIndex, Exec.branchTarget());
        if (Resolution.Invalid) {
          Record(std::move(Current), PathOutcome::InvalidControlTarget,
                 Resolution.Target, Resolution.SourceMode,
                 Resolution.DestinationMode);
          break;
        }

        if (Resolution.BlockId < 0 && !Resolution.IsConcrete &&
            Result == StepResult::IndirectBranch && ShadowTrusted &&
            Terminator && Terminator->NumInputs >= 1) {
          // The expression could not say where this goes, but the concrete run
          // went somewhere, and it went there for a reason the path condition
          // already records.
          if (std::optional<uint64_t> Concrete =
                  Shadow->value(Terminator->Inputs[0])) {
            const uint32_t Width = Exec.branchTarget().isValid()
                                       ? Ctx.width(Exec.branchTarget())
                                       : 64;
            Resolution =
                resolveControlTarget(Ctx, Index, *Block, TerminatorIndex,
                                     Ctx.mkConst(Width, *Concrete));
          } else {
            ShadowTrusted = false;
          }
        }
        if (Resolution.Invalid) {
          Record(std::move(Current), PathOutcome::InvalidControlTarget,
                 Resolution.Target, Resolution.SourceMode,
                 Resolution.DestinationMode);
          break;
        }
        if (Resolution.BlockId < 0) {
          Record(std::move(Current),
                 Resolution.IsConcrete ? PathOutcome::LeftFunction
                                       : PathOutcome::UnresolvedBranch,
                 Resolution.Target, Resolution.SourceMode,
                 Resolution.DestinationMode);
          break;
        }
        Current.BlockId = Resolution.BlockId;
        continue;
      }

      // A conditional branch.  Where the predicate already came out constant
      // the code has decided, and only one side exists to walk — which is what
      // silently undoes an opaque predicate, no rule required.
      SymRef Condition = Exec.branchCondition();
      const std::optional<size_t> InstructionContinuation =
          predicatedInstructionContinuation(*Block, TerminatorIndex);
      ControlTargetResolution TakenResolution = resolveControlTarget(
          Ctx, Index, *Block, TerminatorIndex, Exec.branchTarget(),
          InstructionContinuation ? std::optional<LowInstructionTargetMode>(
                                        LowInstructionTargetMode::Preserve)
                                  : std::nullopt);
      if (TakenResolution.InvalidMetadata) {
        Record(std::move(Current), PathOutcome::InvalidControlTarget,
               TakenResolution.Target, TakenResolution.SourceMode,
               TakenResolution.DestinationMode);
        break;
      }
      const int Taken = TakenResolution.BlockId;
      int NotTaken = InstructionContinuation
                         ? Current.BlockId
                         : Index.fallthroughOf(*Block, Taken);
      const PathOutcome MissingTakenOutcome =
          TakenResolution.IsConcrete ? PathOutcome::LeftFunction
                                     : PathOutcome::UnresolvedBranch;

      auto ContinueAt = [&](Frontier &F, bool Taking) {
        F.BlockId = Taking ? Taken : NotTaken;
        F.NextOp = !Taking && InstructionContinuation ? *InstructionContinuation
                                                      : size_t(0);
        F.IncomingMode = !Taking && !InstructionContinuation
                             ? TakenResolution.SourceMode
                             : std::nullopt;
      };

      if (std::optional<llvm::APInt> Decided = Ctx.asConst(Condition)) {
        const bool Taking = !Decided->isZero();
        if (Taking && TakenResolution.Invalid) {
          Record(std::move(Current), PathOutcome::InvalidControlTarget,
                 TakenResolution.Target, TakenResolution.SourceMode,
                 TakenResolution.DestinationMode);
          break;
        }
        int Next = Taking ? Taken : NotTaken;
        if (Next < 0) {
          Record(std::move(Current),
                 Taking ? MissingTakenOutcome : PathOutcome::LeftFunction,
                 Taking ? TakenResolution.Target : SymRef(),
                 TakenResolution.SourceMode,
                 Taking ? TakenResolution.DestinationMode
                        : TakenResolution.SourceMode);
          break;
        }
        ContinueAt(Current, Taking);
        continue;
      }

      // Both sides are live, and a concolic walk does not treat that as a
      // choice: the concrete run went one way, and recording which way and why
      // is what it was asked for.
      std::optional<uint64_t> Decision;
      if (ShadowTrusted && Terminator && Terminator->NumInputs >= 2) {
        Decision = Shadow->value(Terminator->Inputs[1]);
        ShadowTrusted = Decision.has_value();
      }
      if (Decision) {
        const bool Taking = *Decision != 0;
        ++Current.ConcreteBranches;
        if (!addConstraint(Ctx, Current,
                           Taking ? Condition : Ctx.mkNot(Condition))) {
          Record(std::move(Current), PathOutcome::Infeasible);
          break;
        }
        if (Taking && TakenResolution.Invalid) {
          Record(std::move(Current), PathOutcome::InvalidControlTarget,
                 TakenResolution.Target, TakenResolution.SourceMode,
                 TakenResolution.DestinationMode);
          break;
        }
        const int Next = Taking ? Taken : NotTaken;
        if (Next < 0) {
          Record(std::move(Current),
                 Taking ? MissingTakenOutcome : PathOutcome::LeftFunction,
                 Taking ? TakenResolution.Target : SymRef(),
                 TakenResolution.SourceMode,
                 Taking ? TakenResolution.DestinationMode
                        : TakenResolution.SourceMode);
          break;
        }
        ContinueAt(Current, Taking);
        continue;
      }

      // The fallthrough is pushed first so the taken side is walked first,
      // which puts the deeper, more interesting path at the front of the
      // results.
      if (NotTaken >= 0) {
        Frontier Fork = Current;
        if (addConstraint(Ctx, Fork, Ctx.mkNot(Condition)))
          ContinueAt(Fork, false);
        else
          Fork.Infeasible = true;
        Pending.push_back(std::move(Fork));
      }
      if (!addConstraint(Ctx, Current, Condition)) {
        Record(std::move(Current), PathOutcome::Infeasible);
        break;
      }
      if (TakenResolution.Invalid) {
        Record(std::move(Current), PathOutcome::InvalidControlTarget,
               TakenResolution.Target, TakenResolution.SourceMode,
               TakenResolution.DestinationMode);
        break;
      }
      if (Taken < 0) {
        Record(std::move(Current), MissingTakenOutcome, TakenResolution.Target,
               TakenResolution.SourceMode, TakenResolution.DestinationMode);
        break;
      }
      ContinueAt(Current, true);
    }
  }

  auto reachable = [](const Frontier &F) { return !F.Infeasible; };
  const bool HasReachableFrontier =
      llvm::any_of(Pending, reachable) || llvm::any_of(Waiting, reachable);
  return SymExploration{
      std::move(Finished), !HitIncompleteOutcome && !HasReachableFrontier,
      ReachablePaths,      ExecutedSteps,
      UnmodelledOps,       MergedPaths};
}

std::vector<SymPath> explorePaths(SymContext &Ctx, const LowFunc &Func,
                                  const ExploreOptions &Opts) {
  return explorePathsDetailed(Ctx, Func, Opts).Paths;
}

} // namespace neverd::symbolic
