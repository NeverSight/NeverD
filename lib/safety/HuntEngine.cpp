//===- HuntEngine.cpp - Dangerous-copy overflow hunt ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/HuntEngine.h"

#include "SourceSemantics.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/safety/ArgSlicer.h"
#include "neverd/safety/ObjectModel.h"
#include "neverd/safety/SinkScanner.h"
#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymExec.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>

using namespace neverd;
using namespace neverd::safety;
using namespace neverd::symbolic;
using namespace neverd::solver;

namespace {

void stampSite(Finding &F, const AnalysisInput &In, const MedFunc &Fn,
               const SinkSite &Site) {
  F.Origin = Track::Hunt;
  F.Class = Site.Class;
  F.Function = Fn.Name;
  F.FuncEntry = Site.FuncEntry;
  F.Name = Site.Sink;
  F.Source = Site.Source;
  F.CallVA = Site.CallVA;
  F.Sink = Site.Sink;
  F.ArgIndex = Site.ArgIndex;
  if (In.Dbg) {
    if (auto Loc = In.Dbg->sourceLocation(Site.CallVA);
        Loc && !Loc->File.empty())
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
  SymRef Pred =
      Constraints.size() == 1 ? Constraints[0] : Ctx.mkAnd(Constraints);
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
      !TaintSource.empty() ? (TaintSource == "argv" ? "argv[1]" : TaintSource)
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
    if (const MedOp *Op = medOpAt(F, CI.BlockId, CI.OpIdx);
        Op && Op->Addr == Addr)
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

bool usesWideElements(llvm::StringRef Name) {
  return llvm::StringSwitch<bool>(SinkCatalog::normalize(Name))
      .Cases({"wmemcpy", "wmemmove", "wcscpy", "wcscat"}, true)
      .Default(false);
}

bool requiresStringExtents(llvm::StringRef Name) {
  return llvm::StringSwitch<bool>(SinkCatalog::normalize(Name))
      .Cases({"strcat", "strncat", "strlcat", "strlcpy"}, true)
      .Cases({"strcat_chk", "strncat_chk"}, true)
      .Default(false);
}

std::optional<uint64_t> literalFormattedBytes(llvm::StringRef Format) {
  uint64_t Bytes = 1; // terminating NUL
  for (size_t I = 0; I < Format.size(); ++I) {
    if (Format[I] != '%') {
      ++Bytes;
      continue;
    }
    if (++I >= Format.size() || Format[I] != '%')
      return std::nullopt;
    ++Bytes;
  }
  return Bytes;
}

struct CountedSourceReturn {
  int BoundArg = -1;
  bool AllowsMinusOne = false;
  uint32_t ReturnBits = 0;
  int RequiredZeroArg = -1;
};

struct CountedSourceOutput {
  int BoundArg = -1;
  int CountPointerArg = -1;
  uint32_t CountBits = 0;
  uint32_t BoundBits = 0;
};

struct BoundedStringOutput {
  int BufferArg = -1;
  int BoundArg = -1;
};

CountedSourceReturn countedSourceReturn(llvm::StringRef Name,
                                        BinaryFormat Format) {
  const std::string Normalized = SinkCatalog::normalize(Name);
  if (Normalized == "read" || Normalized == "pread")
    return {2, true,
            Normalized == "read" && Format == BinaryFormat::COFF ? uint32_t(32)
                                                                 : uint32_t(0)};
  if (Normalized == "fread")
    return {2, false, 0};
  if (Normalized == "recv" || Normalized == "recvfrom")
    return {2, true, Format == BinaryFormat::COFF ? uint32_t(32) : uint32_t(0),
            3};
  return {};
}

CountedSourceOutput countedSourceOutput(llvm::StringRef Name,
                                        BinaryFormat Format) {
  if (Format == BinaryFormat::COFF &&
      SinkCatalog::normalize(Name) == "ReadFile")
    return {2, 3, 32, 32};
  return {};
}

BoundedStringOutput boundedStringOutput(llvm::StringRef Name) {
  if (SinkCatalog::normalize(Name) == "fgets")
    return {0, 1};
  return {};
}

bool mayWriteMemory(NdOp Opcode) {
  return Opcode == NdOp::STORE || Opcode == NdOp::ATOMIC_XCHG ||
         Opcode == NdOp::ATOMIC_ADD || Opcode == NdOp::ATOMIC_CMPXCHG;
}

SymRef readLowValue(SymContext &Ctx, SymState &State, const NdVar &Value) {
  const uint16_t Bytes = Value.Size ? Value.Size : uint16_t(8);
  if (Value.isConst())
    return Ctx.mkConst(uint32_t(Bytes) * 8, Value.Offset);
  if (Value.isRam())
    return State.load(Ctx.mkConst(64, Value.Offset), Bytes);
  return State.read(Value.isTemp() ? SymSpace::Temporary : SymSpace::Register,
                    Value.Offset, Bytes);
}

bool writeIsProvablyBeforeSource(SymContext &Ctx, SymState &State,
                                 const LowOp &Op, SymRef Source,
                                 const std::vector<SymRef> &Constraints,
                                 const SafetyBudgets &Budgets) {
  if (!Source.isValid() || Op.NumInputs < 2)
    return false;
  const SymRef Address =
      Ctx.mkZExtOrTrunc(readLowValue(Ctx, State, Op.Inputs[0]), 64);
  const uint64_t Bytes = Op.Inputs[1].Size ? Op.Inputs[1].Size : 1;
  const SymRef NoOverflow =
      Ctx.mkUle(Address, Ctx.mkConst(64, UINT64_MAX - Bytes));
  const SymRef End = Ctx.mkAdd(Address, Ctx.mkConst(64, Bytes));
  const SymRef Before = Ctx.mkUle(End, Ctx.mkZExtOrTrunc(Source, 64));
  std::vector<SymRef> Counterexample = Constraints;
  Counterexample.push_back(Ctx.mkNot(Ctx.mkAnd(NoOverflow, Before)));
  bool Unknown = false;
  return !pathFeasible(Ctx, Counterexample, Budgets, Unknown) && !Unknown;
}

bool expressionsMustEqual(SymContext &Ctx, SymRef Left, SymRef Right,
                          const std::vector<SymRef> &Constraints,
                          const SafetyBudgets &Budgets) {
  if (!Left.isValid() || !Right.isValid())
    return false;
  Left = Ctx.mkZExtOrTrunc(Left, 64);
  Right = Ctx.mkZExtOrTrunc(Right, 64);
  if (Left == Right)
    return true;
  std::vector<SymRef> Counterexample = Constraints;
  Counterexample.push_back(Ctx.mkNe(Left, Right));
  bool Unknown = false;
  return !pathFeasible(Ctx, Counterexample, Budgets, Unknown) && !Unknown;
}

bool predicateMustHold(SymContext &Ctx, SymRef Predicate,
                       const std::vector<SymRef> &Constraints,
                       const SafetyBudgets &Budgets) {
  if (!Predicate.isValid())
    return true;
  std::vector<SymRef> Counterexample = Constraints;
  Counterexample.push_back(Ctx.mkNot(Predicate));
  bool Unknown = false;
  return !pathFeasible(Ctx, Counterexample, Budgets, Unknown) && !Unknown;
}

bool valuesShareUniqueLoadOrigin(const SymState &State, SymRef Left,
                                 SymRef Right) {
  if (!Left.isValid() || !Right.isValid())
    return false;
  const SymState::LoadOrigin *LeftOrigin = State.loadOrigin(Left);
  const SymState::LoadOrigin *RightOrigin = State.loadOrigin(Right);
  return LeftOrigin && RightOrigin && *LeftOrigin == *RightOrigin;
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

std::optional<int> takenSucc(const LowFunc &LF, const LowBlock &B,
                             const LowOp &Br, SymContext &Ctx, SymExec &Exec) {
  if (Br.NumInputs < 1)
    return std::nullopt;
  SymRef T = Exec.branchTarget();
  if (!T.isValid() && Br.Inputs[0].isConst())
    T = Ctx.mkConst(64, Br.Inputs[0].Offset);
  if (std::optional<llvm::APInt> Addr = Ctx.asConst(T)) {
    int Id = blockIdAt(LF, Addr->getZExtValue());
    if (Id >= 0 && B.hasSucc(Id))
      return Id;
  }
  return std::nullopt;
}

std::optional<int> entryBlockId(const LowFunc &LF) {
  for (const LowBlock &B : LF.Blocks)
    if (B.StartAddr == LF.Entry)
      return B.Id;
  for (const LowBlock &B : LF.Blocks)
    if (LF.Entry >= B.StartAddr && LF.Entry < B.EndAddr)
      return B.Id;

  llvm::DenseSet<int> HasIncoming;
  for (const LowBlock &B : LF.Blocks) {
    if (!B.Preds.empty())
      HasIncoming.insert(B.Id);
    for (int Succ : B.Succs)
      HasIncoming.insert(Succ);
  }
  std::optional<int> Entry;
  for (const LowBlock &B : LF.Blocks) {
    if (HasIncoming.count(B.Id))
      continue;
    if (Entry)
      return std::nullopt;
    Entry = B.Id;
  }
  return Entry;
}

int otherSucc(const LowBlock &B, int Taken) {
  for (int S : B.Succs)
    if (S != Taken)
      return S;
  return -1;
}

SymRef readCallArgument(const AnalysisInput &In, const MedFunc &Fn,
                        int ArgIndex, const MedCallInfo &CI, SymState &State) {
  SymContext &Ctx = State.context();
  auto fromVar = [&](const MedVar &V) -> SymRef {
    if (V.isConst())
      return Ctx.mkConst(64, V.ConstVal);
    if (V.Kind == MedVar::Reg && V.Size > 0)
      return State.read(SymSpace::Register, V.RegOff, V.Size);
    if (V.Kind == MedVar::Param)
      return Ctx.mkVar("param$" + std::to_string(V.Id),
                       uint32_t(V.Size ? V.Size : uint16_t(8)) * 8);
    return SymRef();
  };

  if (ArgIndex >= 0 && ArgIndex < static_cast<int>(CI.Args.size())) {
    if (SymRef R = fromVar(CI.Args[ArgIndex]); R.isValid())
      return Ctx.mkZExtOrTrunc(R, 64);
  }

  if (!In.Img)
    return SymRef();
  const TargetRegInfo &TRI = getTargetRegInfo(In.Img->Arch);
  llvm::ArrayRef<uint64_t> Params =
      Fn.CC == CallingConv::Win64 ? TRI.Win64ParamRegs : TRI.IntParamRegs;
  if (ArgIndex < 0 || ArgIndex >= static_cast<int>(Params.size()))
    return SymRef();
  const uint16_t Bytes = TRI.PointerSize ? TRI.PointerSize : 8;
  return Ctx.mkZExtOrTrunc(
      State.read(SymSpace::Register, Params[ArgIndex], Bytes), 64);
}

struct ExploreHit {
  enum Kind { Miss, Overflow, InBound, Unresolved } Kind = Miss;
  uint64_t WitnessLen = 0;
  std::string PredText;
  bool Budget = false;
  bool SolverUnknown = false;
  bool SemanticUnknown = false;
  bool MissingLength = false;
  bool Reached = false;
  bool SourceEvidence = false;
};

void considerSink(ExploreHit &Best, SymContext &Ctx, SymRef Pred, SymRef Len,
                  SymRef RuntimeCap, uint64_t Capacity,
                  const SafetyBudgets &Budgets) {
  if (!Len.isValid()) {
    Best.MissingLength = true;
    if (Best.Kind == ExploreHit::Miss)
      Best.Kind = ExploreHit::Unresolved;
    return;
  }

  BitVectorModel Model;
  SymRef WideLen = Ctx.mkZExtOrTrunc(Len, 64);
  SymRef Cap = Ctx.mkConst(64, Capacity);
  SymRef Over = Ctx.mkUgt(WideLen, Cap);
  llvm::SmallVector<SymRef, 4> Parts;
  if (!Ctx.isConstOnes(Pred))
    Parts.push_back(Pred);
  Parts.push_back(Over);
  if (RuntimeCap.isValid())
    Parts.push_back(Ctx.mkUle(WideLen, Ctx.mkZExtOrTrunc(RuntimeCap, 64)));
  // A compact witness for an unconstrained length.  Do not add it when a path
  // predicate already forces a larger overflow — the bound would unsat a real
  // defect.
  if (Ctx.isConstOnes(Pred) && !Ctx.isConst(WideLen) &&
      Capacity <= UINT64_MAX - 1)
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

ExploreHit exploreSink(const AnalysisInput &In, const SinkCatalog &Cat,
                       const MedFunc &Fn, const SinkSite &Site,
                       const SinkEntry &E, uint64_t Capacity,
                       const SafetyBudgets &Budgets,
                       llvm::StringRef RequiredSource,
                       std::optional<uint64_t> FixedLength = std::nullopt) {
  ExploreHit Best;
  const LowFunc *LF = In.findLowFunc(Fn.Entry);
  if (!LF || LF->Blocks.empty())
    return Best;

  if (!LF->hasCompleteInstructionLift()) {
    Best.SemanticUnknown = true;
    return Best;
  }

  if (Site.CallVA == 0) {
    Best.SemanticUnknown = true;
    return Best;
  }
  int SinkBlk = blockIdAt(*LF, Site.CallVA);
  if (SinkBlk < 0) {
    Best.SemanticUnknown = true;
    return Best;
  }
  llvm::DenseSet<int> Reach = reverseReachable(*LF, SinkBlk);
  if (Reach.empty())
    for (const LowBlock &B : LF->Blocks)
      Reach.insert(B.Id);

  const unsigned MaxPaths = Budgets.MaxPaths ? Budgets.MaxPaths : 64;
  const unsigned MaxSteps = Budgets.MaxSteps ? Budgets.MaxSteps : (1u << 16);
  const unsigned MaxLoop = Budgets.MaxLoop ? Budgets.MaxLoop : 3;

  const MedCallInfo *CI = Site.CallInfoIndex < Fn.CallInfos.size()
                              ? &Fn.CallInfos[Site.CallInfoIndex]
                              : nullptr;
  const bool Implicit = E.LenArg < 0;

  struct Frontier {
    SymState State;
    std::vector<SymRef> Constraints;
    int BlockId = -1;
    unsigned Steps = 0;
    llvm::DenseMap<int, unsigned> Visits;
    SymRef ImplicitLen;
    SymRef ImplicitSource;
    SymRef ImplicitSuccess;
    std::vector<std::pair<std::string, SymRef>> SourceEvents;
    bool ImplicitFromLength = false;
    bool ImplicitLenIncludesTerminator = false;
    bool SemanticUnknown = false;
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
    std::optional<int> Entry = entryBlockId(*LF);
    if (!Entry) {
      Best.SemanticUnknown = true;
      return Best;
    }
    Frontier Start(Ctx);
    Start.BlockId = *Entry;
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
        const bool IsCall =
            Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL;
        if (++Cur.Steps > MaxSteps) {
          Best.Budget = true;
          Stopped = true;
          break;
        }

        const bool IsSinkCall = IsCall && Op.Addr == Site.CallVA;
        if (IsSinkCall) {
          bool Unknown = false;
          if (!pathFeasible(Ctx, Cur.Constraints, Budgets, Unknown)) {
            Stopped = true;
            break;
          }
          if (Unknown) {
            Best.SolverUnknown = true;
            Best.Reached = true;
            ++Finished;
            HitSink = true;
            Stopped = true;
            break;
          }
          if (Cur.SemanticUnknown) {
            Best.SemanticUnknown = true;
            Best.Reached = true;
            ++Finished;
            HitSink = true;
            Stopped = true;
            break;
          }
          SymRef Pred =
              Cur.Constraints.empty()
                  ? Ctx.mkTrue()
                  : (Cur.Constraints.size() == 1 ? Cur.Constraints[0]
                                                 : Ctx.mkAnd(Cur.Constraints));
          SymRef Len =
              FixedLength
                  ? Ctx.mkConst(64, *FixedLength)
                  : (CI ? readCallArgument(In, Fn, E.LenArg, *CI, Cur.State)
                        : SymRef());
          SymRef SinkSource =
              CI ? readCallArgument(In, Fn, E.SrcArg, *CI, Cur.State)
                 : SymRef();
          if (!RequiredSource.empty()) {
            for (const auto &[Name, Buffer] : Cur.SourceEvents) {
              if (Name != RequiredSource)
                continue;
              if (expressionsMustEqual(Ctx, Buffer, SinkSource, Cur.Constraints,
                                       Budgets) ||
                  valuesShareUniqueLoadOrigin(Cur.State, Buffer, SinkSource)) {
                Best.SourceEvidence = true;
                break;
              }
            }
          }
          const bool SameImplicitSource =
              expressionsMustEqual(Ctx, Cur.ImplicitSource, SinkSource,
                                   Cur.Constraints, Budgets) ||
              (Cur.ImplicitFromLength &&
               valuesShareUniqueLoadOrigin(Cur.State, Cur.ImplicitSource,
                                           SinkSource));
          bool ConditionalBoundNotProven = false;
          if (!Len.isValid() && Cur.ImplicitLen.isValid() &&
              SameImplicitSource) {
            Len = Ctx.mkZExtOrTrunc(Cur.ImplicitLen, 64);
            if (Implicit && !Cur.ImplicitLenIncludesTerminator)
              Len = Ctx.mkAdd(Len, Ctx.mkConst(64, 1));
            ConditionalBoundNotProven = !predicateMustHold(
                Ctx, Cur.ImplicitSuccess, Cur.Constraints, Budgets);
          }
          SymRef RuntimeCap;
          if (E.CapArg >= 0) {
            RuntimeCap = CI ? readCallArgument(In, Fn, E.CapArg, *CI, Cur.State)
                            : SymRef();
            if (!RuntimeCap.isValid()) {
              Best.SemanticUnknown = true;
              Best.Reached = true;
              ++Finished;
              HitSink = true;
              Stopped = true;
              break;
            }
          }
          Best.Reached = true;
          considerSink(Best, Ctx, Pred, Len, RuntimeCap, Capacity, Budgets);
          if (ConditionalBoundNotProven)
            Best.SemanticUnknown = true;
          ++Finished;
          HitSink = true;
          Stopped = true;
          break;
        }

        const MedCallInfo *Callee = IsCall ? medCallAt(Fn, Op.Addr) : nullptr;
        const std::string CalleeName =
            Callee ? resolveCallName(In, *Callee) : "";
        const bool LengthCall = Callee && isStringLengthCall(CalleeName);
        const CountedSourceReturn CountedReturn =
            Callee ? countedSourceReturn(CalleeName,
                                         In.Img ? In.Img->Format
                                                : BinaryFormat::Unknown)
                   : CountedSourceReturn{};
        const CountedSourceOutput CountedOutput =
            Callee ? countedSourceOutput(CalleeName,
                                         In.Img ? In.Img->Format
                                                : BinaryFormat::Unknown)
                   : CountedSourceOutput{};
        const BoundedStringOutput BoundedString =
            Callee ? boundedStringOutput(CalleeName) : BoundedStringOutput{};
        bool CountedReturnApplies = Callee && CountedReturn.BoundArg >= 0;
        if (CountedReturnApplies && CountedReturn.RequiredZeroArg >= 0) {
          const SymRef Flags = readCallArgument(
              In, Fn, CountedReturn.RequiredZeroArg, *Callee, Cur.State);
          CountedReturnApplies =
              Flags.isValid() &&
              predicateMustHold(
                  Ctx, Ctx.mkEq(Ctx.mkZExtOrTrunc(Flags, 64), Ctx.mkZero(64)),
                  Cur.Constraints, Budgets);
        }
        const bool CountedOutputApplies =
            Callee && CountedOutput.BoundArg >= 0 &&
            CountedOutput.CountPointerArg >= 0 && CountedOutput.CountBits > 0;
        const SourceEntry *CountedSource =
            CountedReturnApplies || CountedOutputApplies
                ? Cat.matchSource(CalleeName)
                : nullptr;
        const SourceEntry *CalleeSource =
            Callee ? Cat.matchSource(CalleeName) : nullptr;
        std::vector<std::pair<std::string, SymRef>> NewSourceEvents;
        if (Callee && CalleeSource && CalleeSource->OutArg >= 0 &&
            CalleeSource->OutArg < static_cast<int>(Callee->Args.size())) {
          SymRef Output = readCallArgument(In, Fn, CalleeSource->OutArg,
                                           *Callee, Cur.State);
          if (Output.isValid())
            NewSourceEvents.emplace_back(SinkCatalog::normalize(CalleeName),
                                         Output);
        }
        if (Callee && CalleeSource)
          if (std::optional<safety::detail::FormattedSourceOutputs> Outputs =
                  safety::detail::recoverFormattedSourceOutputs(
                      In.Img, CalleeName, Callee->Args);
              Outputs && Outputs->Kind ==
                             safety::detail::FormattedSourceKind::ExternalInput)
            for (int ArgIndex : Outputs->UnboundedTextArgs) {
              SymRef Output =
                  readCallArgument(In, Fn, ArgIndex, *Callee, Cur.State);
              if (Output.isValid())
                NewSourceEvents.emplace_back(SinkCatalog::normalize(CalleeName),
                                             Output);
            }
        SymRef ReturnBound;
        if (CountedReturnApplies)
          ReturnBound = readCallArgument(In, Fn, CountedReturn.BoundArg,
                                         *Callee, Cur.State);
        SymRef OutputBound;
        SymRef OutputCountPointer;
        if (CountedOutputApplies) {
          OutputBound = readCallArgument(In, Fn, CountedOutput.BoundArg,
                                         *Callee, Cur.State);
          OutputCountPointer = readCallArgument(
              In, Fn, CountedOutput.CountPointerArg, *Callee, Cur.State);
        }
        SymRef NextImplicitSource;
        SymRef BoundedStringLimit;
        const bool CaptureLength = LengthCall && Callee;
        const bool CaptureCountedSource =
            E.LenArg >= 0 && Callee && CountedSource &&
            CountedSource->OutArg >= 0 &&
            CountedSource->OutArg < static_cast<int>(Callee->Args.size());
        const bool CaptureBoundedString =
            Implicit && Callee && CalleeSource &&
            BoundedString.BufferArg >= 0 && BoundedString.BoundArg >= 0 &&
            BoundedString.BufferArg < static_cast<int>(Callee->Args.size()) &&
            BoundedString.BoundArg < static_cast<int>(Callee->Args.size());
        if (CaptureLength)
          NextImplicitSource = readCallArgument(In, Fn, 0, *Callee, Cur.State);
        else if (CaptureCountedSource)
          NextImplicitSource = readCallArgument(In, Fn, CountedSource->OutArg,
                                                *Callee, Cur.State);
        else if (CaptureBoundedString) {
          NextImplicitSource = readCallArgument(In, Fn, BoundedString.BufferArg,
                                                *Callee, Cur.State);
          BoundedStringLimit = readCallArgument(In, Fn, BoundedString.BoundArg,
                                                *Callee, Cur.State);
        }
        if (IsCall) {
          bool Summarized = LengthCall;
          if (Callee && Cat.matchSource(CalleeName))
            Summarized = true;
          if (Callee)
            if (const SinkEntry *Known = Cat.matchSink(CalleeName))
              Summarized = Summarized || Known->Kind == SinkKind::Alloc ||
                           Known->Kind == SinkKind::StackAlloc ||
                           Known->Kind == SinkKind::Realloc;
          if (!Summarized)
            Cur.SemanticUnknown = true;
        }
        StepResult SR = Exec.step(Op);
        if (IsCall)
          Cur.SourceEvents.insert(Cur.SourceEvents.end(),
                                  NewSourceEvents.begin(),
                                  NewSourceEvents.end());
        SymRef CountedReturnValue;
        if (CountedReturnApplies) {
          SymRef Ret = captureReturn(Op, Cur.State, In);
          if (!ReturnBound.isValid() || !Ret.isValid())
            Cur.SemanticUnknown = true;
          else {
            SymRef ConstraintRet = Ret;
            if (CountedReturn.ReturnBits != 0 &&
                CountedReturn.ReturnBits < Ctx.width(Ret))
              ConstraintRet = Ctx.mkExtract(Ret, 0, CountedReturn.ReturnBits);
            const uint32_t ReturnWidth = Ctx.width(ConstraintRet);
            const SymRef IsError =
                Ctx.mkEq(ConstraintRet, Ctx.mkOnes(ReturnWidth));
            if (CountedReturn.ReturnBits != 0 &&
                CountedReturn.ReturnBits < Ctx.width(Ret)) {
              const uint16_t PointerBytes =
                  In.Img ? getTargetRegInfo(In.Img->Arch).PointerSize : 0;
              const uint32_t SemanticBits =
                  PointerBytes ? uint32_t(PointerBytes) * 8 : Ctx.width(Ret);
              CountedReturnValue =
                  Ctx.mkIte(IsError, Ctx.mkOnes(SemanticBits),
                            Ctx.mkZExtOrTrunc(ConstraintRet, SemanticBits));
            } else {
              CountedReturnValue = Ret;
            }
            const SymRef WideRet = Ctx.mkZExtOrTrunc(ConstraintRet, 64);
            const SymRef WideBound = Ctx.mkZExtOrTrunc(ReturnBound, 64);
            const SymRef WithinBound = Ctx.mkUle(WideRet, WideBound);
            if (CountedReturn.AllowsMinusOne) {
              const SymRef IsNonnegative =
                  Ctx.mkSge(ConstraintRet, Ctx.mkZero(ReturnWidth));
              Cur.Constraints.push_back(
                  Ctx.mkOr(IsError, Ctx.mkAnd(IsNonnegative, WithinBound)));
            } else {
              Cur.Constraints.push_back(WithinBound);
            }
          }
        }
        SymRef CountedOutputValue;
        if (CountedOutputApplies) {
          const std::optional<llvm::APInt> CountPointerConstant =
              OutputCountPointer.isValid() ? Ctx.asConst(OutputCountPointer)
                                           : std::optional<llvm::APInt>();
          if (!OutputBound.isValid() || !OutputCountPointer.isValid() ||
              (CountPointerConstant && CountPointerConstant->isZero())) {
            Cur.SemanticUnknown = true;
          } else {
            CountedOutputValue =
                Cur.State.freshInput("source_count", CountedOutput.CountBits);
            SymRef SemanticBound = OutputBound;
            if (CountedOutput.BoundBits != 0 &&
                CountedOutput.BoundBits < Ctx.width(SemanticBound))
              SemanticBound =
                  Ctx.mkExtract(SemanticBound, 0, CountedOutput.BoundBits);
            Cur.Constraints.push_back(
                Ctx.mkUle(Ctx.mkZExtOrTrunc(CountedOutputValue, 64),
                          Ctx.mkZExtOrTrunc(SemanticBound, 64)));
            Cur.State.store(OutputCountPointer, CountedOutputValue);
          }
        }
        if (IsCall) {
          if (CaptureLength) {
            Cur.ImplicitLen = captureReturn(Op, Cur.State, In);
            Cur.ImplicitSource = NextImplicitSource;
            Cur.ImplicitSuccess = SymRef();
            Cur.ImplicitFromLength = true;
            Cur.ImplicitLenIncludesTerminator = false;
          } else if (CaptureCountedSource && (CountedReturnValue.isValid() ||
                                              CountedOutputValue.isValid())) {
            Cur.ImplicitLen = CountedReturnValue.isValid() ? CountedReturnValue
                                                           : CountedOutputValue;
            Cur.ImplicitSource = NextImplicitSource;
            Cur.ImplicitSuccess = SymRef();
            Cur.ImplicitFromLength = false;
            Cur.ImplicitLenIncludesTerminator = false;
          } else if (CaptureBoundedString && NextImplicitSource.isValid() &&
                     BoundedStringLimit.isValid()) {
            const SymRef Ret = captureReturn(Op, Cur.State, In);
            if (!Ret.isValid()) {
              Cur.SemanticUnknown = true;
              Cur.ImplicitLen = SymRef();
              Cur.ImplicitSource = SymRef();
              Cur.ImplicitSuccess = SymRef();
              Cur.ImplicitFromLength = false;
              Cur.ImplicitLenIncludesTerminator = false;
            } else {
              const SymRef WideRet = Ctx.mkZExtOrTrunc(Ret, 64);
              const SymRef WideLimit =
                  Ctx.mkZExtOrTrunc(BoundedStringLimit, 64);
              Cur.ImplicitLen = WideLimit;
              Cur.ImplicitSource = NextImplicitSource;
              Cur.ImplicitSuccess =
                  Ctx.mkAnd(Ctx.mkNe(WideRet, Ctx.mkZero(64)),
                            Ctx.mkUgt(WideLimit, Ctx.mkZero(64)));
              Cur.ImplicitFromLength = false;
              Cur.ImplicitLenIncludesTerminator = true;
            }
          } else {
            Cur.ImplicitLen = SymRef();
            Cur.ImplicitSource = SymRef();
            Cur.ImplicitSuccess = SymRef();
            Cur.ImplicitFromLength = false;
            Cur.ImplicitLenIncludesTerminator = false;
          }
        } else if (mayWriteMemory(Op.Opcode) &&
                   (Cur.ImplicitFromLength ||
                    !writeIsProvablyBeforeSource(Ctx, Cur.State, Op,
                                                 Cur.ImplicitSource,
                                                 Cur.Constraints, Budgets))) {
          Cur.ImplicitLen = SymRef();
          Cur.ImplicitSource = SymRef();
          Cur.ImplicitSuccess = SymRef();
          Cur.ImplicitFromLength = false;
          Cur.ImplicitLenIncludesTerminator = false;
        }
        if (SR == StepResult::Unmodelled) {
          Cur.SemanticUnknown = true;
          continue;
        }
        if (SR == StepResult::Continue)
          continue;
        if (SR == StepResult::Return) {
          Stopped = true;
          ++Finished;
          break;
        }
        if (SR == StepResult::Branch || SR == StepResult::IndirectBranch) {
          std::optional<int> Next = takenSucc(*LF, *Block, Op, Ctx, Exec);
          if (!Next) {
            Best.SemanticUnknown = true;
            Stopped = true;
            break;
          }
          if (Reach.count(*Next)) {
            Frontier N = Cur;
            N.BlockId = *Next;
            Forks.push_back(std::move(N));
          }
          Stopped = true;
          break;
        }
        if (SR == StepResult::CondBranch) {
          std::optional<int> Taken = takenSucc(*LF, *Block, Op, Ctx, Exec);
          SymRef Cond = Exec.branchCondition();
          if (!Taken || !Cond.isValid()) {
            Best.SemanticUnknown = true;
            Stopped = true;
            break;
          }
          int NotTaken = otherSucc(*Block, *Taken);
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
          fork(*Taken, Cond);
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
  if (!E)
    return std::nullopt;

  if (E->Kind == SinkKind::Format) {
    Finding Out;
    stampSite(Out, In, F, Site);
    if (Site.CallInfoIndex >= F.CallInfos.size() || E->FmtArg < 0 ||
        E->FmtArg >=
            static_cast<int>(F.CallInfos[Site.CallInfoIndex].Args.size())) {
      Out.TheVerdict = Verdict::Unknown;
      Out.TheConfidence = Confidence::Low;
      Out.Detail = "format-string argument was not recovered";
      return Out;
    }

    const MedVar &FormatArg = F.CallInfos[Site.CallInfoIndex].Args[E->FmtArg];
    ArgClassification Arg =
        classifyArgument(In, Cat, F, Site.CallInfoIndex, E->FmtArg);
    Out.Flow = Arg.Flow;
    const Segment *FormatSegment =
        In.Img && FormatArg.isConst()
            ? In.Img->getSegmentFor(FormatArg.ConstVal)
            : nullptr;
    std::optional<std::string> ConstantFormat;
    if (Arg.Flow != ArgFlow::Tainted && FormatSegment &&
        !FormatSegment->isWritable())
      ConstantFormat = safety::detail::readMappedCString(In.Img, FormatArg);
    if (Arg.Flow != ArgFlow::Tainted && ConstantFormat) {
      if (E->DstArg < 0) {
        Out.TheVerdict = Verdict::Safe;
        Out.TheConfidence = Confidence::High;
        Out.SkipReason = "constant format string";
        Out.Detail = "format string is a mapped constant";
        return Out;
      }

      Out.Class = VulnClass::BufferOverflow;
      Out.ArgIndex = E->LenArg >= 0 ? E->LenArg : E->FmtArg;
      DestObject Dst =
          resolveDestination(In, Cat, F, Site.CallInfoIndex, E->DstArg);
      if (Dst.Capacity) {
        Out.Capacity = Dst.Capacity;
        Out.CapacityExact = Dst.CapacityExact;
      }

      auto classifyCallArg =
          [&](int Index) -> std::optional<ArgClassification> {
        if (Index < 0 || Site.CallInfoIndex >= F.CallInfos.size() ||
            Index >=
                static_cast<int>(F.CallInfos[Site.CallInfoIndex].Args.size()))
          return std::nullopt;
        return classifyArgument(In, Cat, F, Site.CallInfoIndex, Index);
      };

      if (E->CapArg >= 0) {
        std::optional<ArgClassification> ObjectBound =
            classifyCallArg(E->CapArg);
        if (!ObjectBound) {
          Out.TheVerdict = Verdict::Unknown;
          Out.TheConfidence = Confidence::Low;
          Out.Detail = "fortified destination bound was not recovered";
          return Out;
        }
        if (Dst.Capacity && Dst.CapacityExact && ObjectBound->UpperBound &&
            *ObjectBound->UpperBound <= *Dst.Capacity) {
          Out.TheVerdict = Verdict::Safe;
          Out.TheConfidence = Confidence::High;
          Out.SkipReason = "fortified runtime bound fits the destination";
          Out.Detail =
              "fortified formatting cannot write beyond the recovered object";
          return Out;
        }
        Out.TheVerdict = Verdict::Unknown;
        Out.TheConfidence = Confidence::Low;
        Out.Detail = "fortified destination bound does not prove the recovered "
                     "object is large enough";
        return Out;
      }

      std::optional<uint64_t> ExactWritten =
          literalFormattedBytes(*ConstantFormat);
      std::optional<uint64_t> MaxWritten = ExactWritten;
      if (E->LenArg >= 0) {
        std::optional<ArgClassification> WriteLimit =
            classifyCallArg(E->LenArg);
        if (!WriteLimit) {
          Out.TheVerdict = Verdict::Unknown;
          Out.TheConfidence = Confidence::Low;
          Out.Detail = "formatted write limit was not recovered";
          return Out;
        }
        if (WriteLimit->ConstValue) {
          MaxWritten = ExactWritten
                           ? std::min(*ExactWritten, *WriteLimit->ConstValue)
                           : WriteLimit->ConstValue;
          if (ExactWritten)
            ExactWritten = MaxWritten;
        } else if (WriteLimit->UpperBound) {
          MaxWritten = ExactWritten
                           ? std::min(*ExactWritten, *WriteLimit->UpperBound)
                           : WriteLimit->UpperBound;
          ExactWritten.reset();
        } else {
          MaxWritten.reset();
          ExactWritten.reset();
        }
      }

      if (MaxWritten && *MaxWritten == 0) {
        Out.TheVerdict = Verdict::Safe;
        Out.TheConfidence = Confidence::High;
        Out.SkipReason = "zero formatted write limit";
        Out.Detail = "formatting is configured to write no destination bytes";
        return Out;
      }
      if (!Dst.Capacity) {
        Out.TheVerdict = Verdict::Unknown;
        Out.TheConfidence = Confidence::Low;
        Out.Detail = "format string is constant, but destination extent is "
                     "unresolved";
        return Out;
      }
      if (MaxWritten && *MaxWritten <= *Dst.Capacity) {
        Out.TheVerdict = Dst.CapacityExact ? Verdict::Safe : Verdict::Unknown;
        Out.TheConfidence = Confidence::High;
        Out.SkipReason = "formatted output fits the destination";
        Out.Detail = Dst.CapacityExact
                         ? "formatted output cannot exceed destination capacity"
                         : "formatted output fits a containing-region bound, "
                           "but the object size is unresolved";
        return Out;
      }
      if (ExactWritten && *ExactWritten > *Dst.Capacity) {
        ExploreHit Hit = exploreSink(In, Cat, F, Site, *E, *Dst.Capacity,
                                     Budgets, llvm::StringRef(), ExactWritten);
        if (Hit.Kind == ExploreHit::Overflow) {
          Out.TheVerdict = Verdict::Unsafe;
          Out.TheConfidence = Confidence::High;
          Out.Detail = "formatted output exceeds destination capacity on a "
                       "reachable path";
          Out.Witness.push_back(
              {"formatted_bytes", std::to_string(*ExactWritten)});
          Out.Constraints = Hit.PredText;
          Out.Corroboration =
              "constant formatted output and overflow are jointly reachable";
          Out.BudgetHit = Hit.Budget || Hit.SolverUnknown;
          return Out;
        }
        Out.TheVerdict = Verdict::Unknown;
        Out.TheConfidence = Confidence::Low;
        Out.BudgetHit = Hit.Budget || Hit.SolverUnknown;
        if (Hit.Budget)
          Out.Detail = "exploration budget exhausted";
        else if (Hit.SemanticUnknown)
          Out.Detail = "path contains an operation or call without a summary";
        else if (Hit.SolverUnknown)
          Out.Detail = "solver could not establish path feasibility";
        else
          Out.Detail = "formatted overflow was not established on a reachable "
                       "path";
        return Out;
      }

      Out.TheVerdict = Verdict::Unknown;
      Out.TheConfidence = Confidence::Low;
      Out.Detail = "formatted output extent is unresolved";
      return Out;
    }

    if (Arg.Flow != ArgFlow::Tainted) {
      Out.TheVerdict = Verdict::Unknown;
      Out.TheConfidence = Confidence::Low;
      Out.Detail = "format-string provenance unresolved";
      return Out;
    }

    const std::string Source = SinkCatalog::normalize(Arg.TaintSource);
    const bool EntrySource = Source == "argv";
    SinkEntry Probe = *E;
    Probe.LenArg = -1;
    Probe.SrcArg = E->FmtArg;
    Probe.CapArg = -1;
    ExploreHit Hit = exploreSink(In, Cat, F, Site, Probe, UINT64_MAX, Budgets,
                                 EntrySource ? llvm::StringRef() : Source);
    if (Hit.Reached && !Hit.Budget && !Hit.SolverUnknown &&
        !Hit.SemanticUnknown && (EntrySource || Hit.SourceEvidence)) {
      Out.TheVerdict = Verdict::Unsafe;
      Out.TheConfidence = Confidence::High;
      Out.Detail =
          "attacker-controlled format string reaches a formatting call";
      Out.Witness.push_back({"format_string", "%n"});
      if (!Source.empty())
        Out.Witness.push_back({"source", Source});
      Out.Corroboration =
          "source and formatting call occur on one feasible path";
      return Out;
    }

    Out.TheVerdict = Verdict::Unknown;
    Out.TheConfidence = Confidence::Low;
    Out.BudgetHit = Hit.Budget || Hit.SolverUnknown;
    if (Hit.Budget)
      Out.Detail = "exploration budget exhausted";
    else if (Hit.SemanticUnknown)
      Out.Detail = "path contains an operation or call without a summary";
    else if (Hit.SolverUnknown)
      Out.Detail = "solver could not establish path feasibility";
    else
      Out.Detail =
          "format-string source was not established on a reachable path";
    return Out;
  }

  if (E->Kind != SinkKind::Copy)
    return std::nullopt;

  Finding Out;
  stampSite(Out, In, F, Site);
  const bool WideElements = usesWideElements(Site.Sink);
  const bool NeedsStringExtents = requiresStringExtents(Site.Sink);
  const bool UnboundedInput = E->UnboundedWrite;

  ArgClassification Arg =
      classifyArgument(In, Cat, F, Site.CallInfoIndex, Site.ArgIndex);
  if (UnboundedInput) {
    Arg.Flow = ArgFlow::Tainted;
    Arg.TaintSource = SinkCatalog::normalize(Site.Sink);
    Arg.Reason = "sink receives unbounded external input";
  }
  Out.Flow = Arg.Flow;

  const bool Fortified = E->CapArg >= 0;
  std::optional<ArgClassification> RuntimeBound;
  if (Fortified) {
    if (Site.CallInfoIndex >= F.CallInfos.size() ||
        E->CapArg >=
            static_cast<int>(F.CallInfos[Site.CallInfoIndex].Args.size())) {
      Out.TheVerdict = Verdict::Unknown;
      Out.TheConfidence = Confidence::Low;
      Out.Detail = "fortified destination bound was not recovered";
      return Out;
    }
    RuntimeBound = classifyArgument(In, Cat, F, Site.CallInfoIndex, E->CapArg);
  }

  DestObject Dst =
      resolveDestination(In, Cat, F, Site.CallInfoIndex, E->DstArg);
  if (Dst.Capacity) {
    Out.Capacity = Dst.Capacity;
    Out.CapacityExact = Dst.CapacityExact;
  }

  if (Fortified && E->LenArg >= 0 && Arg.ConstValue &&
      RuntimeBound->UpperBound && *Arg.ConstValue > *RuntimeBound->UpperBound) {
    Out.TheVerdict = Verdict::Safe;
    Out.TheConfidence = Confidence::High;
    Out.SkipReason = "fortified runtime bound rejects the constant length";
    Out.Detail = "fortified copy aborts before writing the requested length";
    return Out;
  }

  if (Fortified && Dst.CapacityExact && RuntimeBound->UpperBound &&
      *RuntimeBound->UpperBound <= *Dst.Capacity) {
    Out.TheVerdict = Verdict::Safe;
    Out.TheConfidence = Confidence::High;
    Out.SkipReason = "fortified runtime bound fits the destination";
    Out.Detail = "fortified copy cannot write beyond the recovered object";
    return Out;
  }

  if (WideElements || NeedsStringExtents) {
    Out.TheVerdict = Verdict::Unknown;
    Out.TheConfidence = Confidence::Low;
    Out.Detail = WideElements
                     ? "wide-element byte width is not established"
                     : "source and destination string extents are unresolved";
    return Out;
  }

  if (E->LenArg >= 0 && Arg.ConstValue && Dst.Capacity) {
    if (*Arg.ConstValue <= *Dst.Capacity && Dst.CapacityExact) {
      Out.TheVerdict = Verdict::Safe;
      Out.TheConfidence = Confidence::High;
      Out.SkipReason = Arg.Reason.empty() ? "constant length" : Arg.Reason;
      Out.Detail = "constant length within destination capacity";
      return Out;
    }
    if (*Arg.ConstValue <= *Dst.Capacity) {
      Out.TheVerdict = Verdict::Unknown;
      Out.TheConfidence = Confidence::High;
      Out.SkipReason = Arg.Reason.empty() ? "constant length" : Arg.Reason;
      Out.Detail = "constant length is bounded, but the exact destination size "
                   "is unresolved";
      return Out;
    }
  }

  if (!Dst.Capacity) {
    Out.TheVerdict = Verdict::Unknown;
    Out.TheConfidence = Confidence::Low;
    Out.Detail = "destination capacity could not be recovered";
    return Out;
  }

  if (E->LenArg >= 0 && Arg.Flow == ArgFlow::Bounded && Arg.UpperBound &&
      *Arg.UpperBound <= *Dst.Capacity) {
    Out.TheVerdict = Dst.CapacityExact ? Verdict::Safe : Verdict::Unknown;
    Out.TheConfidence =
        Dst.CapacityExact ? Confidence::Medium : Confidence::High;
    Out.SkipReason = Arg.Reason.empty() ? "bounded length" : Arg.Reason;
    Out.Detail =
        Dst.CapacityExact
            ? "proven argument bound fits destination capacity"
            : "length is bounded, but the exact destination size is unresolved";
    return Out;
  }

  std::string RequiredSource;
  if (!UnboundedInput && E->LenArg < 0 && E->SrcArg >= 0)
    if (const SourceEntry *Source = Cat.matchSource(Arg.TaintSource);
        Source &&
        (Source->OutArg >= 0 ||
         safety::detail::formattedSourceName(Arg.TaintSource).has_value()))
      RequiredSource = SinkCatalog::normalize(Arg.TaintSource);
  ExploreHit Hit =
      exploreSink(In, Cat, F, Site, *E, *Dst.Capacity, Budgets, RequiredSource);
  if (Hit.Kind == ExploreHit::Overflow) {
    Out.TheVerdict = Verdict::Unsafe;
    Out.TheConfidence = Confidence::High;
    Out.Detail =
        "copy length can exceed destination capacity on a reachable path";
    Out.Constraints = Hit.PredText;
    addWitness(Out, *E, Hit.WitnessLen, Arg.TaintSource);
    Out.Corroboration = "path predicate and overflow are jointly satisfiable";
    Out.BudgetHit = Hit.Budget || Hit.SolverUnknown;
    return Out;
  }
  if (Hit.Kind == ExploreHit::InBound && Hit.Reached && !Hit.Budget &&
      !Hit.SolverUnknown && !Hit.SemanticUnknown && !Hit.MissingLength) {
    Out.TheVerdict = Dst.CapacityExact ? Verdict::Safe : Verdict::Unknown;
    Out.TheConfidence = Confidence::High;
    Out.Detail = Dst.CapacityExact
                     ? "every explored path keeps the copy within capacity"
                     : "paths fit a containing-region upper bound, but the "
                       "object size is unresolved";
    Out.Constraints = Hit.PredText;
    return Out;
  }

  if (Arg.Flow == ArgFlow::Tainted && E->LenArg < 0 &&
      (E->SrcArg >= 0 || UnboundedInput) && Hit.Reached && !Hit.Budget &&
      !Hit.SolverUnknown && !Hit.SemanticUnknown &&
      (RequiredSource.empty() || Hit.SourceEvidence)) {
    const bool GuardAllowsOverflow =
        !Fortified ||
        (RuntimeBound->ConstValue && *RuntimeBound->ConstValue > *Dst.Capacity);
    if (GuardAllowsOverflow)
      if (auto Len = abstractOverflowLen(*Dst.Capacity, Budgets)) {
        Out.TheVerdict = Verdict::Unsafe;
        Out.TheConfidence = Confidence::High;
        Out.Detail =
            "attacker-controlled length can exceed destination capacity";
        addWitness(Out, *E, *Len, Arg.TaintSource);
        Out.Corroboration = "sink reachable on a summarized symbolic path";
        return Out;
      }
  }

  Out.TheVerdict = Verdict::Unknown;
  Out.TheConfidence = Confidence::Low;
  Out.BudgetHit = Hit.Budget || Hit.SolverUnknown;
  if (Hit.Budget)
    Out.Detail = "exploration budget exhausted";
  else if (Hit.SemanticUnknown)
    Out.Detail = "path contains an operation or call without a summary";
  else if (Hit.SolverUnknown)
    Out.Detail = "solver could not establish path feasibility";
  else
    Out.Detail = "length provenance unresolved";
  return Out;
}
