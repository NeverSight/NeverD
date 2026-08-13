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
/// table base is materialised.
std::optional<uint64_t> CFGBuilder::foldRegConstant(const BinaryImage &Img,
                                                    const InsnRecord &Rec,
                                                    uint64_t Reg,
                                                    va_t CutoffAddr) const {
  // Emulate up to (exclusive) the cutoff instruction.  The default cutoff is
  // the branch itself, but a table-base register may be reused (clobbered)
  // between the table load and the indirect branch — e.g. x86-64 `lea
  // tab(%rip),%r11; movslq (%r11,%idx,4),%r10; add %r11,%r10; mov %edx,%r11d;
  // jmp *%r10` — so callers fold it at the table load, before the clobber.
  va_t Cutoff = (CutoffAddr != InvalidVA) ? CutoffAddr : Rec.Addr;
  auto emulateFrom = [&](va_t Start) -> std::optional<uint64_t> {
    std::vector<LowOp> Prefix;
    for (auto It = Insns.lower_bound(Start);
         It != Insns.end() && It->first < Cutoff; ++It)
      for (auto &Op : It->second.Ops)
        Prefix.push_back(Op);
    NdOpEmulator Emu(Img);
    Emu.setCallPreservedRegisters(callPreservedRegs(Img));
    size_t Executed = Emu.run(Prefix);
    int LastRegDef = -1;
    for (int I = static_cast<int>(Prefix.size()) - 1; I >= 0; --I)
      if (Prefix[I].Output.isReg() && Prefix[I].Output.Offset == Reg) {
        LastRegDef = I;
        break;
      }
    auto V = Emu.getRegister(Reg);
    // A failed load or unsupported operation can stop emulation before the
    // table-base LEA while leaving an older value in the same register.  That
    // stale value may coincidentally fall inside .text (for example RCX == 1),
    // so accept it only when the defining op itself was actually executed.
    if (LastRegDef >= 0 && Executed > static_cast<size_t>(LastRegDef) && V &&
        *V && Img.getSegmentFor(*V))
      return V;

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
    for (size_t I = 0; I < Prefix.size(); ++I)
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

    int RegDef = -1;
    for (int I = static_cast<int>(StraightEnd) - 1; I >= 0; --I)
      if (Prefix[I].Output.isReg() && Prefix[I].Output.Offset == Reg) {
        RegDef = I;
        break;
      }
    if (RegDef < 0)
      return std::nullopt;

    std::set<int> SliceIdx;
    std::function<bool(const NdVar &, int)> addConstantDef =
        [&](const NdVar &Var, int Before) -> bool {
      if (Var.isConst())
        return true;
      if (!Var.isReg() && !Var.isTemp())
        return false;
      int Def = -1;
      for (int I = std::min(Before, static_cast<int>(StraightEnd) - 1); I >= 0;
           --I)
        if (Prefix[I].Output.Space == Var.Space &&
            Prefix[I].Output.Offset == Var.Offset) {
          Def = I;
          break;
        }
      if (Def < 0)
        return false;
      if (!SliceIdx.insert(Def).second)
        return true;
      for (uint8_t I = 0; I < Prefix[Def].NumInputs; ++I)
        if (!addConstantDef(Prefix[Def].Inputs[I], Def - 1))
          return false;
      return true;
    };

    if (!addConstantDef(Prefix[RegDef].Output, RegDef))
      return std::nullopt;
    std::vector<LowOp> Slice;
    Slice.reserve(SliceIdx.size());
    for (int I : SliceIdx)
      Slice.push_back(Prefix[I]);
    NdOpEmulator LocalEmu(Img);
    if (LocalEmu.run(Slice) != Slice.size())
      return std::nullopt;
    auto LocalV = LocalEmu.getRegister(Reg);
    if (LocalV && *LocalV && Img.getSegmentFor(*LocalV))
      return LocalV;
    return std::nullopt;
  };

  // Block-local first: the base may be materialised in the INDIR_BR block
  // (ARM32 `add r, pc, #imm`).  Then fall back to the function prefix for a
  // loop-invariant base set in a dominator (x86 `lea table(%rip)`); run()
  // halts at the first branch, so the dominating block is still covered.
  va_t BlkStart = CurrentFuncEntry;
  auto BIt = BlockStarts.upper_bound(Rec.Addr);
  if (BIt != BlockStarts.begin()) {
    --BIt;
    BlkStart = *BIt;
  }
  if (auto V = emulateFrom(BlkStart))
    return V;

  // A call before the base's materialisation (e.g. an FP `bl` inside a switch
  // block, then `add r,pc,#imm` forms the table base) halts run() at the call,
  // so the block-start emulation never reaches the base.  Retry from the
  // instruction after the last call/branch before the cutoff: an ARM
  // PC-relative base is a per-instruction constant, so it materialises fully
  // there and any caller-saved clobber is irrelevant to a base computed after
  // the call.
  va_t AfterLastTerm = InvalidVA;
  for (auto It = Insns.lower_bound(BlkStart);
       It != Insns.end() && It->first < Cutoff; ++It) {
    bool IsTerm = false;
    for (auto &Op : It->second.Ops)
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
    if (IsTerm) {
      auto Nx = std::next(It);
      AfterLastTerm = (Nx != Insns.end()) ? Nx->first : InvalidVA;
    }
  }
  if (AfterLastTerm != InvalidVA && AfterLastTerm < Cutoff)
    if (auto V = emulateFrom(AfterLastTerm))
      return V;

  // The base may be loop-invariant and materialised in a dominating block that
  // is neither the INDIR_BR's own block nor reachable from the function entry
  // without crossing a branch — e.g. a loop preheader's `lea table(%rip)` that
  // sits *after* a peeled first iteration's switch, so emulating from the entry
  // halts at the peeled INDIR_BR before reaching the `lea`.  Walk the
  // preceding block starts (nearest first) and emulate each block's own prefix;
  // the preheader resolves the base where the entry-prefix emulation stalls.
  int Tries = 0;
  for (auto It = BlockStarts.lower_bound(BlkStart);
       It != BlockStarts.begin() && Tries < limits::kMaxQuasiCopyDepth;) {
    --It;
    if (auto V = emulateFrom(*It))
      return V;
    ++Tries;
  }
  if (BlkStart != CurrentFuncEntry)
    if (auto V = emulateFrom(CurrentFuncEntry))
      return V;
  return std::nullopt;
}

} // namespace neverd
