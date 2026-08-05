//===- JumpTableResolver.cpp - Jump table detection and resolution --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Jump table resolution from indirect branch patterns and metadata
/// extraction into LowFunc::JumpTables.
///
/// The resolver uses a multi-strategy approach with fallback:
///
///   1. **ARM-family detectors** — the architecture-gated recognizers for
///      the ARM TBB/TBH table-branch and the AArch64 compact byte/halfword
///      table.  These are the only target-specific strategies and live in
///      JumpTableResolverARM.cpp; every strategy below is architecture-neutral.
///
///   2. **PIC-relative tables** — handle the common x64 pattern where
///      each table entry is a 32-bit signed offset from the table base:
///        target = base + (int32_t)table[index]
///
///   3. **Backward slicing** — trace data flow from the INDIR_BR input
///      through INT_ADD, INT_MULT, LOAD, INT_ZEXT, INT_LEFT, INT_RIGHT,
///      INT_ASHR, INT_SEXT, SUBBYTES, and COPY to identify the base
///      address and entry layout.  Cross-instruction base recovery
///      (stack-materialized, two-table, prior-insn PIC bases) lives in
///      JumpTableResolverSource.cpp.
///
///   4. **Guard analysis** — scan preceding instructions *and* CFG
///      predecessor blocks for comparison/mask ops (INT_LESS,
///      INT_LESSEQUAL, INT_SUB, INT_AND) that bound the switch
///      variable, giving a precise entry count.  Lives in
///      JumpTableResolverGuards.cpp.
///
///   5. **Multi-format entries** — read 1, 2, 4, or 8 byte entries,
///      both signed and unsigned, with tolerance for sparse invalid
///      entries in bounded tables.
///
///   6. **Sanity validation** — each target is checked for executable
///      segment membership, data availability at the target address,
///      reasonable distance from the function, and duplicate-run limits.
///
///   7. **Multi-stage fallback** — when the primary strategy produces
///      too few entries, retry with alternative entry sizes to recover
///      tables that use an unexpected format.
///
/// See CFGBuilder.cpp for the main CFG construction logic.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/NdOpEmulator.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <set>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

/// Registers that survive a call by ABI on \p Img's architecture — the stack
/// pointer, frame pointer, and callee-saved registers.  Handed to the emulator
/// so it can step over an intervening call (keeping these, dropping the
/// caller-saved rest) while folding a table base, instead of halting.
static std::vector<uint64_t> callPreservedRegs(const BinaryImage &Img) {
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  std::vector<uint64_t> Regs;
  Regs.reserve(TRI.CalleeSaveRegs.size() + 2);
  Regs.push_back(TRI.StackPointer);
  Regs.push_back(TRI.FramePointer);
  for (uint64_t R : TRI.CalleeSaveRegs)
    Regs.push_back(R);
  return Regs;
}

//===----------------------------------------------------------------------===//
// sliceBackForTableBase — backward data-flow slicing
//===----------------------------------------------------------------------===//

bool CFGBuilder::sliceBackForTableBase(const InsnRecord &Rec,
                                       JumpTableInfo &Info) {
  bool FoundBase = false;
  bool FoundSize = false;
  bool SawLoad = false;
  uint16_t LoadWidth = 0;

  for (int I = static_cast<int>(Rec.Ops.size()) - 1; I >= 0; --I) {
    if (Rec.Ops[I].Opcode != NdOp::INDIR_BR)
      continue;

    int Depth = 0;
    for (int J = I - 1; J >= 0 && Depth < limits::kMaxSliceDepth; --J) {
      ++Depth;
      auto &Op = Rec.Ops[J];
      switch (Op.Opcode) {
      case NdOp::INT_ADD:
        if (!FoundBase && Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
            Op.Inputs[1].Offset != 0) {
          Info.BaseAddr = Op.Inputs[1].Offset;
          FoundBase = true;
        } else if (!FoundBase && Op.NumInputs >= 2 && Op.Inputs[0].isConst() &&
                   Op.Inputs[0].Offset != 0) {
          Info.BaseAddr = Op.Inputs[0].Offset;
          FoundBase = true;
        }
        break;

      case NdOp::INT_MULT:
        if (!FoundSize && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
          Info.EntrySize = static_cast<uint16_t>(Op.Inputs[1].Offset);
          FoundSize = true;
        }
        break;

      case NdOp::INT_LEFT:
        if (!FoundSize && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
          uint64_t Shift = Op.Inputs[1].Offset;
          if (Shift <= limits::kMaxShiftForEntrySize) {
            Info.EntrySize = static_cast<uint16_t>(1u << Shift);
            FoundSize = true;
          }
        }
        break;

      case NdOp::INT_RIGHT:
      case NdOp::INT_ASHR:
        if (!FoundSize && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
          uint64_t Shift = Op.Inputs[1].Offset;
          if (Shift <= limits::kMaxShiftForEntrySize && Op.Output.Size > 0) {
            Info.EntrySize = Op.Output.Size;
            FoundSize = true;
          }
        }
        break;

      case NdOp::INT_SEXT:
        Info.IsSigned = true;
        Info.IsRelative = true;
        break;

      case NdOp::INT_ZEXT:
        if (!Info.IsSigned)
          Info.IsRelative = true;
        break;

      case NdOp::SUBBYTES:
        if (!FoundSize && Op.Output.Size > 0 &&
            Op.Output.Size <= limits::kMaxEntryBytes) {
          Info.EntrySize = Op.Output.Size;
          FoundSize = true;
        }
        break;

      case NdOp::COPY:
        break;

      case NdOp::LOAD:
        SawLoad = true;
        LoadWidth = Op.Output.Size;
        if (Op.NumInputs >= 1 && Op.Inputs[0].isConst() && !FoundBase) {
          Info.BaseAddr = Op.Inputs[0].Offset;
          FoundBase = true;
        }
        break;

      default:
        break;
      }
    }
    break;
  }

  if (SawLoad && !FoundSize && LoadWidth > 0 &&
      LoadWidth <= limits::kMaxEntryBytes) {
    Info.EntrySize = LoadWidth;
    FoundSize = true;
  }

  if (SawLoad && FoundBase && LoadWidth > 0 &&
      LoadWidth < limits::kMaxEntryBytes)
    Info.IsRelative = true;

  return FoundBase && FoundSize;
}

//===----------------------------------------------------------------------===//
// tryRelativeTable — PIC-relative jump table detection
//===----------------------------------------------------------------------===//

// The backward-slicing helpers (reachingDefIdx, traceToRegister,
// scaledIndexReg, ...) are declared in JumpTableResolverDetail.h and defined
// further below; the relative-table heuristic uses them to reject spill/reload
// relays.

/// Whether the LOAD address \p AddrV (defined before \p FromIdx in \p Ops) is a
/// plain stack/frame slot — `frameReg` or `frameReg + const`, with no scaled
/// index.  An indirect jump dispatched through such a load is a spill/reload
/// relay (`mov [esp+k], target; jmp *[esp+k]`): the computed target was stored
/// there a few instructions earlier, so the single-instruction relative-table
/// heuristic must not mistake the stack displacement for a table base.  The
/// genuine indexed table that produced the stored target is recovered by the
/// cross-instruction resolver instead.
static bool loadAddrIsFrameSlot(const std::vector<LowOp> &Ops, int FromIdx,
                                NdVar AddrV, const TargetRegInfo &TRI) {
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    if (AddrV.isReg())
      return TRI.isFrameReg(AddrV.Offset);
    int D = reachingDefIdx(Ops, FromIdx, AddrV);
    if (D < 0)
      return false;
    const LowOp &A = Ops[D];
    if (A.Opcode == NdOp::COPY && A.NumInputs >= 1) {
      AddrV = A.Inputs[0];
      FromIdx = D - 1;
      continue;
    }
    if (A.Opcode != NdOp::INT_ADD || A.NumInputs < 2)
      return false;
    int CW = A.Inputs[1].isConst() ? 1 : (A.Inputs[0].isConst() ? 0 : -1);
    if (CW < 0)
      return false; // base + base: not a simple frame slot
    if (scaledIndexReg(Ops, D - 1, A.Inputs[1 - CW]) != InvalidVA)
      return false; // base + index*scale: a genuine table access
    uint64_t Reg = traceToRegister(Ops, D - 1, A.Inputs[1 - CW]);
    return Reg != InvalidVA && TRI.isFrameReg(Reg);
  }
  return false;
}

bool CFGBuilder::tryRelativeTable(const BinaryImage &Img, const InsnRecord &Rec,
                                  JumpTableInfo &Info) {
  va_t CodeBase = 0;
  va_t TableBase = 0;
  uint16_t LoadSize = 0;
  bool HasSext = false;

  for (int I = static_cast<int>(Rec.Ops.size()) - 1; I >= 0; --I) {
    if (Rec.Ops[I].Opcode != NdOp::INDIR_BR)
      continue;

    for (int J = I - 1; J >= 0; --J) {
      auto &Op = Rec.Ops[J];
      if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs >= 2) {
        if (Op.Inputs[1].isConst() && Op.Inputs[1].Offset != 0) {
          CodeBase = Op.Inputs[1].Offset;
          break;
        }
        if (Op.Inputs[0].isConst() && Op.Inputs[0].Offset != 0) {
          CodeBase = Op.Inputs[0].Offset;
          break;
        }
      }
    }

    for (int J = I - 1; J >= 0; --J) {
      auto &Op = Rec.Ops[J];
      if (Op.Opcode == NdOp::INT_SEXT)
        HasSext = true;
      if (Op.Opcode == NdOp::LOAD) {
        // Reject a spill/reload relay: a target loaded from a frame slot is not
        // a table entry, and its stack displacement must not be read as a table
        // base.  Defer to the cross-instruction resolver for the real table.
        const NdVar &LAddr =
            (Op.NumInputs >= 2) ? Op.Inputs[1] : Op.Inputs[0];
        if (loadAddrIsFrameSlot(Rec.Ops, J - 1, LAddr,
                                getTargetRegInfo(Img.Arch)))
          return false;
        LoadSize = Op.Output.Size;
        for (int K = J - 1; K >= 0; --K) {
          if (Rec.Ops[K].Opcode == NdOp::INT_ADD && Rec.Ops[K].NumInputs >= 2) {
            if (Rec.Ops[K].Inputs[1].isConst())
              TableBase = Rec.Ops[K].Inputs[1].Offset;
            else if (Rec.Ops[K].Inputs[0].isConst())
              TableBase = Rec.Ops[K].Inputs[0].Offset;
            break;
          }
        }
        break;
      }
    }
    break;
  }

  if (CodeBase == 0 || LoadSize == 0)
    return false;

  const auto *CSeg = Img.getSegmentFor(CodeBase);
  if (!CSeg || !CSeg->isExecutable())
    return false;

  if (TableBase == 0)
    TableBase = CodeBase;

  Info.BaseAddr = TableBase;
  Info.EntrySize = LoadSize;
  Info.IsRelative = true;
  Info.IsSigned = HasSext || (LoadSize < limits::kMaxEntryBytes);
  return true;
}

//===----------------------------------------------------------------------===//
// tryCrossInstrRelativeTable — PIC table whose base is set in a prior insn
//===----------------------------------------------------------------------===//

/// Reaching-definition index of `V` searching backward from `FromIdx`.
int reachingDefIdx(const std::vector<LowOp> &Ops, int FromIdx,
                   const NdVar &V) {
  for (int I = FromIdx; I >= 0; --I) {
    const NdVar &O = Ops[I].Output;
    if (O.Space == V.Space && O.Offset == V.Offset)
      return I;
  }
  return -1;
}

/// Trace a nd-var backward through COPY chains to a plain register.
uint64_t traceToRegister(const std::vector<LowOp> &Ops, int FromIdx,
                         NdVar V) {
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    if (V.isReg())
      return V.Offset;
    if (!V.isTemp())
      return InvalidVA;
    int D = reachingDefIdx(Ops, FromIdx, V);
    if (D < 0 || Ops[D].Opcode != NdOp::COPY || Ops[D].NumInputs < 1)
      return InvalidVA;
    V = Ops[D].Inputs[0];
    FromIdx = D - 1;
  }
  return InvalidVA;
}

/// Trace a register back through reaching value-preserving definitions (COPY,
/// ZEXT/SEXT, low-half SUBBYTES) within the op list to its ultimate source
/// register.  Unlike traceToRegister this follows register->register copies and
/// register<-temp chains, recovering e.g. the `mov ecx,edi` or the
/// `movzbl sil,eax` (lifted as ZEXT of a SUBBYTES temp) that aliases a switch
/// index before it is used to address the table — so a guard on the original
/// register (`cmp edi,N` / `cmpb sil,N`) is still matched to the table index.
uint64_t traceRegSource(const std::vector<LowOp> &Ops, int FromIdx,
                        uint64_t RegOff) {
  NdVar Cur = NdVar::reg(RegOff, 8);
  uint64_t LastReg = RegOff;
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    int D = -1;
    for (int I = FromIdx; I >= 0; --I) {
      const NdVar &O = Ops[I].Output;
      if (O.Space == Cur.Space && O.Offset == Cur.Offset) {
        D = I;
        break;
      }
    }
    if (D < 0)
      return LastReg;
    const LowOp &Op = Ops[D];
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      if (Op.NumInputs < 1 || (!Op.Inputs[0].isReg() && !Op.Inputs[0].isTemp()))
        return LastReg;
      Cur = Op.Inputs[0];
      break;
    case NdOp::SUBBYTES:
      if (Op.NumInputs < 2 ||
          (!Op.Inputs[0].isReg() && !Op.Inputs[0].isTemp()) ||
          !Op.Inputs[1].isConst() || Op.Inputs[1].Offset != 0)
        return LastReg;
      Cur = Op.Inputs[0];
      break;
    default:
      return LastReg;
    }
    if (Cur.isReg())
      LastReg = Cur.Offset;
    FromIdx = D - 1;
  }
  return LastReg;
}

/// Like traceToRegister but also follows zero/sign-extension and low-half
/// subpiece.  Recovers the index register of a scaled table index that was
/// widened before scaling — e.g. AArch64 `ldr x,[base,w,uxtw #3]`, where the
/// 32-bit index is zero-extended (INT_ZEXT) ahead of the `<<3`.
static uint64_t traceIndexToRegister(const std::vector<LowOp> &Ops, int FromIdx,
                                     NdVar V) {
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    if (V.isReg())
      return V.Offset;
    if (!V.isTemp())
      return InvalidVA;
    int D = reachingDefIdx(Ops, FromIdx, V);
    if (D < 0)
      return InvalidVA;
    const LowOp &Op = Ops[D];
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      if (Op.NumInputs < 1)
        return InvalidVA;
      V = Op.Inputs[0];
      break;
    case NdOp::SUBBYTES:
      if (Op.NumInputs < 2 || !Op.Inputs[1].isConst() ||
          Op.Inputs[1].Offset != 0)
        return InvalidVA;
      V = Op.Inputs[0];
      break;
    default:
      return InvalidVA;
    }
    FromIdx = D - 1;
  }
  return InvalidVA;
}

/// If a nd-var is a scaled index (traced through COPY/ZEXT/SEXT to an
/// INT_MULT(reg, const>1) or INT_LEFT(reg, const)), return the source index
/// register; otherwise InvalidVA.
uint64_t scaledIndexReg(const std::vector<LowOp> &Ops, int FromIdx, NdVar V) {
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    if (!V.isTemp() && !V.isReg())
      return InvalidVA;
    int D = reachingDefIdx(Ops, FromIdx, V);
    if (D < 0)
      return InvalidVA;
    const LowOp &Op = Ops[D];
    bool Scaled = (Op.Opcode == NdOp::INT_MULT && Op.NumInputs >= 2 &&
                   Op.Inputs[1].isConst() && Op.Inputs[1].Offset > 1) ||
                  (Op.Opcode == NdOp::INT_LEFT && Op.NumInputs >= 2 &&
                   Op.Inputs[1].isConst() && Op.Inputs[1].Offset >= 1);
    if (Scaled)
      return traceIndexToRegister(Ops, D - 1, Op.Inputs[0]);
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      if (Op.NumInputs < 1)
        return InvalidVA;
      V = Op.Inputs[0];
      FromIdx = D - 1;
      break;
    default:
      return InvalidVA;
    }
  }
  return InvalidVA;
}

/// Resolve a LOAD address of the form INT_ADD(base, index*scale) into its
/// base register, requiring a genuine scaled index so plain pointer loads
/// are not mistaken for tables.
bool CFGBuilder::analyzeTableLoadAddr(const std::vector<LowOp> &Ops,
                                      int FromIdx, const NdVar &AddrV,
                                      uint64_t &BaseReg, uint64_t &IndexReg,
                                      bool &HasScaledIndex, uint64_t &Disp,
                                      va_t *AddrAddVA) const {
  Disp = 0;
  int AddIdx = reachingDefIdx(Ops, FromIdx, AddrV);
  // The effective address may be materialised in a register and copied to the
  // load operand (`lea base(,idx,8),%rN; mov %rN,%rM; jmp *(%rM)` — the
  // threaded/interleaved dispatch shape); follow the COPY chain (through both
  // temps and registers) to the defining INT_ADD.
  for (int Guard = 0;
       AddIdx >= 0 && Ops[AddIdx].Opcode == NdOp::COPY &&
       Ops[AddIdx].NumInputs >= 1 &&
       (Ops[AddIdx].Inputs[0].isReg() || Ops[AddIdx].Inputs[0].isTemp()) &&
       Guard < limits::kMaxQuasiCopyDepth;
       ++Guard)
    AddIdx = reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[0]);
  if (AddIdx < 0 || Ops[AddIdx].Opcode != NdOp::INT_ADD ||
      Ops[AddIdx].NumInputs < 2)
    return false;

  for (int Which = 0; Which < 2; ++Which) {
    uint64_t Idx = scaledIndexReg(Ops, AddIdx - 1, Ops[AddIdx].Inputs[Which]);
    if (Idx == InvalidVA)
      continue;
    uint64_t Reg =
        traceToRegister(Ops, AddIdx - 1, Ops[AddIdx].Inputs[1 - Which]);
    if (Reg != InvalidVA) {
      BaseReg = Reg;
      IndexReg = Idx;
      HasScaledIndex = true;
      // The base+index combining add: clang -O0 on ARM folds the scaled index
      // into the base register here (`add rB,rB,idx,lsl#k`), so a caller that
      // needs the *base* constant must fold rB before this add executes, not at
      // the load (where rB already holds base+index).
      if (AddrAddVA)
        *AddrAddVA = Ops[AddIdx].Addr;
      return true;
    }
  }

  // i386 PIC GOTOFF form: addr = (base + index*scale) + disp, where the GOTOFF
  // displacement is folded into the load.  Peel the outer constant and recurse
  // into the inner `base + index*scale`.
  for (int Which = 0; Which < 2; ++Which) {
    if (!Ops[AddIdx].Inputs[Which].isConst())
      continue;
    uint64_t D = Ops[AddIdx].Inputs[Which].Offset;
    int InnerIdx =
        reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[1 - Which]);
    if (InnerIdx < 0 || Ops[InnerIdx].Opcode != NdOp::INT_ADD ||
        Ops[InnerIdx].NumInputs < 2)
      continue;
    for (int W2 = 0; W2 < 2; ++W2) {
      uint64_t Idx =
          scaledIndexReg(Ops, InnerIdx - 1, Ops[InnerIdx].Inputs[W2]);
      if (Idx == InvalidVA)
        continue;
      uint64_t Reg =
          traceToRegister(Ops, InnerIdx - 1, Ops[InnerIdx].Inputs[1 - W2]);
      if (Reg != InvalidVA) {
        BaseReg = Reg;
        IndexReg = Idx;
        HasScaledIndex = true;
        Disp = D;
        return true;
      }
    }
  }
  return false;
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

/// Count the run of consecutive absolute code-pointer relocation slots starting
/// at TableAddr, stepping by EntrySize.  The loader records such slots in
/// Img.CodePtrRelocSlots; the run length is the exact entry count of a
/// computed-goto / threaded-dispatch jump table.
uint32_t countCodePtrRelocRun(const BinaryImage &Img, va_t TableAddr,
                              uint16_t EntrySize) {
  if (EntrySize == 0 || Img.CodePtrRelocSlots.empty())
    return 0;
  uint32_t Run = 0;
  for (va_t VA = TableAddr; Run < limits::kMaxJumpTableEntries;
       VA += EntrySize) {
    if (!Img.CodePtrRelocSlots.count(VA))
      break;
    ++Run;
  }
  return Run;
}

/// Count the run of consecutive PC-relative-to-code relocation slots starting
/// at the table base — the entries of a PIC `switch` jump table.  The run
/// length is the exact entry count, which bounds a `switch(x % N)` table whose
/// modulus constrains the index with no `cmp` range guard.
static uint32_t countRelCodeRelocRun(const BinaryImage &Img, va_t TableAddr,
                                     uint16_t EntrySize) {
  if (EntrySize == 0 || Img.RelCodeRelocSlots.empty())
    return 0;
  uint32_t Run = 0;
  for (va_t VA = TableAddr; Run < limits::kMaxJumpTableEntries;
       VA += EntrySize) {
    if (!Img.RelCodeRelocSlots.count(VA))
      break;
    ++Run;
  }
  return Run;
}

/// Truncate a RelCodeReloc entry run so it stops at the next PIC jump-table
/// base anchor.  Two unguarded PIC tables laid out back-to-back in rodata share
/// one continuous RelCodeReloc entry run, so a raw run count from the first
/// table's base over-reads past its end into the second — recovering bogus
/// successor edges (each an entry of the second table decoded relative to the
/// first base) that misroute the first dispatch (§15.2 adjacent-unguarded-pic-
/// table).  The next table's own base anchor — a rodata VA a `lea`/`adrp+add`/
/// GOTOFF materializes AND itself a RelCodeReloc entry position (so a plain
/// string/constant `lea` never truncates a real table) — is this table's exact
/// end.  Returns the run capped to the distance to that anchor.
static uint32_t boundRelRunByNextAnchor(const BinaryImage &Img, va_t BaseAddr,
                                        uint16_t EntrySize, uint32_t Run) {
  if (EntrySize == 0 || Run == 0)
    return Run;
  va_t NextAnchor = 0;
  for (auto It = Img.RelCodeTableAnchors.upper_bound(BaseAddr);
       It != Img.RelCodeTableAnchors.end(); ++It)
    if (Img.RelCodeRelocSlots.count(*It)) {
      NextAnchor = *It;
      break;
    }
  if (NextAnchor <= BaseAddr)
    return Run;
  uint32_t Cap = static_cast<uint32_t>((NextAnchor - BaseAddr) / EntrySize);
  return std::min(Run, Cap);
}

/// Resolve an address nd-var to a stack/frame slot key (base = SP/FP register
/// plus a constant byte offset).  Returns false for any non-SP/FP base or a
/// scaled-index address, so store-to-load forwarding never crosses heap/global
/// memory (which would be unsound).
bool frameSlotKey(const std::vector<LowOp> &Ops, int FromIdx, NdVar AddrV,
                  const TargetRegInfo &TRI, uint64_t &BaseReg, int64_t &Off) {
  Off = 0;
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    if (AddrV.isReg()) {
      if (!TRI.isFrameReg(AddrV.Offset))
        return false;
      BaseReg = AddrV.Offset;
      return true;
    }
    if (!AddrV.isTemp())
      return false;
    int D = reachingDefIdx(Ops, FromIdx, AddrV);
    if (D < 0)
      return false;
    const LowOp &A = Ops[D];
    if (A.Opcode == NdOp::COPY && A.NumInputs >= 1) {
      AddrV = A.Inputs[0];
      FromIdx = D - 1;
      continue;
    }
    if (A.Opcode == NdOp::INT_ADD && A.NumInputs >= 2) {
      int CW = A.Inputs[1].isConst() ? 1 : (A.Inputs[0].isConst() ? 0 : -1);
      if (CW < 0)
        return false;
      if (scaledIndexReg(Ops, D - 1, A.Inputs[1 - CW]) != InvalidVA)
        return false;
      Off += static_cast<int64_t>(A.Inputs[CW].Offset);
      AddrV = A.Inputs[1 - CW];
      FromIdx = D - 1;
      continue;
    }
    if (A.Opcode == NdOp::INT_SUB && A.NumInputs >= 2 &&
        A.Inputs[1].isConst()) {
      Off -= static_cast<int64_t>(A.Inputs[1].Offset);
      AddrV = A.Inputs[0];
      FromIdx = D - 1;
      continue;
    }
    return false;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// ARM-family target detectors (tryARMTableBranch, tryAArch64CompactTable) live
// in JumpTableResolverARM.cpp.  They are the only architecture-gated table
// recognizers; every other strategy -- the guard/bounds analysis in
// JumpTableResolverGuards.cpp, the base-address detectors in
// JumpTableResolverSource.cpp, and the framework below -- is pattern-based and
// architecture-neutral, so there is no corresponding x86 detector to split out
// (LLVM target-dispatch pattern).
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// isValidTarget — sanity check a resolved target address
//===----------------------------------------------------------------------===//

uint32_t CFGBuilder::getInsnAlignment() const {
  if (!CurrentImg)
    return 1;
  uint32_t Align = getTargetRegInfo(CurrentImg->Arch).MinInsnAlign;
  return Align ? Align : 1;
}

bool CFGBuilder::isValidTarget(const BinaryImage &Img, va_t Target,
                               va_t FuncEntry) {
  if (Target == 0)
    return false;

  const auto *Seg = Img.getSegmentFor(Target);
  if (!Seg || !Seg->isExecutable())
    return false;

  uint64_t Dist = Target > FuncEntry ? Target - FuncEntry : FuncEntry - Target;
  if (Dist > limits::kMaxJumpTargetDistance)
    return false;

  uint32_t Align = getInsnAlignment();
  if (Align > 1 && (Target % Align) != 0)
    return false;

  if (KnownFuncEntries && KnownFuncEntries->count(Target) &&
      Target != FuncEntry)
    return false;

  return true;
}

//===----------------------------------------------------------------------===//
// sanityCheckTargets — post-read validation with truncation
//===----------------------------------------------------------------------===//

bool CFGBuilder::sanityCheckTargets(const BinaryImage &Img,
                                    std::vector<va_t> &Targets) const {
  if (Targets.empty())
    return false;

  if (Targets.size() <= limits::kMinJumpTableEntries)
    return Targets.size() >= limits::kMinJumpTableEntries;

  uint32_t Align = getInsnAlignment();

  va_t RefAddr = Targets[0];
  size_t TruncAt = Targets.size();
  int InvalidCount = 0;

  for (size_t I = 1; I < Targets.size(); ++I) {
    uint64_t Dist =
        Targets[I] > RefAddr ? Targets[I] - RefAddr : RefAddr - Targets[I];
    if (Dist > limits::kMaxJumpTargetDistance) {
      TruncAt = I;
      break;
    }

    const auto *TSeg = Img.getSegmentFor(Targets[I]);
    if (!TSeg || !TSeg->isExecutable()) {
      TruncAt = I;
      break;
    }

    // A target only needs room for one minimum-size instruction.  Using a
    // fixed 4-byte slack wrongly rejects short trailing blocks (e.g. an x86
    // `ret` is 1 byte), truncating an otherwise valid table at that entry.
    size_t TOff = static_cast<size_t>(Targets[I] - TSeg->VA);
    if (!rangeInBounds(TOff, getInsnAlignment(), TSeg->Data.size())) {
      TruncAt = I;
      break;
    }

    if (Align > 1 && (Targets[I] % Align) != 0)
      ++InvalidCount;

    if (KnownFuncEntries && KnownFuncEntries->count(Targets[I]) &&
        Targets[I] != CurrentFuncEntry)
      ++InvalidCount;
  }

  if (TruncAt < Targets.size()) {
    LLVM_DEBUG(llvm::dbgs()
               << "  sanity: truncating table from " << Targets.size() << " to "
               << TruncAt << " entries\n");
    Targets.resize(TruncAt);
  }

  if (Targets.size() > limits::kMinJumpTableEntries && InvalidCount > 0) {
    int ValidPercent = static_cast<int>((Targets.size() - InvalidCount) * 100 /
                                        Targets.size());
    if (ValidPercent < limits::kMinValidTargetPercent) {
      LLVM_DEBUG(llvm::dbgs() << "  sanity: only " << ValidPercent
                              << "% valid targets, rejecting table\n");
      Targets.clear();
      return false;
    }
  }

  return Targets.size() >= limits::kMinJumpTableEntries;
}

//===----------------------------------------------------------------------===//
// readTableEntries — read entries from memory with format awareness
//===----------------------------------------------------------------------===//

/// Decode a single table entry into a target address.  For the compact-table
/// form (TargetBase != 0) the target is `TargetBase + entry * Scale`; otherwise
/// relative tables use `BaseAddr + entry` and absolute tables store the target.
static va_t decodeTableEntry(const uint8_t *P, uint16_t EntrySize,
                             bool IsRelative, bool IsSigned, va_t BaseAddr,
                             va_t TargetBase = 0, uint32_t Scale = 1) {
  if (TargetBase != 0) {
    int64_t Entry = 0;
    switch (EntrySize) {
    case 1:
      Entry = IsSigned ? static_cast<int8_t>(*P) : static_cast<uint8_t>(*P);
      break;
    case 2: {
      uint16_t Val;
      std::memcpy(&Val, P, 2);
      Entry = IsSigned ? static_cast<int16_t>(Val) : static_cast<int64_t>(Val);
      break;
    }
    case 4: {
      uint32_t Val;
      std::memcpy(&Val, P, 4);
      Entry = IsSigned ? static_cast<int32_t>(Val) : static_cast<int64_t>(Val);
      break;
    }
    default:
      break;
    }
    return static_cast<va_t>(static_cast<int64_t>(TargetBase) +
                             Entry * static_cast<int64_t>(Scale));
  }
  if (IsRelative) {
    int64_t Offset = 0;
    switch (EntrySize) {
    case 1:
      Offset = IsSigned ? static_cast<int8_t>(*P) : static_cast<uint8_t>(*P);
      break;
    case 2: {
      uint16_t Val;
      std::memcpy(&Val, P, 2);
      Offset = IsSigned ? static_cast<int16_t>(Val) : static_cast<int64_t>(Val);
      break;
    }
    case 4: {
      uint32_t Val;
      std::memcpy(&Val, P, 4);
      Offset = IsSigned ? static_cast<int32_t>(Val) : static_cast<int64_t>(Val);
      break;
    }
    default:
      break;
    }
    return static_cast<va_t>(static_cast<int64_t>(BaseAddr) + Offset);
  }

  va_t Target = 0;
  switch (EntrySize) {
  case 8:
    std::memcpy(&Target, P, 8);
    break;
  case 4: {
    uint32_t Val;
    std::memcpy(&Val, P, 4);
    Target = Val;
    break;
  }
  case 2: {
    uint16_t Val;
    std::memcpy(&Val, P, 2);
    Target = Val;
    break;
  }
  case 1:
    Target = *P;
    break;
  default:
    break;
  }
  return Target;
}

std::vector<va_t>
CFGBuilder::readTableEntries(const BinaryImage &Img, const JumpTableInfo &Info,
                             std::vector<uint32_t> *KeptIndices) {
  if (KeptIndices)
    KeptIndices->clear();
  const auto *Seg = Img.getSegmentFor(Info.BaseAddr);
  if (!Seg || Seg->Data.empty())
    return {};

  const bool Bounded = Info.MaxEntries > 0;
  uint32_t Limit = Info.MaxEntries;
  if (Limit == 0 || Limit > limits::kMaxJumpTableEntries)
    Limit = limits::kMaxJumpTableEntries;

  std::vector<va_t> Targets;
  Targets.reserve(std::min(Limit, 64u));
  size_t Off = static_cast<size_t>(Info.BaseAddr - Seg->VA);
  va_t PrevTarget = InvalidVA;
  int DuplicateRun = 0;
  int SkippedRun = 0;

  for (uint32_t I = 0; I < Limit; ++I) {
    size_t EntryOff = Off + I * Info.EntrySize;
    if (!rangeInBounds(EntryOff, Info.EntrySize, Seg->Data.size()))
      break;

    const uint8_t *P = Seg->Data.data() + EntryOff;
    va_t Target =
        decodeTableEntry(P, Info.EntrySize, Info.IsRelative, Info.IsSigned,
                         Info.BaseAddr, Info.TargetBase, Info.EntryScale);

    bool IsZeroOffset =
        Info.IsRelative && Info.TargetBase == 0 && (Target == Info.BaseAddr);
    if ((Target == 0 || IsZeroOffset) && !Bounded)
      break;

    if (!isValidTarget(Img, Target, CurrentFuncEntry)) {
      if (Bounded) {
        ++SkippedRun;
        if (SkippedRun > limits::kMaxSkippedEntries)
          break;
        continue;
      }
      break;
    }
    SkippedRun = 0;

    if (Target == PrevTarget) {
      if (++DuplicateRun > limits::kMaxDuplicateRun && !Bounded)
        break;
    } else {
      DuplicateRun = 0;
      PrevTarget = Target;
    }

    Targets.push_back(Target);
    if (KeptIndices)
      KeptIndices->push_back(I);
  }

  return Targets;
}

//===----------------------------------------------------------------------===//
// detectNormalization — detect index normalization (sub/shift) in the slice
//===----------------------------------------------------------------------===//

/// Walk the table-index computation backward from `V` (the index operand at the
/// table load), following value-preserving ops (COPY / ZEXT / SEXT /
/// SUBBYTES@0) and the stride/range mask (INT_AND), and record an
/// INT_SUB(x,const) base or INT_RIGHT/INT_ASHR(x,const) shift that genuinely
/// transforms the switch variable into the index.  Anchoring at the load keeps
/// a register the index is reused for *after* the table read (e.g. a peeled
/// loop iteration's accumulator) from contributing a phantom normalization.
static void traceIndexTransform(const std::vector<LowOp> &Ops, int FromIdx,
                                NdVar V, int64_t &NormBase,
                                uint32_t &NormShift, uint32_t &Stride) {
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    if (!V.isReg() && !V.isTemp())
      return;
    int D = reachingDefIdx(Ops, FromIdx, V);
    if (D < 0)
      return;
    const LowOp &Op = Ops[D];
    bool HasSrc =
        Op.NumInputs >= 1 && (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp());
    switch (Op.Opcode) {
    case NdOp::INT_SUB:
      if (Op.NumInputs >= 2 && Op.Inputs[1].isConst() && HasSrc) {
        int64_t Base = static_cast<int64_t>(Op.Inputs[1].Offset);
        if (NormBase == 0 && Base > 0 && Base <= limits::kMaxNormBase)
          NormBase = Base;
        V = Op.Inputs[0];
        FromIdx = D - 1;
        continue;
      }
      return;
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR:
      if (Op.NumInputs >= 2 && Op.Inputs[1].isConst() && HasSrc) {
        uint32_t Shift = static_cast<uint32_t>(Op.Inputs[1].Offset);
        if (NormShift == 0 && Shift > 0 && Shift <= limits::kMaxNormShift)
          NormShift = Shift;
        V = Op.Inputs[0];
        FromIdx = D - 1;
        continue;
      }
      return;
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      if (!HasSrc)
        return;
      V = Op.Inputs[0];
      FromIdx = D - 1;
      continue;
    case NdOp::INT_AND:
      // A mask on the switch variable preserves the surviving bits, so keep
      // tracing through it.  Trailing zero bits in the mask mean the index is
      // always a multiple of 2^k — a genuine stride — but only when this mask
      // is in the index dataflow (an unrelated `and x,6` in the prologue is
      // not).
      if (HasSrc && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
        uint64_t Mask = Op.Inputs[1].Offset;
        // A contiguous low-bits mask (2^k - 1) bounds the index to [0, 2^k):
        // the masked value *is* the terminal switch index, i.e. the source is
        // an explicit index expression like `(expr >> j) & m`, not a normalized
        // `switch(x)` whose case labels need reconstructing.  The ops feeding
        // the mask only *compute* that index, so they carry no label
        // normalization — stop here, leaving case labels as the raw indices
        // 0..N-1 the emitter's switch (which dispatches on the post-mask value)
        // actually compares. Without this the `>> j` inside the index
        // expression is mis-read as a NormShift and bogus `i << j` case values
        // are emitted that never match the runtime index (only the index-0 case
        // lands, the rest fall through to the default — observed on ARM32
        // inline PC-relative word tables, whose entries carry no relocation and
        // so skip the RelRun label reset).
        if (Mask != 0 && (Mask & (Mask + 1)) == 0) {
          NormBase = 0;
          NormShift = 0;
          return;
        }
        if (Stride <= 1 && Mask != 0) {
          uint32_t S = 1;
          uint64_t M = Mask;
          while ((M & 1) == 0 && S < limits::kMaxStrideScanBits) {
            M >>= 1;
            S <<= 1;
          }
          if (S > 1 && S <= limits::kMaxEntryBytes)
            Stride = S;
        }
        V = Op.Inputs[0];
        FromIdx = D - 1;
        continue;
      }
      return;
    case NdOp::SUBBYTES:
      if (HasSrc && Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
          Op.Inputs[1].Offset == 0) {
        V = Op.Inputs[0];
        FromIdx = D - 1;
        continue;
      }
      return;
    default:
      return;
    }
  }
}

//===----------------------------------------------------------------------===//
// evalLinearMultiple — read the integer multiplier of a single base value
//===----------------------------------------------------------------------===//

/// Fold `V` (or its COPY/ZEXT/SEXT chain) to a constant value, if any.  Used to
/// read a modulus that was materialised in a register (`mov wN,#11; msub`) as
/// the back-multiply constant, not just an immediate operand.
static std::optional<int64_t> constValueOf(const std::vector<LowOp> &Ops,
                                           int FromIdx, NdVar V,
                                           int Depth = 0) {
  if (V.isConst())
    return static_cast<int64_t>(V.Offset);
  if ((!V.isReg() && !V.isTemp()) || Depth > limits::kMaxQuasiCopyDepth)
    return std::nullopt;
  int D = reachingDefIdx(Ops, FromIdx, V);
  if (D < 0)
    return std::nullopt;
  const LowOp &Op = Ops[D];
  if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
       Op.Opcode == NdOp::INT_SEXT) &&
      Op.NumInputs >= 1)
    return constValueOf(Ops, D - 1, Op.Inputs[0], Depth + 1);
  return std::nullopt;
}

/// Decompose `V` into `base * Coef`, where Coef is built from the shift /
/// small-constant-multiply / add / subtract terms of one conceptual base.
/// Recovers the modulus N out of the `quotient * N` back-multiply clang emits
/// for `x % N` (rendered as shift/add/sub trees, e.g. q*7=(q<<3)-q,
/// q*9=(q<<3)+q, q*10=(q<<3)+(q<<1), or a direct `q*N` where N may live in a
/// register).  Any op that is not a multiplier-tree node (the magic
/// `(x*recip)>>s` quotient, a load, a param) terminates a branch as the base
/// with coefficient 1; the caller gates on a multiply being present and on the
/// recovered N matching the table's real entry count so this leniency cannot
/// misread an ordinary table.
static bool evalLinearMultiple(const std::vector<LowOp> &Ops, int FromIdx,
                               NdVar V, int Depth, int64_t &Coef) {
  if (Depth > limits::kMaxModuloDecompDepth)
    return false;
  if (!V.isReg() && !V.isTemp())
    return false;
  int D = reachingDefIdx(Ops, FromIdx, V);
  if (D < 0) {
    Coef = 1; // No definition in the slice: the base itself.
    return true;
  }
  const LowOp &Op = Ops[D];
  auto isVar = [](const NdVar &X) { return X.isReg() || X.isTemp(); };
  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    if (Op.NumInputs >= 1 && isVar(Op.Inputs[0]))
      return evalLinearMultiple(Ops, D - 1, Op.Inputs[0], Depth + 1, Coef);
    Coef = 1; // COPY of a constant: a materialised base.
    return true;
  case NdOp::SUBBYTES:
    if (Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
        Op.Inputs[1].Offset == 0 && isVar(Op.Inputs[0]))
      return evalLinearMultiple(Ops, D - 1, Op.Inputs[0], Depth + 1, Coef);
    return false;
  case NdOp::INT_LEFT:
    if (Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
        Op.Inputs[1].Offset < 32 && isVar(Op.Inputs[0])) {
      int64_t C;
      if (!evalLinearMultiple(Ops, D - 1, Op.Inputs[0], Depth + 1, C))
        return false;
      Coef = C << Op.Inputs[1].Offset;
      return true;
    }
    return false;
  case NdOp::INT_MULT: {
    // base * const, where the const may be an immediate or a register/temp that
    // resolves to one (e.g. `msub` with the modulus in a register).
    for (int CK = 0; CK < Op.NumInputs && CK < 2; ++CK) {
      int BK = 1 - CK;
      if (BK >= Op.NumInputs || !isVar(Op.Inputs[BK]))
        continue;
      std::optional<int64_t> C = constValueOf(Ops, D - 1, Op.Inputs[CK]);
      if (!C)
        continue;
      int64_t Bc;
      if (!evalLinearMultiple(Ops, D - 1, Op.Inputs[BK], Depth + 1, Bc))
        return false;
      Coef = Bc * (*C);
      return true;
    }
    Coef = 1; // q*recip (the magic quotient itself): base.
    return true;
  }
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
    if (Op.NumInputs >= 2 && isVar(Op.Inputs[0]) && isVar(Op.Inputs[1])) {
      int64_t A, B;
      if (!evalLinearMultiple(Ops, D - 1, Op.Inputs[0], Depth + 1, A) ||
          !evalLinearMultiple(Ops, D - 1, Op.Inputs[1], Depth + 1, B))
        return false;
      Coef = (Op.Opcode == NdOp::INT_ADD) ? (A + B) : (A - B);
      return true;
    }
    return false;
  default:
    Coef = 1; // Quotient produced by a non-multiplier op (shift/divide): base.
    return true;
  }
}

void CFGBuilder::detectNormalization(const InsnRecord &Rec,
                                     JumpTableInfo &Info) {
  // Flatten the INDIR_BR block (inclusive of Rec) so an index computation
  // spanning several instructions is visible to the trace below.
  va_t BlkStart = CurrentFuncEntry;
  auto BIt = BlockStarts.upper_bound(Rec.Addr);
  if (BIt != BlockStarts.begin()) {
    --BIt;
    BlkStart = *BIt;
  }
  std::vector<LowOp> BlockOps;
  for (auto It = Insns.lower_bound(BlkStart);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      BlockOps.push_back(Op);

  // Precise path: locate the table LOAD (scaled-index address) and trace the
  // index dataflow backward from it.  This restricts normalization/stride
  // detection to ops that actually transform the switch variable, so unrelated
  // arithmetic in the block (a shift carry-flag helper temp `t = t - 1`, a case
  // body's `and x,6`) cannot inject a phantom base/shift/stride.
  //
  // It also recovers the scaled-index register itself.  A plain absolute or
  // relative table folded straight out of the dispatch (`jmp *tab(,idx,8)` with
  // the base a constant in the branch record) never had its index register
  // identified by a dedicated detector, leaving Info.IndexReg unset — which
  // disables every index-keyed bound strategy (mask / modulo / range-guard).
  // Anchoring on the table load recovers it here so those strategies engage.
  for (int I = static_cast<int>(BlockOps.size()) - 1; I >= 0; --I) {
    auto &L = BlockOps[I];
    if (L.Opcode != NdOp::LOAD || L.NumInputs < 1)
      continue;
    const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
    uint64_t BaseReg = InvalidVA, IdxReg = InvalidVA, LoadDisp = 0;
    bool Scaled = false;
    if (!analyzeTableLoadAddr(BlockOps, I - 1, AddrV, BaseReg, IdxReg, Scaled,
                              LoadDisp)) {
      // analyzeTableLoadAddr requires a *register* base (`base + index*scale`).
      // A non-PIC absolute table folds the base to a constant in the dispatch
      // (`jmp *tab(,idx,8)` => addr = INT_ADD(index*scale, const)), so it is
      // rejected there.  Recover the scaled-index register directly from the
      // load-address INT_ADD instead — either operand may be the scaled index,
      // the other the constant base — so the index-keyed bound strategies still
      // engage for constant-base tables.
      int AddIdx = reachingDefIdx(BlockOps, I - 1, AddrV);
      for (int G = 0; AddIdx >= 0 && BlockOps[AddIdx].Opcode == NdOp::COPY &&
                      BlockOps[AddIdx].NumInputs >= 1 &&
                      G < limits::kMaxQuasiCopyDepth;
           ++G)
        AddIdx = reachingDefIdx(BlockOps, AddIdx - 1, BlockOps[AddIdx].Inputs[0]);
      if (AddIdx < 0 || BlockOps[AddIdx].Opcode != NdOp::INT_ADD ||
          BlockOps[AddIdx].NumInputs < 2)
        continue;
      for (int W = 0; W < 2; ++W) {
        uint64_t Idx =
            scaledIndexReg(BlockOps, AddIdx - 1, BlockOps[AddIdx].Inputs[W]);
        if (Idx != InvalidVA) {
          IdxReg = Idx;
          break;
        }
      }
      if (IdxReg == InvalidVA)
        continue;
    }
    if (Info.IndexReg == InvalidVA)
      Info.IndexReg = IdxReg;
    traceIndexTransform(BlockOps, I - 1, NdVar::reg(IdxReg, 8), Info.NormBase,
                        Info.NormShift, Info.Stride);
    return;
  }

  // Fallback: when the index/table load could not be resolved, scan only the
  // INDIR_BR record's own ops for a local normalization (low-risk, no
  // cross-instruction guessing).
  for (int I = static_cast<int>(Rec.Ops.size()) - 1; I >= 0; --I) {
    if (Rec.Ops[I].Opcode != NdOp::INDIR_BR)
      continue;
    for (int J = I - 1; J >= 0; --J) {
      auto &Op = Rec.Ops[J];
      if (Op.Opcode == NdOp::INT_SUB && Op.NumInputs >= 2 &&
          Op.Inputs[1].isConst()) {
        int64_t Base = static_cast<int64_t>(Op.Inputs[1].Offset);
        if (Base > 0 && Base <= limits::kMaxNormBase)
          Info.NormBase = Base;
      }
      if ((Op.Opcode == NdOp::INT_RIGHT || Op.Opcode == NdOp::INT_ASHR) &&
          Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
        uint32_t Shift = static_cast<uint32_t>(Op.Inputs[1].Offset);
        if (Shift > 0 && Shift <= limits::kMaxNormShift)
          Info.NormShift = Shift;
      }
    }
    break;
  }
}

//===----------------------------------------------------------------------===//
// inferBoundsFromModulo — bound a `switch(x % N)` table from its remainder
//===----------------------------------------------------------------------===//

/// A power-of-two modulo switch (`switch(x % 2^k)` / `switch(x & (2^k-1))`)
/// lowers the index to `and $(2^k-1)` with no `cmp` range guard.  When such a
/// table sits adjacent to another in rodata the two form one continuous
/// relocation run, so the run-length count over-reads the first table into the
/// second — fabricating bogus successor edges (and, with x87 residents, an
/// unbalanced stack the TOP propagation cannot reconcile).  The mask is a hard
/// upper bound on the index: it confines it to [0, M].  A following `-c` (clang
/// emits `dec` when a peeled iteration proved the low cases dead) lowers the
/// top index to M-c, so the table holds at most (M + Offset) + 1 entries.
/// Returns that bound, or 0 when the index does not reduce to a clean low-bit
/// mask.
uint32_t CFGBuilder::inferBoundsFromMask(const InsnRecord &Rec,
                                         const JumpTableInfo &Info,
                                         bool AllowNonContiguous) const {
  if (Info.IndexReg == InvalidVA)
    return 0;

  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      Ops.push_back(Op);

  // The index register is frequently reused to hold the loaded entry after the
  // table LOAD, so trace from just before the last load.
  int LastLoad = -1;
  for (int I = 0; I < static_cast<int>(Ops.size()); ++I)
    if (Ops[I].Opcode == NdOp::LOAD)
      LastLoad = I;
  if (LastLoad < 0)
    return 0;

  // Sign-extend an operand-width constant so a 32-bit `dec` (add 0xFFFFFFFF)
  // reads back as -1 rather than a 4-billion offset.
  auto signedConst = [](const NdVar &C) -> int64_t {
    uint64_t U = C.Offset;
    int Sz = C.Size;
    if (Sz > 0 && Sz < 8) {
      uint64_t Mask = (1ULL << (Sz * 8)) - 1;
      U &= Mask;
      if (U & (1ULL << (Sz * 8 - 1)))
        return static_cast<int64_t>(U | ~Mask);
    }
    return static_cast<int64_t>(U);
  };

  const TargetRegInfo &TRImask =
      getTargetRegInfo(CurrentImg ? CurrentImg->Arch : Arch::Unknown);

  NdVar V = NdVar::reg(Info.IndexReg, 8);
  int From = LastLoad - 1;
  int64_t Offset = 0;
  for (int Step = 0; Step < limits::kMaxQuasiCopyDepth; ++Step) {
    int D = reachingDefIdx(Ops, From, V);
    if (D < 0)
      return 0;
    const LowOp &Op = Ops[D];

    if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
         Op.Opcode == NdOp::INT_SEXT) &&
        Op.NumInputs >= 1 && (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
      V = Op.Inputs[0];
      From = D - 1;
      continue;
    }
    // Trace through an -O0 stack spill/reload: the masked index is written into
    // a frame slot (`and idx,7; str idx,[sp,#k]`) and reloaded into a fresh
    // register right before the dispatch (`ldr idx,[sp,#k]; ldr
    // t,[tab,idx,4]`). Hop from the reload LOAD to the value the matching STORE
    // saved, so the trace reaches the mask that would otherwise be hidden
    // behind the slot.
    if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1) {
      const NdVar &LAddr = (Op.NumInputs >= 2) ? Op.Inputs[1] : Op.Inputs[0];
      uint64_t SlotReg = InvalidVA;
      int64_t SlotOff = 0;
      if (frameSlotKey(Ops, D - 1, LAddr, TRImask, SlotReg, SlotOff)) {
        int StoreIdx = -1;
        for (int I = D - 1; I >= 0; --I) {
          const LowOp &S = Ops[I];
          if (S.Opcode != NdOp::STORE || S.NumInputs < 2)
            continue;
          uint64_t SB = InvalidVA;
          int64_t SO = 0;
          if (frameSlotKey(Ops, I - 1, S.Inputs[0], TRImask, SB, SO) &&
              SB == SlotReg && SO == SlotOff) {
            StoreIdx = I;
            break;
          }
        }
        if (StoreIdx >= 0 && (Ops[StoreIdx].Inputs[1].isReg() ||
                              Ops[StoreIdx].Inputs[1].isTemp())) {
          V = Ops[StoreIdx].Inputs[1];
          From = StoreIdx - 1;
          continue;
        }
      }
      return 0;
    }
    if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
        Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0 &&
        (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
      V = Op.Inputs[0];
      From = D - 1;
      continue;
    }
    // index = masked +/- constant: accumulate the offset, keep tracing.
    if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
        Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
        (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
      int64_t C = signedConst(Op.Inputs[1]);
      Offset += (Op.Opcode == NdOp::INT_ADD) ? C : -C;
      if (Offset < -64 || Offset > 64)
        return 0;
      V = Op.Inputs[0];
      From = D - 1;
      continue;
    }
    // The binding mask: a contiguous low-bit mask (2^k - 1) caps the index.  It
    // may be EITHER operand (AND is commutative) and the size optimizer may
    // materialise it in a register hoisted to the loop preheader (ARM32 -Oz
    // `mov rM,#7; ... and idx,rM,x`), so resolve each operand to a constant —
    // directly or through a COPY chain visible in the function-wide Ops.
    if (Op.Opcode == NdOp::INT_AND && Op.NumInputs >= 2) {
      auto constOf = [&](NdVar In) -> std::optional<uint64_t> {
        for (int G = 0, F = D - 1; G < limits::kMaxQuasiCopyDepth; ++G) {
          if (In.isConst())
            return In.Offset;
          if (!In.isReg() && !In.isTemp())
            return std::nullopt;
          int DD = reachingDefIdx(Ops, F, In);
          if (DD < 0 || Ops[DD].Opcode != NdOp::COPY || Ops[DD].NumInputs < 1)
            return std::nullopt;
          In = Ops[DD].Inputs[0];
          F = DD - 1;
        }
        return std::nullopt;
      };
      for (int W = 0; W < 2; ++W) {
        auto MOpt = constOf(Op.Inputs[W]);
        if (!MOpt)
          continue;
        uint64_t M = *MOpt;
        if (M == 0)
          continue;
        // A contiguous low-bit mask (2^k - 1) bounds the index to [0, M]
        // exactly.  A non-contiguous mask (e.g. 0x1e) still bounds the raw
        // masked value, but its maximum is the mask's covering value — every
        // bit below the top set bit filled — since any subset of the mask's
        // bits can be simultaneously set.  The table is then dense over that
        // raw index with default filler in the unused (gap) slots, so the
        // covering value + 1 is the physical entry count.  Fold the covering
        // mask so a `switch(x & M)` with an arbitrary M is still bounded.
        if ((M & (M + 1)) != 0) {
          if (!AllowNonContiguous)
            continue;
          uint64_t Cover = M;
          Cover |= Cover >> 1;
          Cover |= Cover >> 2;
          Cover |= Cover >> 4;
          Cover |= Cover >> 8;
          Cover |= Cover >> 16;
          Cover |= Cover >> 32;
          M = Cover;
        }
        int64_t Hi = static_cast<int64_t>(M) + Offset;
        if (Hi < static_cast<int64_t>(limits::kMinJumpTableEntries) - 1)
          continue;
        uint64_t Bound = static_cast<uint64_t>(Hi) + 1;
        if (Bound <= limits::kMaxJumpTableEntries)
          return static_cast<uint32_t>(Bound);
      }
      return 0;
    }
    return 0;
  }
  return 0;
}

/// A modulo switch (`switch(x % N)`, N not a power of two) carries no `cmp`
/// range guard — the remainder is already in [0, N) — so the entry count must
/// come from the modulus N itself.  clang computes `x % N` as
/// `idx = x - (x / N) * N` with a magic-reciprocal division for the quotient
/// and a shift/add/sub tree for the `* N` back-multiply.  This recovers N by
/// decomposing that back-multiply, which bounds tables that carry no entry
/// relocations (AArch64 byte/halfword compact tables, ARM32 inline `.text` word
/// tables) and so cannot use the relocation-run count (#403).  Returns true and
/// sets Info.MaxEntries when a magic-division remainder yields a sane modulus.
bool CFGBuilder::inferBoundsFromModulo(const BinaryImage &Img,
                                       const InsnRecord &Rec,
                                       JumpTableInfo &Info) {
  if (Info.BaseAddr == 0 || Info.EntrySize == 0 || Info.IndexReg == InvalidVA)
    return false;

  // Flatten from the function entry through the dispatch so both the
  // remainder computation (in the loop body) and the modulus constant (often
  // materialised once in the prologue, e.g. AArch64 `mov w11,#N` before an
  // `msub`) are visible to the backward trace.  Reaching-definition scans pick
  // the nearest def, so the wider range only supplies the otherwise-missing
  // prologue constant.
  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      Ops.push_back(Op);

  // Gate: a magic-reciprocal division always leaves a multiply in the block
  // (the `x * recip`).  The modulus may live in a register, so the presence of
  // a multiply is the structural gate; the recovered N is then confirmed below
  // against the table's real entry count.
  bool SawMul = false;
  int LastLoad = -1;
  for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
    if (Ops[I].Opcode == NdOp::INT_MULT)
      SawMul = true;
    if (Ops[I].Opcode == NdOp::LOAD)
      LastLoad = I;
  }
  if (!SawMul || LastLoad < 0)
    return false;

  // Follow the index register back from just before the table LOAD (the index
  // register is frequently reused to hold the loaded entry afterwards) through
  // value-preserving reshapes to the `idx = dividend - backMul` remainder
  // subtraction that computes `x % N`.
  NdVar V = NdVar::reg(Info.IndexReg, 8);
  int From = LastLoad - 1;
  for (int Step = 0; Step < limits::kMaxQuasiCopyDepth; ++Step) {
    int D = reachingDefIdx(Ops, From, V);
    if (D < 0)
      return false;
    const LowOp &Op = Ops[D];
    if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
         Op.Opcode == NdOp::INT_SEXT) &&
        Op.NumInputs >= 1 && (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
      V = Op.Inputs[0];
      From = D - 1;
      continue;
    }
    if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
        Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0 &&
        (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
      V = Op.Inputs[0];
      From = D - 1;
      continue;
    }
    if ((Op.Opcode != NdOp::INT_SUB && Op.Opcode != NdOp::INT_ADD) ||
        Op.NumInputs < 2)
      return false;

    // The remainder is `dividend - quotient*N` (INT_SUB) or, when the modulus
    // folds into the multiply, `dividend + quotient*(-N)` (INT_ADD, e.g. an
    // AArch64 `msub`).  One operand is the dividend (coefficient ~1); the other
    // is the back-multiply whose |coefficient| is the modulus N.  Decompose
    // each and take the term whose magnitude is a sane entry count.
    for (int Which : {1, 0}) {
      const NdVar &Cand = Op.Inputs[Which];
      if (!Cand.isReg() && !Cand.isTemp())
        continue;
      int64_t Coef = 0;
      if (!evalLinearMultiple(Ops, D - 1, Cand, 0, Coef))
        continue;
      int64_t N = Coef < 0 ? -Coef : Coef;
      if (N < static_cast<int64_t>(limits::kMinJumpTableEntries) ||
          N > static_cast<int64_t>(limits::kMaxJumpTableEntries))
        continue;
      // Confirm N against the table: a real `x % N` dispatch has at least N
      // valid consecutive entries (the in-range cases), which rejects a
      // coincidental linear index that is not actually a modulo remainder.
      JumpTableInfo Probe = Info;
      Probe.MaxEntries = 0;
      if (static_cast<int64_t>(readTableEntries(Img, Probe).size()) < N)
        continue;
      Info.MaxEntries = static_cast<uint32_t>(N);
      Info.RelocBounded = true;
      Info.NormBase = 0;
      Info.NormShift = 0;
      Info.Stride = 1;
      return true;
    }
    return false;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// detectStride — infer switch variable stride from AND masks
//===----------------------------------------------------------------------===//

/// Detect a stride (power-of-2 alignment) on the switch variable by
/// looking for INT_AND with a mask that has known-zero low bits.  When
/// the guard bound is N and the stride is S, the effective table size
/// is N / S.
void CFGBuilder::detectStride(const InsnRecord &Rec, JumpTableInfo &Info) {
  uint64_t SwitchReg = InvalidVA;
  for (auto &Op : Rec.Ops) {
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1 &&
        Op.Inputs[0].isReg()) {
      SwitchReg = Op.Inputs[0].Offset;
      break;
    }
  }
  if (SwitchReg == InvalidVA)
    return;

  uint64_t SwitchSrc =
      quasiCopySource(Rec.Ops, static_cast<int>(Rec.Ops.size()) - 1, SwitchReg);

  // Scan the INDIR_BR instruction's ops for an AND mask.
  for (auto &Op : Rec.Ops) {
    if (Op.Opcode != NdOp::INT_AND || Op.NumInputs < 2 ||
        !Op.Inputs[1].isConst())
      continue;
    if (!Op.Inputs[0].isReg())
      continue;

    uint64_t GuardSrc = quasiCopySource(
        Rec.Ops, static_cast<int>(Rec.Ops.size()) - 1, Op.Inputs[0].Offset);
    if (GuardSrc != SwitchSrc)
      continue;

    uint64_t Mask = Op.Inputs[1].Offset;
    if (Mask == 0)
      continue;

    uint32_t Stride = 1;
    uint64_t M = Mask;
    while ((M & 1) == 0 && Stride < limits::kMaxStrideScanBits) {
      M >>= 1;
      Stride <<= 1;
    }

    if (Stride > 1 && Stride <= limits::kMaxEntryBytes) {
      Info.Stride = Stride;
      LLVM_DEBUG(llvm::dbgs()
                 << "  stride: detected stride=" << Stride
                 << " from AND mask 0x" << llvm::utohexstr(Mask) << "\n");
    }
    break;
  }

  // A cross-instruction stride (a mask applied to the index in an earlier
  // instruction of the block) is recovered by detectNormalization's index-chain
  // trace, which is confined to the dataflow that feeds the table load.  An
  // unconstrained block scan here would instead latch onto any masked value
  // (e.g. a case body's `and x,6`) and report a phantom stride, so it is
  // intentionally omitted.
}

//===----------------------------------------------------------------------===//
// pullBackBound — adjust a guard bound through normalization operations
//===----------------------------------------------------------------------===//

uint32_t CFGBuilder::pullBackBound(uint32_t RawBound,
                                   const JumpTableInfo &Info) const {
  uint32_t Adjusted = RawBound;

  if (Info.NormBase > 0 && Adjusted > static_cast<uint32_t>(Info.NormBase))
    Adjusted -= static_cast<uint32_t>(Info.NormBase);

  if (Info.NormShift > 0)
    Adjusted >>= Info.NormShift;

  if (Adjusted == 0)
    return RawBound;
  return Adjusted;
}

//===----------------------------------------------------------------------===//
// recoverCaseLabels — inverse-normalize table indices to original case values
//===----------------------------------------------------------------------===//

void CFGBuilder::recoverCaseLabels(JumpTable &JT,
                                   const JumpTableInfo &Info) const {
  // The recovered case labels must be expressed in the SAME coordinate the
  // emitter dispatches on, which depends on the table kind:
  //
  //   * Resolver-register dispatch (AArch64 compact byte/halfword tables and
  //     pre-scaled computed gotos) dispatches on the recovered index register,
  //     the *pre-normalization* switch variable — so the labels invert the
  //     normalization: label = (position * Stride << NormShift) + NormBase.
  //     This mirrors MedLLVMEmitter::emitJumpTableSwitch's own path selection
  //     `(TargetBase != 0 && EntrySize <= 2) || PreScaledIndex`.
  //
  //   * Every other (regular relative/absolute) table is dispatched via
  //     MedLLVMEmitter::findSwitchIndex, which returns the value feeding the
  //     table-address scale — the *post-normalization* index, after any
  //     `sub base` / mask / shift that computed it.  Its labels are therefore
  //     the raw table positions 0..N-1 (Stride still scales a pre-scaled byte
  //     index).  Folding NormBase/NormShift in here would shift them into the
  //     pre-normalization coordinate the emitter never compares against,
  //     sending every arm to the wrong target — e.g. an ARM32 inline
  //     PC-relative word table for `switch(st)`, st in [1,5], with index
  //     st-1 in [0,4]: the emitter dispatches on st-1 but labels {1..5} would
  //     then match nothing (a masked index already zeroes NormBase in
  //     traceIndexTransform, so only the non-masked `sub` form was affected).
  //
  // EntryIndices (a bounded sparse table's kept slot indices) is always the
  // real table position and so is coordinate-correct for either path.
  bool HasGap = !Info.EntryIndices.empty();
  bool DispatchesPreNormIndex =
      (Info.TargetBase != 0 && Info.EntrySize <= 2) || Info.PreScaledIndex;
  int64_t NormBase = DispatchesPreNormIndex ? Info.NormBase : 0;
  uint32_t NormShift = DispatchesPreNormIndex ? Info.NormShift : 0;

  if (!HasGap && NormBase == 0 && NormShift == 0 && Info.Stride <= 1)
    return;

  JT.CaseLabels.reserve(JT.Targets.size());
  for (size_t I = 0; I < JT.Targets.size(); ++I) {
    int64_t Label = (HasGap && I < Info.EntryIndices.size())
                        ? static_cast<int64_t>(Info.EntryIndices[I])
                        : static_cast<int64_t>(I);
    if (Info.Stride > 1)
      Label *= static_cast<int64_t>(Info.Stride);
    if (NormShift > 0)
      Label <<= NormShift;
    Label += NormBase;
    JT.CaseLabels.push_back(Label);
  }

  LLVM_DEBUG({
    llvm::dbgs() << "  case-labels: recovered " << JT.CaseLabels.size()
                 << " labels (base=" << NormBase << ", shift=" << NormShift
                 << ", stride=" << Info.Stride << ")\n";
  });
}

//===----------------------------------------------------------------------===//
// resolveJumpTable — top-level multi-strategy resolution
//===----------------------------------------------------------------------===//

std::vector<va_t> CFGBuilder::resolveJumpTable(const BinaryImage &Img,
                                               const InsnRecord &Rec) {
  JumpTableInfo Info;

  // Strategies are tried most-specific first, falling back to the generic
  // pattern matchers.  Strategies 1 and 1b are the architecture-gated
  // ARM-family detectors (defined in JumpTableResolverARM.cpp); every strategy
  // from 1c onward is architecture-neutral pattern matching that covers x86,
  // x64, ARM32, and AArch64 alike — which is why there is no x86-specific
  // detector to dispatch to here.

  // Strategy 1: ARM TBB/TBH table-branch (most specific, check first).
  bool Recovered = tryARMTableBranch(Img, Rec, Info);

  // Strategy 1b: AArch64 compact byte/halfword table (separate entry base and
  // code anchor, scaled entries) — must precede the generic relative resolver,
  // which would otherwise latch onto the entry base as the (wrong) target base.
  if (!Recovered)
    Recovered = tryAArch64CompactTable(Img, Rec, Info);

  // Strategy 1c: runtime-selected table base (`base = cond ? A : B; jmp
  // *base[idx]`) — two adjacent code-pointer tables merged into one.  Must
  // precede the generic relative/cross-instruction resolvers, which would fold
  // only one arm of the select and recover half the table.
  if (!Recovered)
    Recovered = tryTwoTableSelect(Img, Rec, Info);

  // Strategy 1d: two-level index-byte table (`jmptab[idxtab[switchvar]]`, the
  // classic MSVC sparse-switch lowering).  Must precede the generic
  // relative/cross-instruction resolvers, which would otherwise recover only
  // the inner address table (jmptab) and dispatch on the intermediate table
  // index instead of the real switch variable — collapsing the case set and
  // losing the true labels.  It composes the per-case targets into
  // ExplicitTargets, so it short-circuits the single-base machinery below.
  if (!Recovered)
    Recovered = tryTwoLevelIndexTable(Img, Rec, Info);

  // Strategy 2: PIC-relative table (architecture-neutral; common on x64).
  if (!Recovered)
    Recovered = tryRelativeTable(Img, Rec, Info);

  // Strategy 2b: PIC-relative table whose base register is materialised in
  // a preceding instruction (x86 `lea table(%rip),%reg` / ARM32 ADR).  The
  // per-record slice above cannot see the base, so fold it across
  // instructions by emulating the dominating prefix.
  if (!Recovered)
    Recovered = tryCrossInstrRelativeTable(Img, Rec, Info);

  // Strategy 2c: constant-base absolute table whose load is decoupled from the
  // branch by an -O0 spill/reload relay (`... mov tab(,idx,W),%r; mov %r,[slot];
  // ... mov [slot],%r; jmp *%r`), including a shared multi-site computed-goto
  // dispatch where several goto-site predecessors feed one common table.  The
  // cross-instruction resolver above only reaches a load in the branch's own
  // block or a single-predecessor path, so a many-predecessor shared dispatch
  // reaches none; this recovers the table from the code-pointer relocation run
  // at its constant base regardless of how many goto sites share it.
  if (!Recovered)
    Recovered = tryConstBaseAbsoluteTable(Img, Rec, Info);

  // Strategy 3: Backward slicing for absolute tables.
  if (!Recovered && !sliceBackForTableBase(Rec, Info))
    return {};

  // A runtime-selected dispatch over two non-adjacent code-pointer tables
  // carries its complete target set explicitly (the union of both runs), which
  // no single-base contiguous read can reconstruct.  Use it verbatim: the
  // guard / normalization / stride / emulation machinery below all assume one
  // contiguous base and would corrupt the two-run layout.  The set is already
  // exact and validated (every entry a resolved in-function code pointer), so
  // the dispatch lowers directly to the merged two-table switch.
  if (!Info.ExplicitTargets.empty()) {
    std::vector<va_t> Targets = Info.ExplicitTargets;
    // Every entry was validated during the run read; a sanity-check truncation
    // would desync the concatenated positional labels, so require it to keep
    // the full set rather than emit a mis-aligned switch.
    if (!sanityCheckTargets(Img, Targets) ||
        Targets.size() != Info.ExplicitTargets.size() ||
        Targets.size() < limits::kMinJumpTableEntries)
      return {};
    ResolvedTableInfo[Rec.Addr] = Info;
    LLVM_DEBUG(llvm::dbgs()
               << "Jump table @ 0x" << llvm::utohexstr(Rec.Addr) << ": "
               << Targets.size() << " entries ("
               << (Info.TwoLevelIndex ? "two-level index-byte"
                                      : "runtime-selected two-table")
               << ", base=0x" << llvm::utohexstr(Info.BaseAddr) << ")\n");
    return Targets;
  }

  // Detect normalization (INT_SUB base, right-shift) so we can
  // pull back guard bounds and recover case labels later.  A reloc-absolute
  // computed-goto table is indexed directly by `tab[idx]` with idx in [0,N), so
  // the case values are the raw indices 0..N-1 — there is no case-label
  // normalization to invert.  Crucially, the shift in an index expression like
  // `(acc >> k) & 3` is part of *computing* the index, not a table
  // normalization, so running the detectors here would mis-read it as NormShift
  // and emit bogus `i << k` case values that no longer match the runtime index.
  if (!Info.RelocAbsolute) {
    detectNormalization(Rec, Info);

    // Detect stride from AND masks on the switch variable.  When
    // the index has known-zero low bits the effective table size is
    // guard_bound / stride.
    detectStride(Rec, Info);
  }

  // Refine entry count: first try CircleRange-based guard analysis for
  // precise modular-arithmetic bounds, then fall back to integer bounds.
  // A reloc-absolute computed-goto table already has its exact entry count from
  // the relocation run and carries no comparison guard, so the guard search is
  // skipped — it could only mis-bound it.
  bool GuardFound = false;
  if (!Info.RelocAbsolute) {
    GuardFound = refineRangeFromGuards(Rec, Info);
    if (!GuardFound)
      GuardFound = inferBoundsFromGuard(Rec, Info) ||
                   inferBoundsFromCFGGuards(Rec, Info) ||
                   inferBoundsFromUnrolledGuard(Rec, Info);
    // Last resort: a guard written against a multi-step normalization of the
    // index (`t = (idx+k) & m; cmp t,N`) that the direct comparison matchers
    // cannot copy-chain back to the index register.  Propagate the guard's
    // value range backward through the count-preserving reshapes onto the
    // index; the size that lands there is the entry count.
    if (!GuardFound)
      GuardFound = inferBoundsFromRangePullback(Rec, Info);
    // Final guard strategy: the guard constrains a *separate reload* of the same
    // spilled switch variable that feeds the index (the -O0 shape where `cmp`
    // and the table index each reload the value from the same stack slot, with
    // no copy chain linking their registers).  Match by exact location
    // equivalence rather than register identity.
    if (!GuardFound)
      GuardFound = inferBoundsFromLoadAliasGuard(Rec, Info);
  }

  // A PIC relative table with no comparison guard at all is a `switch(x % N)`
  // table: the modulus bounds the index, so there is no `cmp` to find.  When
  // the guard search came up empty, trust the relocation run starting at the
  // table base — every entry carries a PC-relative code relocation, so the run
  // length is the exact entry count.  This also discards any spurious
  // normalization the magic-number modulo sequence triggered (its `sub` looks
  // like a NormBase), since a modulo index is the raw value in [0, N).  A
  // genuinely guarded table (signed/normalized switch) keeps its guard-derived
  // bound untouched.
  if (!GuardFound && !Info.RelocAbsolute && Info.IsRelative &&
      Info.BaseAddr != 0 && Info.EntrySize > 0) {
    uint32_t RelRun = countRelCodeRelocRun(Img, Info.BaseAddr, Info.EntrySize);
    // A second unguarded PIC table placed immediately after this one continues
    // the same RelCodeReloc run, so the raw count over-reads into it; cap the
    // run at the next table's base anchor (its exact end).
    RelRun =
        boundRelRunByNextAnchor(Img, Info.BaseAddr, Info.EntrySize, RelRun);
    if (RelRun >= limits::kMinJumpTableEntries) {
      Info.MaxEntries = RelRun;
      Info.RelocBounded = true;
      Info.NormBase = 0;
      Info.NormShift = 0;
      Info.Stride = 1;
    }
  }

  // A run of absolute code-pointer relocations at the table base proves the
  // entries are absolute code pointers, overriding the backward slice's
  // width-based relative guess.  The slice marks any sub-pointer-width load
  // relative, so an i386 4-byte absolute table (`jmpl *tab(,idx,4)` with
  // R_386_32 entries) would otherwise be decoded as PC-relative offsets and
  // dropped.  This classification is independent of how the table is bounded
  // (a `switch(x & mask)` still has an `and`-derived guard), so it must run
  // regardless of the guard search — decode correctness and entry count are
  // separate concerns.
  bool AbsCodePtrRun =
      !Info.RelocAbsolute && !Info.TwoTableSelect && Info.TargetBase == 0 &&
      Info.BaseAddr != 0 && Info.EntrySize > 0 &&
      countCodePtrRelocRun(Img, Info.BaseAddr, Info.EntrySize) >=
          limits::kMinJumpTableEntries;
  if (AbsCodePtrRun && Info.IsRelative) {
    Info.IsRelative = false;
    Info.IsSigned = false;
  }

  // An *absolute* computed jump table bounded only by a mask carries no `cmp`
  // range guard — the mask alone confines the index — so the comparison-guard
  // search above found nothing.  Its code-pointer relocation run gives the
  // exact physical entry count, the absolute analogue of the PC-relative
  // RelCodeReloc run above.  This is what turns an unguarded `switch(x & mask)`
  // lowered non-PIC back into a full switch instead of dropping every target.
  // The run length already reflects the real index range (raw masked value,
  // filler slots included), so it supersedes any stride the mask implied — a
  // later `/ Stride` division would wrongly shrink it.
  if (!GuardFound && Info.MaxEntries == 0 && !Info.RelocBounded &&
      AbsCodePtrRun) {
    Info.MaxEntries = countCodePtrRelocRun(Img, Info.BaseAddr, Info.EntrySize);
    Info.RelocBounded = true;
    Info.NormBase = 0;
    Info.NormShift = 0;
    Info.Stride = 1;
    LLVM_DEBUG(llvm::dbgs()
               << "  abs-reloc-run: bounded absolute table to "
               << Info.MaxEntries
               << " entries from code-pointer relocation run\n");
  }

  // A `switch(x % N)` table whose entries carry no relocations (AArch64 compact
  // byte/halfword tables, ARM32 inline `.text` word tables) cannot use the
  // relocation run above and has no `cmp` range guard.  Read the modulus N out
  // of the magic-division remainder that computes the index, which bounds the
  // table exactly and keeps the single-target readonly fallback below (which
  // only fires at MaxEntries == 0) from collapsing it to one entry.
  if (!GuardFound && Info.MaxEntries == 0 && !Info.RelocAbsolute &&
      Info.IsRelative && Info.BaseAddr != 0 && Info.EntrySize > 0)
    inferBoundsFromModulo(Img, Rec, Info);

  // COND_BR-polarity: `cmp idx,N; ja default` (strict above) makes the table
  // cover [0,N] = N+1 entries, but the CF flag `idx < N` reports only N.  When
  // the guard also consumes the ZF equality `idx == N` it is the ja/jbe family,
  // so recover the inclusive last entry the range analysis dropped.
  if (Info.MaxEntries > 0 && Info.IndexReg != InvalidVA && Info.NormBase == 0 &&
      Info.NormShift == 0 && Info.Stride <= 1 &&
      Info.MaxEntries < limits::kMaxJumpTableEntries &&
      guardUsesInclusiveCompare(Rec, Info.IndexReg, Info.MaxEntries))
    Info.MaxEntries += 1;

  // If a normalization offset is present and the guard bound looks
  // like it was applied to the original (pre-normalization) variable,
  // adjust it down to reflect the actual table size.
  if (Info.MaxEntries > 0 && Info.NormBase > 0) {
    uint32_t Adj = pullBackBound(Info.MaxEntries, Info);
    if (Adj != Info.MaxEntries && Adj >= limits::kMinJumpTableEntries) {
      LLVM_DEBUG(llvm::dbgs()
                 << "  pullback: adjusted bound " << Info.MaxEntries << " -> "
                 << Adj << " (normBase=" << Info.NormBase << ")\n");
      Info.MaxEntries = Adj;
    }
  }

  // Apply stride: when the switch variable has alignment S, a guard
  // bound of N means at most N/S distinct table entries.  A relocation-bounded
  // table already holds the exact entry count (not a raw-variable guard bound),
  // and a pre-scaled computed goto encodes its byte stride here purely for case
  // labels, so the division must not shrink it.
  if (Info.MaxEntries > 0 && Info.Stride > 1 && !Info.RelocBounded) {
    uint32_t Adj = Info.MaxEntries / Info.Stride;
    if (Adj >= limits::kMinJumpTableEntries) {
      LLVM_DEBUG(llvm::dbgs()
                 << "  stride: adjusted bound " << Info.MaxEntries << " -> "
                 << Adj << " (stride=" << Info.Stride << ")\n");
      Info.MaxEntries = Adj;
    }
  }

  // A power-of-two-modulo / masked index (`and $(2^k-1)`, with an optional
  // following `dec` from a peeled iteration) is hard-bounded by the mask, for
  // every table kind (PIC-relative, GOTOFF, absolute).  Two such tables placed
  // back-to-back in rodata form one continuous relocation run / pointer run, so
  // an over-long read runs past the first table into the second — fabricating
  // bogus successor edges (and, with x87 residents, an unbalanced stack the TOP
  // recovery cannot reconcile).  The mask is a hard upper bound on the index,
  // so clamp to it even when a range guard was found: a guard derived from the
  // pre-`dec` mask (`and $7; dec` => index in [-1,6], 7 entries) over-counts by
  // the offset, and min(guard, mask) is always the safe table size.
  // A two-table merge holds 2N entries while the per-table index mask bounds
  // the index to N; the runtime base select supplies the doubling, so the mask
  // must not clamp the merged count.
  if (uint32_t MaskBound = inferBoundsFromMask(Rec, Info);
      !Info.TwoTableSelect && MaskBound > 0 &&
      (Info.MaxEntries == 0 || MaskBound < Info.MaxEntries))
    Info.MaxEntries = MaskBound;

  // Last-resort bound for a `switch(x & M)` table with a *non-contiguous* mask
  // (e.g. `x & 0x1e`) that no other strategy bounded — the shape a fully-linked
  // binary produces, where the absolute entries carry no relocation run to
  // count.  The masked value indexes the table directly, so the table is dense
  // over the raw index (0..coveringmask) with default filler in the unused
  // slots; the covering-mask count is that physical entry count.  Because the
  // raw masked value is the slot index, its trailing-zero "stride" is an
  // artifact of the gaps, not a divisor — force stride 1 so the later `/Stride`
  // shrink does not drop the filler slots and desync the dispatch.  Opt-in
  // (AllowNonContiguous) and gated on no prior bound so it never loosens a
  // table another strategy already sized.
  if (Info.MaxEntries == 0 && !Info.TwoTableSelect && Info.NormShift == 0 &&
      Info.NormBase == 0) {
    if (uint32_t NCMask = inferBoundsFromMask(Rec, Info,
                                              /*AllowNonContiguous=*/true)) {
      Info.MaxEntries = NCMask;
      Info.Stride = 1;
      Info.RelocBounded = true;
      LLVM_DEBUG(llvm::dbgs()
                 << "  covering-mask: bounded table to " << NCMask
                 << " entries from non-contiguous index mask\n");
    }
  }

  if (Info.MaxEntries == 0 || Info.MaxEntries > limits::kMaxJumpTableEntries)
    Info.MaxEntries = 0;

  // Readonly single-value optimisation: when the switch variable is
  // loaded from a readonly segment and no guard constrains it, try
  // reading the value directly from the image to produce a single
  // definite target.  This handles semi-dynamic dispatch vectors
  // whose initial value is baked into the binary.
  //
  // A dispatch vector baked into the binary lives in read-only DATA
  // (.rodata / .data.rel.ro); an *executable* base is instead an inline
  // PC-relative code table (the ARM32 `add rB,pc,#k; ldr rE,[rB,idx,4];
  // add pc,rB,rE` form embedded in .text), which is always a multi-entry
  // table.  Clamping such a table to a single entry here would flip the
  // unbounded "scan until an entry stops decoding to a valid in-function
  // target" read below into a bounded 1-entry read, dropping every other
  // arm and degrading the dispatch to a broken indirect tail call — so the
  // single-value optimisation is restricted to non-executable segments.
  if (Info.MaxEntries == 0 && Info.BaseAddr != 0) {
    const auto *BaseSeg = Img.getSegmentFor(Info.BaseAddr);
    if (BaseSeg && !BaseSeg->isWritable() && !BaseSeg->isExecutable() &&
        !BaseSeg->Data.empty()) {
      size_t Off = static_cast<size_t>(Info.BaseAddr - BaseSeg->VA);
      if (rangeInBounds(Off, Info.EntrySize, BaseSeg->Data.size())) {
        va_t SingleTarget = decodeTableEntry(
            BaseSeg->Data.data() + Off, Info.EntrySize, Info.IsRelative,
            Info.IsSigned, Info.BaseAddr, Info.TargetBase, Info.EntryScale);
        if (isValidTarget(Img, SingleTarget, CurrentFuncEntry)) {
          Info.MaxEntries = 1;
          LLVM_DEBUG(llvm::dbgs() << "  readonly: single target 0x"
                                  << llvm::utohexstr(SingleTarget)
                                  << " from readonly segment\n");
        }
      }
    }
  }

  std::vector<uint32_t> KeptIdx;
  auto Targets = readTableEntries(Img, Info, &KeptIdx);

  // Post-read sanity check with truncation.
  sanityCheckTargets(Img, Targets);
  if (KeptIdx.size() > Targets.size())
    KeptIdx.resize(Targets.size()); // sanity-check truncates trailing entries

  // Dual-path recovery: when the primary guard analysis fails and we
  // have a base address, check for a default-value path pattern where
  // a COND_BR sends one path to a constant (default) and another to the
  // switch computation (default-value path with an explicit COND_BR split).
  if (Targets.size() < limits::kMinJumpTableEntries && Info.MaxEntries == 0 &&
      Info.BaseAddr != 0) {
    if (tryDualPathRecovery(Rec, Info)) {
      auto AltTargets = readTableEntries(Img, Info);
      sanityCheckTargets(Img, AltTargets);
      if (AltTargets.size() > Targets.size()) {
        Targets = std::move(AltTargets);
        KeptIdx.clear(); // dense fallback: positional labels apply
        LLVM_DEBUG(llvm::dbgs() << "  dual-path: recovered " << Targets.size()
                                << " entries via default-value path\n");
      }
    }
  }

  // Multi-stage fallback: if we got a base but the initial read produced
  // too few entries (no guard bound found), retry with relaxed parameters.
  if (Targets.size() < limits::kMinJumpTableEntries && Info.MaxEntries == 0 &&
      Info.BaseAddr != 0) {
    for (uint16_t AltSize : {uint16_t(4), uint16_t(8), uint16_t(2)}) {
      if (AltSize == Info.EntrySize)
        continue;
      JumpTableInfo Alt = Info;
      Alt.EntrySize = AltSize;
      Alt.IsRelative = (AltSize < 8);
      Alt.IsSigned = (AltSize < 8);
      auto AltTargets = readTableEntries(Img, Alt);
      sanityCheckTargets(Img, AltTargets);
      if (AltTargets.size() > Targets.size()) {
        Targets = std::move(AltTargets);
        Info = Alt;
        KeptIdx.clear(); // dense fallback: positional labels apply
        LLVM_DEBUG(llvm::dbgs()
                   << "  fallback: retried with entrySize=" << AltSize
                   << ", got " << Targets.size() << " entries\n");
      }
    }
  }

  // Emulation-based fallback: when all static strategies fail, try
  // running the ops through the NdOp emulator for each candidate index.
  if (Targets.size() < limits::kMinJumpTableEntries && Info.MaxEntries > 0 &&
      CurrentImg) {
    auto EmuTargets = tryEmulatedResolution(Img, Rec, Info);
    if (EmuTargets.size() > Targets.size()) {
      Targets = std::move(EmuTargets);
      KeptIdx.clear(); // dense fallback: positional labels apply
      LLVM_DEBUG(llvm::dbgs() << "  emulated: recovered " << Targets.size()
                              << " entries via NdOp emulation\n");
    }
  }

  // Emulation cross-check for a bounded table that decoded *fewer* targets than
  // its known entry count.  The static reader classifies the entry layout
  // (relative/absolute, sign, scale, target-base) before decoding, and a
  // misclassification can truncate an otherwise-valid table part-way — leaving
  // a plausible-but-incomplete target list that the `< kMin` fallback above
  // (which fires only on near-total failure) never revisits.  Re-run the
  // *actual* dispatch arithmetic through the emulator, which reads the real
  // base+index+load and so cannot mis-guess the layout, and adopt its result
  // only when it is strictly more complete AND reproduces the static decode on
  // every shared index.  That prefix-agreement gate makes this monotonic: it
  // can only append cases the static read dropped, never rewrite one it already
  // decoded, so a correctly recovered table is left untouched.  Restricted to a
  // dense static result (no sparse skips) so the two index coordinates align,
  // and bounded by the same MaxEntries so it can never over-read past the
  // guard/reloc bound.
  if (CurrentImg && Info.MaxEntries > 0 &&
      Targets.size() >= limits::kMinJumpTableEntries &&
      Targets.size() < Info.MaxEntries) {
    bool DenseStatic = KeptIdx.size() == Targets.size();
    for (size_t I = 0; DenseStatic && I < KeptIdx.size(); ++I)
      DenseStatic = KeptIdx[I] == I;
    if (DenseStatic) {
      auto EmuTargets = tryEmulatedResolution(Img, Rec, Info);
      bool ExtendsStatic = EmuTargets.size() > Targets.size();
      for (size_t I = 0; ExtendsStatic && I < Targets.size(); ++I)
        ExtendsStatic = EmuTargets[I] == Targets[I];
      if (ExtendsStatic) {
        LLVM_DEBUG(llvm::dbgs()
                   << "  emu-verify: extended bounded table from "
                   << Targets.size() << " to " << EmuTargets.size()
                   << " entries via dispatch emulation\n");
        Targets = std::move(EmuTargets);
        KeptIdx.clear(); // dense positional labels apply
      }
    }
  }

  // Emulation-based recovery for an unbounded table whose static *entry layout*
  // the decoder misclassified.  The recurring resolver failure is not a missing
  // base but a mis-guessed entry format (relative vs absolute, sign, scale,
  // target-base) that makes readTableEntries decode garbage and drop the
  // dispatch to a degenerate indirect tail call.  When no bound was found, the
  // table base lies in read-only memory, and too few targets were read, emulate
  // the *actual* dispatch arithmetic along the branch path rather than
  // re-deriving the layout: emulation runs the real base+index+load, so a
  // mis-guessed format cannot lose the table.  This is as sound as
  // readTableEntries' own unbounded read — same read-only gate, stop-on-invalid
  // and duplicate-run break, plus a distinct-target requirement — and is
  // monotonic: it only fires where the branch would otherwise degrade to a tail
  // call, so it can never shrink an already-recovered table.
  if (Targets.size() < limits::kMinJumpTableEntries && Info.MaxEntries == 0 &&
      Info.BaseAddr != 0 && CurrentImg) {
    const auto *BaseSeg = Img.getSegmentFor(Info.BaseAddr);
    if (BaseSeg && !BaseSeg->isWritable() && !BaseSeg->Data.empty()) {
      auto EmuTargets =
          tryEmulatedResolution(Img, Rec, Info, /*SelfBounding=*/true);
      if (EmuTargets.size() > Targets.size()) {
        Targets = std::move(EmuTargets);
        KeptIdx.clear(); // dense fallback: positional labels apply
        LLVM_DEBUG(llvm::dbgs()
                   << "  emulated-unbounded: recovered " << Targets.size()
                   << " entries via read-only table emulation\n");
      }
    }
  }

  // Ground-truth cross-check for a plain relative/absolute table: rebuild the
  // targets by executing the *actual* dispatch arithmetic per index instead of
  // trusting the statically classified entry layout (relative-vs-absolute,
  // signedness).  A misclassified layout decodes a full-length but *wrong*
  // target set that still passes the sanity check (every entry lands in the
  // function), a silent miscompile the extend/fallback strategies above never
  // revisit because the count already looks complete.  The emulation reads the
  // same table bytes and applies the same transform the processor would, so
  // when it is fully grounded — every index read the recovered table slot
  // (BaseAddr + i*EntrySize) and produced a valid target — its result is
  // authoritative and supersedes a disagreeing static decode.
  //
  // Guarded to be a no-op wherever the static decode is already trustworthy, so
  // currently-recovered tables keep byte-identical targets: skipped for
  // reloc-bounded / two-table / compact (TargetBase) / pre-scaled tables (whose
  // layout is confirmed by relocations or a dedicated detector), for sparse
  // (gapped) decodes whose positional index would not line up with the emulated
  // slot, and adopted only when the emulation agrees on entry count yet differs
  // on some value.
  bool DenseStatic = KeptIdx.empty() || KeptIdx.size() == Targets.size();
  for (size_t I = 0; DenseStatic && I < KeptIdx.size(); ++I)
    DenseStatic = KeptIdx[I] == I;
  if (CurrentImg && DenseStatic && !Targets.empty() &&
      Info.IndexReg != InvalidVA && Info.BaseAddr != 0 && Info.TargetBase == 0 &&
      Info.EntryScale == 1 && !Info.PreScaledIndex && !Info.TwoTableSelect &&
      !Info.RelocAbsolute && !Info.RelocBounded &&
      (Info.EntrySize == 1 || Info.EntrySize == 2 || Info.EntrySize == 4 ||
       Info.EntrySize == 8)) {
    bool Grounded = false;
    auto EmuTargets = emulateGroundedTargets(
        Img, Rec, Info, static_cast<uint32_t>(Targets.size()), Grounded);
    if (Grounded && EmuTargets.size() == Targets.size() &&
        EmuTargets != Targets) {
      std::vector<va_t> Check = EmuTargets;
      if (sanityCheckTargets(Img, Check) && Check.size() == EmuTargets.size()) {
        LLVM_DEBUG(llvm::dbgs()
                   << "  emu-ground: corrected " << Targets.size()
                   << " statically-misclassified targets via grounded dispatch "
                      "emulation\n");
        Targets = std::move(EmuTargets);
        KeptIdx.clear(); // dense positional labels apply
      }
    }
  }

  if (Targets.size() < limits::kMinJumpTableEntries)
    return {};

  // Carry the kept slot indices so recoverCaseLabels assigns case values by the
  // real table index (a bounded sparse table skips don't-care slots).  Only
  // useful when the kept indices are *not* the trivial 0..N-1 (a gap exists);
  // an empty vector leaves the positional labelling unchanged.
  {
    bool HasGap = KeptIdx.size() != Targets.size();
    for (size_t I = 0; !HasGap && I < KeptIdx.size(); ++I)
      HasGap = KeptIdx[I] != I;
    Info.EntryIndices = HasGap ? std::move(KeptIdx) : std::vector<uint32_t>{};
  }
  ResolvedTableInfo[Rec.Addr] = Info;

  LLVM_DEBUG({
    llvm::dbgs() << "Jump table @ 0x" << llvm::utohexstr(Rec.Addr) << ": "
                 << Targets.size() << " entries, base=0x"
                 << llvm::utohexstr(Info.BaseAddr)
                 << ", entrySize=" << Info.EntrySize
                 << (Info.IsRelative ? " (relative" : " (absolute")
                 << (Info.IsSigned ? ", signed)" : ")") << "\n";
  });

  return Targets;
}

//===----------------------------------------------------------------------===//
// tryDualPathRecovery — default-value path detection
//===----------------------------------------------------------------------===//

/// When the standard guard analysis fails to produce a bound, check for
/// a dual-path pattern: the block containing the INDIR_BR has two
/// predecessor paths, one carrying a default constant and one carrying
/// the real switch computation.  A COND_BR at the split point acts as
/// the guard for switches with an explicit default path.
bool CFGBuilder::tryDualPathRecovery(const InsnRecord &Rec,
                                     JumpTableInfo &Info) {
  auto BlockIt = BlockStarts.upper_bound(Rec.Addr);
  if (BlockIt == BlockStarts.begin())
    return false;
  --BlockIt;
  va_t BranchBlockStart = *BlockIt;

  // Collect all predecessor blocks that branch into our switch block.
  std::set<va_t> Visited;
  Visited.insert(BranchBlockStart);
  std::vector<va_t> Preds;
  collectPredBlocks(BranchBlockStart, Visited, Preds);

  if (Preds.size() < 2 || Preds.size() > limits::kMaxDualPathPreds)
    return false;

  // Look for the pattern: one predecessor ends with a COND_BR that
  // gates a constant-producing path vs. a computation path.
  // The COND_BR predecessor that has a bound comparison is our guard.
  uint32_t BestBound = 0;
  for (va_t PredStart : Preds) {
    auto NextBlock = BlockStarts.upper_bound(PredStart);
    va_t PredEnd = (NextBlock != BlockStarts.end()) ? *NextBlock : InvalidVA;

    for (auto It = Insns.lower_bound(PredStart); It != Insns.end(); ++It) {
      if (It->first >= PredEnd)
        break;
      auto &IRec = It->second;
      if (!IRec.IsBranch || !IRec.IsCond)
        continue;

      // This pred has a COND_BR — scan its ops for a guard bound.
      uint32_t Bound = findBestBound(IRec.Ops, 0, Info.IndexReg);
      if (Bound > 0 && (BestBound == 0 || Bound < BestBound))
        BestBound = Bound;

      // Also scan the ops preceding the COND_BR in this block.
      for (auto InnerIt = Insns.lower_bound(PredStart); InnerIt != It;
           ++InnerIt) {
        Bound = findBestBound(InnerIt->second.Ops, BestBound, Info.IndexReg);
        if (Bound > 0 && (BestBound == 0 || Bound < BestBound))
          BestBound = Bound;
      }
    }
  }

  if (BestBound == 0)
    return false;

  Info.MaxEntries = BestBound;
  LLVM_DEBUG(llvm::dbgs() << "  dual-path: found guard bound " << BestBound
                          << " from " << Preds.size() << " predecessors\n");
  return true;
}

//===----------------------------------------------------------------------===//
// inferBoundsFromUnrolledGuard — detect duplicated guard across preds
//===----------------------------------------------------------------------===//

/// When multiple predecessor blocks each terminate with a COND_BR, and
/// each carries a guard comparison on the switch variable, the guard
/// has been "unrolled" (duplicated).  This detects that pattern and
/// extracts the tightest common bound.
bool CFGBuilder::inferBoundsFromUnrolledGuard(const InsnRecord &Rec,
                                              JumpTableInfo &Info) {
  auto BlockIt = BlockStarts.upper_bound(Rec.Addr);
  if (BlockIt == BlockStarts.begin())
    return false;
  --BlockIt;
  va_t BranchBlockStart = *BlockIt;

  std::set<va_t> Visited;
  Visited.insert(BranchBlockStart);
  std::vector<va_t> Preds;
  collectPredBlocks(BranchBlockStart, Visited, Preds);

  if (Preds.size() < 2 ||
      static_cast<int>(Preds.size()) > limits::kMaxUnrolledGuardPreds)
    return false;

  // Every predecessor must end with a COND_BR for this to be an
  // unrolled guard pattern.
  uint32_t CommonBound = 0;
  int CBranchCount = 0;

  for (va_t PredStart : Preds) {
    auto NextBlock = BlockStarts.upper_bound(PredStart);
    va_t PredEnd = (NextBlock != BlockStarts.end()) ? *NextBlock : InvalidVA;

    bool FoundCBranch = false;
    for (auto It = Insns.lower_bound(PredStart); It != Insns.end(); ++It) {
      if (It->first >= PredEnd)
        break;
      auto &IRec = It->second;
      if (!IRec.IsBranch || !IRec.IsCond)
        continue;
      FoundCBranch = true;
      ++CBranchCount;

      uint32_t PredBound = findBestBound(IRec.Ops, 0, Info.IndexReg);
      if (PredBound == 0) {
        for (auto InnerIt = Insns.lower_bound(PredStart); InnerIt != It;
             ++InnerIt)
          PredBound =
              findBestBound(InnerIt->second.Ops, PredBound, Info.IndexReg);
      }

      if (PredBound > 0) {
        if (CommonBound == 0 || PredBound < CommonBound)
          CommonBound = PredBound;
      }
    }
    if (!FoundCBranch)
      return false;
  }

  if (CBranchCount < 2 || CommonBound == 0)
    return false;

  if (Info.MaxEntries == 0 || CommonBound < Info.MaxEntries) {
    Info.MaxEntries = CommonBound;
    LLVM_DEBUG(llvm::dbgs()
               << "  unrolled-guard: found common bound " << CommonBound
               << " across " << Preds.size() << " predecessor COND_BRs\n");
    return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// collectPathOps — gather ops along the predecessor path to INDIR_BR
//===----------------------------------------------------------------------===//

/// Walk backward through CFG predecessor blocks and collect all ops
/// along the dominant path leading to the INDIR_BR block.  The
/// resulting op sequence can be fed to the NdOp emulator for
/// cross-block switch-target computation via emulation-based jump
/// table resolution.
std::vector<LowOp> CFGBuilder::collectPathOps(va_t BranchBlockStart,
                                              va_t BranchInsnAddr) const {
  std::vector<LowOp> Result;

  // Collect ops within the INDIR_BR block first (up to and including
  // the INDIR_BR instruction).
  auto NextBlock = BlockStarts.upper_bound(BranchBlockStart);
  va_t BlkEnd = (NextBlock != BlockStarts.end()) ? *NextBlock : InvalidVA;

  for (auto It = Insns.lower_bound(BranchBlockStart); It != Insns.end(); ++It) {
    if (It->first > BranchInsnAddr || It->first >= BlkEnd)
      break;
    for (auto &Op : It->second.Ops)
      Result.push_back(Op);
  }

  // Walk backward through single-predecessor blocks, collecting their ops.
  std::set<va_t> Visited;
  Visited.insert(BranchBlockStart);
  va_t CurBlockStart = BranchBlockStart;
  int Depth = 0;

  while (Depth < limits::kMaxPathEmulationDepth &&
         static_cast<int>(Result.size()) < limits::kMaxPathEmulationOps) {

    std::vector<va_t> Preds;
    std::set<va_t> PredVisited = Visited;
    collectPredBlocks(CurBlockStart, PredVisited, Preds);

    // Only follow single-predecessor paths to avoid ambiguity.
    if (Preds.size() != 1)
      break;

    va_t PredStart = Preds[0];
    if (!Visited.insert(PredStart).second)
      break;

    auto PredNext = BlockStarts.upper_bound(PredStart);
    va_t PredEnd = (PredNext != BlockStarts.end()) ? *PredNext : InvalidVA;

    std::vector<LowOp> PredOps;
    for (auto It = Insns.lower_bound(PredStart); It != Insns.end(); ++It) {
      if (It->first >= PredEnd)
        break;
      for (auto &Op : It->second.Ops) {
        if (Op.Opcode == NdOp::COND_BR || Op.Opcode == NdOp::BRANCH)
          continue;
        PredOps.push_back(Op);
      }
    }

    // Prepend predecessor ops before current ops.
    PredOps.insert(PredOps.end(), Result.begin(), Result.end());
    Result = std::move(PredOps);

    CurBlockStart = PredStart;
    ++Depth;
  }

  LLVM_DEBUG(llvm::dbgs() << "  path-ops: collected " << Result.size()
                          << " ops across " << (Depth + 1) << " blocks\n");
  return Result;
}

//===----------------------------------------------------------------------===//
// findCommonSwitchVar — common-pred register identification
//===----------------------------------------------------------------------===//

/// Examine multiple predecessor paths to the INDIR_BR block and
/// identify register definitions common to all paths.  When the switch
/// variable flows through a merge point (multiple predecessors each
/// define it), this pinpoints the true switch register even when
/// quasi-copy tracing from a single path fails.
uint64_t CFGBuilder::findCommonSwitchVar(va_t BranchBlockStart,
                                         uint64_t BranchIndReg) const {
  std::set<va_t> Visited;
  Visited.insert(BranchBlockStart);
  std::vector<va_t> Preds;
  collectPredBlocks(BranchBlockStart, Visited, Preds);

  if (Preds.size() < 2)
    return BranchIndReg;

  // For each predecessor, collect the set of registers it defines.
  std::vector<std::set<uint64_t>> PredDefs;
  for (va_t PredStart : Preds) {
    auto NextBlock = BlockStarts.upper_bound(PredStart);
    va_t PredEnd = (NextBlock != BlockStarts.end()) ? *NextBlock : InvalidVA;

    std::set<uint64_t> Defs;
    for (auto It = Insns.lower_bound(PredStart); It != Insns.end(); ++It) {
      if (It->first >= PredEnd)
        break;
      for (auto &Op : It->second.Ops) {
        if (Op.Output.isReg())
          Defs.insert(Op.Output.Offset);
      }
    }
    PredDefs.push_back(std::move(Defs));
  }

  // Intersect: find registers defined in ALL predecessors.
  std::set<uint64_t> Common = PredDefs[0];
  for (size_t I = 1; I < PredDefs.size(); ++I) {
    std::set<uint64_t> Inter;
    for (uint64_t R : Common)
      if (PredDefs[I].count(R))
        Inter.insert(R);
    Common = std::move(Inter);
  }

  // If BranchIndReg (or a quasi-copy source of it from the INDIR_BR
  // block) is among the common defs, that confirms it as the switch var.
  if (Common.count(BranchIndReg))
    return BranchIndReg;

  // Otherwise, try to match through the branch block's ops: find which
  // common register feeds into BranchIndReg.
  auto NextBlock = BlockStarts.upper_bound(BranchBlockStart);
  va_t BlkEnd = (NextBlock != BlockStarts.end()) ? *NextBlock : InvalidVA;

  for (auto It = Insns.lower_bound(BranchBlockStart); It != Insns.end(); ++It) {
    if (It->first >= BlkEnd)
      break;
    for (auto &Op : It->second.Ops) {
      if (Op.Output.isReg() && Op.Output.Offset == BranchIndReg) {
        for (int K = 0; K < Op.NumInputs; ++K) {
          if (Op.Inputs[K].isReg() && Common.count(Op.Inputs[K].Offset)) {
            LLVM_DEBUG(llvm::dbgs()
                       << "  common-pred: resolved switch var to reg 0x"
                       << llvm::utohexstr(Op.Inputs[K].Offset)
                       << " (common across " << Preds.size()
                       << " predecessors)\n");
            return Op.Inputs[K].Offset;
          }
        }
      }
    }
  }

  return BranchIndReg;
}

//===----------------------------------------------------------------------===//
// tryEmulatedResolution — NdOp emulation fallback
//===----------------------------------------------------------------------===//

std::vector<va_t> CFGBuilder::tryEmulatedResolution(const BinaryImage &Img,
                                                    const InsnRecord &Rec,
                                                    const JumpTableInfo &Info,
                                                    bool SelfBounding) {
  uint64_t IndexReg = InvalidVA;
  for (auto &Op : Rec.Ops) {
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1 &&
        Op.Inputs[0].isReg()) {
      IndexReg = Op.Inputs[0].Offset;
      break;
    }
  }
  if (IndexReg == InvalidVA)
    return {};

  uint64_t SwitchReg =
      quasiCopySource(Rec.Ops, static_cast<int>(Rec.Ops.size()) - 1, IndexReg);

  // When the INDIR_BR block has multiple predecessors, identify the true
  // switch register through path intersection (common-pred defs).
  auto BlockIt = BlockStarts.upper_bound(Rec.Addr);
  if (BlockIt != BlockStarts.begin()) {
    --BlockIt;
    uint64_t CommonReg = findCommonSwitchVar(*BlockIt, SwitchReg);
    if (CommonReg != SwitchReg) {
      SwitchReg = CommonReg;
      LLVM_DEBUG(llvm::dbgs() << "  emu: using common-pred switch reg 0x"
                              << llvm::utohexstr(SwitchReg) << "\n");
    }
  }

  uint32_t Limit = Info.MaxEntries;
  if (Limit == 0 || Limit > limits::kMaxJumpTableEntries)
    Limit = limits::kMaxJumpTableEntries;

  std::vector<va_t> Targets;
  Targets.reserve(std::min(Limit, 64u));

  NdOpEmulator Emu(Img);
  Emu.setCallPreservedRegisters(callPreservedRegs(Img));

  // Emulate one target per candidate index into \p Out.  A self-bounding
  // (unbounded) scan must stop on a long run of identical targets: an
  // index-independent branch (a function-pointer tail call, not a switch)
  // emulates to the same destination for every index and would otherwise fill
  // the whole limit with one bogus target.  A bounded scan keeps every entry,
  // since a real switch legitimately repeats targets (several cases sharing a
  // body) within its known range.
  auto emulateSeries = [&](const std::vector<LowOp> &Ops,
                           std::vector<va_t> &Out) {
    va_t PrevTgt = InvalidVA;
    int DupRun = 0;
    for (uint32_t Idx = 0; Idx < Limit; ++Idx) {
      auto Tgt = Emu.computeTarget(Ops, SwitchReg, Idx);
      if (!Tgt || !isValidTarget(Img, *Tgt, CurrentFuncEntry))
        break;
      if (SelfBounding) {
        if (*Tgt == PrevTgt) {
          if (++DupRun > limits::kMaxDuplicateRun)
            break;
        } else {
          DupRun = 0;
          PrevTgt = *Tgt;
        }
      }
      Out.push_back(*Tgt);
    }
  };

  // Phase 1: Try single-instruction emulation (fast path).
  emulateSeries(Rec.Ops, Targets);

  sanityCheckTargets(Img, Targets);

  // Phase 2: Cross-block emulation when single-instruction emulation
  // fails.  Collect ops from predecessor blocks along the dominant
  // path and re-emulate with the full op sequence.
  if (Targets.size() < limits::kMinJumpTableEntries) {
    auto BIt = BlockStarts.upper_bound(Rec.Addr);
    if (BIt != BlockStarts.begin()) {
      --BIt;
      auto PathOps = collectPathOps(*BIt, Rec.Addr);
      if (PathOps.size() > Rec.Ops.size()) {
        std::vector<va_t> CrossBlockTargets;
        CrossBlockTargets.reserve(std::min(Limit, 64u));

        emulateSeries(PathOps, CrossBlockTargets);

        sanityCheckTargets(Img, CrossBlockTargets);
        if (CrossBlockTargets.size() > Targets.size()) {
          Targets = std::move(CrossBlockTargets);
          LLVM_DEBUG(llvm::dbgs()
                     << "  cross-block emu: recovered " << Targets.size()
                     << " entries via multi-block path\n");
        }
      }
    }
  }

  // A self-bounding scan must recover a genuine multi-way dispatch: require at
  // least kMinJumpTableEntries *distinct* targets so an indirect tail call
  // whose (single, constant) destination happens to validate is never
  // mismodeled as a switch.
  if (SelfBounding) {
    std::set<va_t> Distinct(Targets.begin(), Targets.end());
    if (Distinct.size() < limits::kMinJumpTableEntries)
      return {};
  }

  return Targets;
}

//===----------------------------------------------------------------------===//
// emulateGroundedTargets — execute the real dispatch per index
//===----------------------------------------------------------------------===//

std::vector<va_t>
CFGBuilder::emulateGroundedTargets(const BinaryImage &Img,
                                   const InsnRecord &Rec,
                                   const JumpTableInfo &Info, uint32_t Count,
                                   bool &Grounded) {
  Grounded = false;
  std::vector<va_t> Out;
  if (Count == 0 || Count > limits::kMaxJumpTableEntries || Info.EntrySize == 0)
    return Out;

  // Collect the dispatch path: the INDIR_BR block plus its single-predecessor
  // chain (so a table base materialised in a dominating block is in scope).
  va_t BlkStart = CurrentFuncEntry;
  auto BIt = BlockStarts.upper_bound(Rec.Addr);
  if (BIt != BlockStarts.begin()) {
    --BIt;
    BlkStart = *BIt;
  }
  std::vector<LowOp> Ops = collectPathOps(BlkStart, Rec.Addr);
  if (Ops.empty())
    return Out;

  // The INDIR_BR whose input register/temp holds the computed target.
  NdVar TargetVar;
  bool HaveTarget = false;
  for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I)
    if (Ops[I].Opcode == NdOp::INDIR_BR && Ops[I].NumInputs >= 1 &&
        (Ops[I].Inputs[0].isReg() || Ops[I].Inputs[0].isTemp())) {
      TargetVar = Ops[I].Inputs[0];
      HaveTarget = true;
      break;
    }
  if (!HaveTarget)
    return Out;

  // Locate the table LOAD (last scaled `base + index*EntrySize` access) and the
  // variable feeding its scale — the *post-normalization* table index, i.e. the
  // value that, set to i, makes the load read table slot i.  Injecting there
  // (rather than at the resolver's traced-to-source index register) skips the
  // switch-variable normalization while preserving the base materialisation and
  // the post-load transform, so the emulated target is exactly what the
  // processor computes for slot i.
  NdVar InjVar;
  int LoadPos = -1;
  auto peelCopy = [&](int D) {
    for (int G = 0; D >= 0 && Ops[D].Opcode == NdOp::COPY &&
                    Ops[D].NumInputs >= 1 &&
                    (Ops[D].Inputs[0].isReg() || Ops[D].Inputs[0].isTemp()) &&
                    G < limits::kMaxQuasiCopyDepth;
         ++G)
      D = reachingDefIdx(Ops, D - 1, Ops[D].Inputs[0]);
    return D;
  };
  for (int I = static_cast<int>(Ops.size()) - 1; I >= 0 && LoadPos < 0; --I) {
    if (Ops[I].Opcode != NdOp::LOAD || Ops[I].NumInputs < 1 ||
        Ops[I].Output.Size != Info.EntrySize)
      continue;
    const NdVar &AddrV = (Ops[I].NumInputs >= 2) ? Ops[I].Inputs[1] : Ops[I].Inputs[0];
    int AddIdx = peelCopy(reachingDefIdx(Ops, I - 1, AddrV));
    if (AddIdx < 0 || Ops[AddIdx].Opcode != NdOp::INT_ADD ||
        Ops[AddIdx].NumInputs < 2)
      continue;
    for (int W = 0; W < 2 && LoadPos < 0; ++W) {
      int SD = peelCopy(reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[W]));
      if (SD < 0)
        continue;
      const LowOp &S = Ops[SD];
      uint64_t Scale = 0;
      if (S.Opcode == NdOp::INT_MULT && S.NumInputs >= 2 &&
          S.Inputs[1].isConst())
        Scale = S.Inputs[1].Offset;
      else if (S.Opcode == NdOp::INT_LEFT && S.NumInputs >= 2 &&
               S.Inputs[1].isConst() && S.Inputs[1].Offset < 6)
        Scale = 1ull << S.Inputs[1].Offset;
      else
        continue;
      if (Scale != Info.EntrySize)
        continue;
      if (S.Inputs[0].isReg() || S.Inputs[0].isTemp()) {
        InjVar = S.Inputs[0];
        LoadPos = I;
      }
    }
  }
  if (LoadPos < 0)
    return Out;

  // Split point: the last write to the injected index variable before the table
  // LOAD.  The prefix (through that write) materialises the base and other
  // loop-invariant registers; the tail (after it) is re-run per index with the
  // injected value overriding the normalization's output.
  int LastDef = -1;
  for (int I = LoadPos - 1; I >= 0; --I)
    if (Ops[I].Output.Space == InjVar.Space &&
        Ops[I].Output.Offset == InjVar.Offset) {
      LastDef = I;
      break;
    }
  std::vector<LowOp> Prefix(Ops.begin(), Ops.begin() + (LastDef + 1));
  std::vector<LowOp> Tail(Ops.begin() + (LastDef + 1), Ops.end());

  NdOpEmulator Emu(Img);
  Emu.setCallPreservedRegisters(callPreservedRegs(Img));
  Emu.setLoadCollect(true);
  std::vector<va_t> Targets;
  Targets.reserve(Count);
  for (uint32_t I = 0; I < Count; ++I) {
    Emu.reset();
    Emu.run(Prefix);
    Emu.setRegister(InjVar.Offset, I);
    Emu.run(Tail);

    // Grounding: the emulation must have read exactly table slot i.  A wrong
    // injection point or an unmaterialised base reads a different address, so
    // this ties the emulated target to the recovered table and makes adoption
    // safe regardless of how the injection site was chosen.
    uint64_t Slot = Info.BaseAddr + static_cast<uint64_t>(I) * Info.EntrySize;
    bool Hit = false;
    for (auto &L : Emu.getLoadRecords())
      if (L.Addr == Slot) {
        Hit = true;
        break;
      }
    if (!Hit)
      return {};
    auto T = Emu.getRegister(TargetVar.Offset);
    if (!T || !isValidTarget(Img, *T, CurrentFuncEntry))
      return {};
    Targets.push_back(*T);
  }

  Grounded = true;
  return Targets;
}

//===----------------------------------------------------------------------===//
// reconcileSharedTables — align branches dispatching through the same table
//===----------------------------------------------------------------------===//

bool CFGBuilder::reconcileSharedTables(const BinaryImage &Img, Decoder &Dec) {
  bool Changed = false;
  // A clang-peeled first loop iteration and the loop body dispatch through the
  // *same* rodata jump table.  The peeled copy lives in the large
  // function-prologue block, where pre-SSA register reuse can leave the bound /
  // normalization analysis short (e.g. a case body's `and x,15` intersecting
  // the index's `and x,31`), while the loop body — sitting in a small, clean
  // block — recovers fully.  Group resolved branches by table base and let
  // every short copy adopt the most complete sibling, so a peeled copy never
  // drops cases.
  std::map<va_t, va_t> BestByBase;
  for (auto &[Addr, Rec] : Insns) {
    if (Rec.JumpTableTargets.empty())
      continue;
    auto It = ResolvedTableInfo.find(Addr);
    if (It == ResolvedTableInfo.end() || It->second.BaseAddr == 0)
      continue;
    auto B = BestByBase.find(It->second.BaseAddr);
    if (B == BestByBase.end() ||
        Insns[B->second].JumpTableTargets.size() < Rec.JumpTableTargets.size())
      BestByBase[It->second.BaseAddr] = Addr;
  }

  // Collect the branches to upgrade first; applying them calls explore(), which
  // mutates Insns and would invalidate an in-flight iterator.
  std::vector<va_t> ToUpgrade;
  for (auto &[Addr, Rec] : Insns) {
    if (Rec.JumpTableTargets.empty())
      continue;
    auto It = ResolvedTableInfo.find(Addr);
    if (It == ResolvedTableInfo.end() || It->second.BaseAddr == 0)
      continue;
    auto B = BestByBase.find(It->second.BaseAddr);
    if (B == BestByBase.end() || B->second == Addr)
      continue;
    const auto &BestInfo = ResolvedTableInfo[B->second];
    if (BestInfo.EntrySize != It->second.EntrySize ||
        BestInfo.IsRelative != It->second.IsRelative ||
        BestInfo.TargetBase != It->second.TargetBase)
      continue;
    if (Insns[B->second].JumpTableTargets.size() <= Rec.JumpTableTargets.size())
      continue;
    ToUpgrade.push_back(Addr);
  }

  for (va_t Addr : ToUpgrade) {
    auto RecIt = Insns.find(Addr);
    if (RecIt == Insns.end())
      continue;
    InsnRecord &Rec = RecIt->second;
    va_t Base = ResolvedTableInfo[Addr].BaseAddr;

    // Re-read with the sibling's (more complete) table parameters, keeping this
    // branch's own index register so its switch variable stays correct.
    JumpTableInfo Adopted = ResolvedTableInfo[BestByBase[Base]];
    Adopted.IndexReg = ResolvedTableInfo[Addr].IndexReg;
    auto NewTargets = readTableEntries(Img, Adopted);
    if (NewTargets.size() <= Rec.JumpTableTargets.size())
      continue;

    Rec.JumpTableTargets = NewTargets;
    ResolvedTableInfo[Addr] = Adopted;
    Changed = true;
    for (va_t T : NewTargets) {
      if (!ExploredAddrs.count(T)) {
        BlockStarts.insert(T);
        explore(Img, Dec, T);
      }
    }
  }
  return Changed;
}

//===----------------------------------------------------------------------===//
// extractJumpTables — collect jump-table metadata from decoded instructions
//===----------------------------------------------------------------------===//

void CFGBuilder::extractJumpTables(LowFunc &Func) {
  for (auto &[Addr, Rec] : Insns) {
    if (Rec.JumpTableTargets.empty())
      continue;

    JumpTable JT;
    JT.InsnAddr = Addr;
    JT.Targets = Rec.JumpTableTargets;

    // Reuse the cached analysis from resolveJumpTable when available,
    // avoiding the duplicate backward-slicing logic.
    auto CachedIt = ResolvedTableInfo.find(Addr);
    if (CachedIt != ResolvedTableInfo.end()) {
      auto &Info = CachedIt->second;
      JT.BaseAddr = Info.BaseAddr;
      JT.EntrySize = Info.EntrySize;
      JT.IsRelative = Info.IsRelative;
      JT.IsSigned = Info.IsSigned;
      JT.TargetBase = Info.TargetBase;
      JT.PreScaledIndex = Info.PreScaledIndex;
      JT.TwoTableSelect = Info.TwoTableSelect;
      JT.TwoTableOffset = Info.TwoTableOffset;
      JT.TwoTableHiPositive = Info.TwoTableHiPositive;
      JT.TwoLevelIndex = Info.TwoLevelIndex;
      JT.MutatedUnsafe = Info.MutatedUnsafe;
      if (Info.IndexReg != InvalidVA)
        JT.IndexRegOff = static_cast<int>(Info.IndexReg);
    } else {
      // Fallback: quick extraction from NdOp ops.
      bool FoundBase = false;
      bool FoundSize = false;
      bool SawLoad = false;
      uint16_t LoadWidth = 0;
      bool SawSext = false;

      for (auto &Op : Rec.Ops) {
        switch (Op.Opcode) {
        case NdOp::INT_ADD:
          if (!FoundBase && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
            JT.BaseAddr = Op.Inputs[1].Offset;
            FoundBase = true;
          }
          break;
        case NdOp::INT_MULT:
          if (!FoundSize && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
            JT.EntrySize = static_cast<uint16_t>(Op.Inputs[1].Offset);
            FoundSize = true;
          }
          break;
        case NdOp::INT_LEFT:
          if (!FoundSize && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
            uint64_t Shift = Op.Inputs[1].Offset;
            if (Shift <= limits::kMaxShiftForEntrySize) {
              JT.EntrySize = static_cast<uint16_t>(1u << Shift);
              FoundSize = true;
            }
          }
          break;
        case NdOp::LOAD:
          SawLoad = true;
          LoadWidth = Op.Output.Size;
          break;
        case NdOp::INT_SEXT:
          SawSext = true;
          break;
        default:
          break;
        }
      }

      if (SawLoad && !FoundSize && LoadWidth > 0 &&
          LoadWidth <= limits::kMaxEntryBytes) {
        JT.EntrySize = LoadWidth;
        FoundSize = true;
      }
      if (SawLoad && FoundBase && LoadWidth > 0 &&
          LoadWidth < limits::kMaxEntryBytes)
        JT.IsRelative = true;
      JT.IsSigned = SawSext || (SawLoad && LoadWidth < limits::kMaxEntryBytes);
    }

    auto CachedIt2 = ResolvedTableInfo.find(Addr);
    if (CachedIt2 != ResolvedTableInfo.end())
      recoverCaseLabels(JT, CachedIt2->second);

    Func.JumpTables.push_back(JT);
  }
}

} // namespace neverd
