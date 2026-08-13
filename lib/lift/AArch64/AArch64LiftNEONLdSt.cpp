//===- AArch64LiftNEONLdSt.cpp - NEON structured load/store ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// De-interleaving structure loads LD1..LD4 and their
/// replicating LD1R..LD4R forms, plus the interleaving structure
/// stores ST1..ST4.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftNEONLdSt(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // NEON structure load/store.  The last operand is the memory ADDRESS — it
  // must go through operandEffAddr (compute EA) rather than operandRead (which
  // performs a LOAD and returns the loaded value, causing a double indirection
  // that reads array data as a pointer -> wild address / UC_ERR_READ_UNMAPPED).
  case AARCH64_INS_LD1:
  case AARCH64_INS_LD2:
  case AARCH64_INS_LD3:
  case AARCH64_INS_LD4:
  case AARCH64_INS_LD1R:
  case AARCH64_INS_LD2R:
  case AARCH64_INS_LD3R:
  case AARCH64_INS_LD4R: {
    if (ARM64.op_count < 2)
      break;
    bool IsReplicate =
        (Insn->id == AARCH64_INS_LD1R || Insn->id == AARCH64_INS_LD2R ||
         Insn->id == AARCH64_INS_LD3R || Insn->id == AARCH64_INS_LD4R);
    int MemIdx = -1;
    for (int I = 0; I < ARM64.op_count; ++I)
      if (ARM64.operands[I].type == AARCH64_OP_MEM) {
        MemIdx = I;
        break;
      }
    if (MemIdx < 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar EA = L.operandEffAddr(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::LOAD, Dst, {EA});
      break;
    }
    unsigned NRegs = static_cast<unsigned>(MemIdx);
    unsigned Struct =
        (Insn->id == AARCH64_INS_LD2 || Insn->id == AARCH64_INS_LD2R)   ? 2
        : (Insn->id == AARCH64_INS_LD3 || Insn->id == AARCH64_INS_LD3R) ? 3
        : (Insn->id == AARCH64_INS_LD4 || Insn->id == AARCH64_INS_LD4R) ? 4
                                                                        : 1;
    unsigned ElemSz = neonElemSize(ARM64.operands[0].vas);
    int LaneIdx = ARM64.operands[0].vector_index;
    // See ST1-4: structure addressing is `[Xn]`; take base register directly so
    // a post-index disp (`[x9], #4`) is not folded into the load address.
    NdVar Base;
    {
      const auto &MO = ARM64.operands[MemIdx].mem;
      if (MO.base != AARCH64_REG_INVALID) {
        auto RI = mapCapstoneReg(static_cast<aarch64_reg>(MO.base));
        Base = S.makeTemp(8);
        S.emit(NdOp::COPY, Base, {NdVar::reg(RI.Offset, 8)});
      } else {
        Base = L.operandEffAddr(S, ARM64.operands[ARM64.op_count - 1]);
      }
    }
    NdVar Dst0 = L.operandWrite(ARM64.operands[0]);
    unsigned RegSize = Dst0.Size;
    auto addrAt = [&](unsigned Off) -> NdVar {
      if (Off == 0)
        return Base;
      NdVar A = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, A, {Base, NdVar::cst(Off, 8)});
      return A;
    };
    unsigned TotalBytes = 0;
    if (IsReplicate && ElemSz > 0 && ElemSz < RegSize &&
        RegSize % ElemSz == 0) {
      // LDxR: load one element per register and replicate across all lanes.
      unsigned NLanes = RegSize / ElemSz;
      for (unsigned R = 0; R < NRegs; ++R) {
        NdVar Elem = S.makeTemp(ElemSz);
        S.emit(NdOp::LOAD, Elem, {addrAt(R * ElemSz)});
        NdVar Acc = Elem;
        for (unsigned I = 1; I < NLanes; ++I) {
          NdVar Next = S.makeTemp(Acc.Size + ElemSz);
          S.emit(NdOp::CONCAT, Next, {Elem, Acc});
          Acc = Next;
        }
        S.emit(NdOp::COPY, L.operandWrite(ARM64.operands[R]), {Acc});
      }
      TotalBytes = NRegs * ElemSz;
    } else if (LaneIdx >= 0 && ElemSz > 0) {
      // Indexed single-element form: read-modify-write lane LaneIdx of each
      // reg.
      for (unsigned R = 0; R < NRegs; ++R) {
        // Read the *whole* register as the merge base.  operandRead would
        // honour the operand's vector_index and hand back just the indexed
        // lane, which breaks the read-modify-write (the kept lanes come out
        // wrong).
        NdVar Out = L.operandWrite(ARM64.operands[R]);
        NdVar Cur = Out;
        NdVar El = S.makeTemp(ElemSz);
        S.emit(NdOp::LOAD, El, {addrAt(R * ElemSz)});
        unsigned ByteOff = static_cast<unsigned>(LaneIdx) * ElemSz;
        // LaneIdx is only checked as >= 0; guard against a malformed index
        // whose lane spills past the register, which would underflow the
        // Cur.Size - ByteOff - ElemSz high-slice size below into a huge
        // makeTemp.
        if (ByteOff + ElemSz > Cur.Size)
          break;
        if (ByteOff == 0) {
          NdVar Hi = S.makeTemp(Cur.Size - ElemSz);
          S.emit(NdOp::SUBBYTES, Hi, {Cur, NdVar::cst(ElemSz, 4)});
          S.emit(NdOp::CONCAT, Out, {Hi, El});
        } else if (ByteOff + ElemSz == Cur.Size) {
          NdVar Lo = S.makeTemp(ByteOff);
          S.emit(NdOp::SUBBYTES, Lo, {Cur, NdVar::cst(0, 4)});
          S.emit(NdOp::CONCAT, Out, {El, Lo});
        } else {
          NdVar Lo = S.makeTemp(ByteOff);
          S.emit(NdOp::SUBBYTES, Lo, {Cur, NdVar::cst(0, 4)});
          NdVar Hi = S.makeTemp(Cur.Size - ByteOff - ElemSz);
          S.emit(NdOp::SUBBYTES, Hi, {Cur, NdVar::cst(ByteOff + ElemSz, 4)});
          NdVar Mid = S.makeTemp(ByteOff + ElemSz);
          S.emit(NdOp::CONCAT, Mid, {El, Lo});
          S.emit(NdOp::CONCAT, Out, {Hi, Mid});
        }
      }
      TotalBytes = NRegs * ElemSz;
    } else if (Struct == 1 || ElemSz == 0 || RegSize == 0 ||
               RegSize % ElemSz != 0) {
      // Contiguous: load each whole register back-to-back.
      for (unsigned R = 0; R < NRegs; ++R)
        S.emit(NdOp::LOAD, L.operandWrite(ARM64.operands[R]),
               {addrAt(R * RegSize)});
      TotalBytes = NRegs * RegSize;
    } else {
      // De-interleaved load (LD2/LD3/LD4): Regs[R][L] = in[L*NRegs + R].
      unsigned NLanes = RegSize / ElemSz;
      for (unsigned R = 0; R < NRegs; ++R) {
        NdVar Acc = S.makeTemp(0);
        bool First = true;
        for (unsigned L = 0; L < NLanes; ++L) {
          NdVar El = S.makeTemp(ElemSz);
          S.emit(NdOp::LOAD, El, {addrAt((L * NRegs + R) * ElemSz)});
          if (First) {
            Acc = El;
            First = false;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + ElemSz);
            S.emit(NdOp::CONCAT, Next, {El, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, L.operandWrite(ARM64.operands[R]), {Acc});
      }
      TotalBytes = NLanes * NRegs * ElemSz;
    }
    if (Insn->detail->writeback &&
        ARM64.operands[MemIdx].mem.base != AARCH64_REG_INVALID) {
      const auto &M = ARM64.operands[MemIdx];
      auto BI = mapCapstoneReg(static_cast<aarch64_reg>(M.mem.base));
      NdVar BaseReg = NdVar::reg(BI.Offset, 8);
      aarch64_reg PostReg = AARCH64_REG_INVALID;
      if (M.mem.index != AARCH64_REG_INVALID)
        PostReg = static_cast<aarch64_reg>(M.mem.index);
      else if (MemIdx + 1 < ARM64.op_count &&
               ARM64.operands[MemIdx + 1].type == AARCH64_OP_REG)
        PostReg = static_cast<aarch64_reg>(ARM64.operands[MemIdx + 1].reg);
      if (PostReg != AARCH64_REG_INVALID) {
        auto PI = mapCapstoneReg(PostReg);
        S.emit(NdOp::INT_ADD, BaseReg, {BaseReg, NdVar::reg(PI.Offset, 8)});
      } else {
        int64_t WB = M.mem.disp != 0 ? M.mem.disp : (int64_t)TotalBytes;
        if (MemIdx + 1 < ARM64.op_count &&
            ARM64.operands[MemIdx + 1].type == AARCH64_OP_IMM)
          WB = ARM64.operands[MemIdx + 1].imm;
        if (WB != 0)
          S.emit(NdOp::INT_ADD, BaseReg,
                 {BaseReg, NdVar::cst(static_cast<uint64_t>(WB), 8)});
      }
    }
    break;
  }
  case AARCH64_INS_ST1:
  case AARCH64_INS_ST2:
  case AARCH64_INS_ST3:
  case AARCH64_INS_ST4: {
    if (ARM64.op_count < 2)
      break;
    // `stN {v0,...,vk}.<T>, [Xn]{, <wb>}` transfers a *list* of registers.
    // ST1 is contiguous; ST2/ST3/ST4 INTERLEAVE the registers element-by-
    // element (out[lane*N + reg] = reg[lane]).  The previous handler stored
    // only operands[0] contiguously, so `st4 {v0-v3}` (clang's transposed
    // matrix store) wrote one register and zero-filled the rest.
    int MemIdx = -1;
    for (int I = 0; I < ARM64.op_count; ++I)
      if (ARM64.operands[I].type == AARCH64_OP_MEM) {
        MemIdx = I;
        break;
      }
    if (MemIdx < 1) {
      NdVar Src = L.operandRead(S, ARM64.operands[0]);
      NdVar EA = L.operandEffAddr(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::STORE, {}, {EA, Src});
      break;
    }
    unsigned NRegs = static_cast<unsigned>(MemIdx);
    unsigned Struct = (Insn->id == AARCH64_INS_ST2)   ? 2
                      : (Insn->id == AARCH64_INS_ST3) ? 3
                      : (Insn->id == AARCH64_INS_ST4) ? 4
                                                      : 1;
    unsigned ElemSz = neonElemSize(ARM64.operands[0].vas);
    int LaneIdx = ARM64.operands[0].vector_index;
    // NEON structure addressing is always `[Xn]`; a post-index disp/reg is a
    // writeback amount, NOT part of the access EA.  operandEffAddr would add
    // mem.disp (e.g. `[x9], #4` -> x9+4), shifting every element by one — so
    // take the base register directly.
    NdVar Base;
    {
      const auto &MO = ARM64.operands[MemIdx].mem;
      if (MO.base != AARCH64_REG_INVALID) {
        auto RI = mapCapstoneReg(static_cast<aarch64_reg>(MO.base));
        Base = S.makeTemp(8);
        S.emit(NdOp::COPY, Base, {NdVar::reg(RI.Offset, 8)});
      } else {
        Base = L.operandEffAddr(S, ARM64.operands[MemIdx]);
      }
    }
    std::vector<NdVar> Regs;
    for (unsigned R = 0; R < NRegs; ++R)
      Regs.push_back(L.operandRead(S, ARM64.operands[R]));
    unsigned RegSize = Regs.empty() ? 0 : Regs[0].Size;
    auto addrAt = [&](unsigned Off) -> NdVar {
      if (Off == 0)
        return Base;
      NdVar A = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, A, {Base, NdVar::cst(Off, 8)});
      return A;
    };
    unsigned TotalBytes = 0;
    if (LaneIdx >= 0 && ElemSz > 0) {
      // Indexed single-element form: store lane LaneIdx of each register.
      // Read the whole register (operandRead would already extract the indexed
      // lane, making the SUBBYTES below double-extract / read out of range).
      for (unsigned R = 0; R < NRegs; ++R) {
        NdVar FullReg = L.operandWrite(ARM64.operands[R]);
        NdVar El = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, El, {FullReg, NdVar::cst(LaneIdx * ElemSz, 4)});
        S.emit(NdOp::STORE, {}, {addrAt(R * ElemSz), El});
      }
      TotalBytes = NRegs * ElemSz;
    } else if (Struct == 1 || ElemSz == 0 || RegSize == 0 ||
               RegSize % ElemSz != 0) {
      // Contiguous: store each whole register back-to-back.
      for (unsigned R = 0; R < NRegs; ++R)
        S.emit(NdOp::STORE, {}, {addrAt(R * RegSize), Regs[R]});
      TotalBytes = NRegs * RegSize;
    } else {
      // Interleaved store (ST2/ST3/ST4): out[(L*NRegs + R)] = Regs[R][L].
      unsigned NLanes = RegSize / ElemSz;
      for (unsigned L = 0; L < NLanes; ++L)
        for (unsigned R = 0; R < NRegs; ++R) {
          NdVar El = S.makeTemp(ElemSz);
          S.emit(NdOp::SUBBYTES, El, {Regs[R], NdVar::cst(L * ElemSz, 4)});
          S.emit(NdOp::STORE, {}, {addrAt((L * NRegs + R) * ElemSz), El});
        }
      TotalBytes = NLanes * NRegs * ElemSz;
    }
    (void)Struct;
    if (Insn->detail->writeback &&
        ARM64.operands[MemIdx].mem.base != AARCH64_REG_INVALID) {
      const auto &M = ARM64.operands[MemIdx];
      auto BI = mapCapstoneReg(static_cast<aarch64_reg>(M.mem.base));
      NdVar BaseReg = NdVar::reg(BI.Offset, 8);
      aarch64_reg PostReg = AARCH64_REG_INVALID;
      if (M.mem.index != AARCH64_REG_INVALID)
        PostReg = static_cast<aarch64_reg>(M.mem.index);
      else if (MemIdx + 1 < ARM64.op_count &&
               ARM64.operands[MemIdx + 1].type == AARCH64_OP_REG)
        PostReg = static_cast<aarch64_reg>(ARM64.operands[MemIdx + 1].reg);
      if (PostReg != AARCH64_REG_INVALID) {
        auto PI = mapCapstoneReg(PostReg);
        S.emit(NdOp::INT_ADD, BaseReg, {BaseReg, NdVar::reg(PI.Offset, 8)});
      } else {
        int64_t WB = M.mem.disp != 0 ? M.mem.disp : (int64_t)TotalBytes;
        if (MemIdx + 1 < ARM64.op_count &&
            ARM64.operands[MemIdx + 1].type == AARCH64_OP_IMM)
          WB = ARM64.operands[MemIdx + 1].imm;
        if (WB != 0)
          S.emit(NdOp::INT_ADD, BaseReg,
                 {BaseReg, NdVar::cst(static_cast<uint64_t>(WB), 8)});
      }
    }
    break;
  }
  default:
    return false;
  }
  return true;
}

} // namespace neverd
