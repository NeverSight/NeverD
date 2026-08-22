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
#include "neverd/ir/med/LowToMed.h"

#include <algorithm>
#include <set>
#include <vector>

namespace neverd {

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
  auto isCall = [](const MedOp &Op) {
    return Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL;
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

  auto succHasFPRetPhi = [&](int BlockId) -> bool {
    for (const auto &B : Func.Blocks) {
      bool IsSucc = false;
      for (const auto &Cur : Func.Blocks)
        if (Cur.Id == BlockId)
          for (int S : Cur.Succs)
            if (S == B.Id)
              IsSucc = true;
      if (!IsSucc)
        continue;
      for (const auto &Phi : B.Phis)
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

  // A successor FP-return PHI fed by \p BlockId is the loop-carried result of
  // this call only when its value is GENUINELY consumed: read by a
  // non-self-zero op before the register is redefined in that successor.  A
  // loop where the carried FP value is merely self-zeroed each iteration (the
  // caller recomputes XMM0 to form the next call's FP argument, while the call
  // actually returns an integer in the int register) leaves the PHI dead —
  // routing the call's output there would steal it from the live integer return
  // and drop the result.
  auto fpRetPhiLiveInSucc = [&](int BlockId) -> bool {
    for (const auto &B : Func.Blocks) {
      bool IsSucc = false;
      for (const auto &Cur : Func.Blocks)
        if (Cur.Id == BlockId)
          for (int S : Cur.Succs)
            if (S == B.Id)
              IsSucc = true;
      if (!IsSucc)
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
          if (isCall(Nx))
            break;
          if (!isFPRetSelfZero(Nx))
            for (uint8_t I = 0; I < Nx.NumInputs; ++I)
              if (Nx.Inputs[I].Kind == MedVar::Reg &&
                  Nx.Inputs[I].RegOff == FPRet && Nx.Inputs[I].Id == PV.Id &&
                  Nx.Inputs[I].SSAVer == PV.SSAVer)
                return true; // genuine consumer of the carried FP value
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
    for (const auto &Cur : Func.Blocks)
      if (Cur.Id == BlockId)
        Work.assign(Cur.Succs.begin(), Cur.Succs.end());
    while (!Work.empty()) {
      int BId = Work.back();
      Work.pop_back();
      if (!Seen.insert(BId).second)
        continue;
      const MedBlock *SB = nullptr;
      for (const auto &B : Func.Blocks)
        if (B.Id == BId) {
          SB = &B;
          break;
        }
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
        if (isCall(Nx)) {
          Redef = true;
          break;
        }
        if (!isFPRetSelfZero(Nx))
          for (uint8_t I = 0; I < Nx.NumInputs; ++I)
            if (Nx.Inputs[I].Kind == MedVar::Reg &&
                Nx.Inputs[I].RegOff == FPRet)
              return true;
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
      // A call already remodeled as a multi-register struct return (its output
      // is the flat aggregate temp, its FP return register claimed by an
      // extract op) is handled there — do not also route its FP register here.
      if (Op.Output.Kind == MedVar::Temp)
        continue;

      // Is the call's result consumed via the FP return register?  Either a
      // straight-line read in this block before the register is redefined, or
      // (for a loop-carried result) a successor PHI on this block's edge — both
      // observed before the register is redefined after the call.
      bool ReadAfter = false, RedefAfter = false;
      for (size_t J = OI + 1; J < Blk.Ops.size(); ++J) {
        auto &Nx = Blk.Ops[J];
        if (isCall(Nx))
          break;
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

      int NewVer = NextVer++;
      MedVar Out;
      Out.Kind = MedVar::Reg;
      Out.Id = VecId;
      Out.Size = VecSize;
      Out.RegOff = FPRet;
      Out.SSAVer = NewVer;
      Out.TheArch = TargetArch;
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
        if (isCall(Nx)) {
          LaterRedef = true;
          break;
        }
        for (uint8_t I = 0; I < Nx.NumInputs; ++I)
          if (Nx.Inputs[I].Kind == MedVar::Reg &&
              Nx.Inputs[I].RegOff == FPRet) {
            Nx.Inputs[I].Id = VecId;
            Nx.Inputs[I].SSAVer = NewVer;
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
          MedBlock *SB = nullptr;
          for (auto &B : Func.Blocks)
            if (B.Id == BId) {
              SB = &B;
              break;
            }
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
            if (isCall(Nx)) {
              Redef = true;
              break;
            }
            for (uint8_t I = 0; I < Nx.NumInputs; ++I)
              if (Nx.Inputs[I].Kind == MedVar::Reg &&
                  Nx.Inputs[I].RegOff == FPRet) {
                Nx.Inputs[I].Id = VecId;
                Nx.Inputs[I].SSAVer = NewVer;
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
        for (int Succ : Blk.Succs)
          for (auto &B : Func.Blocks) {
            if (B.Id != Succ)
              continue;
            for (auto &Phi : B.Phis)
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

} // namespace neverd
