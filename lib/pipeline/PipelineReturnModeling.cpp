//===- PipelineReturnModeling.cpp - Return-value ABI recovery ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Return-value ABI recovery passes run on the whole MedIR module between type
/// inference and call-ABI recovery: wide-integer register-pair returns on
/// 32-bit targets and multi-register by-value struct / HFA returns.  Split out
/// of Pipeline.cpp to keep that translation unit focused on phase
/// orchestration; see PipelineReturnModeling.h for the per-pass contract.
///
//===----------------------------------------------------------------------===//

#include "PipelineReturnModeling.h"

#include "neverd/Common.h"
#include "neverd/Support/BinaryEncoding.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/StringRef.h"

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
        verifyMedFunc(Result.MedFuncs[I], "reconvert(i64 caller)");
      }

      // Safety net for high halves the pre-SSA modeling does not reach — an
      // INDIRECT i64 call (no constant target) or a call whose low result is
      // not the return register: the post-SSA heuristic still rewires the
      // straight-line / loop-carried reads (a wide-temp direct call is
      // already remodeled, so it is skipped here).
      for (auto &MF : Result.MedFuncs) {
        modelCallWideIntReturn(MF, Img.Arch, &I64RetCallees);
        verifyMedFunc(MF, "modelCallWideIntReturn(forced)");
      }
    }
  }
}

// A small struct returned by value across multiple registers (x86-64 SysV
// eightbytes / AArch64 HFA): the caller-side remodel (modelCallStructReturn,
// run during low->med) rewrote each such direct call to produce a flat
// aggregate temp it SUBBYTES into the field return registers.  Read that
// remodeling back to learn each callee's multi-register return shape, then
// re-type the callee so its RETURN emits the matching LLVM aggregate and the
// backend's ABI lowering places the fields in the right registers.
void recoverStructReturnFromCallers(const BinaryImage &Img,
                                    PipelineResult &Result) {
  const auto &TRI = getTargetRegInfo(Img.Arch);
  if (TRI.PointerSize == 8) {
    std::map<va_t, std::vector<MedReturnReg>> StructRetCallees;
    for (const auto &MF : Result.MedFuncs)
      for (const auto &Blk : MF.Blocks)
        for (size_t I = 0; I < Blk.Ops.size(); ++I) {
          const auto &Op = Blk.Ops[I];
          if (Op.Opcode != NdOp::CALL || Op.Output.Kind != MedVar::Temp ||
              Op.NumInputs < 1 || !Op.Inputs[0].isConst())
            continue;
          std::vector<MedReturnReg> Desc;
          for (size_t J = I + 1; J < Blk.Ops.size(); ++J) {
            const auto &Ex = Blk.Ops[J];
            if (Ex.Opcode != NdOp::SUBBYTES || Ex.NumInputs < 1 ||
                Ex.Inputs[0].Kind != MedVar::Temp ||
                Ex.Inputs[0].Id != Op.Output.Id ||
                Ex.Output.Kind != MedVar::Reg)
              break;
            MedReturnReg RR;
            RR.RegOff = Ex.Output.RegOff;
            RR.Size = Ex.Output.Size;
            RR.IsFP = TRI.isVectorReg(Ex.Output.RegOff);
            Desc.push_back(RR);
          }
          if (Desc.size() >= 2)
            StructRetCallees[Op.Inputs[0].ConstVal] = Desc;
        }
    if (!StructRetCallees.empty())
      for (auto &MF : Result.MedFuncs)
        if (auto It = StructRetCallees.find(MF.Entry);
            It != StructRetCallees.end())
          MF.MultiReturn = It->second;
  }
}

// Struct-return tail-call forwarder shape propagation: `struct S
// f(args){return g(args);}` lowers at -O2 to a lone `b g` (a tail call), so
// f's RETURN forwards exactly g's return value -- f and g have the SAME
// return shape.  The caller-side remodel above learns f's shape from f's own
// direct callers (`main` extracts f's fields), but g -- reached only through
// f's tail call -- is never the target of a struct-return call site, so its
// shape is still unknown here. Propagate f's proven multi-register shape
// FORWARD into such a callee.  Done BEFORE the body-based recovery below and
// only into a callee whose shape is still unknown, so (a) a directly-called
// callee keeps its own caller-proven shape, and (b) a forwarder-reached
// callee takes its forwarder's exact shape rather than the heuristic body
// scan (which can miss a field -- e.g. recover only 2 of a 3-double HFA, or 0
// of a `{x, 2x}` pair whose first field is the passed-through argument).
// Iterated to a fixpoint so a forwarder chain (f -> mid -> g) propagates all
// the way down.  64-bit register struct returns only (the caller-side remodel
// is gated to PointerSize==8).
void propagateStructReturnForwarderShapes(const BinaryImage &Img,
                                          PipelineResult &Result) {
  const auto &TRI = getTargetRegInfo(Img.Arch);
  if (TRI.PointerSize == 8) {
    auto IsCompatibleSubset = [](const std::vector<MedReturnReg> &Subset,
                                 const std::vector<MedReturnReg> &Shape) {
      if (Subset.size() >= Shape.size())
        return false;
      size_t ShapeIdx = 0;
      for (const MedReturnReg &Field : Subset) {
        while (ShapeIdx < Shape.size() &&
               (Field.RegOff != Shape[ShapeIdx].RegOff ||
                Field.Size != Shape[ShapeIdx].Size ||
                Field.IsFP != Shape[ShapeIdx].IsFP))
          ++ShapeIdx;
        if (ShapeIdx == Shape.size())
          return false;
        ++ShapeIdx;
      }
      return true;
    };
    std::map<va_t, MedFunc *> ByEntry;
    for (auto &MF : Result.MedFuncs)
      ByEntry[MF.Entry] = &MF;
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (auto &MF : Result.MedFuncs) {
        if (MF.MultiReturn.size() < 2)
          continue;
        for (const auto &Blk : MF.Blocks) {
          bool Found = false;
          for (size_t I = 0; I + 1 < Blk.Ops.size(); ++I) {
            const auto &Op = Blk.Ops[I];
            if (Op.Opcode != NdOp::CALL || Op.NumInputs < 1 ||
                !Op.Inputs[0].isConst() || Op.Output.Kind != MedVar::Reg)
              continue;
            const auto &Ret = Blk.Ops[I + 1];
            if (Ret.Opcode != NdOp::RETURN || Ret.NumInputs < 1 ||
                Ret.Inputs[0].Kind != MedVar::Reg ||
                Ret.Inputs[0].RegOff != Op.Output.RegOff)
              continue;
            auto It = ByEntry.find(Op.Inputs[0].ConstVal);
            if (It != ByEntry.end() &&
                (It->second->MultiReturn.empty() ||
                 IsCompatibleSubset(It->second->MultiReturn, MF.MultiReturn))) {
              It->second->MultiReturn = MF.MultiReturn;
              Changed = true;
            }
            Found = true;
            break;
          }
          if (Found)
            break;
        }
      }
    }
  }
}

// A function reached ONLY through a function pointer is never the target of a
// direct CALL, so the caller-side struct-return remodel above never learns
// its multi-register return shape (an indirect call site cannot name its
// callee). Recover it from the callee's own body instead: clang's by-value
// small-struct return loads each field into its return register straight-line
// before RETURN
// (`ldr x0,[..]; ldr x1,[..]; ret` for a GP struct; `ldr d0; ldr d1; ret` for
// a 2-double HFA).  A candidate return register (x0:x1, or the HFA v0..v3) is
// a genuine return field when, before a RETURN with no intervening call, it
// is written by a value op (not a live-in self-copy) whose result is LIVE-OUT
// -- not read again before the RETURN.  The live-out test is what separates a
// real field from an FP scratch register: a `double f(double a,double
// b){return a*b;}` reloads b into d1 and then CONSUMES it in the multiply (d1
// is read after its last write), so d1 is not a field; a 2-double HFA leaves
// d0 and d1 untouched after loading them, so both are fields.  AArch64
// returns a small aggregate either all-GP or all-HFA, never mixed (a mixed
// live-out set is a scalar FP return plus the lifter's dead integer
// placeholder), so a mixed set is rejected.  declareFunc then emits the
// aggregate return ({i64,i64} / {double,double} / {float,float}) and the
// indirect call site (modeled by modelCallStructReturn for INDIR_CALL) reads
// every field.  Gated to AArch64 (the x86-64 RAX/RDX overlap with div/mul
// byproducts is handled on the direct-call path's redefine tracking) and to
// functions WITHOUT an already recovered MultiReturn (a directly-called
// struct returner keeps the proven Pipeline- propagated shape).
void recoverStructReturnFromBody(const BinaryImage &Img,
                                 PipelineResult &Result) {
  const auto &TRI = getTargetRegInfo(Img.Arch);
  struct RetCand {
    uint64_t RegOff;
    bool IsFP;
  };
  std::vector<RetCand> Cands;
  for (uint64_t R : TRI.IntReturnRegs)
    Cands.push_back({R, false});
  for (uint64_t R : TRI.FPReturnRegs)
    Cands.push_back({R, true});
  if (Img.Arch == Arch::AArch64 && Cands.size() >= 2) {
    for (auto &MF : Result.MedFuncs) {
      if (!MF.MultiReturn.empty())
        continue;

      // A single 128-bit vector returned by value in V0 (a NEON `int32x4` /
      // `float __attribute__((vector_size(16)))`): the callee assembles the
      // whole 16-byte value into V0 with REAL data in the high 64 bits and
      // returns it, unlike a 2..4 element HFA which puts each element in its
      // own D/S register.  The HFA recovery below would pair the wide V0 with
      // a stale V1 (often a reloaded argument register) into a bogus
      // {double,double}, silently corrupting lanes 2-3.  The discriminator is
      // the high-64 content, NOT the bare write width: the lifter models
      // every `ldr d0` (an 8-byte HFA element) as `INT_ZEXT Q0 <- d` -- also
      // a 16-byte V0 write, but a zero-extension whose high 64 bits are 0.  A
      // genuine vector instead reaches V0 through a CONCAT lane-assembly /
      // 16-byte LOAD / NEON op (real high lanes).  Detect the latter before a
      // RETURN with no genuine GP-return-register write (a pure FP/vector
      // return, so not an __int128 returned in X0:X1 that clang stages
      // through V0 at the call site) and type the function as a 16-byte
      // vector return; the emitter's fpAbiType lowers it to a <2 x i64>
      // return carrying all 16 bytes (V0 read whole).
      if (!TRI.FPReturnRegs.empty()) {
        const uint64_t V0 = TRI.FPReturnRegs.front();
        std::map<std::pair<int, int>, const MedOp *> Defs;
        for (const auto &Blk : MF.Blocks)
          for (const auto &O : Blk.Ops)
            if (O.Output.Id >= 0)
              Defs[{O.Output.Id, O.Output.SSAVer}] = &O;
        // True iff \p V (traced through COPY chains) is a genuine >=16-byte
        // value -- a real vector -- rather than a zero-extended <=8-byte
        // scalar (an `INT_ZEXT Qn <- Dn` HFA element / scalar FP).
        auto isWideVec = [&](MedVar V) -> bool {
          for (int Depth = 0; Depth < 8; ++Depth) {
            if (V.Kind == MedVar::Const)
              return false;
            auto It = Defs.find({V.Id, V.SSAVer});
            if (It == Defs.end())
              return V.Size >= 16;
            const MedOp *D = It->second;
            if (D->Opcode == NdOp::INT_ZEXT)
              return false; // zero-extended scalar/HFA element, high 64 = 0
            if (D->Opcode == NdOp::COPY && D->NumInputs >= 1) {
              V = D->Inputs[0];
              continue;
            }
            return D->Output.Size >= 16; // CONCAT / LOAD / NEON real 16 bytes
          }
          return false;
        };
        bool WideVecRet = false;
        for (const auto &Blk : MF.Blocks) {
          int RetIdx = -1;
          for (size_t I = 0; I < Blk.Ops.size(); ++I)
            if (Blk.Ops[I].Opcode == NdOp::RETURN) {
              RetIdx = static_cast<int>(I);
              break;
            }
          if (RetIdx < 0)
            continue;
          const MedOp *V0Write = nullptr;
          bool GPWritten = false;
          bool GPLiveFromCall = false;
          for (int J = RetIdx - 1; J >= 0; --J) {
            const auto &O = Blk.Ops[J];
            if (O.Opcode == NdOp::CALL || O.Opcode == NdOp::INDIR_CALL) {
              // The function's live return may be THIS call's own GP result,
              // left in the GP return register and not overwritten before the
              // RETURN -- a `p = malloc(n); /* fill *p with NEON q-register
              // stores */ return p;` tail.  The V0 writes after the call are
              // then dead memcpy scratch (the loaded init data on its way to
              // memory), not a vector return; without this the pointer return
              // is mistyped <2 x i64> and the caller dereferences garbage.
              if (!GPWritten && O.Output.Kind == MedVar::Reg)
                for (uint64_t IR : TRI.IntReturnRegs)
                  if (O.Output.RegOff == IR)
                    GPLiveFromCall = true;
              break;
            }
            if (O.Output.Kind != MedVar::Reg || O.Output.Size == 0)
              continue;
            bool SelfCopy = O.Opcode == NdOp::COPY && O.NumInputs >= 1 &&
                            O.Inputs[0].Kind == MedVar::Reg &&
                            O.Inputs[0].RegOff == O.Output.RegOff;
            if (SelfCopy)
              continue;
            if (O.Output.RegOff == V0 && !V0Write)
              V0Write = &O;
            for (uint64_t IR : TRI.IntReturnRegs)
              if (O.Output.RegOff == IR)
                GPWritten = true;
          }
          if (V0Write && !GPWritten && !GPLiveFromCall &&
              isWideVec(V0Write->Output)) {
            WideVecRet = true;
            break;
          }
        }
        if (WideVecRet) {
          MF.ReturnType = NdType::makeFloat(16);
          continue; // single vector return, not a multi-register HFA
        }
      }

      std::vector<MedReturnReg> Fields;
      for (const auto &Blk : MF.Blocks) {
        int RetIdx = -1;
        for (size_t I = 0; I < Blk.Ops.size(); ++I)
          if (Blk.Ops[I].Opcode == NdOp::RETURN) {
            RetIdx = static_cast<int>(I);
            break;
          }
        if (RetIdx < 0)
          continue;
        std::vector<MedReturnReg> BlkFields;
        for (const auto &C : Cands) {
          // Last genuine (non-self-copy) write to this return register before
          // the RETURN, plus its natural element width.  A Q-register zero
          // extension (size 16) is the lifter's normalization of a D/S write,
          // not the field width, so the element size takes the widest write
          // that still fits a single field (<= 8 bytes).
          int WIdx = -1;
          uint16_t ElemSz = 0;
          for (int J = RetIdx - 1; J >= 0; --J) {
            const auto &O = Blk.Ops[J];
            if (O.Opcode == NdOp::CALL || O.Opcode == NdOp::INDIR_CALL)
              break;
            if (O.Output.Kind != MedVar::Reg || O.Output.Size == 0 ||
                O.Output.RegOff != C.RegOff)
              continue;
            bool SelfCopy = O.Opcode == NdOp::COPY && O.NumInputs >= 1 &&
                            O.Inputs[0].Kind == MedVar::Reg &&
                            O.Inputs[0].RegOff == O.Output.RegOff;
            if (SelfCopy)
              continue;
            if (WIdx < 0)
              WIdx = J;
            if (O.Output.Size <= 8)
              ElemSz = std::max(ElemSz, O.Output.Size);
          }
          if (WIdx < 0)
            continue;
          // Live-out test: the last write's value must not be read again
          // before the RETURN (an FP scratch multiplicand is read by its
          // consumer).
          bool Consumed = false;
          for (int J = WIdx + 1; J < RetIdx && !Consumed; ++J) {
            const auto &O = Blk.Ops[J];
            // The lifter emits architectural flag calculations after the
            // value-producing instruction.  Those reads describe side effects
            // of the write; they do not consume the register's returned value.
            if (O.Output.Kind == MedVar::Flag && O.Addr == Blk.Ops[WIdx].Addr)
              continue;
            for (uint8_t K = 0; K < O.NumInputs; ++K)
              if (O.Inputs[K].Kind == MedVar::Reg &&
                  O.Inputs[K].RegOff == C.RegOff) {
                Consumed = true;
                break;
              }
          }
          if (Consumed)
            continue;
          MedReturnReg RR;
          RR.RegOff = C.RegOff;
          RR.IsFP = C.IsFP;
          RR.Size = ElemSz ? (ElemSz >= 8 ? 8 : (ElemSz >= 4 ? 4 : ElemSz)) : 8;
          BlkFields.push_back(RR);
        }
        if (BlkFields.size() >= 2) {
          Fields = std::move(BlkFields);
          break;
        }
      }
      if (Fields.size() < 2)
        continue;
      bool AnyInt = false, AnyFP = false;
      for (const auto &F : Fields)
        (F.IsFP ? AnyFP : AnyInt) = true;
      if (AnyInt && AnyFP)
        continue; // AArch64 never mixes GP and FP fields in a register return
      MF.MultiReturn = std::move(Fields);
    }
  }
}

// Materialize the multi-register return at EVERY call site to a known
// multi-register-return callee -- not only the sites whose result the caller
// reads directly (modelCallStructReturn / the StructRetCallees + body passes
// above handled those).  An unrolled loop `for (i) z = f(z);` over an
// HFA-returning f (-O2) chains each call's FP return registers (d0/d1)
// straight into the next call's FP argument registers with NO intervening
// move; the intermediate calls' results are read by no explicit op, so they
// were left as a single-register CALL whose d0/d1 fields were never
// materialized, and the following call's argument recovery (recoverCallAbi,
// below) then resolved the chained FP arguments to the stale pre-loop d0/d1
// (0) -- every iteration collapses to f(0) (cplxmul: a Julia z=z*z+c step
// degenerates to its constant c).  Re-model each such call into a flat
// aggregate temp + one SUBBYTES per field so the return registers are defined
// and the next call's argument recovery picks them up.  Run AFTER all the
// MultiReturn-setting passes and BEFORE recoverCallAbi.  AArch64 register
// returns only; only a call still producing a single register output (an
// already-remodeled call has a Temp output) is touched.  Skip tail-call
// forwarders (`CALL ret; RETURN ret` with no intervening ops): recoverCallAbi
// keys IsTailReturn off the CALL still writing the integer return register;
// remodeling them here breaks forwarded FP-arg live-in recovery and is
// redundant — Phase B (after recoverCallAbi) remodels struct-return
// forwarders to temp+SUBBYTES instead.
void materializeKnownStructReturnCallSites(const BinaryImage &Img,
                                           PipelineResult &Result) {
  const auto &TRI = getTargetRegInfo(Img.Arch);
  if (Img.Arch == Arch::AArch64 && TRI.PointerSize == 8) {
    auto findCallSite = [](MedFunc &MF, uint32_t SiteId) -> MedOp * {
      for (auto &Blk : MF.Blocks)
        for (auto &Op : Blk.Ops)
          if (Op.CallSiteId == SiteId &&
              (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL))
            return &Op;
      return nullptr;
    };
    auto isTailForwarder = [](const MedFunc &MF) {
      for (const auto &Blk : MF.Blocks)
        for (size_t I = 0; I + 1 < Blk.Ops.size(); ++I) {
          const MedOp &Call = Blk.Ops[I];
          const MedOp &Ret = Blk.Ops[I + 1];
          if (Call.Opcode == NdOp::CALL && Call.Output.Kind == MedVar::Reg &&
              Ret.Opcode == NdOp::RETURN && Ret.NumInputs >= 1 &&
              Ret.Inputs[0].Kind == MedVar::Reg &&
              Ret.Inputs[0].RegOff == Call.Output.RegOff)
            return true;
        }
      return false;
    };
    auto candidateShape = [&](const MedStructReturnCandidate &Candidate) {
      std::vector<MedReturnReg> Shape;
      for (const MedVar &Field : Candidate.Fields) {
        MedReturnReg RR;
        RR.RegOff = Field.RegOff;
        RR.Size = Field.Size;
        RR.IsFP = TRI.isVectorReg(Field.RegOff);
        Shape.push_back(RR);
      }
      return Shape;
    };
    auto isCompleteShape = [&](const std::vector<MedReturnReg> &Shape) {
      if (Shape.size() < 2)
        return false;
      bool IsFP = Shape.front().IsFP;
      llvm::ArrayRef<uint64_t> Regs =
          IsFP ? TRI.FPReturnRegs : TRI.IntReturnRegs;
      if (Shape.size() > Regs.size())
        return false;
      for (size_t I = 0; I < Shape.size(); ++I)
        if (Shape[I].IsFP != IsFP || Shape[I].RegOff != Regs[I])
          return false;
      return true;
    };

    // A tail forwarder has no return-body writes of its own.  Complete field
    // evidence from one of its direct callers seeds its shape, after which the
    // existing forwarder propagation proves the inner callee's shape too.
    std::map<va_t, MedFunc *> ByEntry;
    for (auto &MF : Result.MedFuncs)
      ByEntry[MF.Entry] = &MF;
    for (auto &Caller : Result.MedFuncs)
      for (const MedStructReturnCandidate &Candidate :
           Caller.StructReturnCandidates) {
        MedOp *Call = findCallSite(Caller, Candidate.CallSiteId);
        if (!Call || Call->Opcode != NdOp::CALL || Call->NumInputs < 1 ||
            !Call->Inputs[0].isConst())
          continue;
        auto It = ByEntry.find(Call->Inputs[0].ConstVal);
        if (It == ByEntry.end() || !It->second->MultiReturn.empty() ||
            !isTailForwarder(*It->second))
          continue;
        std::vector<MedReturnReg> Shape = candidateShape(Candidate);
        if (isCompleteShape(Shape))
          It->second->MultiReturn = std::move(Shape);
      }
    propagateStructReturnForwarderShapes(Img, Result);

    std::map<va_t, std::vector<MedReturnReg>> MRByEntry;
    for (const auto &MF : Result.MedFuncs) {
      if (MF.MultiReturn.size() < 2)
        continue;
      MRByEntry[MF.Entry] = MF.MultiReturn;
    }
    for (auto &Caller : Result.MedFuncs) {
      for (const MedStructReturnCandidate &Candidate :
           Caller.StructReturnCandidates) {
        MedOp *Call = findCallSite(Caller, Candidate.CallSiteId);
        if (!Call || Call->Opcode != NdOp::CALL || Call->NumInputs < 1 ||
            !Call->Inputs[0].isConst())
          continue;
        const Import *Imp = Img.findImportAt(Call->Inputs[0].ConstVal);
        if (!Imp)
          continue;
        auto Sig = libc::libcArity(
            llvm::StringRef(Imp->Name).ltrim('_').str());
        if (!Sig || !Sig->FpRetComplex)
          continue;
        std::vector<MedReturnReg> Shape = candidateShape(Candidate);
        if (Shape.size() != 2 || !isCompleteShape(Shape) ||
            !Shape[0].IsFP || !Shape[1].IsFP)
          continue;
        MRByEntry[Call->Inputs[0].ConstVal] = std::move(Shape);
      }
    }
    if (!MRByEntry.empty())
      for (auto &MF : Result.MedFuncs) {
        int NextId = 0, NextVer = 0;
        auto bump = [&](const MedVar &V) {
          NextVer = std::max(NextVer, V.SSAVer + 1);
          NextId = std::max(NextId, V.Id + 1);
        };
        for (const auto &B : MF.Blocks) {
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
        for (const MedCallClobber &Clobber : MF.CallClobbers) {
          bump(Clobber.Value);
          if (Clobber.PreservedPrefixSize > 0)
            bump(Clobber.PreservedInput);
        }
        for (const MedStructReturnCandidate &Candidate :
             MF.StructReturnCandidates)
          for (const MedVar &Field : Candidate.Fields)
            bump(Field);

        std::set<uint32_t> MaterializedSites;
        for (auto &Blk : MF.Blocks) {
          for (size_t OI = 0; OI < Blk.Ops.size(); ++OI) {
            auto &Op = Blk.Ops[OI];
            if (Op.Opcode != NdOp::CALL || Op.NumInputs < 1 ||
                !Op.Inputs[0].isConst())
              continue;
            // Tail-call forwarder: leave CALL on the integer return register
            // until recoverCallAbi + Phase B (see block comment above).
            if (OI + 1 < Blk.Ops.size() &&
                Blk.Ops[OI + 1].Opcode == NdOp::RETURN)
              continue;
            // A call already remodeled (FP/wide/struct) produces a
            // non-register output; only the default single-register CALL is a
            // candidate.
            if (Op.Output.Kind != MedVar::Reg)
              continue;
            auto It = MRByEntry.find(Op.Inputs[0].ConstVal);
            if (It == MRByEntry.end())
              continue;
            uint32_t SiteId = Op.CallSiteId;
            va_t CallAddr = Op.Addr;
            const auto &Fields = It->second;
            uint16_t Total = 0;
            for (const auto &F : Fields)
              Total += F.Size;
            MedVar OrigOut = Op.Output;
            MedVar Tv;
            Tv.Kind = MedVar::Temp;
            Tv.TheArch = Arch::AArch64;
            Tv.Id = NextId++;
            Tv.SSAVer = 0;
            Tv.Size = Total;
            Op.Output = Tv;
            std::vector<MedOp> Extracts;
            const MedStructReturnCandidate *Candidate = nullptr;
            for (const MedStructReturnCandidate &C : MF.StructReturnCandidates)
              if (C.CallSiteId == Op.CallSiteId) {
                Candidate = &C;
                break;
              }
            uint16_t Cum = 0;
            for (const auto &F : Fields) {
              MedVar Out;
              if (F.RegOff == OrigOut.RegOff) {
                Out = OrigOut;
              } else {
                const MedVar *Exact = nullptr;
                if (Candidate)
                  for (const MedVar &Field : Candidate->Fields)
                    if (Field.RegOff == F.RegOff) {
                      Exact = &Field;
                      break;
                    }
                if (!Exact)
                  for (const MedCallClobber &Clobber : MF.CallClobbers)
                    if (Clobber.CallSiteId == Op.CallSiteId &&
                        Clobber.Value.RegOff == F.RegOff &&
                        (!Exact || Clobber.Value.Size == F.Size)) {
                      Exact = &Clobber.Value;
                      if (Exact->Size == F.Size)
                        break;
                    }
                if (Exact)
                  Out = *Exact;
                else {
                  Out.Kind = MedVar::Reg;
                  Out.TheArch = Arch::AArch64;
                  Out.RegOff = F.RegOff;
                  Out.Id = NextId++;
                  Out.SSAVer = NextVer++;
                }
              }
              Out.Size = F.Size;
              MedOp Ex;
              Ex.Opcode = NdOp::SUBBYTES;
              Ex.Addr = Op.Addr;
              Ex.Output = Out;
              Ex.addInput(Tv);
              Ex.addInput(MedVar::makeConst(Cum, TRI.PointerSize));
              Extracts.push_back(Ex);

              // Define every width-view clobbered by this call from the same
              // proven return field, so existing exact SSA uses stay linked.
              for (const MedCallClobber &Clobber : MF.CallClobbers) {
                if (Clobber.CallSiteId != Op.CallSiteId ||
                    Clobber.Value.RegOff != F.RegOff || Clobber.Value == Out)
                  continue;
                MedOp Alias;
                Alias.Addr = CallAddr;
                Alias.Output = Clobber.Value;
                if (Alias.Output.Size < Out.Size) {
                  Alias.Opcode = NdOp::SUBBYTES;
                  Alias.addInput(Out);
                  Alias.addInput(MedVar::makeConst(0, TRI.PointerSize));
                } else if (Alias.Output.Size > Out.Size) {
                  Alias.Opcode = NdOp::INT_ZEXT;
                  Alias.addInput(Out);
                } else {
                  Alias.Opcode = NdOp::COPY;
                  Alias.addInput(Out);
                }
                Extracts.push_back(Alias);
              }
              Cum += F.Size;
            }
            MF.CallClobbers.erase(
                std::remove_if(MF.CallClobbers.begin(), MF.CallClobbers.end(),
                               [&](const MedCallClobber &Clobber) {
                                 if (Clobber.CallSiteId != Op.CallSiteId)
                                   return false;
                                 return std::any_of(
                                     Fields.begin(), Fields.end(),
                                     [&](const MedReturnReg &F) {
                                       return F.RegOff == Clobber.Value.RegOff;
                                     });
                               }),
                MF.CallClobbers.end());
            Blk.Ops.insert(Blk.Ops.begin() + OI + 1, Extracts.begin(),
                           Extracts.end());
            OI += Extracts.size();
            MaterializedSites.insert(SiteId);
          }
        }
        MF.StructReturnCandidates.erase(
            std::remove_if(MF.StructReturnCandidates.begin(),
                           MF.StructReturnCandidates.end(),
                           [&](const MedStructReturnCandidate &Candidate) {
                             return MaterializedSites.count(
                                        Candidate.CallSiteId) != 0;
                           }),
            MF.StructReturnCandidates.end());
      }
  }
}

// Struct-return tail-call forwarder: `struct S f(args){return g(args);}`
// lowers at -O2 to a lone `b g`, rewritten (CFGBuilder::rewriteAsTailCall) to
// `CALL ret; RETURN ret` carrying only the single integer return register.
// When f returns a small struct across multiple registers -- proven by f's
// own callers, so MF.MultiReturn is already set by the caller-side
// StructRetCallees pass above -- the forwarding call captures only the first
// field register and the RETURN reassembles the trailing fields from
// registers nothing ever wrote (they come back 0: `structfwd` returns {a, 0}
// instead of {a, 2a}).  Two repairs, mirroring how an ordinary struct-return
// call site is modeled (modelCallStructReturn): (1) re-type the in-module
// callee g to the same multi-register struct -- a `return g(...)` returns
// exactly what g returns -- so g actually produces every field register; (2)
// turn the forwarding CALL into a flat aggregate temp with one SUBBYTES per
// field so the RETURN's struct path reads each field register (the emitter
// flattens g's struct result into that temp, the inverse of the callee-side
// aggregate assembly).  Run after the main recoverCallAbi loop so the
// forwarder's IsTailReturn argument recovery (which keys off the CALL still
// writing the integer return register) already ran.  64-bit register struct
// returns only (modelCallStructReturn and the caller-side remodel are gated
// to PointerSize==8; 32-bit aggregates return through sret / the wide-int
// pair).
void remodelStructReturnForwarderCalls(const BinaryImage &Img,
                                       PipelineResult &Result) {
  const auto &TRI = getTargetRegInfo(Img.Arch);
  if (TRI.PointerSize == 8) {
    std::map<va_t, MedFunc *> ByEntryMut;
    for (auto &MF : Result.MedFuncs)
      ByEntryMut[MF.Entry] = &MF;
    for (auto &MF : Result.MedFuncs) {
      // The function returns a multi-register struct (proven by its callers).
      // The forwarder is detected structurally below by the tail `CALL ret;
      // RETURN ret` pattern (rewriteAsTailCall's output) -- not by an empty
      // parameter list, since recoverCallAbi has already surfaced the
      // forwarded incoming arguments as this function's parameters.
      if (MF.MultiReturn.size() < 2)
        continue;
      bool Rewrote = false;
      for (auto &Blk : MF.Blocks) {
        for (size_t I = 0; I + 1 < Blk.Ops.size(); ++I) {
          auto &Op = Blk.Ops[I];
          if (Op.Opcode != NdOp::CALL || Op.NumInputs < 1 ||
              !Op.Inputs[0].isConst() || Op.Output.Kind != MedVar::Reg)
            continue;
          auto &Ret = Blk.Ops[I + 1];
          if (Ret.Opcode != NdOp::RETURN || Ret.NumInputs < 1 ||
              Ret.Inputs[0].Kind != MedVar::Reg ||
              Ret.Inputs[0].RegOff != Op.Output.RegOff)
            continue;
          // Only an in-module callee can be re-typed to the struct return; a
          // libc import keeps its own ABI (a struct-returning libc forwarder
          // would need a libcArity entry, rare -- left as a future item).
          auto CIt = ByEntryMut.find(Op.Inputs[0].ConstVal);
          if (CIt == ByEntryMut.end())
            continue;
          MedFunc *G = CIt->second;
          if (G->MultiReturn.empty())
            G->MultiReturn = MF.MultiReturn; // g returns what f returns
          // The forwarder's proven shape and the callee's emitted shape must
          // agree (same register in each field) so the SUBBYTES outputs line
          // up with both the call's packed result and the RETURN reassembly.
          if (G->MultiReturn.size() != MF.MultiReturn.size())
            continue;
          bool Match = true;
          for (size_t K = 0; K < MF.MultiReturn.size() && Match; ++K)
            if (G->MultiReturn[K].RegOff != MF.MultiReturn[K].RegOff)
              Match = false;
          if (!Match)
            continue;
          const auto &Fields = MF.MultiReturn; // == G->MultiReturn

          // Fresh Id/SSAVer for the flat result temp and per-field extracts.
          int MaxId = 0, MaxVer = 0;
          auto bump = [&](const MedVar &V) {
            MaxId = std::max(MaxId, V.Id);
            MaxVer = std::max(MaxVer, V.SSAVer);
          };
          for (const auto &B : MF.Blocks) {
            for (const auto &O : B.Ops) {
              bump(O.Output);
              for (uint8_t K = 0; K < O.NumInputs; ++K)
                bump(O.Inputs[K]);
            }
            for (const auto &Ph : B.Phis) {
              bump(Ph.Output);
              for (const auto &A : Ph.Args)
                bump(A.second);
            }
          }
          int NextId = MaxId + 1, NextVer = MaxVer + 1;
          uint16_t Total = 0;
          for (const auto &F : Fields)
            Total += F.Size ? F.Size : 8;

          MedVar Tv;
          Tv.Kind = MedVar::Temp;
          Tv.TheArch = Img.Arch;
          Tv.Id = NextId++;
          Tv.SSAVer = 0;
          Tv.Size = Total;
          Op.Output = Tv;

          std::vector<MedOp> Extracts;
          uint16_t Cum = 0;
          for (const auto &F : Fields) {
            uint16_t Sz = F.Size ? F.Size : 8;
            MedVar Out;
            Out.Kind = MedVar::Reg;
            Out.TheArch = Img.Arch;
            Out.RegOff = F.RegOff;
            Out.Id = NextId++;
            Out.SSAVer = NextVer++;
            Out.Size = Sz;
            MedOp Ex;
            Ex.Opcode = NdOp::SUBBYTES;
            Ex.Addr = Op.Addr;
            Ex.Output = Out;
            Ex.addInput(Tv);
            Ex.addInput(MedVar::makeConst(Cum, TRI.PointerSize));
            Extracts.push_back(Ex);
            Cum += Sz;
          }
          // rewriteAsTailCall hard-codes the integer return register (X0/RAX)
          // even for HFA forwarders whose fields live in v0/d0.. — wire the
          // RETURN to the first extracted field so SSA stays valid (the
          // struct return path reassembles every field by register
          // regardless).
          if (!Extracts.empty())
            Ret.Inputs[0] = Extracts.front().Output;
          Blk.Ops.insert(Blk.Ops.begin() + I + 1, Extracts.begin(),
                         Extracts.end());
          Rewrote = true;
          break;
        }
        if (Rewrote)
          break;
      }
    }
  }
}

} // namespace neverd
