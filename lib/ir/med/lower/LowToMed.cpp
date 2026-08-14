//===- LowToMed.cpp - LowIR to MedIR conversion orchestration ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// LowIR to MedIR (SSA) conversion: main entry point and nd-var-to-MedVar
/// mapping. Stack analysis, sub-register fixups, call return-value ABI
/// modeling, and the individual passes live in their own translation units.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/LowToMed.h"

#include "neverd/ir/TargetRegInfo.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <map>

#define DEBUG_TYPE "neverd-low-to-med"

namespace neverd {

//===----------------------------------------------------------------------===//
// Stack-probe (chkstk) call neutralization
//===----------------------------------------------------------------------===//

void LowToMedConverter::neutralizeStackProbeCalls(MedFunc &Func) {
  if (!StackProbeSlots || StackProbeSlots->empty())
    return;

  for (auto &Blk : Func.Blocks) {
    for (size_t CI = 0; CI < Blk.Ops.size(); ++CI) {
      MedOp &Op = Blk.Ops[CI];
      if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
          Op.NumInputs < 1 || Op.Output.Size == 0)
        continue;

      // Within-block backward def of a register/temp var, searching only the
      // ops BEFORE position \p Before.  Pre-SSA a register Id is shared by
      // every write (e.g. `adrp x16` then `ldr x16,[x16,#off]` both define
      // x16), so the search must start above each use site rather than always
      // above the call — otherwise resolving the load address's base would
      // re-find the load itself.  Returns {def, its index}.  The probe's
      // GOT-load target chain lives entirely in the prologue (entry) block, so
      // the within-block scope is sufficient and safe.
      auto findDefBefore =
          [&](const MedVar &V,
              size_t Before) -> std::pair<const MedOp *, size_t> {
        if (!V.isConst())
          for (size_t J = Before; J-- > 0;) {
            const MedOp &O = Blk.Ops[J];
            if (O.Output.Size > 0 && O.Output.Kind == V.Kind &&
                O.Output.Id == V.Id)
              return {&O, J};
          }
        return {nullptr, 0};
      };

      // Resolve a var to a constant address, threading copies/width casts and a
      // folded `GOT_base + slot_offset` add (mirrors the emitter's constAddr in
      // isStackProbeCall, but with the position-aware within-block def finder).
      std::function<bool(const MedVar &, uint64_t &, int, size_t)> constAddr =
          [&](const MedVar &V, uint64_t &Out, int Depth,
              size_t Before) -> bool {
        if (Depth > 8)
          return false;
        if (V.isConst()) {
          Out = V.ConstVal;
          return true;
        }
        auto [D, DIdx] = findDefBefore(V, Before);
        if (!D || D->NumInputs < 1)
          return false;
        switch (D->Opcode) {
        case NdOp::COPY:
        case NdOp::INT_ZEXT:
        case NdOp::INT_SEXT:
          return constAddr(D->Inputs[0], Out, Depth + 1, DIdx);
        case NdOp::SUBBYTES:
          return D->NumInputs >= 2 && D->Inputs[1].isConst() &&
                 D->Inputs[1].ConstVal == 0 &&
                 constAddr(D->Inputs[0], Out, Depth + 1, DIdx);
        case NdOp::INT_ADD: {
          uint64_t A = 0, B = 0;
          if (D->NumInputs >= 2 &&
              constAddr(D->Inputs[0], A, Depth + 1, DIdx) &&
              constAddr(D->Inputs[1], B, Depth + 1, DIdx)) {
            Out = A + B;
            return true;
          }
          return false;
        }
        default:
          return false;
        }
      };

      // The call target is `LOAD <slot>` (possibly threaded through copies); a
      // direct CALL to the routine's own address has no GOT indirection.
      uint64_t SlotAddr = 0;
      bool HaveSlot = false;
      auto [D, DIdx] = findDefBefore(Op.Inputs[0], CI);
      for (int Guard = 0; D && Guard <= 8; ++Guard) {
        if (D->Opcode == NdOp::COPY && D->NumInputs >= 1) {
          auto Next = findDefBefore(D->Inputs[0], DIdx);
          D = Next.first;
          DIdx = Next.second;
          continue;
        }
        if (D->Opcode == NdOp::LOAD && D->NumInputs >= 1)
          HaveSlot = constAddr(D->Inputs[0], SlotAddr, 0, DIdx);
        break;
      }
      if (!HaveSlot && Op.Inputs[0].isConst()) {
        SlotAddr = Op.Inputs[0].ConstVal;
        HaveSlot = true;
      }
      if (!HaveSlot || !StackProbeSlots->count(static_cast<va_t>(SlotAddr)))
        continue;

      // Neutralize: the probe preserves argument registers, so its modeled x0
      // definition is spurious.  Clearing the output stops buildSsa's liveness
      // from killing the live-in argument register; the emitter still elides
      // the call by its target (isStackProbeCall keys off the call target
      // operand).
      Op.Output = MedVar{};
      Op.Output.Id = -1;
      Op.Output.Size = 0;
      Op.PreservesCallerSaved = true;
    }
  }
}

MedFunc LowToMedConverter::convert(const LowFunc &Low, Arch TheArch,
                                   BinaryFormat Fmt) {
  TargetArch = TheArch;
  NextVarId = 0;
  NextTempId = 0;
  NextCallSiteId = 1;
  StackSlots.clear();
  RegVarMap.clear();
  TempVarMap.clear();

  analyzeStack(Low);

  MedFunc Func;
  Func.Entry = Low.Entry;
  Func.Name = Low.Name;
  Func.JumpTables = Low.JumpTables;
  Func.ExceptionMetadata = Low.ExceptionMetadata;

  for (const auto &LB : Low.Blocks) {
    MedBlock MB;
    MB.Id = LB.Id;
    MB.StartAddr = LB.StartAddr;
    MB.EndAddr = LB.EndAddr;
    MB.Succs = LB.Succs;
    MB.Preds = LB.Preds;
    MB.ExceptionalSuccs = LB.ExceptionalSuccs;
    MB.ExceptionalPreds = LB.ExceptionalPreds;

    size_t BoundaryIndex = 0;
    for (size_t LowOpIndex = 0; LowOpIndex < LB.Ops.size(); ++LowOpIndex) {
      const LowOp &LOp = LB.Ops[LowOpIndex];
      while (BoundaryIndex < LB.InstructionBoundaries.size()) {
        const LowInstructionBoundary &Boundary =
            LB.InstructionBoundaries[BoundaryIndex];
        if (Boundary.FirstOp > LowOpIndex ||
            LowOpIndex - Boundary.FirstOp < Boundary.OpCount)
          break;
        ++BoundaryIndex;
      }

      MedOp MOp;
      MOp.Opcode = LOp.Opcode;
      MOp.Addr = LOp.Addr;
      if (MOp.Opcode == NdOp::CALL || MOp.Opcode == NdOp::INDIR_CALL) {
        MOp.CallSiteId = NextCallSiteId++;
        if (BoundaryIndex < LB.InstructionBoundaries.size()) {
          const LowInstructionBoundary &Boundary =
              LB.InstructionBoundaries[BoundaryIndex];
          if (Boundary.FirstOp <= LowOpIndex &&
              LowOpIndex - Boundary.FirstOp < Boundary.OpCount)
            MOp.DoesNotReturn = hasLowInstructionControlFlag(
                Boundary.ControlFlags, LowInstructionControlFlag::NoReturn);
        }
      }

      if (LOp.Output.Size > 0)
        MOp.Output = ndVarToMedVar(LOp.Output);

      for (uint8_t I = 0; I < LOp.NumInputs; ++I)
        MOp.addInput(ndVarToMedVar(LOp.Inputs[I]));

      MB.Ops.push_back(MOp);

      // i386 callee-cleanup: a direct CALL to a callee that pops bytes on
      // return (x86 `ret imm`, the SysV hidden struct-return (sret) pointer
      // pop) leaves the caller's stack pointer that many bytes higher than a
      // balanced call. The lifter modeled the CALL as SP-neutral, so add the
      // pop here -- before SSA, so the chain is well-formed -- and later stack
      // accesses (the cdecl `add esp, k` cleanup, the result-buffer reload) use
      // the corrected SP.
      if (CalleePopMap && MOp.Opcode == NdOp::CALL && MOp.NumInputs >= 1 &&
          MOp.Inputs[0].isConst()) {
        auto It = CalleePopMap->find(MOp.Inputs[0].ConstVal);
        if (It != CalleePopMap->end() && It->second > 0) {
          const auto &TRI = getTargetRegInfo(TheArch);
          MedVar Sp;
          Sp.Kind = MedVar::Reg;
          Sp.RegOff = TRI.StackPointer;
          Sp.Size = static_cast<uint16_t>(TRI.PointerSize);
          Sp.TheArch = TheArch;
          // Thread the SAME SSA variable the lifter uses for the stack pointer
          // (ndVarToMedVar keys registers by (RegOff,Size) in RegVarMap).  A
          // hand-built MedVar would otherwise keep the default id 0 — a foreign
          // variable — so buildSsa would split SP into two SSA names and a
          // loop- carried SP would take this call's intermediate +imm value
          // instead of the later balanced (post-`sub esp`) value, drifting each
          // iteration.
          auto SpKey = std::make_pair(static_cast<uint64_t>(TRI.StackPointer),
                                      static_cast<uint16_t>(TRI.PointerSize));
          auto SpIt = RegVarMap.find(SpKey);
          Sp.Id = (SpIt != RegVarMap.end()) ? SpIt->second
                                            : (RegVarMap[SpKey] = allocVarId());
          MedOp Adj;
          Adj.Opcode = NdOp::INT_ADD;
          Adj.Addr = LOp.Addr;
          Adj.Output = Sp;
          Adj.addInput(Sp);
          Adj.addInput(
              MedVar::makeConst(static_cast<uint64_t>(It->second),
                                static_cast<uint16_t>(TRI.PointerSize)));
          MB.Ops.push_back(Adj);
        }
      }
    }

    Func.Blocks.push_back(std::move(MB));
  }

  // Before sub-register fixup: model calls to known i64-returning callees
  // (32-bit register-pair return EDX:EAX / R1:R0) as defining the pair. Running
  // ahead of fixupSubRegisters lets its implicit zero-extension widen BOTH the
  // low (EAX) and the synthesized high (EDX) halves into their 64-bit
  // containers symmetrically — otherwise only the low half (whose wide alias
  // survives from the call's original return-register output) reaches the
  // container and a post-call read of the high half resolves to the stale
  // pre-call value. buildSsa then creates the loop-carried high-half PHI for a
  // threaded i64 accumulator.  No-op unless the pipeline set the i64-callee set
  // (only known after whole-program return-type inference).
  modelKnownWideCallReturns(Func);
  debugVerifyMedFunc(Func, "modelKnownWideCallReturns");

  fixupSubRegisters(Func);
  debugVerifyMedFunc(Func, "fixupSubRegisters");

  simplifyCfg(Func);
  debugVerifyMedFunc(Func, "simplifyCfg");

  // ARM predication is flattened in LowIR as an instruction-local guard plus
  // its same-address effects.  Materialize that micro-CFG before SSA so the
  // skip path keeps the incoming architectural registers while the effect path
  // receives the new definitions.  Doing this in the LLVM emitter would be too
  // late: SSA would already have treated every effect as unconditional.
  materializePredicatedEffects(Func);
  debugVerifyMedFunc(Func, "materializePredicatedEffects");

  // Apple clang's prologue stack-probe (____chkstk_darwin) is modeled as an
  // ordinary call returning in x0; clear that spurious output before SSA so its
  // liveness does not kill the live-in argument registers (the probe preserves
  // every register except x16/x17).  No-op unless the pipeline provided the
  // chkstk slot set (Mach-O only).
  neutralizeStackProbeCalls(Func);
  debugVerifyMedFunc(Func, "neutralizeStackProbeCalls");

  buildSsa(Func);
  debugVerifyMedFunc(Func, "buildSsa");

  // Model a call's floating-point/vector return (x86-64 returns it in XMM0, a
  // caller-saved vector register the lifter did not model the call as
  // defining). Done before copy propagation so a post-call read of the result
  // register is not folded back to the pre-call argument value. Model a direct
  // call's small struct-by-value return across multiple registers (x86-64
  // eightbytes / AArch64 HFA) before modelCallFPReturn so it claims the FP
  // return register of a struct-returning call as one of the aggregate fields
  // rather than the lone scalar FP result.
  modelCallStructReturn(Func);
  debugVerifyMedFunc(Func, "modelCallStructReturn");

  modelCallFPReturn(Func);
  debugVerifyMedFunc(Func, "modelCallFPReturn");

  // Model a call's x87 floating-point return on i386 (the cdecl convention
  // leaves it on the x87 top-of-stack, st0): reconnect the post-call `fstp`
  // read of st0 to the call's result, which the lifter did not model.
  modelCallX87Return(Func);
  debugVerifyMedFunc(Func, "modelCallX87Return");

  // Model a call's 64-bit integer return on 32-bit targets (i386 EDX:EAX, ARM32
  // R1:R0): the lifter did not model the call as defining the high-half
  // register, so reconnect post-call reads of it to the call's high result.
  modelCallWideIntReturn(Func, TargetArch);
  debugVerifyMedFunc(Func, "modelCallWideIntReturn");

  // Post-SSA pass: fix sub-register reads that should reference a loop PHI.
  // When a sub-register (e.g. SIL) has SSAVer=0 (entry block definition)
  // but the current block has a PHI for a wider register (RSI), insert a
  // SUBBYTES and update the read to use the extracted value.
  {
    int MaxSSAVer = 0;
    for (auto &MB : Func.Blocks)
      for (auto &Op : MB.Ops)
        if (Op.Output.SSAVer > MaxSSAVer)
          MaxSSAVer = Op.Output.SSAVer;
    int NextVer = MaxSSAVer + 100;

    for (auto &MB : Func.Blocks) {
      if (MB.Phis.empty())
        continue;
      // Only apply to loop headers (blocks that have themselves as a
      // predecessor).
      bool IsLoopHeader = false;
      for (int P : MB.Preds)
        if (P == MB.Id)
          IsLoopHeader = true;
      if (!IsLoopHeader)
        continue;
      std::map<uint64_t, const PhiNode *> PhiByRegOff;
      for (const auto &Phi : MB.Phis) {
        if (Phi.Output.Kind == MedVar::Reg && Phi.Output.Size > 0) {
          PhiByRegOff[Phi.Output.RegOff] = &Phi;
        }
      }
      if (PhiByRegOff.empty())
        continue;

      struct PostSSASub {
        size_t InsertBefore;
        MedOp Op;
        size_t OpIdx;
        uint8_t InpIdx;
        int NewVer;
      };
      std::vector<PostSSASub> Fixes;
      std::map<std::pair<int, uint64_t>, int> AlreadyFixed;

      for (size_t OI = 0; OI < MB.Ops.size(); ++OI) {
        auto &MOp = MB.Ops[OI];
        for (uint8_t I = 0; I < MOp.NumInputs; ++I) {
          auto &Inp = MOp.Inputs[I];
          if (Inp.Kind != MedVar::Reg || Inp.Size == 0 || Inp.SSAVer != 0)
            continue;
          // Direct RegOff match: if a PHI at the same RegOff has a wider
          // size, the current register is a sub-register of the PHI.
          {
            auto PhiIt = PhiByRegOff.find(Inp.RegOff);
            if (PhiIt == PhiByRegOff.end() ||
                PhiIt->second->Output.Size <= Inp.Size)
              continue;
            auto FixKey = std::make_pair(Inp.Id, Inp.RegOff);
            auto FIt = AlreadyFixed.find(FixKey);
            int NewVarVer;
            if (FIt != AlreadyFixed.end()) {
              NewVarVer = FIt->second;
            } else {
              NewVarVer = NextVer++;
              AlreadyFixed[FixKey] = NewVarVer;
              MedOp Sub;
              Sub.Opcode = NdOp::SUBBYTES;
              Sub.Addr = MOp.Addr;
              Sub.Output = Inp;
              Sub.Output.SSAVer = NewVarVer;
              const auto &PhiOut = PhiIt->second->Output;
              MedVar Wide;
              Wide.Kind = MedVar::Reg;
              Wide.Id = PhiOut.Id;
              Wide.Size = PhiOut.Size;
              Wide.RegOff = PhiOut.RegOff;
              Wide.SSAVer = PhiOut.SSAVer;
              Wide.TheArch = TargetArch;
              Sub.addInput(Wide);
              Sub.addInput(MedVar::makeConst(0, 4));
              Fixes.push_back({OI, std::move(Sub), OI, I, NewVarVer});
            }
            Inp.SSAVer = NewVarVer;
          }
        }
      }
      for (auto It = Fixes.rbegin(); It != Fixes.rend(); ++It)
        MB.Ops.insert(MB.Ops.begin() + static_cast<long>(It->InsertBefore),
                      std::move(It->Op));
    }
  }

  // Complementary direction: the block above fixes a narrow read that should
  // come from a wider loop PHI; this fixes a WIDE read that must merge a
  // narrower loop-carried sub-register PHI (e.g. byte-popcount `movl %edi` over
  // `shrb %dil`).  Runs post-SSA so the narrow phi is visible.
  mergeLoopCarriedPartialReads(Func);
  debugVerifyMedFunc(Func, "mergeLoopCarriedPartialReads");

  // ARM/AArch64 analogue: a wide vector (Q) read at a loop header that resolves
  // to the loop-invariant preamble value because only its 64-bit halves (D
  // sub-registers) are loop-carried via phis.  Reconstruct from the half phis.
  mergeLoopCarriedVectorReads(Func);
  debugVerifyMedFunc(Func, "mergeLoopCarriedVectorReads");

  detectCc(Func, TheArch, Fmt);
  debugVerifyMedFunc(Func, "detectCc");

  propagate(Func);
  debugVerifyMedFunc(Func, "propagate");

  eliminateFlags(Func);
  debugVerifyMedFunc(Func, "eliminateFlags");

  LLVM_DEBUG(llvm::dbgs() << "LowIR -> MedIR: " << Func.Blocks.size()
                          << " blocks, " << Func.Params.size() << " params, "
                          << Func.Locals.size() << " locals\n");
  return Func;
}

//===----------------------------------------------------------------------===//
// NdVar -> MedVar conversion
//===----------------------------------------------------------------------===//

MedVar LowToMedConverter::ndVarToMedVar(const NdVar &VN) {
  MedVar MV;
  MV.Size = VN.Size;

  switch (VN.Space) {
  case VnodeSpace::REG: {
    auto Key = std::make_pair(VN.Offset, VN.Size);
    auto It = RegVarMap.find(Key);
    if (It != RegVarMap.end()) {
      MV.Kind = MedVar::Reg;
      MV.Id = It->second;
    } else {
      MV.Kind = MedVar::Reg;
      MV.Id = allocVarId();
      RegVarMap[Key] = MV.Id;
    }
    MV.RegOff = VN.Offset;
    MV.TheArch = TargetArch;

    {
      const auto &TRI = getTargetRegInfo(TargetArch);
      if (TRI.isFlag(VN.Offset, VN.Size))
        MV.Kind = MedVar::Flag;
    }
    break;
  }
  case VnodeSpace::TEMP: {
    auto It = TempVarMap.find(VN.Offset);
    if (It != TempVarMap.end()) {
      MV.Kind = MedVar::Temp;
      MV.Id = It->second;
    } else {
      MV.Kind = MedVar::Temp;
      MV.Id = allocVarId();
      TempVarMap[VN.Offset] = MV.Id;
    }
    break;
  }
  case VnodeSpace::CONST: {
    MV = MedVar::makeConst(VN.Offset, VN.Size);
    break;
  }
  case VnodeSpace::STACK: {
    MV.Kind = MedVar::Stack;
    for (const auto &Slot : StackSlots) {
      if (Slot.Offset == static_cast<int64_t>(VN.Offset) &&
          Slot.Size == VN.Size) {
        MV.Id = Slot.VarId;
        break;
      }
    }
    MV.StackOff = static_cast<int64_t>(VN.Offset);
    break;
  }
  default:
    MV.Kind = MedVar::Temp;
    MV.Id = allocVarId();
    break;
  }

  return MV;
}

} // namespace neverd
