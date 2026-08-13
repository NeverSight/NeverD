//===- JumpTableResolverSlice.cpp - Backward data-flow slicing ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Backward data-flow slicing over an instruction's micro-ops: reaching-
/// definition lookup, copy/extend chain tracing to a source register, scaled
/// index recovery, table load-address decomposition, and frame-slot keying.
/// Also hosts the two single-record base detectors that are built directly on
/// this slicing — the generic absolute-table slice and the PIC-relative table.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch and JumpTableResolverDetail.h for the shared
/// declarations of the helpers defined here.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"

#include <cstdint>
#include <vector>

namespace neverd {

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
        const NdVar &LAddr = (Op.NumInputs >= 2) ? Op.Inputs[1] : Op.Inputs[0];
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
int reachingDefIdx(const std::vector<LowOp> &Ops, int FromIdx, const NdVar &V) {
  for (int I = FromIdx; I >= 0; --I) {
    const NdVar &O = Ops[I].Output;
    if (O.Space == V.Space && O.Offset == V.Offset)
      return I;
  }
  return -1;
}

/// Trace a nd-var backward through COPY chains to a plain register.
uint64_t traceToRegister(const std::vector<LowOp> &Ops, int FromIdx, NdVar V) {
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

} // namespace neverd
