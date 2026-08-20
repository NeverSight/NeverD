//===- AllocLifetime.cpp - Heap allocation lifetime audit ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/AllocLifetime.h"

#include "neverd/safety/SinkScanner.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/symbolic/SymExplore.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <deque>
#include <tuple>

using namespace neverd;
using namespace neverd::safety;

namespace {

using ValueKey = std::tuple<uint8_t, int, int>;

ValueKey keyOf(const MedVar &V) {
  return {static_cast<uint8_t>(V.Kind), V.Id, V.SSAVer};
}

bool forwardsPointer(NdOp Op) {
  switch (Op) {
  case NdOp::COPY:
  case NdOp::CAST:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::SUBBYTES:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
    return true;
  default:
    return false;
  }
}

// A per-function memory model that resolves stack-slot addresses and records
// the spill/reload traffic, so a handle kept in a local slot is still tracked
// across an unoptimised binary's stores and loads.
struct MemModel {
  const MedFunc &F;
  uint64_t SP = 0, FP = 0;
  bool Have = false;
  llvm::DenseMap<ValueKey, std::pair<int, int>> OpDef;
  llvm::DenseMap<int64_t, llvm::SmallVector<MedVar, 2>> StoredAt;
  llvm::DenseMap<int64_t, llvm::SmallVector<MedVar, 2>> LoadedAt;

  MemModel(const MedFunc &Fn, const AnalysisInput &In) : F(Fn) {
    SP = In.StackPointerReg;
    FP = In.FramePointerReg;
    Have = In.StackRegsKnown;
    for (int Bi = 0; Bi < static_cast<int>(F.Blocks.size()); ++Bi)
      for (int Oi = 0; Oi < static_cast<int>(F.Blocks[Bi].Ops.size()); ++Oi) {
        const MedOp &O = F.Blocks[Bi].Ops[Oi];
        if (!O.Output.isConst() && O.Output.Size > 0)
          OpDef[keyOf(O.Output)] = {Bi, Oi};
      }
    for (const MedBlock &B : F.Blocks)
      for (const MedOp &Op : B.Ops) {
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2) {
          const MedVar &Addr = Op.Inputs[Op.NumInputs >= 3 ? 1 : 0];
          const MedVar &Val = Op.Inputs[Op.NumInputs >= 3 ? 2 : 1];
          if (auto Off = stackOffset(Addr))
            StoredAt[*Off].push_back(Val);
        } else if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
                   !Op.Output.isConst() && Op.Output.Size > 0) {
          const MedVar &Addr = Op.Inputs[Op.NumInputs >= 2 ? 1 : 0];
          if (auto Off = stackOffset(Addr))
            LoadedAt[*Off].push_back(Op.Output);
        }
      }
  }

  const MedOp *def(const MedVar &V) const {
    auto It = OpDef.find(keyOf(V));
    return It == OpDef.end() ? nullptr
                             : &F.Blocks[It->second.first].Ops[It->second.second];
  }

  std::optional<int64_t> stackOffset(const MedVar &V) const {
    llvm::DenseSet<ValueKey> Seen;
    return stackOffsetRec(V, 0, Seen);
  }

  std::optional<int64_t> stackOffsetRec(const MedVar &V, int Depth,
                                        llvm::DenseSet<ValueKey> &Seen) const {
    if (!Have || V.isConst() || Depth > 48 || !Seen.insert(keyOf(V)).second)
      return std::nullopt;
    const MedOp *Op = def(V);
    if (V.Kind == MedVar::Reg && (V.RegOff == SP || V.RegOff == FP)) {
      if (Op && (Op->Opcode == NdOp::INT_ADD || Op->Opcode == NdOp::INT_SUB))
        return affine(*Op, Depth, Seen);
      return V.RegOff == SP ? std::optional<int64_t>(0) : std::nullopt;
    }
    if (!Op)
      return std::nullopt;
    switch (Op->Opcode) {
    case NdOp::COPY:
    case NdOp::CAST:
      return Op->NumInputs >= 1 ? stackOffsetRec(Op->Inputs[0], Depth + 1, Seen)
                                : std::nullopt;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      return affine(*Op, Depth, Seen);
    default:
      return std::nullopt;
    }
  }

  std::optional<int64_t> affine(const MedOp &Op, int Depth,
                                llvm::DenseSet<ValueKey> &Seen) const {
    if (Op.NumInputs < 2)
      return std::nullopt;
    const bool Sub = Op.Opcode == NdOp::INT_SUB;
    if (Op.Inputs[1].isConst()) {
      if (auto B = stackOffsetRec(Op.Inputs[0], Depth + 1, Seen))
        return Sub ? *B - static_cast<int64_t>(Op.Inputs[1].ConstVal)
                   : *B + static_cast<int64_t>(Op.Inputs[1].ConstVal);
    } else if (Op.Inputs[0].isConst() && !Sub) {
      if (auto B = stackOffsetRec(Op.Inputs[1], Depth + 1, Seen))
        return *B + static_cast<int64_t>(Op.Inputs[0].ConstVal);
    }
    return std::nullopt;
  }

  // Every value that aliases the seed, by SSA forwarding and by spill/reload
  // through a stack slot.
  llvm::DenseSet<ValueKey> aliasClosure(const MedVar &Seed) const {
    llvm::DenseSet<ValueKey> Set;
    Set.insert(keyOf(Seed));
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (const MedBlock &B : F.Blocks) {
        for (const PhiNode &Phi : B.Phis) {
          if (Phi.Output.isConst())
            continue;
          for (const auto &[Pred, Arg] : Phi.Args)
            if (!Arg.isConst() && Set.count(keyOf(Arg)) &&
                Set.insert(keyOf(Phi.Output)).second)
              Changed = true;
        }
        for (const MedOp &Op : B.Ops) {
          if (Op.Output.isConst() || Op.Output.Size == 0 ||
              !forwardsPointer(Op.Opcode))
            continue;
          for (unsigned I = 0; I < Op.NumInputs; ++I)
            if (!Op.Inputs[I].isConst() && Set.count(keyOf(Op.Inputs[I])) &&
                Set.insert(keyOf(Op.Output)).second)
              Changed = true;
        }
      }
      // Memory: a slot written with the handle re-supplies it on every reload.
      for (const auto &[Off, Stored] : StoredAt) {
        bool HandleHeld = false;
        for (const MedVar &V : Stored)
          if (!V.isConst() && Set.count(keyOf(V))) {
            HandleHeld = true;
            break;
          }
        if (!HandleHeld)
          continue;
        auto It = LoadedAt.find(Off);
        if (It == LoadedAt.end())
          continue;
        for (const MedVar &L : It->second)
          if (Set.insert(keyOf(L)).second)
            Changed = true;
      }
    }
    return Set;
  }
};

struct CFG {
  const MedFunc &F;
  llvm::DenseMap<int, int> IdToIndex;

  explicit CFG(const MedFunc &Fn) : F(Fn) {
    for (int I = 0; I < static_cast<int>(F.Blocks.size()); ++I)
      IdToIndex[F.Blocks[I].Id] = I;
  }

  bool blockReaches(int FromId, int ToId) const {
    if (FromId == ToId)
      return true;
    llvm::DenseSet<int> Seen;
    std::deque<int> Work{FromId};
    while (!Work.empty()) {
      int Cur = Work.front();
      Work.pop_front();
      auto It = IdToIndex.find(Cur);
      if (It == IdToIndex.end())
        continue;
      for (int Succ : F.Blocks[It->second].Succs) {
        if (Succ == ToId)
          return true;
        if (Seen.insert(Succ).second)
          Work.push_back(Succ);
      }
    }
    return false;
  }

  bool after(int ABlock, int AOp, int BBlock, int BOp) const {
    if (ABlock == BBlock)
      return BOp > AOp;
    return blockReaches(ABlock, BBlock);
  }
};

int paramIndexOf(const MedFunc &F, const MedVar &V) {
  for (int I = 0; I < static_cast<int>(F.Params.size()); ++I)
    if (F.Params[I].Kind == V.Kind && F.Params[I].Id == V.Id &&
        F.Params[I].SSAVer == V.SSAVer)
      return I;
  return -1;
}

const MedOp *opAt(const MedFunc &F, int BlockId, int OpIdx) {
  for (const MedBlock &B : F.Blocks)
    if (B.Id == BlockId)
      return (OpIdx >= 0 && OpIdx < static_cast<int>(B.Ops.size()))
                 ? &B.Ops[OpIdx]
                 : nullptr;
  return nullptr;
}

va_t callVA(const MedFunc &F, const MedCallInfo &CI) {
  if (const MedOp *Op = opAt(F, CI.BlockId, CI.OpIdx))
    return Op->Addr;
  return 0;
}

struct Summary {
  bool ReturnsHeap = false;
  llvm::DenseSet<int> FreesParam;
};

class Auditor {
public:
  Auditor(const AnalysisInput &In, const SinkCatalog &Cat,
          const SafetyBudgets &Budgets)
      : In(In), Cat(Cat), Budgets(Budgets) {
    buildSummaries();
  }

  std::vector<Finding> run() {
    std::vector<Finding> All;
    if (!In.MedFuncs)
      return All;
    for (const MedFunc &F : *In.MedFuncs)
      auditFunction(F, All);
    for (Finding &Fn : All)
      corroborate(Fn);
    return All;
  }

private:
  const AnalysisInput &In;
  const SinkCatalog &Cat;
  const SafetyBudgets &Budgets;
  llvm::DenseMap<va_t, Summary> Summaries;

  int catalogFreeArg(const MedCallInfo &CI) const {
    const SinkEntry *E = Cat.matchSink(CI.TargetName);
    if (!E)
      return -1;
    if (E->Kind == SinkKind::Free || E->Kind == SinkKind::Realloc)
      return E->HandleArg;
    return -1;
  }

  int freeArgOf(const MedFunc &F, const MedCallInfo &CI) const {
    int FA = catalogFreeArg(CI);
    if (FA >= 0)
      return FA;
    if (!CI.IsIndirect) {
      auto It = Summaries.find(CI.TargetAddr);
      if (It != Summaries.end() && !It->second.FreesParam.empty())
        return *It->second.FreesParam.begin();
    }
    return -1;
  }

  bool allocates(const MedCallInfo &CI) const {
    if (const SinkEntry *E = Cat.matchSink(CI.TargetName))
      if (E->Kind == SinkKind::Alloc || E->Kind == SinkKind::Realloc)
        return true;
    if (!CI.IsIndirect) {
      auto It = Summaries.find(CI.TargetAddr);
      if (It != Summaries.end() && It->second.ReturnsHeap)
        return true;
    }
    return false;
  }

  static bool sameSummary(const Summary &A, const Summary &B) {
    if (A.ReturnsHeap != B.ReturnsHeap ||
        A.FreesParam.size() != B.FreesParam.size())
      return false;
    for (int P : A.FreesParam)
      if (!B.FreesParam.count(P))
        return false;
    return true;
  }

  Summary summarize(const MedFunc &F) const {
    Summary S;
    MemModel Mem(F, In);
    for (const MedCallInfo &CI : F.CallInfos) {
      int FA = catalogFreeArg(CI);
      if (FA < 0 && !CI.IsIndirect) {
        auto It = Summaries.find(CI.TargetAddr);
        if (It != Summaries.end() && !It->second.FreesParam.empty())
          FA = *It->second.FreesParam.begin();
      }
      if (FA < 0 || FA >= static_cast<int>(CI.Args.size()))
        continue;
      if (int PI = paramIndexOf(F, CI.Args[FA]); PI >= 0)
        S.FreesParam.insert(PI);
    }
    for (const MedCallInfo &CI : F.CallInfos) {
      bool Alloc = false;
      if (const SinkEntry *E = Cat.matchSink(CI.TargetName))
        Alloc = E->Kind == SinkKind::Alloc || E->Kind == SinkKind::Realloc;
      if (!Alloc && !CI.IsIndirect) {
        auto It = Summaries.find(CI.TargetAddr);
        Alloc = It != Summaries.end() && It->second.ReturnsHeap;
      }
      if (!Alloc)
        continue;
      const MedOp *Op = opAt(F, CI.BlockId, CI.OpIdx);
      if (!Op || Op->Output.isConst() || Op->Output.Size == 0)
        continue;
      llvm::DenseSet<ValueKey> Alias = Mem.aliasClosure(Op->Output);
      for (const MedBlock &B : F.Blocks)
        for (const MedOp &O : B.Ops)
          if (O.Opcode == NdOp::RETURN)
            for (unsigned I = 0; I < O.NumInputs; ++I)
              if (!O.Inputs[I].isConst() && Alias.count(keyOf(O.Inputs[I])))
                S.ReturnsHeap = true;
    }
    return S;
  }

  void buildSummaries() {
    if (!In.MedFuncs)
      return;
    bool Changed = true;
    for (int Guard = 0; Changed && Guard < 32; ++Guard) {
      Changed = false;
      for (const MedFunc &F : *In.MedFuncs) {
        Summary S = summarize(F);
        auto It = Summaries.find(F.Entry);
        if (It == Summaries.end() || !sameSummary(It->second, S)) {
          Summaries[F.Entry] = std::move(S);
          Changed = true;
        }
      }
    }
  }

  bool comparedToNull(const MedFunc &F,
                      const llvm::DenseSet<ValueKey> &Alias) const {
    auto isZero = [](const MedVar &V) { return V.isConst() && V.ConstVal == 0; };
    auto isHandle = [&](const MedVar &V) {
      return !V.isConst() && Alias.count(keyOf(V));
    };
    for (const MedBlock &B : F.Blocks)
      for (const MedOp &Op : B.Ops) {
        switch (Op.Opcode) {
        case NdOp::INT_EQUAL:
        case NdOp::INT_NOTEQUAL:
        case NdOp::INT_LESS:
        case NdOp::INT_LESSEQUAL:
        case NdOp::INT_SLESS:
        case NdOp::INT_SLESSEQUAL:
          if (Op.NumInputs >= 2 &&
              ((isHandle(Op.Inputs[0]) && isZero(Op.Inputs[1])) ||
               (isHandle(Op.Inputs[1]) && isZero(Op.Inputs[0]))))
            return true;
          break;
        default:
          break;
        }
      }
    return false;
  }

  bool isFreeSite(
      int BlockId, int OpIdx,
      const std::vector<std::tuple<int, int, va_t, std::string>> &Frees) const {
    for (const auto &Fr : Frees)
      if (std::get<0>(Fr) == BlockId && std::get<1>(Fr) == OpIdx)
        return true;
    return false;
  }

  bool reachesExitWithoutFree(
      const MedFunc &F, int AllocBlock, int AllocOp,
      const std::vector<std::tuple<int, int, va_t, std::string>> &Frees) const {
    llvm::DenseSet<ValueKey> SeenPts;
    std::deque<std::pair<int, int>> Work;
    auto enqueue = [&](int B, int O) {
      if (SeenPts.insert(ValueKey{0, B, O}).second)
        Work.emplace_back(B, O);
    };
    enqueue(AllocBlock, AllocOp + 1);
    while (!Work.empty()) {
      auto [BlkId, OpIdx] = Work.front();
      Work.pop_front();
      const MedBlock *Blk = nullptr;
      for (const MedBlock &B : F.Blocks)
        if (B.Id == BlkId) {
          Blk = &B;
          break;
        }
      if (!Blk)
        continue;
      if (OpIdx >= static_cast<int>(Blk->Ops.size())) {
        if (Blk->Succs.empty())
          return true;
        for (int S : Blk->Succs)
          enqueue(S, 0);
        continue;
      }
      if (isFreeSite(BlkId, OpIdx, Frees))
        continue;
      if (Blk->Ops[OpIdx].Opcode == NdOp::RETURN)
        return true;
      enqueue(BlkId, OpIdx + 1);
    }
    return false;
  }

  bool shouldReportLeak(
      const MedFunc &F, const CFG &, const llvm::DenseSet<ValueKey> &Alias,
      const MedCallInfo &Alloc,
      const std::vector<std::tuple<int, int, va_t, std::string>> &Frees) const {
    if (Frees.empty())
      return true;
    if (!reachesExitWithoutFree(F, Alloc.BlockId, Alloc.OpIdx, Frees))
      return false;
    return !comparedToNull(F, Alias);
  }

  void corroborate(Finding &Fn) {
    const LowFunc *LF = In.findLowFunc(Fn.FuncEntry);
    if (!LF || LF->Blocks.empty() || Fn.CallVA == 0)
      return;
    symbolic::SymContext Ctx;
    symbolic::ExploreOptions Opts;
    Opts.MaxPaths = Budgets.MaxPaths ? Budgets.MaxPaths : 64;
    Opts.MaxSteps = Budgets.MaxSteps ? Budgets.MaxSteps : (1u << 16);
    Opts.MaxLoopIterations = Budgets.MaxLoop ? Budgets.MaxLoop : 3;
    symbolic::SymExploration Ex =
        symbolic::explorePathsDetailed(Ctx, *LF, Opts);
    bool Reached = false;
    for (const symbolic::SymPath &P : Ex.Paths) {
      for (int Bid : P.Blocks) {
        for (const LowBlock &Blk : LF->Blocks)
          if (Blk.Id == Bid && Fn.CallVA >= Blk.StartAddr &&
              Fn.CallVA < Blk.EndAddr)
            Reached = true;
      }
    }
    if (Reached)
      Fn.Corroboration = "defect reachable on a symbolic path";
    else if (!Ex.Complete) {
      Fn.BudgetHit = true;
      Fn.Corroboration = "reachability not established in budget";
      if (Fn.TheConfidence == Confidence::High)
        Fn.TheConfidence = Confidence::Medium;
    }
  }

  Finding baseFinding(const MedFunc &F, VulnClass Class, va_t VA,
                      const std::string &Name) {
    Finding Fn;
    Fn.Origin = Track::Audit;
    Fn.Class = Class;
    Fn.TheVerdict = Verdict::Unsafe;
    Fn.Function = F.Name;
    Fn.FuncEntry = F.Entry;
    Fn.Name = Name;
    Fn.Sink = Name;
    Fn.CallVA = VA;
    Fn.Source = classifyNameSource(In, VA, Name, false);
    if (In.Dbg)
      if (auto Loc = In.Dbg->sourceLocation(VA); Loc && !Loc->File.empty())
        Fn.SourceLoc = Loc->File + ":" + std::to_string(Loc->Line);
    return Fn;
  }

  void auditFunction(const MedFunc &F, std::vector<Finding> &Out) {
    CFG G(F);
    MemModel Mem(F, In);

    for (size_t I = 0; I < F.CallInfos.size(); ++I) {
      const MedCallInfo &CI = F.CallInfos[I];
      if (!allocates(CI))
        continue;
      const MedOp *AllocOp = opAt(F, CI.BlockId, CI.OpIdx);
      if (!AllocOp || AllocOp->Output.isConst() || AllocOp->Output.Size == 0)
        continue;

      llvm::DenseSet<ValueKey> Alias = Mem.aliasClosure(AllocOp->Output);

      std::vector<std::tuple<int, int, va_t, std::string>> Frees;
      for (const MedCallInfo &FC : F.CallInfos) {
        int FA = freeArgOf(F, FC);
        if (FA < 0 || FA >= static_cast<int>(FC.Args.size()))
          continue;
        if (Alias.count(keyOf(FC.Args[FA])))
          Frees.emplace_back(FC.BlockId, FC.OpIdx, callVA(F, FC), FC.TargetName);
      }

      bool Escapes = escapes(F, Mem, Alias, CI);

      bool ReportedDouble = false;
      for (size_t A = 0; A < Frees.size() && !ReportedDouble; ++A)
        for (size_t B = 0; B < Frees.size(); ++B) {
          if (A == B)
            continue;
          if (G.after(std::get<0>(Frees[A]), std::get<1>(Frees[A]),
                      std::get<0>(Frees[B]), std::get<1>(Frees[B]))) {
            Finding Fn =
                baseFinding(F, VulnClass::DoubleFree, std::get<2>(Frees[B]),
                            std::get<3>(Frees[B]));
            Fn.TheConfidence = Confidence::High;
            Fn.Detail = "handle released twice on a path";
            Out.push_back(std::move(Fn));
            ReportedDouble = true;
            break;
          }
        }

      std::vector<std::tuple<int, int, va_t>> Uses = collectUses(F, Alias, Frees);
      bool ReportedUAF = false;
      for (const auto &Fr : Frees) {
        if (ReportedUAF)
          break;
        for (const auto &U : Uses)
          if (G.after(std::get<0>(Fr), std::get<1>(Fr), std::get<0>(U),
                      std::get<1>(U))) {
            Finding Fn = baseFinding(F, VulnClass::UseAfterFree, std::get<2>(U),
                                     std::get<3>(Fr));
            Fn.TheConfidence = Confidence::High;
            Fn.Detail = "handle used after it was released";
            Out.push_back(std::move(Fn));
            ReportedUAF = true;
            break;
          }
      }

      if (!Escapes && shouldReportLeak(F, G, Alias, CI, Frees)) {
        Finding Fn =
            baseFinding(F, VulnClass::HeapLeak, callVA(F, CI), CI.TargetName);
        Fn.TheConfidence = Confidence::Medium;
        Fn.Detail = "allocation neither released nor escaped on every path";
        Out.push_back(std::move(Fn));
      }
    }
  }

  std::vector<std::tuple<int, int, va_t>>
  collectUses(const MedFunc &F, const llvm::DenseSet<ValueKey> &Alias,
              const std::vector<std::tuple<int, int, va_t, std::string>> &Frees)
      const {
    auto isFree = [&](int BlockId, int OpIdx) {
      for (const auto &Fr : Frees)
        if (std::get<0>(Fr) == BlockId && std::get<1>(Fr) == OpIdx)
          return true;
      return false;
    };

    std::vector<std::tuple<int, int, va_t>> Uses;
    for (const MedBlock &B : F.Blocks)
      for (int Oi = 0; Oi < static_cast<int>(B.Ops.size()); ++Oi) {
        const MedOp &Op = B.Ops[Oi];
        if (Op.Opcode == NdOp::LOAD) {
          const MedVar &Addr = Op.Inputs[Op.NumInputs >= 2 ? 1 : 0];
          if (!Addr.isConst() && Alias.count(keyOf(Addr)))
            Uses.emplace_back(B.Id, Oi, Op.Addr);
        } else if (Op.Opcode == NdOp::STORE) {
          const MedVar &Addr = Op.Inputs[Op.NumInputs >= 3 ? 1 : 0];
          if (!Addr.isConst() && Alias.count(keyOf(Addr)))
            Uses.emplace_back(B.Id, Oi, Op.Addr);
        }
      }
    for (const MedCallInfo &CI : F.CallInfos) {
      if (isFree(CI.BlockId, CI.OpIdx))
        continue;
      int FA = freeArgOf(F, CI);
      for (int I = 0; I < static_cast<int>(CI.Args.size()); ++I) {
        if (I == FA || CI.Args[I].isConst())
          continue;
        if (Alias.count(keyOf(CI.Args[I]))) {
          Uses.emplace_back(CI.BlockId, CI.OpIdx, callVA(F, CI));
          break;
        }
      }
    }
    return Uses;
  }

  bool escapes(const MedFunc &F, const MemModel &Mem,
               const llvm::DenseSet<ValueKey> &Alias,
               const MedCallInfo &Alloc) const {
    for (const MedBlock &B : F.Blocks)
      for (const MedOp &Op : B.Ops) {
        if (Op.Opcode == NdOp::RETURN) {
          for (unsigned I = 0; I < Op.NumInputs; ++I)
            if (!Op.Inputs[I].isConst() && Alias.count(keyOf(Op.Inputs[I])))
              return true;
        } else if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2) {
          const MedVar &Addr = Op.Inputs[Op.NumInputs >= 3 ? 1 : 0];
          const MedVar &Val = Op.Inputs[Op.NumInputs >= 3 ? 2 : 1];
          // Spilling the handle into its own stack frame is not an escape; only
          // a write through a non-stack address publishes it.
          if (!Val.isConst() && Alias.count(keyOf(Val)) &&
              !Mem.stackOffset(Addr))
            return true;
        }
      }
    for (const MedCallInfo &CI : F.CallInfos) {
      if (CI.BlockId == Alloc.BlockId && CI.OpIdx == Alloc.OpIdx)
        continue;
      int FA = freeArgOf(F, CI);
      for (int I = 0; I < static_cast<int>(CI.Args.size()); ++I) {
        if (I == FA || CI.Args[I].isConst())
          continue;
        if (Alias.count(keyOf(CI.Args[I])))
          return true;
      }
    }
    return false;
  }
};

} // namespace

std::vector<Finding> neverd::safety::auditHeap(const AnalysisInput &In,
                                               const SinkCatalog &Cat,
                                               const SafetyBudgets &Budgets) {
  Auditor A(In, Cat, Budgets);
  return A.run();
}
