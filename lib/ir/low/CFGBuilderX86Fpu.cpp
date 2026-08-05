//===- CFGBuilderX86Fpu.cpp - x87 FPU stack fixup for CFGBuilder ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The x86/x86-64 x87 stack-pointer (TOP) fixup for CFGBuilder.  This is the
/// only architecture-gated routine in CFG construction (it returns immediately
/// unless the image is x86 or x86-64), so it lives here following the
/// target-dispatch split used by the jump-table detectors
/// (JumpTableResolverARM.cpp) rather than in the architecture-neutral
/// CFGBuilder.cpp.
///
/// The lifter names x87 registers as physical slots ST((TOP+i)&7) while
/// advancing TOP in worklist (lift) order.  When a branch leaves a value on the
/// FP stack and one arm net-changes the depth, the other arm is lifted with the
/// wrong TOP; fixupFpuStack re-bases each block's ST(i) references so its TOP
/// matches the control-flow predecessor's exit TOP.  For blocks reachable at
/// several distinct TOPs it rebuilds the CFG as the (block x entry-TOP) product
/// so each copy is single-TOP.  A no-op (per-block offset 0) for straight-line
/// or stack-balanced code, which is the overwhelmingly common case.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CFGBuilder.h"

#include "neverd/lift/X86Regs.h"

#include <map>
#include <queue>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

namespace {

bool isStReg(const NdVar &V) {
  return V.isReg() && V.Offset >= x86reg::ST0 && V.Offset <= x86reg::ST7 &&
         ((V.Offset - x86reg::ST0) % x86reg::FPURegStride == 0);
}

void rebaseStReg(NdVar &V, int Offset) {
  if (!isStReg(V))
    return;
  V.Offset = x86reg::stReg((x86reg::stRegIndex(V.Offset) + Offset) & 7);
}

// Sign-extend a per-instruction TOP change (range -2..+2) from its &7 form.
int demaskDelta(int D) {
  D &= 7;
  return D >= 4 ? D - 8 : D;
}

} // namespace

void CFGBuilder::fixupFpuStack(LowFunc &Func) {
  if (!CurrentImg)
    return;
  if (CurrentImg->Arch != Arch::X86 && CurrentImg->Arch != Arch::X64)
    return;
  if (Func.Blocks.empty())
    return;

  const size_t N = Func.Blocks.size();
  std::vector<int> LiftedIn(N, 0), LiftedOut(N, 0), BlockDelta(N, 0);
  std::vector<bool> HasReset(N, false), HasFpu(N, false);

  for (size_t I = 0; I < N; ++I) {
    auto &Blk = Func.Blocks[I];
    bool First = true;
    for (auto It = Insns.lower_bound(Blk.StartAddr);
         It != Insns.end() && It->first < Blk.EndAddr; ++It) {
      const InsnRecord &Rec = It->second;
      if (First) {
        LiftedIn[I] = Rec.FpuTopIn;
        First = false;
      }
      LiftedOut[I] = Rec.FpuTopOut;
      if (Rec.FpuReset)
        HasReset[I] = true;
      BlockDelta[I] += demaskDelta(Rec.FpuTopOut - Rec.FpuTopIn);
    }
    for (auto &Op : Blk.Ops) {
      if (isStReg(Op.Output)) {
        HasFpu[I] = true;
        break;
      }
      for (int K = 0; K < Op.NumInputs && !HasFpu[I]; ++K)
        if (isStReg(Op.Inputs[K]))
          HasFpu[I] = true;
      if (HasFpu[I])
        break;
    }
  }

  // No x87 anywhere: nothing to do (the overwhelmingly common case).
  bool AnyFpu = false;
  for (size_t I = 0; I < N; ++I)
    AnyFpu = AnyFpu || HasFpu[I];
  if (!AnyFpu)
    return;

  int EntryBlk = -1;
  for (size_t I = 0; I < N; ++I)
    if (Func.Blocks[I].StartAddr == Func.Entry) {
      EntryBlk = static_cast<int>(I);
      break;
    }
  if (EntryBlk < 0)
    return;

  // Normalized (0..7) exit TOP for block B entered at TOP T.  A reset block
  // (FNINIT/FNCLEX) restarts from its lifted exit regardless of the entry.
  auto exitTop = [&](int B, int T) {
    return (HasReset[B] ? LiftedOut[B] : (T + BlockDelta[B])) & 7;
  };

  // Propagate the control-flow-correct TOP along CFG edges as a *set* per
  // block: each block inherits every predecessor's exit TOP.  Straight-line /
  // stack- balanced code yields one TOP per block; a block reached at several
  // depths (a switch arm shared by a clang-peeled first iteration and the
  // steady loop body) yields several — x87 ST(i) is TOP-relative, so no single
  // re-base fits.
  std::vector<std::set<int>> TopSets(N);
  {
    std::queue<std::pair<int, int>> WL;
    TopSets[EntryBlk].insert(0);
    WL.push({EntryBlk, 0});
    size_t Steps = 0;
    const size_t StepCap = N * 8 + 16; // bounded: at most 8 TOPs per block
    while (!WL.empty()) {
      if (++Steps > StepCap)
        return; // pathological propagation: leave the function as lifted
      auto [B, T] = WL.front();
      WL.pop();
      int Ex = exitTop(B, T);
      for (int S : Func.Blocks[B].Succs) {
        if (S < 0 || S >= static_cast<int>(N))
          continue;
        if (TopSets[S].insert(Ex).second)
          WL.push({S, Ex});
      }
    }
  }

  bool MultiTop = false;
  for (size_t I = 0; I < N; ++I)
    if (TopSets[I].size() > 1) {
      MultiTop = true;
      break;
    }

  if (!MultiTop) {
    // One TOP per block — re-base each block in place (the common case; a no-op
    // for offset 0, i.e. straight-line / stack-balanced code).
    for (size_t I = 0; I < N; ++I) {
      if (TopSets[I].empty() || HasReset[I] || !HasFpu[I])
        continue;
      int Offset = (*TopSets[I].begin() - LiftedIn[I]) & 7;
      if (Offset == 0)
        continue;
      for (auto &Op : Func.Blocks[I].Ops) {
        rebaseStReg(Op.Output, Offset);
        for (int K = 0; K < Op.NumInputs; ++K)
          rebaseStReg(Op.Inputs[K], Offset);
      }
    }
    return;
  }

  // A block reached at several TOPs cannot be re-based by one offset, so
  // rebuild the CFG as the product (block × entry-TOP): each reached (B,t)
  // state becomes its own block re-based for that TOP.  Every copy is then
  // single-TOP, and the disjoint x87 phases get independent register mappings;
  // each successor edge is routed to the copy whose TOP matches the
  // predecessor's exit TOP.
  std::map<std::pair<int, int>, int> StateIdx;
  std::vector<std::pair<int, int>> States;
  auto addState = [&](int B, int T) {
    auto Key = std::make_pair(B, T);
    if (StateIdx.emplace(Key, static_cast<int>(States.size())).second)
      States.push_back(Key);
  };
  addState(EntryBlk, 0); // entry stays block 0
  for (size_t B = 0; B < N; ++B)
    for (int T : TopSets[B])
      addState(static_cast<int>(B), T);
  // Blocks unreachable in the propagation keep one copy so no block or edge is
  // dropped (they are dead, so the chosen TOP is immaterial).
  for (size_t B = 0; B < N; ++B)
    if (TopSets[B].empty())
      addState(static_cast<int>(B), LiftedIn[B] & 7);

  auto stateFor = [&](int S, int Ex) {
    int T = TopSets[S].count(Ex)  ? Ex
            : !TopSets[S].empty() ? *TopSets[S].begin()
                                  : (LiftedIn[S] & 7);
    auto It = StateIdx.find({S, T});
    return It != StateIdx.end() ? It->second : S;
  };

  std::vector<LowBlock> NewBlocks(States.size());
  for (size_t NI = 0; NI < States.size(); ++NI) {
    int B = States[NI].first, T = States[NI].second;
    LowBlock NB = Func.Blocks[B];
    NB.Id = static_cast<int>(NI);
    NB.Preds.clear();
    NB.Succs.clear();
    if (!HasReset[B] && HasFpu[B]) {
      int Offset = (T - LiftedIn[B]) & 7;
      if (Offset != 0)
        for (auto &Op : NB.Ops) {
          rebaseStReg(Op.Output, Offset);
          for (int K = 0; K < Op.NumInputs; ++K)
            rebaseStReg(Op.Inputs[K], Offset);
        }
    }
    int Ex = exitTop(B, T);
    for (int S : Func.Blocks[B].Succs)
      NB.Succs.push_back(S >= 0 && S < static_cast<int>(N) ? stateFor(S, Ex)
                                                           : S);
    NewBlocks[NI] = std::move(NB);
  }
  for (size_t NI = 0; NI < NewBlocks.size(); ++NI)
    for (int S : NewBlocks[NI].Succs)
      if (S >= 0 && S < static_cast<int>(NewBlocks.size()))
        NewBlocks[S].Preds.push_back(static_cast<int>(NI));
  Func.Blocks = std::move(NewBlocks);
}

} // namespace neverd
