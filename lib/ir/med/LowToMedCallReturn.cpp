//===- LowToMedCallReturn.cpp - Call return-value ABI modeling ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Call return-value ABI modeling for the LowIR to MedIR conversion: recovering
/// how a call's return value is delivered (floating-point registers, split
/// register pairs for wide/struct returns, the x87 stack) so the following SSA
/// passes see a single well-typed def.  The conversion framework itself lives
/// in LowToMed.cpp and drives these from LowToMedConverter::convert.
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

#define DEBUG_TYPE "neverd-low-to-med-callret"

namespace neverd {

//===----------------------------------------------------------------------===//
// Call return-value ABI modeling
//===----------------------------------------------------------------------===//

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

void LowToMedConverter::modelCallStructReturn(MedFunc &Func) {
  const auto &TRI = getTargetRegInfo(TargetArch);
  auto isCallClobber = [&](const MedVar &V, uint32_t CallSiteId) {
    return std::any_of(Func.CallClobbers.begin(), Func.CallClobbers.end(),
                       [&](const MedCallClobber &Clobber) {
                         return Clobber.CallSiteId == CallSiteId &&
                                Clobber.Value == V;
                       });
  };
  // 64-bit targets only: x86-64 SysV and AArch64 return a small struct by value
  // in registers.  32-bit targets return such aggregates through the sret
  // pointer or the wide-int register pair (handled by modelCallWideIntReturn).
  if (TRI.PointerSize != 8)
    return;
  if (TRI.IntReturnRegs.empty() && TRI.FPReturnRegs.empty())
    return;

  // Candidate return registers in canonical aggregate-field order: the integer
  // pair first (RAX/RDX, X0/X1), then the FP/vector registers (XMM0/XMM1,
  // V0..V3).  A field's position here is the order the LLVM struct return type
  // is built in, which the backend's ABI lowering maps back to the registers.
  struct Cand {
    uint64_t RegOff;
    bool IsFP;
  };
  std::vector<Cand> Cands;
  for (uint64_t R : TRI.IntReturnRegs)
    Cands.push_back({R, false});
  for (uint64_t R : TRI.FPReturnRegs)
    Cands.push_back({R, true});
  auto candIdx = [&](uint64_t RegOff) -> int {
    for (size_t K = 0; K < Cands.size(); ++K)
      if (Cands[K].RegOff == RegOff)
        return static_cast<int>(K);
    return 1000;
  };

  int MaxVer = 0, MaxId = 0;
  auto bump = [&](const MedVar &V) {
    MaxVer = std::max(MaxVer, V.SSAVer);
    MaxId = std::max(MaxId, V.Id);
  };
  for (const auto &B : Func.Blocks) {
    for (const auto &Op : B.Ops) {
      bump(Op.Output);
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        bump(Op.Inputs[I]);
    }
    for (const auto &Phi : B.Phis) {
      bump(Phi.Output);
      for (const auto &A : Phi.Args)
        bump(A.second);
    }
  }
  int NextVer = MaxVer + 1, NextId = MaxId + 1;

  for (auto &Blk : Func.Blocks) {
    for (size_t OI = 0; OI < Blk.Ops.size(); ++OI) {
      auto &Op = Blk.Ops[OI];
      // A direct or indirect call whose result is consumed across multiple
      // return registers.  The CALL-SITE remodel is identical for both: the
      // caller's straight-line reads of x0:x1 (or v0:v1) prove the
      // multi-register return regardless of how the target is reached.  A
      // direct call's callee is re-typed via the Pipeline (resolvable in-module
      // target); an indirect call's unknown callee is re-typed from its own
      // body (recoverMultiReturnFromBody), so this remodel only rewrites the
      // call site.
      if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
          Op.NumInputs < 1)
        continue;
      // A call already remodeled (FP/x87/wide produce a non-register output) is
      // left alone; the default-modeled call writes the integer return
      // register.
      if (Op.Output.Kind != MedVar::Reg)
        continue;

      // Which candidate return registers does the caller read straight-line
      // after the call (a genuine input) before that register is redefined?
      struct FieldRead {
        uint64_t RegOff;
        bool IsFP;
        uint16_t Size;
        MedVar Value;
      };
      std::vector<FieldRead> Fields;
      std::set<uint64_t> Redefined;
      auto already = [&](uint64_t R) {
        for (auto &F : Fields)
          if (F.RegOff == R)
            return true;
        return false;
      };
      for (size_t J = OI + 1; J < Blk.Ops.size(); ++J) {
        auto &Nx = Blk.Ops[J];
        if (Nx.Opcode == NdOp::CALL || Nx.Opcode == NdOp::INDIR_CALL)
          break;
        // A self-zeroing idiom (`xorps x,x` / `sub x,x`) reads its register
        // only to discard it — the result is 0, independent of the value — so
        // it redefines the register rather than consuming the call's result. An
        // int call followed by the caller zeroing XMM0 for its own FP scratch
        // must not make XMM0 look like a second struct-return field (#469).
        bool SelfZero =
            (Nx.Opcode == NdOp::INT_XOR || Nx.Opcode == NdOp::INT_SUB) &&
            Nx.NumInputs == 2 && Nx.Inputs[0].Kind == MedVar::Reg &&
            Nx.Inputs[1].Kind == MedVar::Reg &&
            Nx.Inputs[0].RegOff == Nx.Inputs[1].RegOff &&
            Nx.Inputs[0].Id == Nx.Inputs[1].Id &&
            Nx.Inputs[0].SSAVer == Nx.Inputs[1].SSAVer;
        if (!SelfZero)
          for (uint8_t I = 0; I < Nx.NumInputs; ++I) {
            const auto &In = Nx.Inputs[I];
            if (In.Kind != MedVar::Reg)
              continue;
            int CI = candIdx(In.RegOff);
            if (CI < 1000 && !Redefined.count(In.RegOff) && !already(In.RegOff))
              Fields.push_back({In.RegOff, Cands[CI].IsFP, In.Size, In});
          }
        if (Nx.Output.Kind == MedVar::Reg && candIdx(Nx.Output.RegOff) < 1000)
          Redefined.insert(Nx.Output.RegOff);
      }
      // AArch64 __int128 returned in the X0:X1 GP pair: clang -O0 moves it into
      // V0 for storage (`mov v0.d[0],x0; mov v0.d[1],x1; str q0`).  The lifter
      // models each lane insert as reading the old V0 (to preserve the
      // untouched lane) and writing V0 back, so the post-call scan above
      // records a stale V0 read as an FP field — making the pure-GP pair look
      // like a mixed int+FP shape that the AArch64 reject below would drop,
      // losing the real X0:X1 return.  Distinguish this re-housing from a
      // genuine returned HFA field by data flow: an FP register that is WRITTEN
      // by a value assembled from an integer return register (`mov v0.d[k],
      // xN`) is not an independent field — the X0:X1 pair is.  A real HFA
      // field, even when clang spills and reloads it (`str d1; ldr
      // d1,[frame]`), is rewritten only from memory (an untainted LOAD), never
      // from a return register.  Compute the values tainted by an integer
      // return register across the straight-line region, then drop any FP field
      // whose register is written by a tainted value. AArch64 only: x86-64 SysV
      // genuinely returns mixed int+SSE aggregates and does not use this
      // GP→vector idiom.
      if (TargetArch == Arch::AArch64) {
        auto isIntCandReg = [&](const MedVar &V) {
          if (V.Kind != MedVar::Reg)
            return false;
          int CI = candIdx(V.RegOff);
          return CI < 1000 && !Cands[CI].IsFP;
        };
        // (Id, SSAVer) of values derived from an integer return register.  A
        // single forward pass over the straight-line region suffices: the
        // lifter emits each value's definition before its uses.
        std::set<std::pair<int, int>> Tainted;
        std::set<uint64_t> Assembled; // FP regs written from a tainted value
        for (size_t J = OI + 1; J < Blk.Ops.size(); ++J) {
          auto &Nx = Blk.Ops[J];
          if (Nx.Opcode == NdOp::CALL || Nx.Opcode == NdOp::INDIR_CALL)
            break;
          bool InTaint = false;
          for (uint8_t I = 0; I < Nx.NumInputs; ++I) {
            const auto &In = Nx.Inputs[I];
            if (isIntCandReg(In) ||
                (In.Kind != MedVar::Const && Tainted.count({In.Id, In.SSAVer})))
              InTaint = true;
          }
          if (!InTaint)
            continue;
          if (Nx.Output.Kind == MedVar::Reg || Nx.Output.Kind == MedVar::Temp ||
              Nx.Output.Kind == MedVar::Stack) {
            Tainted.insert({Nx.Output.Id, Nx.Output.SSAVer});
            if (Nx.Output.Kind == MedVar::Reg &&
                candIdx(Nx.Output.RegOff) < 1000)
              Assembled.insert(Nx.Output.RegOff);
          }
        }
        Fields.erase(std::remove_if(Fields.begin(), Fields.end(),
                                    [&](const FieldRead &F) {
                                      return F.IsFP &&
                                             Assembled.count(F.RegOff);
                                    }),
                     Fields.end());
      }
      if (Fields.size() < 2)
        continue;
      bool AnyFP = false, AnyInt = false;
      for (auto &F : Fields) {
        if (F.IsFP)
          AnyFP = true;
        else
          AnyInt = true;
      }
      // x86-64 all-integer pair {RAX,RDX} is a 2-eightbyte struct return
      // (`struct{int*;int}`).  The div/mul byproduct concern does not apply to
      // a genuine field read: a divide's `cqo` WRITES RDX (a redefine that
      // drops it from Fields) before any read, so an un-redefined RDX read
      // straight after the call is the second eightbyte.  AArch64's {X0,X1} is
      // likewise safe. AArch64 returns a small struct either entirely in GP
      // registers (a non-HFA aggregate -> X0,X1) or entirely in V registers (an
      // HFA -> V0..V3) — never mixed.  A mixed int+FP read after the call is
      // therefore NOT a struct return: it is a plain FP-returning callee whose
      // lifted call also carries the lifter's dead integer-return-register
      // placeholder. Rejecting the mixed shape keeps such single-FP returns
      // (incl. recursive FP callees) on the scalar path.
      if (TargetArch == Arch::AArch64 && AnyFP && AnyInt)
        continue;

      // Canonical field order (int regs then fp regs).
      std::sort(Fields.begin(), Fields.end(),
                [&](const FieldRead &A, const FieldRead &B) {
                  return candIdx(A.RegOff) < candIdx(B.RegOff);
                });

      // Field sizes: an x86-64 SSE eightbyte is always 8 bytes (XMM0 may pack
      // two floats), so force FP fields to 8 there; AArch64 HFA elements keep
      // their natural width (S=4, D=8).  Integer fields take the read width
      // (a full GPR slot is 8).
      uint16_t Total = 0;
      for (auto &F : Fields) {
        uint16_t Sz;
        if (F.IsFP)
          Sz = (TargetArch == Arch::X64) ? 8 : (F.Size <= 4 ? 4 : 8);
        else
          Sz = F.Size >= 8 ? 8 : (F.Size >= 4 ? 4 : (F.Size ? F.Size : 8));
        F.Size = Sz;
        Total += Sz;
      }

      bool HasImplicitField =
          std::any_of(Fields.begin(), Fields.end(), [&](const FieldRead &F) {
            return isCallClobber(F.Value, Op.CallSiteId);
          });
      if (TargetArch == Arch::AArch64 && Op.Opcode == NdOp::CALL &&
          HasImplicitField) {
        MedStructReturnCandidate Candidate;
        Candidate.CallSiteId = Op.CallSiteId;
        for (const FieldRead &F : Fields) {
          MedVar Field = F.Value;
          Field.Size = F.Size;
          Candidate.Fields.push_back(Field);
        }
        Func.StructReturnCandidates.push_back(std::move(Candidate));
        continue;
      }

      // Remodel: the call produces a flat wide-integer temp; SUBBYTES each
      // field out into its return register at the packed byte offset.
      MedVar OrigOut = Op.Output;
      MedVar Tv;
      Tv.Kind = MedVar::Temp;
      Tv.TheArch = TargetArch;
      Tv.Id = NextId++;
      Tv.SSAVer = 0;
      Tv.Size = Total;
      Op.Output = Tv;

      std::vector<MedOp> Extracts;
      std::vector<std::pair<uint64_t, std::pair<int, int>>> NewVers;
      uint16_t Cum = 0;
      for (auto &F : Fields) {
        int Id = (F.RegOff == OrigOut.RegOff) ? OrigOut.Id : NextId++;
        int Ver = NextVer++;
        MedVar Out;
        Out.Kind = MedVar::Reg;
        Out.TheArch = TargetArch;
        Out.RegOff = F.RegOff;
        Out.Id = Id;
        Out.SSAVer = Ver;
        Out.Size = F.Size;
        MedOp Ex;
        Ex.Opcode = NdOp::SUBBYTES;
        Ex.Addr = Op.Addr;
        Ex.Output = Out;
        Ex.addInput(Tv);
        Ex.addInput(MedVar::makeConst(Cum, TRI.PointerSize));
        Extracts.push_back(Ex);
        NewVers.push_back({F.RegOff, {Id, Ver}});
        Cum += F.Size;
      }
      Blk.Ops.insert(Blk.Ops.begin() + OI + 1, Extracts.begin(),
                     Extracts.end());

      // Rewire straight-line reads of each field register to its extract
      // version until that register is redefined (before copy propagation, so a
      // plain by-RegOff match is exact).
      std::set<uint64_t> Done;
      for (size_t J = OI + 1 + Extracts.size();
           J < Blk.Ops.size() && Done.size() < Fields.size(); ++J) {
        auto &Nx = Blk.Ops[J];
        if (Nx.Opcode == NdOp::CALL || Nx.Opcode == NdOp::INDIR_CALL)
          break;
        for (uint8_t I = 0; I < Nx.NumInputs; ++I) {
          auto &In = Nx.Inputs[I];
          if (In.Kind != MedVar::Reg)
            continue;
          for (auto &NV : NewVers)
            if (In.RegOff == NV.first && !Done.count(NV.first)) {
              In.Id = NV.second.first;
              In.SSAVer = NV.second.second;
            }
        }
        // A SUBBYTES narrowing the field register's own value (input and output
        // share the register offset) is a sub-register VIEW of the extract, not
        // a redefinition; marking it Done would strand a later full-width read
        // of the same field (e.g. a pointer field reused as a load base after
        // its low half is viewed) — the base would keep its stale pre-call
        // version
        // (#470-style sub-register view handling).
        bool SelfView = Nx.Opcode == NdOp::SUBBYTES && Nx.NumInputs >= 1 &&
                        Nx.Inputs[0].Kind == MedVar::Reg &&
                        Nx.Inputs[0].RegOff == Nx.Output.RegOff;
        if (Nx.Output.Kind == MedVar::Reg && !SelfView)
          for (auto &NV : NewVers)
            if (Nx.Output.RegOff == NV.first)
              Done.insert(NV.first);
      }

      // Cross-block straight-line reads: a field still live at the call block's
      // end reaches reads in successor blocks the call dominates (e.g. a
      // returned field consumed in only one arm of `if (cond)`), which the
      // same-block scan above never visits.  Without rewiring them the read
      // resolves to the stale pre-call register value (the call's own argument)
      // — mirrors the cross- block handling in modelCallFPReturn.  Bounded to
      // single-predecessor successors so the call block provably dominates
      // them; a field carried through a PHI is handled by the PHI-arg update
      // below.
      for (auto &NV : NewVers) {
        if (Done.count(NV.first))
          continue;
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
          bool HasFieldPhi = false;
          for (const auto &Phi : SB->Phis)
            if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == NV.first)
              HasFieldPhi = true;
          if (HasFieldPhi)
            continue; // the PHI (updated below) carries the value
          bool Redef = false;
          for (auto &Nx : SB->Ops) {
            for (uint8_t I = 0; I < Nx.NumInputs; ++I)
              if (Nx.Inputs[I].Kind == MedVar::Reg &&
                  Nx.Inputs[I].RegOff == NV.first) {
                Nx.Inputs[I].Id = NV.second.first;
                Nx.Inputs[I].SSAVer = NV.second.second;
              }
            bool SelfView = Nx.Opcode == NdOp::SUBBYTES && Nx.NumInputs >= 1 &&
                            Nx.Inputs[0].Kind == MedVar::Reg &&
                            Nx.Inputs[0].RegOff == Nx.Output.RegOff;
            if (Nx.Output.Kind == MedVar::Reg && Nx.Output.RegOff == NV.first &&
                !SelfView) {
              Redef = true;
              break;
            }
          }
          if (!Redef)
            Work.insert(Work.end(), SB->Succs.begin(), SB->Succs.end());
        }
      }

      // Loop-carried / cross-edge result: update successor PHI arguments to the
      // call's field version — but only for a field register that is NOT
      // redefined after the extract in this block.  If it is redefined (e.g. a
      // loop accumulator `acc += r.x` whose new value becomes the carried
      // value, or is spilled and reloaded), that later definition is the
      // loop-carried value; overriding the PHI arg to the call's field here
      // would drop the accumulation (mirrors modelCallFPReturn / #469).  `Done`
      // holds exactly the field registers redefined after the extracts above.
      for (int Succ : Blk.Succs)
        for (auto &B : Func.Blocks) {
          if (B.Id != Succ)
            continue;
          for (auto &Phi : B.Phis)
            for (auto &A : Phi.Args)
              if (A.first == Blk.Id && A.second.Kind == MedVar::Reg)
                for (auto &NV : NewVers)
                  if (A.second.RegOff == NV.first && !Done.count(NV.first)) {
                    A.second.Id = NV.second.first;
                    A.second.SSAVer = NV.second.second;
                  }
        }
      OI += Extracts.size();
    }
  }
}

void LowToMedConverter::modelCallX87Return(MedFunc &Func) {
  const auto &TRI = getTargetRegInfo(TargetArch);
  // Only x86/x86-64 have an x87 stack.  i386 cdecl returns floating point in
  // st0; x86-64 returns it in XMM0 (handled by modelCallFPReturn), so this pass
  // only fires when a callee actually leaves its result on the x87 stack — the
  // caller reads it back with `fstp [mem]`, a post-call read of an st register
  // the lifter never modeled the call as defining (so it folds to the stale
  // entry value, storing 0).
  if (TargetArch != Arch::X86 && TargetArch != Arch::X64)
    return;

  int MaxVer = 0;
  for (const auto &B : Func.Blocks) {
    for (const auto &Op : B.Ops)
      MaxVer = std::max(MaxVer, Op.Output.SSAVer);
    for (const auto &Phi : B.Phis)
      MaxVer = std::max(MaxVer, Phi.Output.SSAVer);
  }
  int NextVer = MaxVer + 1;

  for (auto &Blk : Func.Blocks) {
    for (size_t OI = 0; OI < Blk.Ops.size(); ++OI) {
      auto &Op = Blk.Ops[OI];
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;

      // The first post-call x87 read (the `fstp` of the FP return), before any
      // op redefines an x87 register.
      MedVar StRead;
      bool Found = false;
      for (size_t J = OI + 1; J < Blk.Ops.size() && !Found; ++J) {
        auto &Nx = Blk.Ops[J];
        for (uint8_t I = 0; I < Nx.NumInputs; ++I)
          if (Nx.Inputs[I].Kind == MedVar::Reg &&
              TRI.isX87StackReg(Nx.Inputs[I].RegOff)) {
            StRead = Nx.Inputs[I];
            Found = true;
            break;
          }
        if (Found)
          break;
        if (Nx.Output.Kind == MedVar::Reg &&
            TRI.isX87StackReg(Nx.Output.RegOff))
          break; // x87 redefined before any read
      }
      if (!Found)
        continue;

      int NewVer = NextVer++;
      MedVar Out;
      Out.Kind = MedVar::Reg;
      Out.Id = StRead.Id;
      Out.Size = StRead.Size;
      Out.RegOff = StRead.RegOff;
      Out.SSAVer = NewVer;
      Out.TheArch = TargetArch;
      Op.Output = Out;

      for (size_t J = OI + 1; J < Blk.Ops.size(); ++J) {
        auto &Nx = Blk.Ops[J];
        for (uint8_t I = 0; I < Nx.NumInputs; ++I)
          if (Nx.Inputs[I].Kind == MedVar::Reg &&
              Nx.Inputs[I].RegOff == StRead.RegOff &&
              Nx.Inputs[I].Id == StRead.Id)
            Nx.Inputs[I].SSAVer = NewVer;
        if (Nx.Output.Kind == MedVar::Reg && Nx.Output.RegOff == StRead.RegOff)
          break;
      }
    }
  }
}

void LowToMedConverter::modelKnownWideCallReturns(MedFunc &Func) {
  const bool HaveDirect = I64Callees && !I64Callees->empty();
  const bool HaveIndirect = I64IndirectSites && !I64IndirectSites->empty();
  if (!HaveDirect && !HaveIndirect)
    return;
  const auto &TRI = getTargetRegInfo(TargetArch);
  if (TRI.PointerSize != 4 || TRI.IntReturnReg2 == 0)
    return;
  const uint64_t LoReg = TRI.IntReturnReg;
  const uint64_t HiReg = TRI.IntReturnReg2;
  const uint16_t Half = static_cast<uint16_t>(TRI.PointerSize);

  // The high return register's pre-SSA variable id, shared with every other
  // EDX/R1 access so buildSsa links the call's high half into the same SSA web
  // and places the loop PHI where a threaded accumulator reuses it.
  auto HiKey = std::make_pair(HiReg, Half);
  auto HiIt = RegVarMap.find(HiKey);
  int HiId = (HiIt != RegVarMap.end()) ? HiIt->second
                                       : (RegVarMap[HiKey] = allocVarId());

  // A call site returns i64 in the pair when it is a direct CALL to a known
  // i64 callee, or an indirect INDIR_CALL flagged as a threaded i64 accumulator.
  auto returnsPair = [&](const MedOp &Op) -> bool {
    if (Op.Opcode == NdOp::CALL)
      return HaveDirect && Op.NumInputs >= 1 && Op.Inputs[0].isConst() &&
             I64Callees->count(Op.Inputs[0].ConstVal);
    if (Op.Opcode == NdOp::INDIR_CALL)
      return HaveIndirect && I64IndirectSites->count(Op.Addr);
    return false;
  };

  for (auto &Blk : Func.Blocks) {
    for (size_t OI = 0; OI < Blk.Ops.size(); ++OI) {
      auto &Op = Blk.Ops[OI];
      if (!returnsPair(Op))
        continue;
      if (Op.Output.Kind != MedVar::Reg || Op.Output.RegOff != LoReg)
        continue;

      // Re-target the call to a fresh 64-bit temp, then SUBBYTES its low/high
      // halves into the two return registers.  Pre-SSA (SSAVer 0): buildSsa
      // versions these and inserts the high-half loop PHI on its own, so a
      // threaded `acc = f(acc, ...)` carries both halves around the loop.
      MedVar OrigLo = Op.Output; // the low return register (EAX / R0)
      MedVar Tv;
      Tv.Kind = MedVar::Temp;
      Tv.TheArch = TargetArch;
      Tv.Id = allocVarId();
      Tv.SSAVer = 0;
      Tv.Size = static_cast<uint16_t>(2 * Half);
      Op.Output = Tv;

      MedOp LoOp;
      LoOp.Opcode = NdOp::SUBBYTES;
      LoOp.Addr = Op.Addr;
      LoOp.Output = OrigLo;
      LoOp.Output.SSAVer = 0;
      LoOp.addInput(Tv);
      LoOp.addInput(MedVar::makeConst(0, Half));

      MedVar HiVar;
      HiVar.Kind = MedVar::Reg;
      HiVar.TheArch = TargetArch;
      HiVar.RegOff = HiReg;
      HiVar.Id = HiId;
      HiVar.SSAVer = 0;
      HiVar.Size = Half;
      MedOp HiOp;
      HiOp.Opcode = NdOp::SUBBYTES;
      HiOp.Addr = Op.Addr;
      HiOp.Output = HiVar;
      HiOp.addInput(Tv);
      HiOp.addInput(MedVar::makeConst(Half, Half));

      Blk.Ops.insert(Blk.Ops.begin() + OI + 1, {LoOp, HiOp});
      OI += 2;
    }
  }
}

void modelCallWideIntReturn(MedFunc &Func, Arch TheArch,
                            const std::set<va_t> *ForceI64Callees) {
  const auto &TRI = getTargetRegInfo(TheArch);
  // Only 32-bit targets that return a 64-bit integer in a register pair (i386
  // EDX:EAX, ARM32 R1:R0).  The lifter did not model a call as defining the
  // high-half register (IntReturnReg2), so a post-call read of the high half
  // resolves to the pre-call (clobbered) value and the result's upper 32 bits
  // are lost.  Mirrors modelCallFPReturn for the integer register pair.
  if (TRI.PointerSize != 4 || TRI.IntReturnReg2 == 0)
    return;
  const uint64_t LoReg = TRI.IntReturnReg;
  const uint64_t HiReg = TRI.IntReturnReg2;
  const uint16_t Half = TRI.PointerSize;

  // A direct call to a callee proven to return i64 is remodeled
  // unconditionally: its high half may be consumed purely as the next call's
  // argument, which the straight-line/loop-carried scan below cannot see (call
  // arguments are not yet recovered at this stage).  callee return-type
  // inference is the reliable signal there.
  auto callForced = [&](const MedOp &O) -> bool {
    return ForceI64Callees && O.Opcode == NdOp::CALL && O.NumInputs >= 1 &&
           O.Inputs[0].isConst() &&
           ForceI64Callees->count(O.Inputs[0].ConstVal);
  };
  bool HasForced = false;
  if (ForceI64Callees)
    for (const auto &B : Func.Blocks)
      for (const auto &O : B.Ops)
        if (callForced(O)) {
          HasForced = true;
          break;
        }

  int HiId = -1;
  uint16_t HiBest = 0;
  int MaxVer = 0, MaxId = 0;
  auto consider = [&](const MedVar &V) {
    if (V.Kind == MedVar::Reg && V.RegOff == HiReg && V.Size > HiBest) {
      HiBest = V.Size;
      HiId = V.Id;
    }
    MaxId = std::max(MaxId, V.Id);
  };
  for (const auto &B : Func.Blocks) {
    for (const auto &Op : B.Ops) {
      consider(Op.Output);
      MaxVer = std::max(MaxVer, Op.Output.SSAVer);
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        consider(Op.Inputs[I]);
    }
    for (const auto &Phi : B.Phis) {
      consider(Phi.Output);
      MaxVer = std::max(MaxVer, Phi.Output.SSAVer);
      for (const auto &A : Phi.Args)
        consider(A.second);
    }
  }
  if (HiId < 0 && !HasForced)
    return; // the high-half return register is never referenced
  int NextVer = MaxVer + 1;
  int NextId = MaxId + 1;
  // A forced i64 callee whose high half is consumed only as the next call's
  // argument leaves the high return register otherwise unreferenced here; give
  // it a fresh SSA id so the synthesized high-half definition is well-formed.
  if (HiId < 0)
    HiId = NextId++;

  auto succHasRegPhi = [&](int BlockId, uint64_t RegOff) -> bool {
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
        if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == RegOff)
          for (const auto &A : Phi.Args)
            if (A.first == BlockId)
              return true;
    }
    return false;
  };

  for (auto &Blk : Func.Blocks) {
    for (size_t OI = 0; OI < Blk.Ops.size(); ++OI) {
      auto &Op = Blk.Ops[OI];
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;
      if (Op.Output.Kind != MedVar::Reg || Op.Output.RegOff != LoReg)
        continue;

      // The call returns i64 only when the caller consumes BOTH halves the same
      // way: both read straight-line before being redefined, or both carried to
      // a successor PHI on this edge.  Requiring one consistent mode avoids the
      // false positive where the low half is an int result and the high
      // register is merely unrelated loop-carried scratch (the low half is
      // consumed immediately while only the scratch high half reaches a loop
      // PHI).
      bool ReadLo = false, ReadHi = false, RedefLo = false, RedefHi = false;
      for (size_t J = OI + 1; J < Blk.Ops.size() && !(RedefLo && RedefHi);
           ++J) {
        auto &Nx = Blk.Ops[J];
        if (Nx.Opcode == NdOp::CALL || Nx.Opcode == NdOp::INDIR_CALL)
          break;
        for (uint8_t I = 0; I < Nx.NumInputs; ++I) {
          const auto &In = Nx.Inputs[I];
          if (In.Kind != MedVar::Reg)
            continue;
          if (!RedefLo && In.RegOff == LoReg)
            ReadLo = true;
          if (!RedefHi && In.RegOff == HiReg)
            ReadHi = true;
        }
        if (Nx.Output.Kind == MedVar::Reg && Nx.Output.RegOff == LoReg)
          RedefLo = true;
        if (Nx.Output.Kind == MedVar::Reg && Nx.Output.RegOff == HiReg)
          RedefHi = true;
      }
      bool StraightLine = ReadLo && ReadHi;
      // Loop-carried only when NEITHER half is consumed straight-line: a
      // genuine i64 accumulator (`acc = f(acc)`) flows both result halves to
      // the successor PHI untouched, whereas an int-returning recursive callee
      // (`return r(n-1, acc*31+n)`) stores R0 as the result here while R1 (the
      // call argument) merely reaches a *dead* merge PHI the SSA builder leaves
      // before the return block reloads R0.  Without the `!ReadLo` guard that
      // dead-PHI pair would be misread as a wide return and split a plain `int`
      // result into R0:R1, dropping it.  (-O0 genuine i64 spills both halves =>
      // ReadLo && ReadHi => StraightLine; -O2 carried i64 reads neither here.)
      bool LoopCarried = !ReadLo && !ReadHi && !RedefLo && !RedefHi &&
                         succHasRegPhi(Blk.Id, LoReg) &&
                         succHasRegPhi(Blk.Id, HiReg);
      bool ForcedI64 = callForced(Op);
      if (!(StraightLine || LoopCarried || ForcedI64))
        continue;

      // Remodel: make the call produce a 64-bit temp and SUBBYTES it into the
      // low/high return registers (i386/ARM32 use two disjoint registers, so a
      // temp is needed where x86-64 would sub-piece a single wide RAX).
      MedVar OrigLo = Op.Output;
      int LoVer = NextVer++, HiVer = NextVer++;
      va_t OpAddr = Op.Addr;

      // The low return register is also a copy target (a callee-saved register
      // holding an argument can be folded onto r0/eax by copy propagation, e.g.
      // `mov r4,r0; ...; op rX,r4` => `op rX,r0`).  After propagation such a
      // read is an unrelated value at the *entry* version, not this call's
      // result, so the low-half rewiring matches only the call's own output
      // version to avoid capturing it (matters for the forced post-propagation
      // run).  The high return register is always a freshly computed value
      // (never a copy target), so it keeps the by-id match that also catches a
      // chained result consumed at the original pre-call version (the final
      // `xor eax,edx` return).
      const int LoOrigVer = OrigLo.SSAVer;

      // A post-call read of the high-return register, before that register's
      // next definition, is always this call's high result: the register is
      // caller-saved (the call clobbers it), so its pre-call value cannot
      // survive.  The lifter only modeled the call as defining the LOW
      // register, so such reads still thread the stale pre-call SSA value — and
      // at -O2 clang may even keep an unrelated loop accumulator in the same
      // physical register (EDX/R1 doubling as the i64-return high half and a
      // carried `acc` half), giving those reads a DIFFERENT SSA id than the
      // call's own result.  Match by register alone (not id/version): every
      // such read is rewired onto the inserted high SUBBYTES.  The high
      // register is never a copy-propagation target (unlike the low one), so
      // this cannot capture an unrelated value.
      auto matchHiLive = [&](const MedVar &V) { return V.RegOff == HiReg; };

      MedVar Tv;
      Tv.Kind = MedVar::Temp;
      Tv.TheArch = TheArch;
      Tv.Id = NextId++;
      Tv.SSAVer = 0;
      Tv.Size = static_cast<uint16_t>(2 * Half);
      Op.Output = Tv;

      auto makeReg = [&](uint64_t RegOff, int Id, int Ver) {
        MedVar V;
        V.Kind = MedVar::Reg;
        V.TheArch = TheArch;
        V.RegOff = RegOff;
        V.Id = Id;
        V.SSAVer = Ver;
        V.Size = Half;
        return V;
      };
      MedOp LoOp;
      LoOp.Opcode = NdOp::SUBBYTES;
      LoOp.Addr = OpAddr;
      LoOp.Output = makeReg(LoReg, OrigLo.Id, LoVer);
      LoOp.addInput(Tv);
      LoOp.addInput(MedVar::makeConst(0, Half));
      MedOp HiOp;
      HiOp.Opcode = NdOp::SUBBYTES;
      HiOp.Addr = OpAddr;
      HiOp.Output = makeReg(HiReg, HiId, HiVer);
      HiOp.addInput(Tv);
      HiOp.addInput(MedVar::makeConst(Half, Half));
      Blk.Ops.insert(Blk.Ops.begin() + OI + 1, {LoOp, HiOp});

      // An op that only derives a different-width VIEW of the SAME physical
      // return register from itself is a sub-register alias artifact, not a
      // genuine redefinition of the architectural value, so it must not stop
      // the rewiring of a return half:
      //   * a narrowing SUBBYTES (`mov ecx,edx` lifts to `SUBBYTES edx_view,
      //     rdx, 0`) — counting it as a redefinition drops a SECOND
      //     back-to-back read of the same half (i386/ARM32 -O3 `mov ecx,edx;
      //     mov ebx,edx`);
      //   * a widening INT_ZEXT/INT_SEXT that promotes the just-rewired
      //   Half-width
      //     return half to its own 64-bit container (`RAX = ZEXT EAX`, the
      //     lifter keeps the wide alias in sync) — counting it as a
      //     redefinition drops a LOOP-CARRIED half whose back-edge PHI then
      //     keeps the stale pre-call value (i64 accumulator threading `acc =
      //     f(acc, ...)`).
      // A real `movzx eax,al` (`EAX = ZEXT AL`) is excluded because its source
      // is a narrower sub-part (Size < Half), not the return half at its full
      // width.
      auto isAliasView = [&](const MedOp &O, uint64_t RegOff) {
        if (O.NumInputs < 1 || O.Inputs[0].Kind != MedVar::Reg ||
            O.Inputs[0].RegOff != RegOff)
          return false;
        if (O.Opcode == NdOp::SUBBYTES)
          return true;
        return (O.Opcode == NdOp::INT_ZEXT || O.Opcode == NdOp::INT_SEXT) &&
               O.Inputs[0].Size == Half && O.Output.Size > Half;
      };

      // Rewire straight-line reads of either half to the split versions until
      // that half is redefined or the next call takes over.
      bool LoDone = false, HiDone = false;
      for (size_t J = OI + 3; J < Blk.Ops.size() && !(LoDone && HiDone); ++J) {
        auto &Nx = Blk.Ops[J];
        if (Nx.Opcode == NdOp::CALL || Nx.Opcode == NdOp::INDIR_CALL)
          break;
        for (uint8_t I = 0; I < Nx.NumInputs; ++I) {
          auto &In = Nx.Inputs[I];
          if (In.Kind != MedVar::Reg)
            continue;
          if (!LoDone && In.RegOff == LoReg && In.Id == OrigLo.Id &&
              In.SSAVer == LoOrigVer)
            In.SSAVer = LoVer;
          else if (!HiDone && matchHiLive(In)) {
            In.Id = HiId;
            In.SSAVer = HiVer;
          }
        }
        if (Nx.Output.Kind == MedVar::Reg && Nx.Output.RegOff == LoReg &&
            !isAliasView(Nx, LoReg))
          LoDone = true;
        if (Nx.Output.Kind == MedVar::Reg && Nx.Output.RegOff == HiReg &&
            !isAliasView(Nx, HiReg))
          HiDone = true;
      }

      // Loop-carried / cross-edge result: update successor PHI arguments, but
      // only for a half still LIVE at the block's exit edge — i.e. this call's
      // split half is the last definition of the return register before the
      // edge.  A half dies when it is explicitly redefined later in the block,
      // or when a SUBSEQUENT call clobbers the return registers; in either case
      // the value crossing the edge is that later definition (already wired by
      // SSA), not the call's split half.  The straight-line scan above only saw
      // redefinitions before the NEXT call, so rescan the whole remainder of
      // the block here — otherwise a half consumed only as a later call's
      // argument
      // (`acc = f(f(acc))`) would have its back-edge PHI hijacked by an earlier
      // call's split half (#521 ②/#523).
      bool LoLive = !LoDone, HiLive = !HiDone;
      for (size_t J = OI + 3; (LoLive || HiLive) && J < Blk.Ops.size(); ++J) {
        auto &Nx = Blk.Ops[J];
        if (Nx.Opcode == NdOp::CALL || Nx.Opcode == NdOp::INDIR_CALL) {
          LoLive = HiLive = false;
          break;
        }
        // A wider-alias / narrower-view write of the same return register (e.g.
        // `RAX = ZEXT EAX`) is not a redefinition, so it leaves the call's
        // split half live across the loop back-edge for the successor-PHI
        // rewrite below.
        if (Nx.Output.Kind == MedVar::Reg) {
          if (Nx.Output.RegOff == LoReg && !isAliasView(Nx, LoReg))
            LoLive = false;
          if (Nx.Output.RegOff == HiReg && !isAliasView(Nx, HiReg))
            HiLive = false;
        }
      }

      // A split return can be consumed directly in a successor that has only
      // this call path as a predecessor (an exit block after a loop is the
      // common shape).  No PHI is needed there, so SSA still names the stale
      // call-clobber version unless the direct reads are rewired explicitly.
      // Walk only single-predecessor blocks, where the call result provably
      // dominates, and stop each register half at its first real definition.
      struct SuccState {
        int BlockId;
        bool Lo;
        bool Hi;
      };
      std::vector<SuccState> Work;
      for (int Succ : Blk.Succs)
        Work.push_back({Succ, LoLive, HiLive});
      std::set<int> Seen;
      while (!Work.empty()) {
        SuccState State = Work.back();
        Work.pop_back();
        if (!Seen.insert(State.BlockId).second)
          continue;
        MedBlock *SB = nullptr;
        for (auto &B : Func.Blocks)
          if (B.Id == State.BlockId) {
            SB = &B;
            break;
          }
        if (!SB || SB->Preds.size() != 1)
          continue;

        // A PHI is itself the reaching definition for reads in this block; its
        // incoming edge is updated by the existing PHI rewrite below.
        for (const auto &Phi : SB->Phis) {
          if (Phi.Output.Kind != MedVar::Reg)
            continue;
          if (Phi.Output.RegOff == LoReg)
            State.Lo = false;
          if (Phi.Output.RegOff == HiReg)
            State.Hi = false;
        }

        for (auto &Nx : SB->Ops) {
          if (Nx.Opcode == NdOp::CALL || Nx.Opcode == NdOp::INDIR_CALL) {
            State.Lo = State.Hi = false;
            break;
          }
          for (uint8_t I = 0; I < Nx.NumInputs; ++I) {
            auto &In = Nx.Inputs[I];
            if (In.Kind != MedVar::Reg)
              continue;
            if (State.Lo && In.RegOff == LoReg && In.Id == OrigLo.Id &&
                In.SSAVer == LoOrigVer)
              In.SSAVer = LoVer;
            else if (State.Hi && matchHiLive(In)) {
              In.Id = HiId;
              In.SSAVer = HiVer;
            }
          }
          if (Nx.Output.Kind == MedVar::Reg) {
            if (State.Lo && Nx.Output.RegOff == LoReg &&
                !isAliasView(Nx, LoReg))
              State.Lo = false;
            if (State.Hi && Nx.Output.RegOff == HiReg &&
                !isAliasView(Nx, HiReg))
              State.Hi = false;
          }
          if (!State.Lo && !State.Hi)
            break;
        }
        if (State.Lo || State.Hi)
          for (int Succ : SB->Succs)
            Work.push_back({Succ, State.Lo, State.Hi});
      }

      for (int Succ : Blk.Succs)
        for (auto &B : Func.Blocks) {
          if (B.Id != Succ)
            continue;
          for (auto &Phi : B.Phis)
            for (auto &A : Phi.Args) {
              if (A.first != Blk.Id || A.second.Kind != MedVar::Reg)
                continue;
              if (LoLive && A.second.RegOff == LoReg &&
                  A.second.Id == OrigLo.Id && A.second.SSAVer == LoOrigVer)
                A.second.SSAVer = LoVer;
              else if (HiLive && matchHiLive(A.second)) {
                A.second.Id = HiId;
                A.second.SSAVer = HiVer;
              }
            }
        }
      OI += 2;
    }
  }
}

} // namespace neverd
