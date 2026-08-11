//===- LowToMed.cpp - LowIR to MedIR conversion --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// LowIR to MedIR (SSA) conversion: main entry point, stack analysis,
/// and nd-var-to-MedVar mapping.  Call return-value ABI modeling lives in
/// LowToMedCallReturn.cpp; the individual passes (SSA construction, DCE,
/// propagation, CFG simplification, flag elimination, calling convention
/// detection) live in their own translation units.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/LowToMed.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <tuple>

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

    for (const auto &LOp : LB.Ops) {
      MedOp MOp;
      MOp.Opcode = LOp.Opcode;
      MOp.Addr = LOp.Addr;
      if (MOp.Opcode == NdOp::CALL || MOp.Opcode == NdOp::INDIR_CALL)
        MOp.CallSiteId = NextCallSiteId++;

      if (LOp.Output.Size > 0)
        MOp.Output = varnodeToMedvar(LOp.Output);

      for (uint8_t I = 0; I < LOp.NumInputs; ++I)
        MOp.addInput(varnodeToMedvar(LOp.Inputs[I]));

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
          // (varnodeToMedvar keys registers by (RegOff,Size) in RegVarMap).  A
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
  verifyMedFunc(Func, "modelKnownWideCallReturns");

  fixupSubRegisters(Func);
  verifyMedFunc(Func, "fixupSubRegisters");

  simplifyCfg(Func);
  verifyMedFunc(Func, "simplifyCfg");

  // Apple clang's prologue stack-probe (____chkstk_darwin) is modeled as an
  // ordinary call returning in x0; clear that spurious output before SSA so its
  // liveness does not kill the live-in argument registers (the probe preserves
  // every register except x16/x17).  No-op unless the pipeline provided the
  // chkstk slot set (Mach-O only).
  neutralizeStackProbeCalls(Func);
  verifyMedFunc(Func, "neutralizeStackProbeCalls");

  buildSsa(Func);
  verifyMedFunc(Func, "buildSsa");

  // Model a call's floating-point/vector return (x86-64 returns it in XMM0, a
  // caller-saved vector register the lifter did not model the call as
  // defining). Done before copy propagation so a post-call read of the result
  // register is not folded back to the pre-call argument value. Model a direct
  // call's small struct-by-value return across multiple registers (x86-64
  // eightbytes / AArch64 HFA) before modelCallFPReturn so it claims the FP
  // return register of a struct-returning call as one of the aggregate fields
  // rather than the lone scalar FP result.
  modelCallStructReturn(Func);
  verifyMedFunc(Func, "modelCallStructReturn");

  modelCallFPReturn(Func);
  verifyMedFunc(Func, "modelCallFPReturn");

  // Model a call's x87 floating-point return on i386 (the cdecl convention
  // leaves it on the x87 top-of-stack, st0): reconnect the post-call `fstp`
  // read of st0 to the call's result, which the lifter did not model.
  modelCallX87Return(Func);
  verifyMedFunc(Func, "modelCallX87Return");

  // Model a call's 64-bit integer return on 32-bit targets (i386 EDX:EAX, ARM32
  // R1:R0): the lifter did not model the call as defining the high-half
  // register, so reconnect post-call reads of it to the call's high result.
  modelCallWideIntReturn(Func, TargetArch);
  verifyMedFunc(Func, "modelCallWideIntReturn");

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
  verifyMedFunc(Func, "mergeLoopCarriedPartialReads");

  // ARM/AArch64 analogue: a wide vector (Q) read at a loop header that resolves
  // to the loop-invariant preamble value because only its 64-bit halves (D
  // sub-registers) are loop-carried via phis.  Reconstruct from the half phis.
  mergeLoopCarriedVectorReads(Func);
  verifyMedFunc(Func, "mergeLoopCarriedVectorReads");

  detectCc(Func, TheArch, Fmt);
  verifyMedFunc(Func, "detectCc");

  propagate(Func);
  verifyMedFunc(Func, "propagate");

  eliminateFlags(Func);
  verifyMedFunc(Func, "eliminateFlags");

  LLVM_DEBUG(llvm::dbgs() << "LowIR -> MedIR: " << Func.Blocks.size()
                          << " blocks, " << Func.Params.size() << " params, "
                          << Func.Locals.size() << " locals\n");
  return Func;
}

//===----------------------------------------------------------------------===//
// Stack analysis
//===----------------------------------------------------------------------===//

void LowToMedConverter::analyzeStack(const LowFunc &Low) {
  std::set<std::pair<int64_t, uint16_t>> SeenSlots;

  const auto &TRI = getTargetRegInfo(TargetArch);
  auto IsFrameReg = [&TRI](const NdVar &VN) -> bool {
    if (!VN.isReg())
      return false;
    return TRI.isFrameReg(VN.Offset);
  };

  auto AddSlot = [&](int64_t Disp, uint16_t Sz) {
    if (SeenSlots.insert({Disp, Sz}).second) {
      StackSlot Slot;
      Slot.Offset = Disp;
      Slot.Size = Sz;
      Slot.VarId = allocVarId();
      StackSlots.push_back(Slot);
    }
  };

  // Pass 0: collect the instruction address and operand pairs of flag-setting
  // add/sub (adds/subs/cmp/cmn).  The lifter models these as an
  // INT_ADD/INT_SUB for the value plus carry/overflow flag ops
  // (INT_CARRY/INT_SOVF/INT_SBOR).  Varnode storage is reused between machine
  // instructions, so both the address and pair must match.  A matching op is a
  // comparison whose result is never a frame address, so it must not seed a
  // stack slot — otherwise a register that once held sp and was reused for
  // data (flow-insensitive AddrMap never
  // invalidates) makes `cmp rN,#imm` (subs tmp, rN, #imm) look like a frame
  // access at offset -imm and inflates FrameSize past the real stack (#387:
  // ipcksum's `while(sum>>16)`
  // -> subs tmp, sum, #0x10000 -> a bogus 0x10090-byte frame ->
  // WRITE_UNMAPPED).
  auto VnKey = [](const NdVar &VN) {
    return std::make_pair(VN.Space, VN.Offset);
  };
  using VnStorage = std::pair<VnodeSpace, uint64_t>;
  std::set<std::tuple<va_t, VnStorage, VnStorage>> FlagArithPairs;
  for (const auto &Blk : Low.Blocks)
    for (const auto &Op : Blk.Ops)
      if ((Op.Opcode == NdOp::INT_CARRY || Op.Opcode == NdOp::INT_SOVF ||
           Op.Opcode == NdOp::INT_SBOR) &&
          Op.NumInputs >= 2) {
        auto A = VnKey(Op.Inputs[0]);
        auto B = VnKey(Op.Inputs[1]);
        FlagArithPairs.insert({Op.Addr, A, B});
        FlagArithPairs.insert({Op.Addr, B, A});
      }

  // Pass 1: track which temp/register varnodes are frame_reg +/- const.  A
  // definition kills an old association unless its opcode is one of the
  // address-preserving forms handled below.  Varnode storage is routinely
  // reused for unrelated data, so retaining an association across an
  // arbitrary redef is unsound even when the resulting phantom displacement
  // happens to be below kMaxFrameSize.
  std::map<std::pair<VnodeSpace, uint64_t>, int64_t> AddrMap;
  std::set<std::pair<VnodeSpace, uint64_t>> FrameDefsInBlock;
  auto IsTrackableOutput = [](const LowOp &Op) {
    return Op.Output.Size > 0 && (Op.Output.isTemp() || Op.Output.isReg());
  };
  auto ClearOutput = [&](const LowOp &Op) {
    if (IsTrackableOutput(Op)) {
      AddrMap.erase(VnKey(Op.Output));
      FrameDefsInBlock.erase(VnKey(Op.Output));
    }
  };
  auto FrameOffset = [&](const NdVar &VN, int64_t &Offset) {
    auto It = AddrMap.find(VnKey(VN));
    if (It != AddrMap.end()) {
      Offset = It->second;
      return true;
    }
    if (IsFrameReg(VN)) {
      Offset = 0;
      return true;
    }
    return false;
  };

  for (const auto &Blk : Low.Blocks) {
    FrameDefsInBlock.clear();
    for (const auto &Op : Blk.Ops) {
      // Refine a frame-derived address at its actual use site.  Address
      // varnodes are scratch storage and may be redefined later in the same
      // function, so a post-pass lookup in the final AddrMap state loses the
      // data width (or, worse, attributes it to the wrong displacement).
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Op.NumInputs >= 1) {
        int64_t Offset = 0;
        if (FrameOffset(Op.Inputs[0], Offset)) {
          uint16_t DataSz = 0;
          if (Op.Opcode == NdOp::LOAD)
            DataSz = Op.Output.Size;
          else if (Op.NumInputs >= 2)
            DataSz = Op.Inputs[1].Size;
          if (DataSz > 0)
            AddSlot(Offset, DataSz);
        }
      }

      if (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) {
        // A flag-setting compare (matching carry/overflow flag ops on the same
        // operands) is a comparison value, never a stack address, so it must
        // not seed a slot.  Clearing its output is essential when the
        // comparison reuses a varnode that previously held a frame-derived
        // value.
        if (Op.NumInputs >= 2 &&
            FlagArithPairs.count(
                {Op.Addr, VnKey(Op.Inputs[0]), VnKey(Op.Inputs[1])})) {
          ClearOutput(Op);
          continue;
        }

        bool Propagated = false;
        for (uint8_t I = 0; I < Op.NumInputs; ++I) {
          // `constant - frame` is not an address derived from the frame.
          if (Op.Opcode == NdOp::INT_SUB && I != 0)
            continue;

          uint8_t Other = 1 - I;
          if (Other >= Op.NumInputs || !Op.Inputs[Other].isConst())
            continue;

          int64_t Base = 0;
          if (!FrameOffset(Op.Inputs[I], Base))
            continue;

          int64_t Disp = static_cast<int64_t>(Op.Inputs[Other].Offset);
          if (Op.Opcode == NdOp::INT_SUB && I == 0) {
            if (Disp == std::numeric_limits<int64_t>::min())
              continue;
            Disp = -Disp;
          }

          if ((Disp > 0 && Base > std::numeric_limits<int64_t>::max() - Disp) ||
              (Disp < 0 && Base < std::numeric_limits<int64_t>::min() - Disp))
            continue;
          int64_t Total = Base + Disp;

          // A genuine frame-slot offset is bounded by the maximum frame size.
          // A |Total| beyond it means the "frame" input was a stale
          // (space,offset)-keyed association on a register that once held sp
          // but is now reused for data -- e.g. a TEA/hash kernel's `sum +=
          // 0x9E3779B9` on such a register looks like frame arithmetic at a
          // ~1.5 GB offset.  Don't seed that bogus slot (computeFrameSize would
          // only drop it later with a warning), and erase the stale association
          // so the data value doesn't cascade into further phantom frame
          // arithmetic.  The bound is far above any real frame (even large
          // vectorized kernels stay in the KB range), so legitimate frame
          // associations are untouched (#387a residual: the COPY-only clear
          // missed non-COPY data redefinitions of a reused EA-scratch temp).
          if (Total > limits::kMaxFrameSize || Total < -limits::kMaxFrameSize)
            continue;

          // Snapshotting Base above must precede this clear: address updates
          // are commonly in-place (`sp = sp - imm`), so output and frame input
          // may be the same varnode.
          ClearOutput(Op);
          if (IsTrackableOutput(Op)) {
            AddrMap[VnKey(Op.Output)] = Total;
            FrameDefsInBlock.insert(VnKey(Op.Output));
          }

          AddSlot(Total, Op.Output.Size > 0 ? Op.Output.Size : 8);
          Propagated = true;
          break;
        }

        // A frame address may carry a runtime index before a later constant
        // displacement, as in x86-64 red-zone accesses `rsp + index - size`.
        // The dynamic term cannot define a concrete slot yet, but the result
        // is still frame-derived.  Preserve its known base so the following
        // constant adjustment can establish a conservative stack bound.
        if (!Propagated && Op.NumInputs >= 2) {
          for (uint8_t I = 0; I < Op.NumInputs; ++I) {
            if (Op.Opcode == NdOp::INT_SUB && I != 0)
              continue;
            uint8_t Other = 1 - I;
            if (Other >= Op.NumInputs || Op.Inputs[Other].isConst())
              continue;
            if (!IsFrameReg(Op.Inputs[I]) &&
                !FrameDefsInBlock.count(VnKey(Op.Inputs[I])))
              continue;
            int64_t Base = 0;
            if (!FrameOffset(Op.Inputs[I], Base))
              continue;
            ClearOutput(Op);
            if (IsTrackableOutput(Op)) {
              AddrMap[VnKey(Op.Output)] = Base;
              FrameDefsInBlock.insert(VnKey(Op.Output));
            }
            Propagated = true;
            break;
          }
        }
        if (!Propagated)
          ClearOutput(Op);
      }
      // These operations retain the numeric address while changing storage or
      // width.  SUBBYTES only preserves an address when selecting byte zero.
      else if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
                Op.Opcode == NdOp::INT_SEXT || Op.Opcode == NdOp::SUBBYTES) &&
               Op.NumInputs >= 1) {
        bool ZeroSubpiece = Op.Opcode != NdOp::SUBBYTES ||
                            (Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
                             Op.Inputs[1].Offset == 0);
        int64_t Offset = 0;
        bool Propagated = ZeroSubpiece && IsTrackableOutput(Op) &&
                          FrameOffset(Op.Inputs[0], Offset);
        ClearOutput(Op);
        if (Propagated) {
          AddrMap[VnKey(Op.Output)] = Offset;
          FrameDefsInBlock.insert(VnKey(Op.Output));
        }
      } else
        ClearOutput(Op);
    }
  }

  std::sort(StackSlots.begin(), StackSlots.end(),
            [](const auto &A, const auto &B) { return A.Offset < B.Offset; });
}

//===----------------------------------------------------------------------===//
// NdVar -> MedVar conversion
//===----------------------------------------------------------------------===//

MedVar LowToMedConverter::varnodeToMedvar(const NdVar &VN) {
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

//===----------------------------------------------------------------------===//
// Sub-register fixup pass (table-driven)
//===----------------------------------------------------------------------===//

void LowToMedConverter::fixupSubRegisters(MedFunc &Func) {
  const auto &TRI = getTargetRegInfo(TargetArch);

  // Phase A: Implicit zero-extension insertion.
  // On x86-64 and AArch64, writing a 32-bit register implicitly zero-extends
  // to 64 bits.  Insert explicit INT_ZEXT so later passes see the dependency.
  for (auto &MB : Func.Blocks) {
    for (size_t OI = 0; OI < MB.Ops.size(); ++OI) {
      auto &MOp = MB.Ops[OI];
      if (MOp.Output.Kind != MedVar::Reg || MOp.Output.Size == 0)
        continue;
      if (MOp.Opcode == NdOp::SUBBYTES) {
        // A SUBBYTES re-viewing the low bytes of the SAME wide register it
        // writes (EAX := low32(RAX), offset 0) is a no-op extract; zero-
        // extending it would wrongly clobber that register's genuine upper
        // bits.  But a SUBBYTES that narrows an UNRELATED value (a temp, or a
        // different register) into a 32-bit GP register is a real zero-
        // extending write — e.g. the low word of a 64-bit MUL product stored
        // to EAX must clear RAX[63:32].  Only skip the self low-slice form.
        bool SelfLowSlice = MOp.NumInputs >= 1 &&
                            MOp.Inputs[0].Kind == MedVar::Reg &&
                            MOp.Inputs[0].RegOff == MOp.Output.RegOff &&
                            MOp.Inputs[0].Size >= MOp.Output.Size &&
                            (MOp.NumInputs < 2 || !MOp.Inputs[1].isConst() ||
                             MOp.Inputs[1].ConstVal == 0);
        if (SelfLowSlice)
          continue;
      }
      if (!TRI.writeZeroExtends(MOp.Output.RegOff, MOp.Output.Size))
        continue;

      auto [WideOff, WideSz] =
          TRI.findWideReg(MOp.Output.RegOff, MOp.Output.Size);
      if (WideSz <= MOp.Output.Size)
        continue;

      bool AlreadyZexted = false;
      if (OI + 1 < MB.Ops.size()) {
        auto &Next = MB.Ops[OI + 1];
        if (Next.Opcode == NdOp::INT_ZEXT && Next.NumInputs >= 1 &&
            Next.Inputs[0].Kind == MedVar::Reg &&
            Next.Inputs[0].RegOff == MOp.Output.RegOff &&
            Next.Inputs[0].Size == MOp.Output.Size &&
            Next.Output.Size == WideSz)
          AlreadyZexted = true;
      }
      if (AlreadyZexted)
        continue;

      auto WideKey = std::make_pair(WideOff, WideSz);
      int WideId;
      auto WideIt = RegVarMap.find(WideKey);
      if (WideIt != RegVarMap.end())
        WideId = WideIt->second;
      else {
        WideId = allocVarId();
        RegVarMap[WideKey] = WideId;
      }

      MedOp Zext;
      Zext.Opcode = NdOp::INT_ZEXT;
      Zext.Addr = MOp.Addr;
      Zext.Output.Kind = MedVar::Reg;
      Zext.Output.RegOff = WideOff;
      Zext.Output.Size = WideSz;
      Zext.Output.Id = WideId;
      Zext.Output.TheArch = TargetArch;
      Zext.addInput(MOp.Output);
      MB.Ops.insert(MB.Ops.begin() + static_cast<long>(OI + 1), Zext);
      ++OI;
    }
  }

  // Phase B: Register-write → narrower-read SUBBYTES insertion.
  // Two-pass approach: first scan to find insertion points, then apply.
  // This avoids iterator invalidation from modifying the ops vector.

  for (auto &MB : Func.Blocks) {
    RegWriteMap AllWrites;
    RegWriteMap ZextWrites;
    size_t Seq = 0;

    // Seed AllWrites with PHI outputs so sub-register reads in the loop
    // body can resolve to the loop-carried phi variable (e.g. SIL → RSI phi).
    for (const auto &Phi : MB.Phis) {
      if (Phi.Output.Size > 0 && Phi.Output.RegOff != 0) {
        auto Key = std::make_pair(Phi.Output.RegOff, Phi.Output.Size);
        AllWrites[Key] = {Phi.Output.Id, Phi.Output.Size, Phi.Output.RegOff,
                          Seq++};
      }
    }

    struct PendingSubpiece {
      size_t InsertBefore;
      MedOp Op;
    };
    std::vector<PendingSubpiece> Pending;

    for (size_t OI = 0; OI < MB.Ops.size(); ++OI) {
      const auto &MOp = MB.Ops[OI];

      for (uint8_t I = 0; I < MOp.NumInputs; ++I) {
        const auto &Inp = MOp.Inputs[I];
        if (Inp.Kind != MedVar::Reg || Inp.Size == 0)
          continue;

        auto tryRecord = [&](uint64_t WideOff, uint16_t WideSz) -> bool {
          if (WideSz <= Inp.Size)
            return false;
          auto WKey = std::make_pair(WideOff, WideSz);
          const RegWriteInfo *Best = nullptr;
          auto WIt = AllWrites.find(WKey);
          if (WIt != AllWrites.end())
            Best = &WIt->second;
          auto ZIt = ZextWrites.find(WKey);
          if (ZIt != ZextWrites.end() && (!Best || ZIt->second.Ord > Best->Ord))
            Best = &ZIt->second;
          if (!Best)
            return false;
          const auto &WI = *Best;
          if (WI.Id == Inp.Id)
            return false;

          int ByteOff =
              TRI.subRegByteOffset(Inp.RegOff, Inp.Size, WideOff, WideSz);
          if (ByteOff < 0 && Inp.RegOff == WideOff)
            ByteOff = 0;
          if (ByteOff < 0)
            return false;

          auto SKey = std::make_pair(Inp.RegOff, Inp.Size);
          auto SIt = AllWrites.find(SKey);
          if (SIt != AllWrites.end() && SIt->second.Ord > WI.Ord)
            return false;
          auto SZIt = ZextWrites.find(SKey);
          if (SZIt != ZextWrites.end() && SZIt->second.Ord > WI.Ord)
            return false;

          MedOp Sub;
          Sub.Opcode = NdOp::SUBBYTES;
          Sub.Addr = MOp.Addr;
          Sub.Output = Inp;
          MedVar Wide;
          Wide.Kind = MedVar::Reg;
          Wide.Id = WI.Id;
          Wide.Size = WideSz;
          Wide.RegOff = WideOff;
          Wide.TheArch = TargetArch;
          Sub.addInput(Wide);
          Sub.addInput(MedVar::makeConst(static_cast<uint64_t>(ByteOff), 4));
          Pending.push_back({OI, std::move(Sub)});
          return true;
        };

        bool Found = false;
        {
          // Collect EVERY wider register that contains this narrow input and
          // was written, then reconstruct from the MOST RECENTLY written one.
          // Two kinds of containment must be considered together:
          //   (a) same start offset, larger size      (EAX within RAX)
          //   (b) the SubRegs table                    (an ARM S lane lives in
          //       both its D parent AND its Q parent)
          // Picking the newest writer is essential and is why (a) and (b) are
          // unified: a mid-register lane such as ARM S(4N+2) has the SAME start
          // offset as the high D half D(2N+1).  If we only matched start offset
          // (case a) we would capture a stale `vld1` that wrote that D half and
          // miss the later `vcvt` that wrote the whole Q — the high lanes of a
          // per-lane int->float reduction would then read a rodata constant
          // (VectorAlgo8 arm32 fmla/fdiv).
          uint64_t BestOff = 0;
          uint16_t BestSz = 0;
          size_t BestOrd = 0;
          bool HaveCandidate = false;
          auto consider = [&](uint64_t WOff, uint16_t WSz) {
            if (WSz <= Inp.Size)
              return;
            int BO = TRI.subRegByteOffset(Inp.RegOff, Inp.Size, WOff, WSz);
            if (BO < 0 && Inp.RegOff == WOff)
              BO = 0;
            if (BO < 0)
              return; // wide register does not actually contain the narrow
                      // input
            auto WKey = std::make_pair(WOff, WSz);
            size_t Ord = 0;
            bool Written = false;
            auto AIt = AllWrites.find(WKey);
            if (AIt != AllWrites.end()) {
              Ord = AIt->second.Ord;
              Written = true;
            }
            auto ZIt = ZextWrites.find(WKey);
            if (ZIt != ZextWrites.end() && ZIt->second.Ord > Ord) {
              Ord = ZIt->second.Ord;
              Written = true;
            }
            if (Written && (!HaveCandidate || Ord > BestOrd)) {
              BestOff = WOff;
              BestSz = WSz;
              BestOrd = Ord;
              HaveCandidate = true;
            }
          };
          // (a) same start offset, wider size.
          for (const auto &[Key, WI] : AllWrites)
            if (Key.first == Inp.RegOff && Key.second > Inp.Size)
              consider(Key.first, Key.second);
          for (const auto &[Key, WI] : ZextWrites)
            if (Key.first == Inp.RegOff && Key.second > Inp.Size)
              consider(Key.first, Key.second);
          // (b) SubRegs table (mid-register lanes whose start offset differs).
          for (const auto &E : TRI.SubRegs)
            if (E.NarrowRegOff == Inp.RegOff && E.NarrowSize == Inp.Size)
              consider(E.WideRegOff, static_cast<uint16_t>(E.WideSize));
          if (HaveCandidate && tryRecord(BestOff, BestSz))
            Found = true;
        }

        if (!Found) {
          uint64_t BestOff = 0;
          uint16_t BestSz = 0;
          size_t BestOrd = 0;
          bool HaveCandidate = false;
          for (const auto &[Key, WI] : ZextWrites) {
            if (Key.first == Inp.RegOff && Key.second > Inp.Size) {
              if (!HaveCandidate || WI.Ord > BestOrd) {
                BestOff = Key.first;
                BestSz = Key.second;
                BestOrd = WI.Ord;
                HaveCandidate = true;
              }
            }
          }
          if (HaveCandidate) {
            auto WKey = std::make_pair(BestOff, BestSz);
            auto WIt = ZextWrites.find(WKey);
            if (WIt != ZextWrites.end()) {
              const auto &WI = WIt->second;
              auto SKey = std::make_pair(Inp.RegOff, Inp.Size);
              auto SIt = AllWrites.find(SKey);
              if (SIt == AllWrites.end() || SIt->second.Ord < WI.Ord) {
                int ByteOff =
                    TRI.subRegByteOffset(Inp.RegOff, Inp.Size, BestOff, BestSz);
                if (ByteOff < 0 && Inp.RegOff == BestOff)
                  ByteOff = 0;
                if (ByteOff >= 0) {
                  MedOp Sub;
                  Sub.Opcode = NdOp::SUBBYTES;
                  Sub.Addr = MOp.Addr;
                  Sub.Output = Inp;
                  MedVar Wide;
                  Wide.Kind = MedVar::Reg;
                  Wide.Id = WI.Id;
                  Wide.Size = BestSz;
                  Wide.RegOff = BestOff;
                  Wide.TheArch = TargetArch;
                  Sub.addInput(Wide);
                  Sub.addInput(
                      MedVar::makeConst(static_cast<uint64_t>(ByteOff), 4));
                  Pending.push_back({OI, std::move(Sub)});
                  Found = true;
                }
              }
            }
          }
        }

        // ARM: a SUBBYTES of a wide NEON register whose extracted bytes lie
        // entirely within a more recently written D/S sub-register may read
        // that narrower register directly (LowToMedARM.cpp).
        if (!Found && I == 0 &&
            redirectWideSubpieceToNarrowARM(MB, OI, AllWrites))
          Found = true;
      }

      // A later narrow read must not be projected from a wider register view
      // that this call clobbered.  This matters for AAPCS64 v8-v15: D9 is
      // preserved while Q9 as a whole is not.  Keep the exact preserved D
      // write, but forget the Q write before considering post-call inputs.
      if ((MOp.Opcode == NdOp::CALL || MOp.Opcode == NdOp::INDIR_CALL) &&
          !MOp.PreservesCallerSaved) {
        auto DiscardClobbered = [&](RegWriteMap &Writes) {
          for (auto It = Writes.begin(); It != Writes.end();) {
            uint64_t RegOff = It->first.first;
            uint16_t Size = It->first.second;
            if (!TRI.isFrameOrLinkReg(RegOff) &&
                !TRI.isCallPreserved(RegOff, Size))
              It = Writes.erase(It);
            else
              ++It;
          }
        };
        DiscardClobbered(AllWrites);
        DiscardClobbered(ZextWrites);
      }

      if (MOp.Output.Kind == MedVar::Reg && MOp.Output.Size > 0) {
        bool IsZextFromNarrow = MOp.Opcode == NdOp::INT_ZEXT &&
                                MOp.NumInputs >= 1 &&
                                MOp.Inputs[0].Kind == MedVar::Reg &&
                                MOp.Inputs[0].RegOff == MOp.Output.RegOff &&
                                MOp.Inputs[0].Size < MOp.Output.Size;
        auto Key = std::make_pair(MOp.Output.RegOff, MOp.Output.Size);
        if (!IsZextFromNarrow) {
          AllWrites[Key] = {MOp.Output.Id, MOp.Output.Size, MOp.Output.RegOff,
                            Seq++};
        } else {
          ZextWrites[Key] = {MOp.Output.Id, MOp.Output.Size, MOp.Output.RegOff,
                             Seq++};
        }
      }
    }

    // Apply pending insertions in reverse order to preserve indices.
    for (auto It = Pending.rbegin(); It != Pending.rend(); ++It)
      MB.Ops.insert(MB.Ops.begin() + static_cast<long>(It->InsertBefore),
                    std::move(It->Op));
  }

  // Phase B2 (x86/x64): merge a more-recent narrow partial write (AL/AH/AX)
  // into a subsequent wider read of the parent register.  See LowToMedX86.cpp.
  fixupPartialWritesX86(Func);

  // Phase B2x (x86/x64): the in-block merge above cannot reach a parent read in
  // another block (the value flows through a phi).  Define the wide parent
  // right after such a partial write so buildSsa carries the merged value
  // across the block boundary.  See LowToMedX86.cpp.
  mergePartialWritesCrossBlockX86(Func);

  // Phase B3 (ARM/AArch64): reconstruct a full-width NEON Q read from its two
  // more-recent 8-byte D halves within a block.  See LowToMedARM.cpp.
  mergeWideVectorReadsARM(Func);

  // Phase C: Wide GP register write → narrow sub-register SUBBYTES.
  // Only insert when: (1) a wide register is the LAST write at that offset
  // in a block, (2) no narrower write follows, and (3) the narrow sub-reg
  // is read in a DIFFERENT block. This targets the case where a loop body
  // mixes 64-bit and 32-bit operations on the same register (e.g. c_gcd).
  {
    std::set<std::pair<uint64_t, uint16_t>> ReadInOtherBlock;
    for (size_t BI = 0; BI < Func.Blocks.size(); ++BI) {
      auto &B = Func.Blocks[BI];
      std::set<int> LocalDefs;
      for (auto &Op : B.Ops) {
        for (uint8_t I2 = 0; I2 < Op.NumInputs; ++I2) {
          auto &Inp = Op.Inputs[I2];
          if (Inp.Kind == MedVar::Reg && Inp.Size > 0 &&
              !LocalDefs.count(Inp.Id))
            ReadInOtherBlock.insert({Inp.RegOff, Inp.Size});
        }
        if (Op.Output.Kind == MedVar::Reg && Op.Output.Size > 0)
          LocalDefs.insert(Op.Output.Id);
      }
    }

    for (auto &MB : Func.Blocks) {
      // Find the LAST write index for each (RegOff, Size) pair, excluding
      // SUBBYTES (a sub-register extract) and INT_ZEXT (an implicit
      // zero-extension synced by Phase A; its real value lives in the
      // narrower write it extends).
      std::map<std::pair<uint64_t, uint16_t>, size_t> LastWide;
      for (size_t OI = 0; OI < MB.Ops.size(); ++OI) {
        auto &MOp = MB.Ops[OI];
        if (MOp.Output.Kind == MedVar::Reg && MOp.Output.Size > 0 &&
            MOp.Opcode != NdOp::SUBBYTES && MOp.Opcode != NdOp::INT_ZEXT)
          LastWide[{MOp.Output.RegOff, MOp.Output.Size}] = OI;
      }

      struct NarrowInsert {
        size_t InsertAfter;
        MedOp Op;
      };
      std::vector<NarrowInsert> NPending;

      // For every narrow sub-register that is read in another block (this
      // includes a loop back-edge when the block is its own predecessor),
      // find the latest wide write at the SAME register offset that strictly
      // contains it, and materialize a SUBBYTES right after that write.  This
      // makes the SSA loop-carried value of the narrow register reflect the
      // wide write's low bits instead of a stale earlier narrow definition.
      //
      // x86 crc8 motivating case: the inner loop's final `cmov %eax`/`mov`
      // updates EAX (and implicitly RAX), but the loop-carried CRC lives in
      // AL.  Without this, AL's phi latch keeps the next-to-last iteration's
      // value, silently dropping one loop iteration.  Iterating over the read
      // narrow registers (rather than over `Size/2` of each wide write) lets
      // us cross more than one sub-register level (EAX→AL), and also still
      // covers the RAX→EAX case used by c_gcd (bug #157e / #154).
      for (const auto &NKey : ReadInOtherBlock) {
        uint64_t NarOff = NKey.first;
        uint16_t NarSz = NKey.second;
        auto NIt = RegVarMap.find(NKey);
        if (NIt == RegVarMap.end())
          continue;

        // The narrow register must be contained in the wide write.  Match
        // either (a) the same starting offset & wider (GP families AL/AX/EAX/
        // RAX, W/X), or (b) a non-zero byte offset recorded in the sub-register
        // table.  Case (b) is essential for NEON: the high D-half D(2N+1) of a
        // Q register lives at WideOff+8, so a wide vcvt/vmul write to Q in one
        // block must materialize *both* D halves for cross-block `vst1
        // {dN,dN+1}` reads (low half shared the same offset and worked; the
        // high half was silently left stale).  Mirrors Phase B's table use.
        size_t BestOI = MB.Ops.size();
        int BestByteOff = 0;
        for (const auto &[WKey, OI] : LastWide) {
          uint64_t WideOff = WKey.first;
          uint16_t WideSz = WKey.second;
          if (WideSz <= NarSz)
            continue;
          int ByteOff;
          if (WideOff == NarOff)
            ByteOff = 0;
          else {
            ByteOff = TRI.subRegByteOffset(NarOff, NarSz, WideOff, WideSz);
            if (ByteOff < 0)
              continue;
          }
          if (BestOI == MB.Ops.size() || OI > BestOI) {
            BestOI = OI;
            BestByteOff = ByteOff;
          }
        }
        if (BestOI == MB.Ops.size())
          continue;
        auto &MOp = MB.Ops[BestOI];
        if (MOp.Output.Id == NIt->second)
          continue;

        // Only skip if the narrow sub-register is written *after* the wide
        // write.  A narrow def that precedes the wide write does not represent
        // the current value at block end — the wide write's low bits do (e.g.
        // AArch64 `sbfx x8,x8,...` overwrites x8 with mid after earlier w8
        // computations, so cross-block w8 reads must see mid).  bug #157e
        bool NarrowDefinedAfter = false;
        for (size_t K = BestOI + 1; K < MB.Ops.size(); ++K) {
          auto &BOp = MB.Ops[K];
          if (BOp.Output.Kind == MedVar::Reg && BOp.Output.RegOff == NarOff &&
              BOp.Output.Size == NarSz) {
            NarrowDefinedAfter = true;
            break;
          }
        }
        if (NarrowDefinedAfter)
          continue;

        MedOp Sub;
        Sub.Opcode = NdOp::SUBBYTES;
        Sub.Addr = MOp.Addr;
        Sub.Output.Kind = MedVar::Reg;
        Sub.Output.RegOff = NarOff;
        Sub.Output.Size = NarSz;
        Sub.Output.Id = NIt->second;
        Sub.Output.TheArch = TargetArch;
        MedVar Wide;
        Wide.Kind = MedVar::Reg;
        Wide.Id = MOp.Output.Id;
        Wide.Size = MOp.Output.Size;
        Wide.RegOff = MOp.Output.RegOff;
        Wide.TheArch = TargetArch;
        Sub.addInput(Wide);
        Sub.addInput(MedVar::makeConst(static_cast<uint64_t>(BestByteOff), 4));
        NPending.push_back({BestOI + 1, std::move(Sub)});
      }

      std::sort(NPending.begin(), NPending.end(),
                [](const NarrowInsert &A, const NarrowInsert &B) {
                  return A.InsertAfter < B.InsertAfter;
                });
      for (auto It = NPending.rbegin(); It != NPending.rend(); ++It)
        MB.Ops.insert(MB.Ops.begin() + static_cast<long>(It->InsertAfter),
                      std::move(It->Op));
    }
  }

  // Phase C2 (ARM/AArch64): synthesize Q = CONCAT(D_high, D_low) after the last
  // D-half write so cross-block wide reads observe the D values.  Runs after
  // the generic Phase C above.  See LowToMedARM.cpp.
  synthesizeWideVectorWritesARM(Func);
}

} // namespace neverd
