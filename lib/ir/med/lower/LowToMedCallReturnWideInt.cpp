//===- LowToMedCallReturnWideInt.cpp - Wide integer call returns ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Models wide integer call returns carried across register pairs.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

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
