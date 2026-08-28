//===- JumpTableResolverFold.cpp - Constant folding of table bases --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Cross-instruction constant folding for jump-table resolution: emulate the
/// straight-line prefix that dominates an indirect branch to recover the
/// concrete value of a register holding a table base, plus the call-preserved
/// register set that lets the emulator step over an intervening call.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch and JumpTableResolverDetail.h for shared
/// backward-slicing helpers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/NdOpEmulator.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <vector>

namespace neverd {

/// Registers that survive a call by ABI on \p Img's architecture — the stack
/// pointer, frame pointer, and callee-saved registers.  Handed to the emulator
/// so it can step over an intervening call (keeping these, dropping the
/// caller-saved rest) while folding a table base, instead of halting.
std::vector<uint64_t> callPreservedRegs(const BinaryImage &Img) {
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  std::vector<uint64_t> Regs;
  Regs.reserve(TRI.CalleeSaveRegs.size() + 2);
  Regs.push_back(TRI.StackPointer);
  Regs.push_back(TRI.FramePointer);
  for (uint64_t R : TRI.CalleeSaveRegs)
    Regs.push_back(R);
  return Regs;
}

/// Fold a register to a constant by emulating the linear prefix up to the
/// branch.  The emulator halts at the first control-flow op, so for a
/// loop-body branch this still executes the dominating block where a PIC
/// table base is materialised.  Address callers retain the default mapped-value
/// requirement; scalar-proof callers may request an unmapped result only as a
/// candidate for a separate point-sensitive proof.
std::optional<uint64_t>
CFGBuilder::foldRegConstant(const BinaryImage &Img, const InsnRecord &Rec,
                            uint64_t Reg, va_t CutoffAddr,
                            std::function<bool(size_t)> ConsumeWork,
                            bool RequireMappedValue,
                            bool AllowUnmappedCOFFImageBase) const {
  bool WorkComplete = true;
  auto consume = [&](size_t Amount = 1) {
    if (!WorkComplete)
      return false;
    if (!ConsumeWork || ConsumeWork(Amount))
      return true;
    WorkComplete = false;
    return false;
  };
  auto consumeProduct = [&](size_t Count, size_t Factor) {
    if (Factor != 0 &&
        Count > std::numeric_limits<size_t>::max() / Factor) {
      consume(std::numeric_limits<size_t>::max());
      WorkComplete = false;
      return false;
    }
    return consume(Count * Factor);
  };
  auto orderedLookupWork = [](size_t Count) {
    size_t Work = 1;
    for (size_t N = Count; N > 1; N = N / 2 + N % 2)
      ++Work;
    return Work;
  };
  auto consumeFactors = [&](std::initializer_list<size_t> Factors) {
    size_t Product = 1;
    for (size_t Factor : Factors) {
      if (Factor != 0 &&
          Product > std::numeric_limits<size_t>::max() / Factor) {
        consume(std::numeric_limits<size_t>::max());
        WorkComplete = false;
        return false;
      }
      Product *= Factor;
    }
    return consume(Product);
  };
  auto prepayEmulatorRun = [&](const std::vector<LowOp> &OpsToRun,
                               size_t PreservedCount, bool StepOverCalls) {
    // Find the structural upper bound of the linear path before inventorying
    // its dynamic state.  The vector emulator stops at the first ordinary
    // control op.  A legacy same-address ARM predicate guard is different: it
    // may skip the rest of that instruction and continue, so retain its whole
    // short address run and everything after it.  Two allocation-free source
    // passes cover address grouping and control classification.
    if (!consumeProduct(OpsToRun.size(), 2))
      return false;
    auto isInstructionControl = [](NdOp Opcode) {
      return Opcode == NdOp::BRANCH || Opcode == NdOp::INDIR_BR ||
             Opcode == NdOp::CALL || Opcode == NdOp::INDIR_CALL ||
             Opcode == NdOp::RETURN;
    };
    size_t ExecutableCount = OpsToRun.size();
    for (size_t Begin = 0; Begin < OpsToRun.size();) {
      size_t End = Begin + 1;
      while (End < OpsToRun.size() &&
             OpsToRun[End].Addr == OpsToRun[Begin].Addr)
        ++End;
      bool HasPredicatedGuard = false;
      bool SawCondBr = false;
      size_t FirstTerminatingControl = End;
      for (size_t I = Begin; I < End; ++I) {
        const LowOp &Op = OpsToRun[I];
        const NdOp Opcode = Op.Opcode;
        if (FirstTerminatingControl == End &&
            (Opcode == NdOp::BRANCH || Opcode == NdOp::COND_BR ||
             Opcode == NdOp::RETURN || Opcode == NdOp::INDIR_BR ||
             (!StepOverCalls &&
              (Opcode == NdOp::CALL || Opcode == NdOp::INDIR_CALL))))
          FirstTerminatingControl = I;
        if (SawCondBr && isInstructionControl(Opcode))
          HasPredicatedGuard = true;
        if (Opcode == NdOp::COND_BR && Op.Addr != 0)
          SawCondBr = true;
      }
      if (!HasPredicatedGuard && FirstTerminatingControl != End) {
        const NdOp Opcode = OpsToRun[FirstTerminatingControl].Opcode;
        // Even an ordinary COND_BR makes predicatedInstructionEnd inspect the
        // rest of its original same-address run twice before it decides that
        // no legacy instruction control follows.  Those ops are not
        // executable, so retain only their scan work here.
        if (Opcode == NdOp::COND_BR &&
            !consumeProduct(End - FirstTerminatingControl - 1, 2))
          return false;
        ExecutableCount = FirstTerminatingControl + 1;
        break;
      }
      Begin = End;
    }
    // Pay the grouped opcode inventory and the emulator's actual linear walk
    // before either observes or mutates emulator state.
    if (!consumeProduct(ExecutableCount, 3))
      return false;
    size_t RegisterAccesses = 0;
    size_t RegisterMutations = 0;
    size_t MemoryReads = 0;
    size_t MemoryWrites = 0;
    size_t Calls = 0;
    size_t PredicatedBranchWork = 0;
    auto addCount = [&](size_t &Total, size_t Amount = 1) {
      if (Amount > std::numeric_limits<size_t>::max() - Total) {
        consume(std::numeric_limits<size_t>::max());
        WorkComplete = false;
        return false;
      }
      Total += Amount;
      return true;
    };
    for (size_t Begin = 0; Begin < ExecutableCount;) {
      size_t End = Begin + 1;
      while (End < ExecutableCount &&
             OpsToRun[End].Addr == OpsToRun[Begin].Addr)
        ++End;
      for (size_t I = Begin; I < End; ++I) {
        const LowOp &Op = OpsToRun[I];
        if (!addCount(RegisterAccesses, Op.NumInputs))
          return false;
        if ((Op.Output.isReg() || Op.Output.isTemp()) &&
            !addCount(RegisterMutations))
          return false;
        switch (Op.Opcode) {
        case NdOp::CALL:
        case NdOp::INDIR_CALL:
          if (!addCount(Calls))
            return false;
          break;
        case NdOp::COND_BR: {
          // NdOpEmulator's legacy vector API scans only the suffix of this
          // machine instruction, then scans that same short suffix for a
          // control op.  Charge that exact interval instead of pessimistically
          // multiplying every block-level branch by the whole function.
          const size_t Suffix = Op.Addr == 0 ? 0 : End - I - 1;
          if (!addCount(PredicatedBranchWork, Suffix) ||
              !addCount(PredicatedBranchWork, Suffix) ||
              !addCount(PredicatedBranchWork))
            return false;
          break;
        }
        case NdOp::LOAD:
          if (!addCount(MemoryReads))
            return false;
          break;
        case NdOp::STORE:
          if (!addCount(MemoryWrites))
            return false;
          break;
        case NdOp::ATOMIC_ADD:
        case NdOp::ATOMIC_CMPXCHG:
          if (!addCount(MemoryReads) || !addCount(MemoryWrites))
            return false;
          break;
        default:
          break;
        }
      }
      Begin = End;
    }

    const size_t RegisterUpper = RegisterMutations;
    const size_t StoreUpper = std::min(
        MemoryWrites, size_t(limits::kMaxEmulatorStoreEntries));
    const size_t RegisterLookup = orderedLookupWork(RegisterUpper);
    const size_t StoreLookup = orderedLookupWork(StoreUpper);
    const size_t PreservedLookup = orderedLookupWork(PreservedCount);

    // NdOpEmulator owns two ordered maps and two vectors.  Before constructing
    // it, reserve a conservative execution envelope from an allocation-free
    // opcode inventory.  Every actual operand and output pays its ordered-map
    // query; memory ops pay overlay lookup, node lifetime, and image-owner
    // scanning; each call that can be stepped over pays a full register-state
    // walk plus ABI-preserved lookup.  All work debits the same caller account.
    return consume(8) &&
           consumeFactors({RegisterAccesses, RegisterLookup}) &&
           consumeFactors({RegisterMutations, RegisterLookup}) &&
           consumeProduct(RegisterMutations, 3) &&
           consumeFactors({MemoryReads, StoreLookup}) &&
           consumeFactors({MemoryWrites, StoreLookup}) &&
           consumeProduct(MemoryWrites, 3) &&
           consumeFactors({MemoryReads, Img.Segments.size()}) &&
           consume(PredicatedBranchWork) &&
           (!StepOverCalls ||
            (consumeFactors({Calls, RegisterUpper, PreservedLookup}) &&
             consumeFactors({Calls, RegisterUpper, 2})));
  };

  // Emulate up to (exclusive) the cutoff instruction.  The default cutoff is
  // the branch itself, but a table-base register may be reused (clobbered)
  // between the table load and the indirect branch — e.g. x86-64 `lea
  // tab(%rip),%r11; movslq (%r11,%idx,4),%r10; add %r11,%r10; mov %edx,%r11d;
  // jmp *%r10` — so callers fold it at the table load, before the clobber.
  va_t Cutoff = (CutoffAddr != InvalidVA) ? CutoffAddr : Rec.Addr;
  auto emulateFrom = [&](va_t Start) -> std::optional<uint64_t> {
    if (ConsumeWork && !consume(2))
      return std::nullopt;
    std::vector<LowOp> Prefix;
    if (ConsumeWork) {
      // Inventory the prefix without allocating, then prepay its exact vector
      // buffer, retained elements, future destruction, and the second source
      // traversal before reserve/push_back can mutate local state.
      if (!consume(orderedLookupWork(Insns.size())))
        return std::nullopt;
      size_t RecordCount = 0;
      size_t OpCount = 0;
      for (auto It = Insns.lower_bound(Start);
           It != Insns.end() && It->first < Cutoff; ++It) {
        if (!consume() || !consume(It->second.Ops.size()))
          return std::nullopt;
        if (RecordCount == std::numeric_limits<size_t>::max() ||
            It->second.Ops.size() >
                std::numeric_limits<size_t>::max() - OpCount) {
          consume(std::numeric_limits<size_t>::max());
          WorkComplete = false;
          return std::nullopt;
        }
        ++RecordCount;
        OpCount += It->second.Ops.size();
      }
      if (!consume(orderedLookupWork(Insns.size())) ||
          !consume(RecordCount) || !consumeProduct(OpCount, 4))
        return std::nullopt;
      Prefix.reserve(OpCount);
      for (auto It = Insns.lower_bound(Start);
           It != Insns.end() && It->first < Cutoff; ++It)
        Prefix.insert(Prefix.end(), It->second.Ops.begin(),
                      It->second.Ops.end());
    } else {
      for (auto It = Insns.lower_bound(Start);
           It != Insns.end() && It->first < Cutoff; ++It)
        Prefix.insert(Prefix.end(), It->second.Ops.begin(),
                      It->second.Ops.end());
    }
    const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
    if (TRI.CalleeSaveRegs.size() >
        std::numeric_limits<size_t>::max() - size_t{2}) {
      consume(std::numeric_limits<size_t>::max());
      WorkComplete = false;
      return std::nullopt;
    }
    const size_t PreservedCount = TRI.CalleeSaveRegs.size() + 2;
    if (!prepayEmulatorRun(Prefix, PreservedCount, true))
      return std::nullopt;
    NdOpEmulator Emu(Img);
    // callPreservedRegs returns an owned vector.  Include its fixed object,
    // buffer, retained values, source traversal, and future destruction before
    // construction; setCallPreservedRegisters then takes that storage by move.
    if (!consumeProduct(PreservedCount, 4) || !consume(2))
      return std::nullopt;
    Emu.setCallPreservedRegisters(callPreservedRegs(Img));
    size_t Executed = Emu.run(Prefix);
    int LastRegDef = -1;
    for (int I = static_cast<int>(Prefix.size()) - 1; I >= 0; --I) {
      if (!consume())
        return std::nullopt;
      if (Prefix[I].Output.isReg() && Prefix[I].Output.Offset == Reg) {
        LastRegDef = I;
        break;
      }
    }
    if (!consume(orderedLookupWork(Prefix.size())))
      return std::nullopt;
    auto V = Emu.getRegister(Reg);
    // A failed load or unsupported operation can stop emulation before the
    // table-base LEA while leaving an older value in the same register.  That
    // stale value may coincidentally fall inside .text (for example RCX == 1),
    // so accept it only when the defining op itself was actually executed.
    if (LastRegDef >= 0 && Executed > static_cast<size_t>(LastRegDef) && V &&
        (!RequireMappedValue || *V)) {
      if (!RequireMappedValue)
        return V;
      if (!consume(Img.Segments.size()))
        return std::nullopt;
      if (Img.getSegmentFor(*V) ||
          (AllowUnmappedCOFFImageBase && Img.isCOFF() && *V == Img.Base))
        return V;
    }

    // The linear emulator may stop at an unrelated operation it cannot model
    // (notably an x87 80-bit LOAD) before a later LEA/ADR materialises the
    // table base.  Recover that common case without treating unknown register
    // inputs as zero: find the last definition of Reg before the first control
    // transfer, then retain it only when every value in its backward slice is
    // a constant or has an earlier definition in the same straight-line
    // prefix.  This covers both one-instruction RIP-relative LEA and a split
    // AArch64 ADRP+ADD after unsupported vector loads; `add reg, unknown_reg`
    // still fails because the unknown input has no definition in the slice.
    size_t StraightEnd = Prefix.size();
    for (size_t I = 0; I < Prefix.size(); ++I) {
      if (!consume())
        return std::nullopt;
      switch (Prefix[I].Opcode) {
      case NdOp::BRANCH:
      case NdOp::COND_BR:
      case NdOp::INDIR_BR:
      case NdOp::RETURN:
        StraightEnd = I;
        I = Prefix.size();
        break;
      default:
        break;
      }
    }

    int RegDef = -1;
    for (int I = static_cast<int>(StraightEnd) - 1; I >= 0; --I) {
      if (!consume())
        return std::nullopt;
      if (Prefix[I].Output.isReg() && Prefix[I].Output.Offset == Reg) {
        RegDef = I;
        break;
      }
    }
    if (RegDef < 0)
      return std::nullopt;

    if (!consume(2))
      return std::nullopt;
    std::set<int> SliceIdx;
    std::function<bool(const NdVar &, int)> addConstantDef =
        [&](const NdVar &Var, int Before) -> bool {
      if (!consume())
        return false;
      if (Var.isConst())
        return true;
      if (!Var.isReg() && !Var.isTemp())
        return false;
      int Def = -1;
      for (int I = std::min(Before, static_cast<int>(StraightEnd) - 1); I >= 0;
           --I) {
        if (!consume())
          return false;
        if (Prefix[I].Output.Space == Var.Space &&
            Prefix[I].Output.Offset == Var.Offset) {
          Def = I;
          break;
        }
      }
      if (Def < 0)
        return false;
      if (!consume(orderedLookupWork(SliceIdx.size())) || !consume(3))
        return false;
      if (!SliceIdx.insert(Def).second)
        return true;
      for (uint8_t I = 0; I < Prefix[Def].NumInputs; ++I) {
        if (!consume())
          return false;
        if (!addConstantDef(Prefix[Def].Inputs[I], Def - 1))
          return false;
      }
      return true;
    };

    if (!addConstantDef(Prefix[RegDef].Output, RegDef))
      return std::nullopt;
    // Prepay the Slice vector object, exact buffer, retained elements, and
    // future destruction.  The loop's existing debit remains the source-set
    // traversal; the post-loop debit pays emulator work.
    if (!consumeProduct(SliceIdx.size(), 3) || !consume(2))
      return std::nullopt;
    std::vector<LowOp> Slice;
    Slice.reserve(SliceIdx.size());
    for (int I : SliceIdx) {
      if (!consume())
        return std::nullopt;
      Slice.push_back(Prefix[I]);
    }
    if (!prepayEmulatorRun(Slice, 0, false))
      return std::nullopt;
    NdOpEmulator LocalEmu(Img);
    if (LocalEmu.run(Slice) != Slice.size())
      return std::nullopt;
    if (!consume(orderedLookupWork(Slice.size())))
      return std::nullopt;
    auto LocalV = LocalEmu.getRegister(Reg);
    if (LocalV && (!RequireMappedValue || *LocalV)) {
      if (!RequireMappedValue)
        return LocalV;
      if (!consume(Img.Segments.size()))
        return std::nullopt;
      if (Img.getSegmentFor(*LocalV) ||
          (AllowUnmappedCOFFImageBase && Img.isCOFF() &&
           *LocalV == Img.Base))
        return LocalV;
    }
    return std::nullopt;
  };

  // Block-local first: the base may be materialised in the INDIR_BR block
  // (ARM32 `add r, pc, #imm`).  Then fall back to the function prefix for a
  // loop-invariant base set in a dominator (x86 `lea table(%rip)`); run()
  // halts at the first branch, so the dominating block is still covered.
  va_t BlkStart = CurrentFuncEntry;
  if (!consume(orderedLookupWork(BlockStarts.size())))
    return std::nullopt;
  auto BIt = BlockStarts.upper_bound(Rec.Addr);
  if (BIt != BlockStarts.begin()) {
    --BIt;
    BlkStart = *BIt;
  }
  if (auto V = emulateFrom(BlkStart))
    return V;
  if (!WorkComplete)
    return std::nullopt;

  // A call before the base's materialisation (e.g. an FP `bl` inside a switch
  // block, then `add r,pc,#imm` forms the table base) halts run() at the call,
  // so the block-start emulation never reaches the base.  Retry from the
  // instruction after the last call/branch before the cutoff: an ARM
  // PC-relative base is a per-instruction constant, so it materialises fully
  // there and any caller-saved clobber is irrelevant to a base computed after
  // the call.
  va_t AfterLastTerm = InvalidVA;
  if (!consume(orderedLookupWork(Insns.size())))
    return std::nullopt;
  for (auto It = Insns.lower_bound(BlkStart);
       It != Insns.end() && It->first < Cutoff; ++It) {
    if (!consume())
      return std::nullopt;
    bool IsTerm = false;
    for (auto &Op : It->second.Ops) {
      if (!consume())
        return std::nullopt;
      switch (Op.Opcode) {
      case NdOp::CALL:
      case NdOp::INDIR_CALL:
      case NdOp::INTRINSIC:
      case NdOp::BRANCH:
      case NdOp::COND_BR:
      case NdOp::INDIR_BR:
        IsTerm = true;
        break;
      default:
        break;
      }
    }
    if (IsTerm) {
      auto Nx = std::next(It);
      AfterLastTerm = (Nx != Insns.end()) ? Nx->first : InvalidVA;
    }
  }
  if (AfterLastTerm != InvalidVA && AfterLastTerm < Cutoff)
    if (auto V = emulateFrom(AfterLastTerm))
      return V;
  if (!WorkComplete)
    return std::nullopt;

  // The base may be loop-invariant and materialised in a dominating block that
  // is neither the INDIR_BR's own block nor reachable from the function entry
  // without crossing a branch — e.g. a loop preheader's `lea table(%rip)` that
  // sits *after* a peeled first iteration's switch, so emulating from the entry
  // halts at the peeled INDIR_BR before reaching the `lea`.  Walk the
  // preceding block starts (nearest first) and emulate each block's own prefix;
  // the preheader resolves the base where the entry-prefix emulation stalls.
  int Tries = 0;
  if (!consume(orderedLookupWork(BlockStarts.size())))
    return std::nullopt;
  for (auto It = BlockStarts.lower_bound(BlkStart);
       It != BlockStarts.begin() && Tries < limits::kMaxQuasiCopyDepth;) {
    if (!consume())
      return std::nullopt;
    --It;
    if (auto V = emulateFrom(*It))
      return V;
    if (!WorkComplete)
      return std::nullopt;
    ++Tries;
  }
  if (BlkStart != CurrentFuncEntry)
    if (auto V = emulateFrom(CurrentFuncEntry))
      return V;
  return std::nullopt;
}

} // namespace neverd
