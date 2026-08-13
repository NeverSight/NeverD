//===- LowToMedCallReturnStruct.cpp - Struct call returns ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Models small struct call returns carried across multiple registers.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

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

} // namespace neverd
