//===- PipelineWideIntReturn.cpp - Wide integer return recovery ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Whole-module recovery of register-pair wide-integer returns on 32-bit
/// targets.
///
//===----------------------------------------------------------------------===//

#include "PipelineReturnModelingDetail.h"

#include "neverd/Common.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/pipeline/Pipeline.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {

namespace {

// Whether \p MF returns a 64-bit integer in the register pair
// (IntReturnReg:IntReturnReg2 — i386 EDX:EAX, ARM32 R1:R0): every
// RETURN-terminated block materializes the high-half return register the way
// the emitter's wide-return splice looks for it (a non-SUBBYTES write, a
// high-half SUBBYTES, or a high-register phi).  A plain i32-returning function
// writes only the low half at its epilogue, so the high register is unavailable
// there and this returns false.  Used to widen a register-pair callee reached
// ONLY through a function pointer, which never appears as a direct-call target.
bool returnsRegisterPairI64(const MedFunc &MF, const TargetRegInfo &TRI) {
  if (TRI.PointerSize != 4 || TRI.IntReturnReg2 == 0)
    return false;
  bool SawReturn = false;
  for (const auto &Blk : MF.Blocks) {
    bool HasRet = false;
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::RETURN) {
        HasRet = true;
        break;
      }
    if (!HasRet)
      continue;
    SawReturn = true;
    bool HiAvail = false;
    // Mirror emitter WantWide64 HiVar: walk backwards from RETURN, skipping
    // low-half SUBBYTES narrows (flag-computation scratch).  A non-SUBBYTES or
    // high-half SUBBYTES write counts unless its value is consumed only to
    // build the low return register — ARM32 pointer returns park a base in R1
    // before folding it into R0, which is not a genuine i64 high half.
    for (auto RIt = Blk.Ops.rbegin(); RIt != Blk.Ops.rend(); ++RIt) {
      if (RIt->Opcode == NdOp::RETURN)
        continue;
      if (RIt->Output.Kind != MedVar::Reg ||
          RIt->Output.RegOff != TRI.IntReturnReg2 || RIt->Output.Size == 0)
        continue;
      bool IsHighSubpiece = RIt->Opcode == NdOp::SUBBYTES &&
                            RIt->NumInputs >= 2 && RIt->Inputs[1].isConst() &&
                            RIt->Inputs[1].ConstVal == TRI.PointerSize;
      if (RIt->Opcode == NdOp::SUBBYTES && !IsHighSubpiece)
        continue;
      bool FeedsLowRet = false;
      const MedVar &HiOut = RIt->Output;
      for (auto FIt = std::next(RIt.base()); FIt != Blk.Ops.end(); ++FIt) {
        if (FIt->Opcode == NdOp::RETURN)
          break;
        if (FIt->Output.Kind == MedVar::Reg &&
            FIt->Output.RegOff == TRI.IntReturnReg) {
          for (uint8_t K = 0; K < FIt->NumInputs; ++K) {
            const auto &In = FIt->Inputs[K];
            if (In.Kind == HiOut.Kind && In.Id == HiOut.Id &&
                In.SSAVer == HiOut.SSAVer) {
              FeedsLowRet = true;
              break;
            }
          }
        }
        if (FeedsLowRet)
          break;
      }
      // The closest non-narrowing high-half write before RETURN is decisive:
      // if it only feeds the low return register it is pointer-return scratch,
      // and any earlier high-half write was overwritten and is irrelevant.
      if (FeedsLowRet)
        break;
      HiAvail = true;
      break;
    }
    if (!HiAvail)
      for (const auto &Phi : Blk.Phis)
        if (Phi.Output.Kind == MedVar::Reg &&
            Phi.Output.RegOff == TRI.IntReturnReg2 && Phi.Output.Size > 0) {
          HiAvail = true;
          break;
        }
    if (!HiAvail)
      return false;
  }
  return SawReturn;
}

// Resolve a call-target operand to its absolute VA, folding the COPY / INT_ADD
// / INT_SUB / literal-pool LOAD chain the emitter resolves — so an ARM32
// PC-relative function pointer (`add rT, pc, [litpool]`), which never appears
// as a single constant operand, is still recognized as targeting a known
// function. Returns nullopt for a genuine runtime pointer (loaded from a
// writable slot, an argument, etc.).
std::optional<va_t> resolveCallTargetVA(const MedFunc &MF,
                                        const BinaryImage &Img, const MedVar &V,
                                        int Depth = 0) {
  if (V.isConst())
    return V.ConstVal;
  if (Depth > 8)
    return std::nullopt;

  const MedOp *Def = nullptr;
  for (const auto &Blk : MF.Blocks) {
    for (const auto &Op : Blk.Ops)
      if (Op.Output.Kind == V.Kind && Op.Output.Id == V.Id &&
          Op.Output.SSAVer == V.SSAVer) {
        Def = &Op;
        break;
      }
    if (Def)
      break;
  }
  if (!Def)
    return std::nullopt;

  switch (Def->Opcode) {
  case NdOp::COPY:
    return Def->NumInputs >= 1
               ? resolveCallTargetVA(MF, Img, Def->Inputs[0], Depth + 1)
               : std::nullopt;
  case NdOp::INT_ADD:
  case NdOp::INT_SUB: {
    if (Def->NumInputs < 2)
      return std::nullopt;
    auto A = resolveCallTargetVA(MF, Img, Def->Inputs[0], Depth + 1);
    auto B = resolveCallTargetVA(MF, Img, Def->Inputs[1], Depth + 1);
    if (!A || !B)
      return std::nullopt;
    return Def->Opcode == NdOp::INT_ADD ? *A + *B : *A - *B;
  }
  case NdOp::LOAD: {
    // Literal-pool word: it lives in a read-only segment with relocations
    // already applied, so read it directly (a writable slot is a real pointer).
    if (Def->NumInputs < 1)
      return std::nullopt;
    auto Addr = resolveCallTargetVA(MF, Img, Def->Inputs[0], Depth + 1);
    if (!Addr)
      return std::nullopt;
    const auto *Seg = Img.getSegmentFor(*Addr);
    if (!Seg || Seg->isWritable() || Seg->Data.empty())
      return std::nullopt;
    size_t Off = static_cast<size_t>(*Addr - Seg->VA);
    uint16_t Sz = Def->Output.Size ? Def->Output.Size : 4;
    if (Sz > 8 || !rangeInBounds(Off, Sz, Seg->Data.size()))
      return std::nullopt;
    uint64_t Val = 0;
    std::memcpy(&Val, Seg->Data.data() + Off, Sz);
    // The literal holds a signed PC-relative displacement; sign-extend so the
    // subsequent `+ pc` yields the absolute target.
    if (Sz < 8 && (Val & (1ull << (Sz * 8 - 1))))
      Val |= ~uint64_t(0) << (Sz * 8);
    return Val;
  }
  default:
    return std::nullopt;
  }
}

} // anonymous namespace

// A 32-bit target returns a 64-bit integer in a register pair (i386 EDX:EAX,
// ARM32 R1:R0).  modelCallWideIntReturn (LowToMed) already rewrote every call
// whose caller consumes the high half into one producing a 64-bit temp; the
// target of such a call returns i64, so override its (low-half-only) inferred
// return type to match the call site and splice both halves at its RETURN.
void modelWideIntReturns(const BinaryImage &Img, PipelineResult &Result) {
  const auto &TRI = getTargetRegInfo(Img.Arch);
  if (TRI.PointerSize == 4 && TRI.IntReturnReg2 != 0) {
    std::set<va_t> WideRetCallees;
    bool HasIndirectWideRet = false;
    for (const auto &MF : Result.MedFuncs)
      for (const auto &Blk : MF.Blocks)
        for (const auto &Op : Blk.Ops) {
          if (Op.Output.Kind != MedVar::Temp ||
              Op.Output.Size != 2 * TRI.PointerSize)
            continue;
          if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
              Op.Inputs[0].isConst())
            WideRetCallees.insert(Op.Inputs[0].ConstVal);
          else if (Op.Opcode == NdOp::INDIR_CALL)
            HasIndirectWideRet = true;
        }

    // When the indirect target itself folds to a known function (notably an
    // ARM32 PC-relative literal-pool pointer), the widened call site is direct
    // evidence for that callee's register-pair return.  The address-taken scan
    // below cannot see this shape because no operand contains the final target
    // VA as a standalone constant.
    if (HasIndirectWideRet)
      for (const auto &MF : Result.MedFuncs)
        for (const auto &Blk : MF.Blocks)
          for (const auto &Op : Blk.Ops) {
            if (Op.Opcode != NdOp::INDIR_CALL ||
                Op.Output.Kind != MedVar::Temp ||
                Op.Output.Size != 2 * TRI.PointerSize || Op.NumInputs < 1)
              continue;
            auto Tgt = resolveCallTargetVA(MF, Img, Op.Inputs[0]);
            if (!Tgt)
              continue;
            for (const auto &Callee : Result.MedFuncs)
              if (Callee.Entry == *Tgt && returnsRegisterPairI64(Callee, TRI)) {
                WideRetCallees.insert(*Tgt);
                break;
              }
          }

    // A register-pair i64 callee reached ONLY through a function pointer
    // never appears as a direct-call target, so the scan above misses it; its
    // RETURN would splice only the low half and drop the high 32 bits the
    // (already widened) indirect call site reads.  When the program has at
    // least one indirect 64-bit-returning call, widen every ADDRESS-TAKEN
    // function whose body materializes the high return register at its
    // RETURN.  A function's address is "taken" when its (non-zero) entry VA
    // appears as a constant operand somewhere other than a direct call's
    // target slot; a bare 0 is a literal, never a code address, and would
    // otherwise collide with the entry function at VA 0.
    if (HasIndirectWideRet) {
      std::set<va_t> Entries;
      for (const auto &MF : Result.MedFuncs)
        Entries.insert(MF.Entry);
      std::set<va_t> AddressTaken;
      for (const auto &MF : Result.MedFuncs)
        for (const auto &Blk : MF.Blocks)
          for (const auto &Op : Blk.Ops) {
            bool DirectCall = Op.Opcode == NdOp::CALL;
            for (uint8_t I = 0; I < Op.NumInputs; ++I) {
              if (DirectCall && I == 0)
                continue; // the call's own constant target, not a taken addr
              const auto &In = Op.Inputs[I];
              if (In.isConst() && In.ConstVal != 0 &&
                  Entries.count(In.ConstVal))
                AddressTaken.insert(In.ConstVal);
            }
          }
      for (const auto &MF : Result.MedFuncs)
        if (AddressTaken.count(MF.Entry) && returnsRegisterPairI64(MF, TRI))
          WideRetCallees.insert(MF.Entry);
    }

    // A register-pair i64 callee whose result is THREADED as a 64-bit
    // accumulator — `acc = f(acc, ...)` in a loop — is missed by the
    // wide-temp scan: the high half (EDX/R1) is consumed only as the NEXT
    // iteration's call argument, which argument recovery has not run yet, so
    // the straight- line scan saw only the low half read and produced a
    // low-only call.  Both a DIRECT call (the constant target's body is the
    // signal) and an INDIRECT call through a function pointer (no constant
    // target — an address-taken register-pair i64 callee is the plausible
    // target) need the same fix; the gate in both is that the low result is
    // carried to a successor PHI on a loop back-edge, which rejects an i32
    // callee merely leaving scratch in the high register (e.g. an idiv
    // remainder) at its epilogue.
    std::map<va_t, std::set<va_t>> IndirectThreadSitesByFunc;
    {
      auto lowResultLoopCarried = [&](const MedFunc &MF, const MedBlock &B,
                                      const MedOp &Call) -> bool {
        if (Call.Output.Kind != MedVar::Reg ||
            Call.Output.RegOff != TRI.IntReturnReg)
          return false;
        for (const auto &Succ : MF.Blocks) {
          bool IsSucc = false;
          for (int S : B.Succs)
            if (S == Succ.Id) {
              IsSucc = true;
              break;
            }
          if (!IsSucc)
            continue;
          for (const auto &Phi : Succ.Phis)
            if (Phi.Output.Kind == MedVar::Reg &&
                Phi.Output.RegOff == TRI.IntReturnReg)
              for (const auto &A : Phi.Args)
                if (A.first == B.Id && A.second.Kind == MedVar::Reg &&
                    A.second.RegOff == TRI.IntReturnReg)
                  return true;
        }
        return false;
      };

      // Direct threaded calls: the callee is the constant target.
      std::set<va_t> ThreadedTargets;
      for (const auto &MF : Result.MedFuncs)
        for (const auto &Blk : MF.Blocks)
          for (const auto &Op : Blk.Ops)
            if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
                Op.Inputs[0].isConst() && lowResultLoopCarried(MF, Blk, Op))
              ThreadedTargets.insert(Op.Inputs[0].ConstVal);
      if (!ThreadedTargets.empty())
        for (const auto &MF : Result.MedFuncs)
          if (ThreadedTargets.count(MF.Entry) &&
              returnsRegisterPairI64(MF, TRI))
            WideRetCallees.insert(MF.Entry);

      // Indirect threaded calls: no constant target, so require an address-
      // taken function returning the register pair as the plausible callee.
      // An address is "taken" when its non-zero entry VA appears as a
      // constant operand outside a direct call's target slot (a bare 0 is a
      // literal).
      bool HasThreadedIndirect = false;
      for (const auto &MF : Result.MedFuncs)
        for (const auto &Blk : MF.Blocks)
          for (const auto &Op : Blk.Ops)
            if (Op.Opcode == NdOp::INDIR_CALL &&
                lowResultLoopCarried(MF, Blk, Op))
              HasThreadedIndirect = true;
      if (HasThreadedIndirect) {
        std::map<va_t, const MedFunc *> ByEntry;
        for (const auto &MF : Result.MedFuncs)
          ByEntry[MF.Entry] = &MF;
        auto returnsI64 = [&](va_t VA) -> bool {
          auto It = ByEntry.find(VA);
          return It != ByEntry.end() &&
                 returnsRegisterPairI64(*It->second, TRI);
        };

        // x86/i386 fold a function pointer to a constant operand, so an i64
        // callee whose address is taken (its non-zero entry VA appears as a
        // constant outside a direct call's target slot) is the plausible
        // indirect target when the actual target cannot be resolved.
        bool HasAddrTakenI64 = false;
        for (const auto &MF : Result.MedFuncs)
          for (const auto &Blk : MF.Blocks)
            for (const auto &Op : Blk.Ops) {
              bool DirectCall = Op.Opcode == NdOp::CALL;
              for (uint8_t I = 0; I < Op.NumInputs; ++I) {
                if (DirectCall && I == 0)
                  continue;
                const auto &In = Op.Inputs[I];
                if (In.isConst() && In.ConstVal != 0 &&
                    returnsI64(In.ConstVal)) {
                  WideRetCallees.insert(In.ConstVal);
                  HasAddrTakenI64 = true;
                }
              }
            }

        // Flag each threaded INDIR_CALL site.  Resolve its actual target VA
        // first (folds an ARM32 PC-relative literal-pool pointer that never
        // appears as a constant operand); fall back to the address-taken i64
        // callee when the target is an opaque runtime pointer.
        for (const auto &MF : Result.MedFuncs)
          for (const auto &Blk : MF.Blocks)
            for (const auto &Op : Blk.Ops) {
              if (Op.Opcode != NdOp::INDIR_CALL ||
                  !lowResultLoopCarried(MF, Blk, Op) || Op.NumInputs < 1)
                continue;
              std::optional<va_t> Tgt =
                  resolveCallTargetVA(MF, Img, Op.Inputs[0]);
              bool TargetI64 = Tgt && returnsI64(*Tgt);
              if (TargetI64)
                WideRetCallees.insert(*Tgt);
              if (TargetI64 || HasAddrTakenI64)
                IndirectThreadSitesByFunc[MF.Entry].insert(Op.Addr);
            }
      }
    }

    if (!WideRetCallees.empty())
      for (auto &MF : Result.MedFuncs)
        if (WideRetCallees.count(MF.Entry))
          MF.ReturnType = NdType::makeInt(2 * TRI.PointerSize);

    // Conversely, a call whose callee is now known to return i64 but whose
    // high half is consumed only as the *next* call's argument escapes the
    // per-function heuristic (call arguments are not recovered until below,
    // so the high register has no visible reader at low->med time).  Remodel
    // those call sites here, where callee return-type inference is the
    // reliable signal, so the high 32 bits reach the next call instead of
    // resolving to a stale pre-call value (acc = f(acc, ...) i64 threading).
    std::set<va_t> I64RetCallees;
    for (const auto &MF : Result.MedFuncs)
      if (MF.ReturnType && MF.ReturnType->Kind == NdTypeKind::Int &&
          MF.ReturnType->Size == 2 * TRI.PointerSize)
        I64RetCallees.insert(MF.Entry);
    if (!I64RetCallees.empty()) {
      // Re-lift every DIRECT caller of an i64 callee from LowIR with the
      // i64-callee set known, so the call is modeled as defining the EDX:EAX
      // / R1:R0 pair BEFORE buildSsa.  SSA construction then creates the
      // loop- carried high-half PHI a post-SSA patch cannot — the threaded
      // i64 accumulator `acc = f(acc, ...)` carries BOTH halves around the
      // loop and the high 32 bits reach the next call rather than a stale
      // pre-call value.
      std::map<va_t, int> CalleePop;
      for (const auto &LF : Result.LowFuncs)
        if (LF.CalleePopBytes > 0)
          CalleePop[LF.Entry] = LF.CalleePopBytes;

      // Stack-probe slot set for the i64-caller re-conversion (mirrors
      // buildMedIR): keep the prologue chkstk call from killing the live-in
      // argument registers.  Mach-O only -> empty/no-op elsewhere.
      std::set<va_t> StackProbeSlots;
      for (const auto &[SlotVA, SymName] : Img.ImportPtrSlots) {
        if (isDarwinStackProbeName(SymName))
          StackProbeSlots.insert(SlotVA);
      }

      for (size_t I = 0; I < Result.MedFuncs.size(); ++I) {
        bool CallsI64 = false;
        for (const auto &Blk : Result.MedFuncs[I].Blocks) {
          for (const auto &Op : Blk.Ops)
            if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
                Op.Inputs[0].isConst() &&
                I64RetCallees.count(Op.Inputs[0].ConstVal)) {
              CallsI64 = true;
              break;
            }
          if (CallsI64)
            break;
        }
        // A caller threading an i64 accumulator through an INDIRECT call also
        // needs re-lifting so the flagged INDIR_CALL sites are modeled pre-SSA.
        auto IndIt = IndirectThreadSitesByFunc.find(Result.MedFuncs[I].Entry);
        bool HasIndThread = IndIt != IndirectThreadSitesByFunc.end();
        if (!CallsI64 && !HasIndThread)
          continue;
        // Re-conversion re-infers this function's own return type from
        // scratch, so a function that is ITSELF an i64 callee must have its
        // widening restored afterwards.
        bool SelfWide = WideRetCallees.count(Result.MedFuncs[I].Entry) != 0;
        LowToMedConverter Reconv;
        Reconv.setCalleePopMap(&CalleePop);
        Reconv.setStackProbeSlots(&StackProbeSlots);
        Reconv.setI64Callees(&I64RetCallees);
        if (HasIndThread)
          Reconv.setI64IndirectSites(&IndIt->second);
        MedFunc NewMF =
            Reconv.convert(Result.LowFuncs[I], Img.Arch, Img.Format);
        NewMF.OriginalSize = Result.LowFuncs[I].OriginalSize;
        NewMF.DebugName = Result.LowFuncs[I].DebugName;
        NewMF.SourceFile = Result.LowFuncs[I].SourceFile;
        NewMF.SourceLine = Result.LowFuncs[I].SourceLine;
        Result.MedFuncs[I] = std::move(NewMF);
        inferMedTypes(Result.MedFuncs[I], Img.Arch);
        if (SelfWide)
          Result.MedFuncs[I].ReturnType = NdType::makeInt(2 * TRI.PointerSize);
        debugVerifyMedFunc(Result.MedFuncs[I], "reconvert(i64 caller)");
      }

      // Safety net for high halves the pre-SSA modeling does not reach — an
      // INDIRECT i64 call (no constant target) or a call whose low result is
      // not the return register: the post-SSA heuristic still rewires the
      // straight-line / loop-carried reads (a wide-temp direct call is
      // already remodeled, so it is skipped here).
      for (auto &MF : Result.MedFuncs) {
        modelCallWideIntReturn(MF, Img.Arch, &I64RetCallees);
        debugVerifyMedFunc(MF, "modelCallWideIntReturn(forced)");
      }
    }
  }
}

} // namespace neverd
