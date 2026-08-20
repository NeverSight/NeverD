//===- HuntEngine.cpp - Dangerous-copy overflow hunt ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/HuntEngine.h"

#include "neverd/safety/ArgSlicer.h"
#include "neverd/safety/ObjectModel.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymExec.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

using namespace neverd;
using namespace neverd::safety;
using namespace neverd::symbolic;
using namespace neverd::solver;

namespace {

void stampSite(Finding &F, const AnalysisInput &In, const MedFunc &Fn,
               const SinkSite &Site) {
  F.Origin = Track::Hunt;
  F.Class = VulnClass::BufferOverflow;
  F.Function = Fn.Name;
  F.FuncEntry = Site.FuncEntry;
  F.Name = Site.Sink;
  F.Source = Site.Source;
  F.CallVA = Site.CallVA;
  F.Sink = Site.Sink;
  F.ArgIndex = Site.ArgIndex;
  if (In.Dbg) {
    if (auto Loc = In.Dbg->sourceLocation(Site.CallVA); Loc && !Loc->File.empty())
      F.SourceLoc = Loc->File + ":" + std::to_string(Loc->Line);
  }
}

solver::SolverOptions solverOpts(const SafetyBudgets &Budgets) {
  solver::SolverOptions Opts;
  if (Budgets.SolverConflicts)
    Opts.Sat.MaxConflicts = Budgets.SolverConflicts;
  return Opts;
}

bool pathFeasible(SymContext &Ctx, const std::vector<SymRef> &Constraints,
                  const SafetyBudgets &Budgets, bool &Unknown) {
  Unknown = false;
  if (Constraints.empty())
    return true;
  SymRef Pred = Constraints.size() == 1 ? Constraints[0] : Ctx.mkAnd(Constraints);
  if (std::optional<llvm::APInt> C = Ctx.asConst(Pred))
    return !C->isZero();
  SatResult R = checkSat(Ctx, Pred, nullptr, solverOpts(Budgets));
  if (R == SatResult::Unsat)
    return false;
  if (R == SatResult::Sat)
    return true;
  Unknown = true;
  return true;
}

std::optional<uint64_t> abstractOverflowLen(uint64_t Capacity,
                                            const SafetyBudgets &Budgets) {
  SymContext Ctx;
  const uint32_t W = 64;
  SymRef N = Ctx.mkVar("copy_len", W);
  SymRef Cap = Ctx.mkConst(W, Capacity);
  SymRef Over = Ctx.mkUgt(N, Cap);
  SymRef Prop = Over;
  if (Capacity <= UINT64_MAX - 1) {
    SymRef Small = Ctx.mkUle(N, Ctx.mkConst(W, Capacity + 1));
    SymRef Conj[] = {Over, Small};
    Prop = Ctx.mkAnd(Conj);
  }
  BitVectorModel Model;
  SatResult R = checkSat(Ctx, Prop, &Model, solverOpts(Budgets));
  if (R != SatResult::Sat)
    return std::nullopt;
  if (auto V = Model.value(Ctx, N))
    return V->getZExtValue();
  return Capacity + 1;
}

void addWitness(Finding &F, const SinkEntry &E, uint64_t Length,
                const std::string &TaintSource) {
  F.Witness.push_back({"copy_length", std::to_string(Length)});
  const bool ImplicitLen = E.LenArg < 0 && E.SrcArg >= 0;
  std::string InputName =
      !TaintSource.empty()
          ? (TaintSource == "argv" ? "argv[1]" : TaintSource)
          : (ImplicitLen ? "source" : "length");
  if (ImplicitLen || !TaintSource.empty())
    F.Witness.push_back({InputName, std::to_string(Length) + " bytes"});
}

const LowBlock *blockById(const LowFunc &LF, int Id) {
  for (const LowBlock &B : LF.Blocks)
    if (B.Id == Id)
      return &B;
  return nullptr;
}

int blockIdAt(const LowFunc &LF, va_t Addr) {
  if (!Addr)
    return -1;
  for (const LowBlock &B : LF.Blocks)
    for (const LowOp &Op : B.Ops)
      if (Op.Addr == Addr)
        return B.Id;
  for (const LowBlock &B : LF.Blocks)
    if (Addr >= B.StartAddr && Addr < B.EndAddr)
      return B.Id;
  for (const LowBlock &B : LF.Blocks)
    if (B.StartAddr == Addr)
      return B.Id;
  return -1;
}

const MedOp *medOpAt(const MedFunc &F, int BlockId, int OpIdx) {
  for (const MedBlock &B : F.Blocks)
    if (B.Id == BlockId)
      return (OpIdx >= 0 && OpIdx < static_cast<int>(B.Ops.size()))
                 ? &B.Ops[OpIdx]
                 : nullptr;
  return nullptr;
}

const MedCallInfo *medCallAt(const MedFunc &F, va_t Addr) {
  if (!Addr)
    return nullptr;
  for (const MedCallInfo &CI : F.CallInfos)
    if (const MedOp *Op = medOpAt(F, CI.BlockId, CI.OpIdx); Op && Op->Addr == Addr)
      return &CI;
  return nullptr;
}

bool isStringLengthCall(llvm::StringRef Name) {
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define SAFETY_CALL_TRAIT(NAME, IS_LENGTH, IS_BOUNDED)                         \
  .Case(NAME, IS_LENGTH != 0)
#include "neverd/safety/SafetyCallTraits.inc"
#undef SAFETY_CALL_TRAIT
      .Default(false);
}

SymRef captureReturn(const LowOp &Op, SymState &State,
                     const AnalysisInput &In) {
  if (Op.Output.isReg() && Op.Output.Size)
    return State.read(SymSpace::Register, Op.Output.Offset, Op.Output.Size);
  if (Op.Output.isTemp() && Op.Output.Size)
    return State.read(SymSpace::Temporary, Op.Output.Offset, Op.Output.Size);
  if (!In.Img)
    return {};
  const TargetRegInfo &TRI = getTargetRegInfo(In.Img->Arch);
  const uint16_t Bytes = TRI.PointerSize ? TRI.PointerSize : 8;
  return State.read(SymSpace::Register, TRI.IntReturnReg, Bytes);
}

llvm::DenseSet<int> reverseReachable(const LowFunc &LF, int SinkId) {
  llvm::DenseMap<int, llvm::SmallVector<int, 4>> Pred;
  llvm::DenseSet<int> Ids;
  for (const LowBlock &B : LF.Blocks) {
    Ids.insert(B.Id);
    for (int S : B.Succs)
      Pred[S].push_back(B.Id);
    for (int P : B.Preds)
      Pred[B.Id].push_back(P);
  }
  llvm::DenseSet<int> Reach;
  std::deque<int> Work;
  if (SinkId >= 0) {
    Reach.insert(SinkId);
    Work.push_back(SinkId);
  }
  while (!Work.empty()) {
    int Cur = Work.front();
    Work.pop_front();
    auto It = Pred.find(Cur);
    if (It == Pred.end())
      continue;
    for (int P : It->second)
      if (Ids.count(P) && Reach.insert(P).second)
        Work.push_back(P);
  }
  return Reach;
}

int takenSucc(const LowFunc &LF, const LowBlock &B, const LowOp &Br,
              SymContext &Ctx, SymExec &Exec) {
  if (Br.NumInputs < 1)
    return B.Succs.empty() ? -1 : B.Succs.front();
  SymRef T = Exec.branchTarget();
  if (!T.isValid() && Br.Inputs[0].isConst())
    T = Ctx.mkConst(64, Br.Inputs[0].Offset);
  if (std::optional<llvm::APInt> Addr = Ctx.asConst(T)) {
    int Id = blockIdAt(LF, Addr->getZExtValue());
    if (Id >= 0)
      return Id;
  }
  return B.Succs.empty() ? -1 : B.Succs.front();
}

int otherSucc(const LowBlock &B, int Taken) {
  for (int S : B.Succs)
    if (S != Taken)
      return S;
  return -1;
}

SymRef readLength(const AnalysisInput &In, const MedFunc &Fn,
                  const SinkEntry &E, const MedCallInfo &CI, SymState &State) {
  SymContext &Ctx = State.context();
  auto fromVar = [&](const MedVar &V) -> SymRef {
    if (V.isConst())
      return Ctx.mkConst(64, V.ConstVal);
    if (V.Kind == MedVar::Reg && V.Size > 0)
      return State.read(SymSpace::Register, V.RegOff, V.Size);
    return SymRef();
  };

  if (E.LenArg >= 0 && E.LenArg < static_cast<int>(CI.Args.size())) {
    if (SymRef R = fromVar(CI.Args[E.LenArg]); R.isValid())
      return Ctx.mkZExtOrTrunc(R, 64);
  }

  if (!In.Img)
    return SymRef();
  const TargetRegInfo &TRI = getTargetRegInfo(In.Img->Arch);
  llvm::ArrayRef<uint64_t> Params =
      Fn.CC == CallingConv::Win64 ? TRI.Win64ParamRegs : TRI.IntParamRegs;
  const int Idx = E.LenArg >= 0 ? E.LenArg : -1;
  if (Idx < 0 || Idx >= static_cast<int>(Params.size()))
    return SymRef();
  const uint16_t Bytes = TRI.PointerSize ? TRI.PointerSize : 8;
  return Ctx.mkZExtOrTrunc(
      State.read(SymSpace::Register, Params[Idx], Bytes), 64);
}

struct ExploreHit {
  enum Kind { Miss, Overflow, InBound, Unresolved } Kind = Miss;
  uint64_t WitnessLen = 0;
  std::string PredText;
  bool Budget = false;
  bool SolverUnknown = false;
  bool Reached = false;
};

void considerSink(ExploreHit &Best, SymContext &Ctx, SymRef Pred, SymRef Len,
                  uint64_t Capacity, const SafetyBudgets &Budgets) {
  if (!Len.isValid())
    return;

  BitVectorModel Model;
  SymRef WideLen = Ctx.mkZExtOrTrunc(Len, 64);
  SymRef Cap = Ctx.mkConst(64, Capacity);
  SymRef Over = Ctx.mkUgt(WideLen, Cap);
  llvm::SmallVector<SymRef, 4> Parts;
  if (!Ctx.isConstOnes(Pred))
    Parts.push_back(Pred);
  Parts.push_back(Over);
  // A compact witness for an unconstrained length.  Do not add it when a path
  // predicate already forces a larger overflow — the bound would unsat a real
  // defect.
  if (Ctx.isConstOnes(Pred) && Capacity <= UINT64_MAX - 1)
    Parts.push_back(Ctx.mkUle(WideLen, Ctx.mkConst(64, Capacity + 1)));
  SymRef Query = Parts.size() == 1 ? Parts[0] : Ctx.mkAnd(Parts);

  SatResult R = checkSat(Ctx, Query, &Model, solverOpts(Budgets));
  if (R == SatResult::Sat) {
    Best.Kind = ExploreHit::Overflow;
    Best.PredText = Ctx.toString(Pred);
    if (auto C = Ctx.asConst(WideLen))
      Best.WitnessLen = C->getZExtValue();
    else if (auto V = Model.value(Ctx, WideLen))
      Best.WitnessLen = V->getZExtValue();
    else {
      std::vector<llvm::APInt> Vals = Model.asVarValues(Ctx);
      Best.WitnessLen = Ctx.eval(WideLen, Vals).getZExtValue();
    }
    return;
  }
  if (R == SatResult::Unsat) {
    if (Best.Kind == ExploreHit::Miss || Best.Kind == ExploreHit::Unresolved)
      Best.Kind = ExploreHit::InBound;
    return;
  }
  Best.SolverUnknown = true;
  if (Best.Kind == ExploreHit::Miss)
    Best.Kind = ExploreHit::Unresolved;
}

ExploreHit exploreSink(const AnalysisInput &In, const MedFunc &Fn,
                       const SinkSite &Site, const SinkEntry &E,
                       uint64_t Capacity, const SafetyBudgets &Budgets) {
  ExploreHit Best;
  const LowFunc *LF = In.findLowFunc(Fn.Entry);
  if (!LF || LF->Blocks.empty())
    return Best;

  int SinkBlk = Site.CallVA ? blockIdAt(*LF, Site.CallVA) : -1;
  if (SinkBlk < 0 && !LF->Blocks.empty())
    SinkBlk = LF->Blocks.front().Id;
  llvm::DenseSet<int> Reach = reverseReachable(*LF, SinkBlk);
  if (Reach.empty())
    for (const LowBlock &B : LF->Blocks)
      Reach.insert(B.Id);

  const unsigned MaxPaths = Budgets.MaxPaths ? Budgets.MaxPaths : 64;
  const unsigned MaxSteps = Budgets.MaxSteps ? Budgets.MaxSteps : (1u << 16);
  const unsigned MaxLoop = Budgets.MaxLoop ? Budgets.MaxLoop : 3;

  const MedCallInfo *CI =
      Site.CallInfoIndex < Fn.CallInfos.size() ? &Fn.CallInfos[Site.CallInfoIndex]
                                               : nullptr;
  const bool Implicit = E.LenArg < 0;

  struct Frontier {
    SymState State;
    std::vector<SymRef> Constraints;
    int BlockId = -1;
    unsigned Steps = 0;
    llvm::DenseMap<int, unsigned> Visits;
    SymRef ImplicitLen;
    explicit Frontier(SymContext &C) : State(C) {}
  };

  std::vector<SymRegisterRange> Preserved;
  if (In.Img) {
    const TargetRegInfo &TRI = getTargetRegInfo(In.Img->Arch);
    for (const TargetRegisterRange &R : TRI.callPreservedRanges(In.Img->Format))
      Preserved.push_back({R.Offset, R.Bytes});
  }

  SymContext Ctx;
  llvm::SmallVector<Frontier, 8> Pending;
  {
    Frontier Start(Ctx);
    Start.BlockId = LF->Blocks.front().Id;
    Pending.push_back(std::move(Start));
  }

  unsigned Finished = 0;
  while (!Pending.empty() && Finished < MaxPaths) {
    Frontier Cur = std::move(Pending.back());
    Pending.pop_back();
    if (!Reach.count(Cur.BlockId))
      continue;

    const LowBlock *Block = blockById(*LF, Cur.BlockId);
    if (!Block)
      continue;
    if (++Cur.Visits[Cur.BlockId] > MaxLoop) {
      Best.Budget = true;
      continue;
    }

    llvm::SmallVector<Frontier, 2> Forks;
    bool Stopped = false;
    bool HitSink = false;
    {
      SymExec Exec(Ctx, Cur.State);
      Exec.setCallPreservedRegisters(Preserved);
      for (size_t Oi = 0; Oi < Block->Ops.size(); ++Oi) {
        const LowOp &Op = Block->Ops[Oi];
        if (++Cur.Steps > MaxSteps) {
          Best.Budget = true;
          Stopped = true;
          break;
        }

        const bool IsSinkCall =
            (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
            ((Site.CallVA != 0 && Op.Addr == Site.CallVA) ||
             (Site.CallVA == 0 && Cur.BlockId == SinkBlk));
        if (IsSinkCall) {
          bool Unknown = false;
          if (!pathFeasible(Ctx, Cur.Constraints, Budgets, Unknown)) {
            Stopped = true;
            break;
          }
          if (Unknown)
            Best.SolverUnknown = true;
          SymRef Pred = Cur.Constraints.empty()
                            ? Ctx.mkTrue()
                            : (Cur.Constraints.size() == 1
                                   ? Cur.Constraints[0]
                                   : Ctx.mkAnd(Cur.Constraints));
          SymRef Len = CI ? readLength(In, Fn, E, *CI, Cur.State) : SymRef();
          if (!Len.isValid() && Cur.ImplicitLen.isValid()) {
            Len = Ctx.mkZExtOrTrunc(Cur.ImplicitLen, 64);
            if (Implicit)
              Len = Ctx.mkAdd(Len, Ctx.mkConst(64, 1));
          }
          Best.Reached = true;
          considerSink(Best, Ctx, Pred, Len, Capacity, Budgets);
          ++Finished;
          HitSink = true;
          Stopped = true;
          break;
        }

        const MedCallInfo *Callee = medCallAt(Fn, Op.Addr);
        const bool LengthCall =
            Callee && isStringLengthCall(Callee->TargetName);
        StepResult SR = Exec.step(Op);
        if (LengthCall)
          Cur.ImplicitLen = captureReturn(Op, Cur.State, In);
        if (SR == StepResult::Continue || SR == StepResult::Unmodelled)
          continue;
        if (SR == StepResult::Return) {
          Stopped = true;
          ++Finished;
          break;
        }
        if (SR == StepResult::Branch || SR == StepResult::IndirectBranch) {
          int Next = takenSucc(*LF, *Block, Op, Ctx, Exec);
          if (Next >= 0 && Reach.count(Next)) {
            Frontier N = Cur;
            N.BlockId = Next;
            Forks.push_back(std::move(N));
          }
          Stopped = true;
          break;
        }
        if (SR == StepResult::CondBranch) {
          int Taken = takenSucc(*LF, *Block, Op, Ctx, Exec);
          int NotTaken = otherSucc(*Block, Taken);
          SymRef Cond = Exec.branchCondition();
          auto fork = [&](int Next, SymRef Assume) {
            if (Next < 0 || !Reach.count(Next))
              return;
            Frontier F = Cur;
            F.BlockId = Next;
            F.Constraints.push_back(Assume);
            bool Unknown = false;
            if (!pathFeasible(Ctx, F.Constraints, Budgets, Unknown))
              return;
            if (Unknown)
              Best.SolverUnknown = true;
            Forks.push_back(std::move(F));
          };
          fork(Taken, Cond);
          fork(NotTaken, Ctx.mkNot(Cond));
          Stopped = true;
          break;
        }
      }
    }
    for (Frontier &F : Forks)
      Pending.push_back(std::move(F));
    if (Stopped || HitSink)
      continue;
    if (Block->Succs.size() == 1 && Reach.count(Block->Succs.front())) {
      Cur.BlockId = Block->Succs.front();
      Pending.push_back(std::move(Cur));
    } else {
      ++Finished;
    }
  }
  if (!Pending.empty())
    Best.Budget = true;
  return Best;
}

} // namespace

std::optional<Finding> neverd::safety::huntSink(const AnalysisInput &In,
                                                const SinkCatalog &Cat,
                                                const SafetyBudgets &Budgets,
                                                const MedFunc &F,
                                                const SinkSite &Site) {
  const SinkEntry *E = Cat.matchSink(Site.Sink);
  if (!E || E->Kind != SinkKind::Copy)
    return std::nullopt;

  Finding Out;
  stampSite(Out, In, F, Site);

  if (E->CapArg >= 0) {
    Out.TheVerdict = Verdict::Safe;
    Out.TheConfidence = Confidence::High;
    Out.Detail = "fortified copy with a runtime destination bound";
    return Out;
  }

  ArgClassification Arg =
      classifyArgument(In, Cat, F, Site.CallInfoIndex, Site.ArgIndex);
  Out.Flow = Arg.Flow;

  DestObject Dst = resolveDestination(In, Cat, F, Site.CallInfoIndex, E->DstArg);
  if (Dst.Capacity)
    Out.Capacity = Dst.Capacity;

  if (Arg.ConstValue && Dst.Capacity) {
    if (*Arg.ConstValue <= *Dst.Capacity) {
      Out.TheVerdict = Verdict::Safe;
      Out.TheConfidence = Confidence::High;
      Out.Detail = "constant length within destination capacity";
    } else {
      Out.TheVerdict = Verdict::Unsafe;
      Out.TheConfidence = Confidence::High;
      Out.Detail = "constant length exceeds destination capacity";
      Out.Witness.push_back({"copy_length", std::to_string(*Arg.ConstValue)});
      Out.Witness.push_back({"capacity", std::to_string(*Dst.Capacity)});
    }
    return Out;
  }

  if (Arg.Flow == ArgFlow::Bounded) {
    Out.TheVerdict = Verdict::Safe;
    Out.TheConfidence = Confidence::Medium;
    Out.SkipReason = Arg.Reason.empty() ? "bounded length" : Arg.Reason;
    Out.Detail = "retired by the argument prefilter";
    return Out;
  }

  if (!Dst.Capacity) {
    Out.TheVerdict = Verdict::Unknown;
    Out.TheConfidence = Confidence::Low;
    Out.Detail = "destination capacity could not be recovered";
    return Out;
  }

  ExploreHit Hit = exploreSink(In, F, Site, *E, *Dst.Capacity, Budgets);
  if (Hit.Kind == ExploreHit::Overflow) {
    Out.TheVerdict = Verdict::Unsafe;
    Out.TheConfidence = Confidence::High;
    Out.Detail = "copy length can exceed destination capacity on a reachable path";
    Out.Constraints = Hit.PredText;
    addWitness(Out, *E, Hit.WitnessLen, Arg.TaintSource);
    Out.Corroboration = "path predicate and overflow are jointly satisfiable";
    Out.BudgetHit = Hit.Budget;
    return Out;
  }
  if (Hit.Kind == ExploreHit::InBound && !Hit.Budget && !Hit.SolverUnknown) {
    Out.TheVerdict = Verdict::Safe;
    Out.TheConfidence = Confidence::High;
    Out.Detail = "every explored path keeps the copy within capacity";
    Out.Constraints = Hit.PredText;
    return Out;
  }

  if (Arg.Flow == ArgFlow::Tainted) {
    if (auto Len = abstractOverflowLen(*Dst.Capacity, Budgets)) {
      Out.TheVerdict = Verdict::Unsafe;
      Out.TheConfidence = Confidence::High;
      Out.Detail = "attacker-controlled length can exceed destination capacity";
      addWitness(Out, *E, *Len, Arg.TaintSource);
      if (Hit.Budget && !Hit.Reached) {
        Out.BudgetHit = true;
        Out.Corroboration = "sink reachability not established in budget";
      } else if (Hit.Reached) {
        Out.Corroboration = "sink reachable on a symbolic path";
      }
      return Out;
    }
  }

  Out.TheVerdict = Verdict::Unknown;
  Out.TheConfidence = Confidence::Low;
  Out.BudgetHit = Hit.Budget;
  if (Hit.Budget)
    Out.Detail = "exploration budget exhausted";
  else
    Out.Detail = "length provenance unresolved";
  return Out;
}
