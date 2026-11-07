//===- LowToMedCallReturnFP.cpp - Floating-point call returns -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Models floating-point call returns carried in vector registers.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedCallClobber.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {
namespace {
using ValueKey = std::tuple<int, int, int, uint16_t>;
ValueKey valueKey(const MedVar &V) {
  return {static_cast<int>(V.Kind), V.Id, V.SSAVer, V.Size};
}

// A use in real control flow is stronger result evidence than merely saving
// another volatile register. Follow SSA values, including views and joins;
// an unrelated later call's result must not claim an earlier call's output.
// RETURN is deliberately not a root: the lifter's default integer return
// operand alone cannot establish an integer result for a floating function.
std::set<ValueKey> valuesFeedingControl(const MedFunc &Func) {
  std::map<ValueKey, std::vector<MedVar>> Inputs;
  std::vector<MedVar> Work;
  for (const auto &Block : Func.Blocks) {
    for (const auto &Phi : Block.Phis) {
      auto &Incoming = Inputs[valueKey(Phi.Output)];
      for (const auto &[Pred, Value] : Phi.Args)
        Incoming.push_back(Value);
      if (Phi.ExceptionalEntry)
        Incoming.push_back(*Phi.ExceptionalEntry);
    }
    for (const auto &Op : Block.Ops) {
      if (Op.Dead)
        continue;
      if (Op.Opcode == NdOp::COND_BR)
        Work.insert(Work.end(), Op.Inputs.begin(),
                    Op.Inputs.begin() + Op.NumInputs);
      if (Op.Output.Size && !Op.Output.isConst() && Op.Opcode != NdOp::CALL &&
          Op.Opcode != NdOp::INDIR_CALL)
        Inputs[valueKey(Op.Output)].assign(Op.Inputs.begin(),
                                           Op.Inputs.begin() + Op.NumInputs);
    }
  }
  std::set<ValueKey> Seen;
  while (!Work.empty()) {
    const auto V = Work.back();
    Work.pop_back();
    if (V.isConst() || !Seen.insert(valueKey(V)).second)
      continue;
    if (const auto It = Inputs.find(valueKey(V)); It != Inputs.end())
      Work.insert(Work.end(), It->second.begin(), It->second.end());
  }
  return Seen;
}
} // namespace

void LowToMedConverter::modelCallFPReturn(MedFunc &Func) {
  const auto &TRI = getTargetRegInfo(TargetArch);
  const uint64_t FPRet = TRI.FPReturnReg;
  // Only architectures that return floating point in a vector register (x86-64
  // XMM0) need this: where the lifter models the FP return in the integer
  // return register (ARM/AArch64), the existing integer-return call output
  // already carries it.
  if (FPRet == 0 || !TRI.isVectorReg(FPRet))
    return;

  // The widest reference to the FP return register is its full vector form; its
  // Id is shared by every SSA version, keying the reads that must be rewired.
  int VecId = -1;
  uint16_t VecSize = 0;
  auto consider = [&](const MedVar &V) {
    if (V.Kind == MedVar::Reg && V.RegOff == FPRet && V.Size > VecSize) {
      VecSize = V.Size;
      VecId = V.Id;
    }
  };
  for (const auto &B : Func.Blocks) {
    for (const auto &Op : B.Ops) {
      consider(Op.Output);
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        consider(Op.Inputs[I]);
    }
    for (const auto &Phi : B.Phis) {
      consider(Phi.Output);
      for (const auto &A : Phi.Args)
        consider(A.second);
    }
  }
  if (VecId < 0)
    return; // the FP return register is never referenced anywhere

  const auto ControlValues = TargetFormat == BinaryFormat::COFF
                                 ? valuesFeedingControl(Func)
                                 : std::set<ValueKey>{};

  // The architectural container may be wider than the ABI carrier.  In
  // particular, x86 AVX-512 aliases XMM0 with a 64-byte ZMM0 container, but a
  // scalar/vector function result still arrives in the low 128-bit XMM view.
  // Publishing a full ZMM call output would turn ordinary calls into an
  // invented i512 return convention.
  const uint16_t FPReturnWidth =
      TRI.FPABIRegWidth != 0 ? std::min(VecSize, TRI.FPABIRegWidth) : VecSize;

  int MaxVer = 0;
  for (const auto &B : Func.Blocks) {
    for (const auto &Op : B.Ops)
      MaxVer = std::max(MaxVer, Op.Output.SSAVer);
    for (const auto &Phi : B.Phis)
      MaxVer = std::max(MaxVer, Phi.Output.SSAVer);
  }
  int NextVer = MaxVer + 1;

  // A SUBBYTES reading the FP-return register's wide vector form and writing a
  // narrower sub-register of it is an extraction/view of the current value —
  // the wide value is unchanged — not a redefinition.  The lifter emits one
  // such extraction per use (e.g. the Z/N/V flags of a post-call `fcmp`), so
  // the straight-line rewrite must look past them to reach every later read of
  // the call result; stopping at the first would leave the rest reading the
  // stale pre-call value (the call's own FP argument).
  auto isFPRetExtraction = [&](const MedOp &Op) -> bool {
    if (Op.Opcode != NdOp::SUBBYTES || Op.Output.Kind != MedVar::Reg ||
        Op.Output.RegOff != FPRet)
      return false;
    for (uint8_t I = 0; I < Op.NumInputs; ++I)
      if (Op.Inputs[I].Kind == MedVar::Reg && Op.Inputs[I].RegOff == FPRet &&
          Op.Inputs[I].Id == VecId)
        return true;
    return false;
  };

  // The blocks of each id, in block order, which the searches below read as
  // a scan of the blocks finds them: a function's calls each ask, so a scan
  // per call made large functions quadratic.
  std::map<int, std::vector<MedBlock *>> BlocksById;
  for (auto &B : Func.Blocks)
    BlocksById[B.Id].push_back(&B);
  auto firstBlock = [&](int Id) -> MedBlock * {
    const auto Found = BlocksById.find(Id);
    return Found == BlocksById.end() ? nullptr : Found->second.front();
  };
  std::map<int, const MedBlock *> BlockById;
  for (const auto &[Id, Blocks] : BlocksById)
    BlockById.emplace(Id, Blocks.front());
  auto succHasFPRetPhi = [&](int BlockId) -> bool {
    auto Cur = BlockById.find(BlockId);
    if (Cur == BlockById.end())
      return false;
    std::set<int> Visited;
    for (int S : Cur->second->Succs) {
      if (!Visited.insert(S).second)
        continue;
      auto Succ = BlockById.find(S);
      if (Succ == BlockById.end())
        continue;
      for (const auto &Phi : Succ->second->Phis)
        if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == FPRet)
          for (const auto &A : Phi.Args)
            if (A.first == BlockId)
              return true;
    }
    return false;
  };

  // Self-zeroing of the FP-return register (`xorps xmm0,xmm0` / `subss x,x`):
  // it reads the register only to discard it (result is 0), so it is a
  // redefinition, never a genuine consumption of a carried value.
  auto isFPRetSelfZero = [&](const MedOp &Op) -> bool {
    return (Op.Opcode == NdOp::INT_XOR || Op.Opcode == NdOp::INT_SUB) &&
           Op.NumInputs == 2 && Op.Inputs[0].Kind == MedVar::Reg &&
           Op.Inputs[0].RegOff == FPRet && Op.Inputs[1].Kind == MedVar::Reg &&
           Op.Inputs[1].RegOff == FPRet && Op.Inputs[0].Id == Op.Inputs[1].Id &&
           Op.Inputs[0].SSAVer == Op.Inputs[1].SSAVer;
  };

  // SSA publishes the authoritative caller-saved definitions per exact call
  // site.  Use that record instead of treating every call as a barrier: stack
  // probes and other PreservesCallerSaved calls deliberately have no clobber,
  // while both direct and indirect ordinary calls do.  A call may still read
  // the previous FP value as an explicit argument; callers inspect inputs
  // before applying this boundary.
  std::set<uint32_t> FPRetClobberSites;
  for (const MedCallClobber &Clobber : Func.CallClobbers)
    if (Clobber.Value.Kind == MedVar::Reg && Clobber.Value.RegOff == FPRet)
      FPRetClobberSites.insert(Clobber.CallSiteId);
  auto clobbersFPRet = [&](const MedOp &Op) -> bool {
    return !Op.PreservesCallerSaved && Op.CallSiteId != 0 &&
           FPRetClobberSites.count(Op.CallSiteId) != 0;
  };

  // A successor FP-return PHI fed by \p BlockId is the loop-carried result of
  // this call only when its value is GENUINELY consumed: read by a
  // non-self-zero op before the register is redefined in that successor.  A
  // loop where the carried FP value is merely self-zeroed each iteration (the
  // caller recomputes XMM0 to form the next call's FP argument, while the call
  // actually returns an integer in the int register) leaves the PHI dead —
  // routing the call's output there would steal it from the live integer return
  // and drop the result.
  auto fpRetPhiLiveInSucc = [&](int BlockId) -> bool {
    std::set<int> SuccIds;
    if (const auto Found = BlocksById.find(BlockId); Found != BlocksById.end())
      for (const MedBlock *Cur : Found->second)
        SuccIds.insert(Cur->Succs.begin(), Cur->Succs.end());
    for (const auto &B : Func.Blocks) {
      if (!SuccIds.count(B.Id))
        continue;
      for (const auto &Phi : B.Phis) {
        if (Phi.Output.Kind != MedVar::Reg || Phi.Output.RegOff != FPRet)
          continue;
        bool FedByBlock = false;
        for (const auto &A : Phi.Args)
          if (A.first == BlockId)
            FedByBlock = true;
        if (!FedByBlock)
          continue;
        const MedVar &PV = Phi.Output;
        for (const auto &Nx : B.Ops) {
          if (!isFPRetSelfZero(Nx))
            for (uint8_t I = 0; I < Nx.NumInputs; ++I)
              if (Nx.Inputs[I].Kind == MedVar::Reg &&
                  Nx.Inputs[I].RegOff == FPRet && Nx.Inputs[I].Id == PV.Id &&
                  Nx.Inputs[I].SSAVer == PV.SSAVer)
                return true; // genuine consumer of the carried FP value
          if (clobbersFPRet(Nx))
            break;
          if (Nx.Output.Kind == MedVar::Reg && Nx.Output.RegOff == FPRet)
            break; // redefined before any genuine read
        }
      }
    }
    return false;
  };

  // The FP result is consumed when a successor block the call dominates reads
  // the FP-return register directly (not straight-line in the call block, not
  // via a PHI) — e.g. the result feeds only one arm of `cond ? h(r) : r*2` or a
  // post-branch use.  Such consumption is invisible to ReadAfter and the PHI
  // probe, so without it the call would not be routed and the cross-block read
  // resolves to the stale pre-call register value.  Bounded to single-pred
  // successors (provably dominated); a value carried through a PHI is the
  // succHasFPRetPhi path instead.
  auto fpRetReadInDominatedSucc = [&](int BlockId) -> bool {
    std::set<int> Seen{BlockId};
    std::vector<int> Work;
    if (const auto Found = BlocksById.find(BlockId); Found != BlocksById.end())
      Work.assign(Found->second.back()->Succs.begin(),
                  Found->second.back()->Succs.end());
    while (!Work.empty()) {
      int BId = Work.back();
      Work.pop_back();
      if (!Seen.insert(BId).second)
        continue;
      const MedBlock *SB = firstBlock(BId);
      if (!SB || SB->Preds.size() != 1)
        continue;
      bool HasPhi = false;
      for (const auto &Phi : SB->Phis)
        if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == FPRet)
          HasPhi = true;
      if (HasPhi)
        continue;
      bool Redef = false;
      for (const auto &Nx : SB->Ops) {
        if (!isFPRetSelfZero(Nx))
          for (uint8_t I = 0; I < Nx.NumInputs; ++I)
            if (Nx.Inputs[I].Kind == MedVar::Reg &&
                Nx.Inputs[I].RegOff == FPRet)
              return true;
        if (clobbersFPRet(Nx)) {
          Redef = true;
          break;
        }
        if (Nx.Output.Kind == MedVar::Reg && Nx.Output.RegOff == FPRet) {
          Redef = true;
          break;
        }
      }
      if (!Redef)
        Work.insert(Work.end(), SB->Succs.begin(), SB->Succs.end());
    }
    return false;
  };

  for (auto &Blk : Func.Blocks) {
    for (size_t OI = 0; OI < Blk.Ops.size(); ++OI) {
      auto &Op = Blk.Ops[OI];
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;
      if (Op.PreservesCallerSaved || Op.SourceCallHint)
        continue;
      if (Op.Output.Kind == MedVar::Reg && TRI.isX87StackReg(Op.Output.RegOff))
        continue; // LowIR already bound the proven x87 result.
      if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == FPRet)
        continue; // SSA already bound every reader to this FP result.
      // A call already remodeled as a multi-register struct return (its output
      // is the flat aggregate temp, its FP return register claimed by an
      // extract op) is handled there — do not also route its FP register here.
      // A call with no output at all (a callee that leaves the integer return
      // register untouched) may still return in the FP register.
      if (Op.Output.Kind == MedVar::Temp && Op.Output.Id >= 0 &&
          Op.Output.Size > 0)
        continue;
      // A summarized callee that provably never writes the FP return register
      // returns nothing there: the register keeps the caller's value, which
      // is also what a callee passing its argument straight back returns.
      if (callPreservesRegisterBytes(TargetArch, Op.CallPreservedGPRs, FPRet,
                                     FPReturnWidth))
        continue;

      // Is the call's result consumed via the FP return register?  Either a
      // straight-line read in this block before the register is redefined, or
      // (for a loop-carried result) a successor PHI on this block's edge — both
      // observed before the register is redefined after the call.
      bool ReadAfter = false, RedefAfter = false;
      for (size_t J = OI + 1; J < Blk.Ops.size(); ++J) {
        auto &Nx = Blk.Ops[J];
        // `xorps xmm0,xmm0` / `subss x,x` zeroes the register: it reads FPRet
        // only to discard it (result is 0, independent of the value), so it is
        // a redefinition, not a consumption of the call's FP return.  An
        // integer call followed by the caller zeroing XMM0 for its own FP
        // scratch must NOT be mistaken for an FP-returning call (which would
        // drop the real integer return in RAX).
        bool SelfZero =
            (Nx.Opcode == NdOp::INT_XOR || Nx.Opcode == NdOp::INT_SUB) &&
            Nx.NumInputs == 2 && Nx.Inputs[0].Kind == MedVar::Reg &&
            Nx.Inputs[0].RegOff == FPRet && Nx.Inputs[1].Kind == MedVar::Reg &&
            Nx.Inputs[1].RegOff == FPRet &&
            Nx.Inputs[0].Id == Nx.Inputs[1].Id &&
            Nx.Inputs[0].SSAVer == Nx.Inputs[1].SSAVer;
        if (!SelfZero)
          for (uint8_t I = 0; I < Nx.NumInputs; ++I)
            if (Nx.Inputs[I].Kind == MedVar::Reg &&
                Nx.Inputs[I].RegOff == FPRet) {
              ReadAfter = true;
              break;
            }
        if (ReadAfter)
          break;
        if (clobbersFPRet(Nx)) {
          RedefAfter = true;
          break;
        }
        if (Nx.Output.Kind == MedVar::Reg && Nx.Output.RegOff == FPRet) {
          RedefAfter = true;
          break;
        }
      }
      bool Consumed =
          ReadAfter ||
          (!RedefAfter &&
           (fpRetReadInDominatedSucc(Blk.Id) ||
            (succHasFPRetPhi(Blk.Id) && fpRetPhiLiveInSucc(Blk.Id))));
      if (!Consumed)
        continue;

      // An ordinary Win64 call has one result carrier. Keep the existing
      // integer projection when that exact value feeds control flow; the
      // vector's SSA clobber still supplies every post-call read. A declared
      // scalar FP result retains its contract, and other conventions retain
      // their aggregate/independent-register recovery above.
      if (TargetArch == Arch::X64 && TargetFormat == BinaryFormat::COFF &&
          !Op.CalleeFloatReturnBytes && Op.Output.Kind == MedVar::Reg &&
          Op.Output.RegOff == TRI.IntReturnReg &&
          ControlValues.count(valueKey(Op.Output)))
        continue;

      MedVar Out;
      const auto Existing = std::find_if(
          Func.CallClobbers.begin(), Func.CallClobbers.end(),
          [&](const MedCallClobber &Clobber) {
            return Op.CallSiteId && Clobber.CallSiteId == Op.CallSiteId &&
                   Clobber.Value.Kind == MedVar::Reg &&
                   Clobber.Value.RegOff == FPRet && Clobber.Value.Id == VecId &&
                   Clobber.Value.Size == FPReturnWidth &&
                   Clobber.PreservedPrefixSize == 0;
          });
      if (Existing != Func.CallClobbers.end()) {
        // This is the same post-call value SSA already gave every path.
        // A fresh version would leave joins reading a second, independent
        // observation of the very register now used as the call's result.
        Out = Existing->Value;
        Func.CallClobbers.erase(Existing);
      } else {
        Out.Kind = MedVar::Reg;
        Out.Id = VecId;
        Out.Size = FPReturnWidth;
        Out.RegOff = FPRet;
        Out.SSAVer = NextVer++;
        Out.TheArch = TargetArch;
      }
      const int NewVer = Out.SSAVer;
      keepReplacedCallOutput(Func, Op, Op.Output);
      Op.Output = Out;

      // Straight-line reads after the call observe the result until a redef.
      // Every post-call read of the FP-return register — whether the wide
      // vector form (Id == VecId) or a narrow sub-register view (a separate Id,
      // e.g. a loop-carried `D0` accumulator PHI read directly without a
      // SUBBYTES) — is the call result, so point each at the call's wide output
      // version; getVar narrows it to the read's width.
      bool LaterRedef = false;
      for (size_t J = OI + 1; J < Blk.Ops.size(); ++J) {
        auto &Nx = Blk.Ops[J];
        for (uint8_t I = 0; I < Nx.NumInputs; ++I)
          if (Nx.Inputs[I].Kind == MedVar::Reg &&
              Nx.Inputs[I].RegOff == FPRet) {
            Nx.Inputs[I].Id = VecId;
            Nx.Inputs[I].SSAVer = NewVer;
          }
        if (clobbersFPRet(Nx)) {
          LaterRedef = true;
          break;
        }
        if (Nx.Output.Kind == MedVar::Reg && Nx.Output.RegOff == FPRet &&
            !isFPRetExtraction(Nx)) {
          LaterRedef = true;
          break;
        }
      }

      // Cross-block straight-line reads: when the FP return is still live at
      // the call block's end (no same-block redef), its value reaches reads in
      // the successor blocks the call dominates — e.g. an `if(r>x) ... else
      // if(r<y)` whose else branch compares the FP return in a separate block.
      // Without rewiring those, the cross-block read resolves to the stale
      // pre-call register value (the call's own FP argument).  Bounded to
      // single- predecessor successors so the call block provably dominates
      // them; a merge point carries the value through its FPRet PHI (handled
      // below).
      if (!LaterRedef) {
        std::set<int> Seen{Blk.Id};
        std::vector<int> Work(Blk.Succs.begin(), Blk.Succs.end());
        while (!Work.empty()) {
          int BId = Work.back();
          Work.pop_back();
          if (!Seen.insert(BId).second)
            continue;
          MedBlock *SB = firstBlock(BId);
          if (!SB || SB->Preds.size() != 1)
            continue; // not dominated solely by the call chain
          bool HasFPRetPhi = false;
          for (const auto &Phi : SB->Phis)
            if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == FPRet)
              HasFPRetPhi = true;
          if (HasFPRetPhi)
            continue; // the PHI (updated below) carries the value
          bool Redef = false;
          for (auto &Nx : SB->Ops) {
            for (uint8_t I = 0; I < Nx.NumInputs; ++I)
              if (Nx.Inputs[I].Kind == MedVar::Reg &&
                  Nx.Inputs[I].RegOff == FPRet) {
                Nx.Inputs[I].Id = VecId;
                Nx.Inputs[I].SSAVer = NewVer;
              }
            if (clobbersFPRet(Nx)) {
              Redef = true;
              break;
            }
            if (Nx.Output.Kind == MedVar::Reg && Nx.Output.RegOff == FPRet &&
                !isFPRetExtraction(Nx)) {
              Redef = true;
              break;
            }
          }
          if (!Redef)
            Work.insert(Work.end(), SB->Succs.begin(), SB->Succs.end());
        }
      }

      // Loop-carried / cross-edge result: update successor PHI arguments on
      // this block's edge to the call's result version — but only when the call
      // result is the block's *final* value of the FP return register.  If a
      // later op redefines it (e.g. the result is folded into a spilled
      // accumulator that is reloaded back into XMM0 before the backedge), that
      // later definition is the loop-carried value; overriding the PHI arg to
      // the call version here would drop the accumulation.
      // The PHI arg may name the wide vector form (Id == VecId) or a narrow
      // sub-register view of the FP return (a separate Id, e.g. an 8-byte `D0`
      // accumulator PHI merged at `acc += cond ? h(x) : ...`); point either at
      // the call's wide output version, which getVar narrows to the arg's
      // width.
      if (!LaterRedef)
        for (int Succ : Blk.Succs) {
          const auto Found = BlocksById.find(Succ);
          if (Found == BlocksById.end())
            continue;
          for (MedBlock *B : Found->second) {
            for (auto &Phi : B->Phis)
              if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == FPRet)
                for (auto &A : Phi.Args)
                  if (A.first == Blk.Id && A.second.Kind == MedVar::Reg &&
                      A.second.RegOff == FPRet) {
                    A.second.Id = VecId;
                    A.second.SSAVer = NewVer;
                  }
          }
        }
    }
  }
}

} // namespace neverd
