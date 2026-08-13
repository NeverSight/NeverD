//===- AArch64LiftNEONPermute.cpp - NEON select, permute and table lookup -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bitwise insert/select (BSL/BIT/BIF), interleave and transpose
/// (TRN1/TRN2/ZIP1/ZIP2/UZP1/UZP2), pair extract (EXT) and the
/// per-byte table lookups TBL/TBX.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftNEONPermute(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // NEON vector bitwise insert
  case AARCH64_INS_BSL: {
    // BSL Vd, Vn, Vm: Vd = (Vn & Vd_old) | (Vm & ~Vd_old)
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar Mask = NdVar::reg(Dst.Offset, Dst.Size);
    NdVar T1 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T1, {A, Mask});
    NdVar NMask = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NMask, {Mask});
    NdVar T2 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T2, {B, NMask});
    S.emit(NdOp::INT_OR, Dst, {T1, T2});
    break;
  }
  case AARCH64_INS_BIT: {
    // BIT Vd, Vn, Vm: Vd = (Vn & Vm) | (Vd_old & ~Vm)
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar T1 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T1, {A, B});
    NdVar NB = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, NB, {B});
    NdVar T2 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T2, {NdVar::reg(Dst.Offset, Dst.Size), NB});
    S.emit(NdOp::INT_OR, Dst, {T1, T2});
    break;
  }
  case AARCH64_INS_BIF: {
    // BIF Vd, Vn, Vm: Vd = (Vd_old & Vm) | (Vn & ~Vm)
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    NdVar T1 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T1, {NdVar::reg(Dst.Offset, Dst.Size), B});
    NdVar NB = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, NB, {B});
    NdVar T2 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T2, {A, NB});
    S.emit(NdOp::INT_OR, Dst, {T1, T2});
    break;
  }
  // NEON permute / interleave (ZIP1/2, UZP1/2, TRN1/2) — real per-lane
  // permutation built from SUBBYTES element reads + CONCAT concatenation.
  // (Previously emitted as intrinsics with no backend handler, which
  //  silently returned 0 — breaking e.g. mulhi's uzp2-based >>16 extraction.)
  case AARCH64_INS_TRN1:
  case AARCH64_INS_TRN2:
  case AARCH64_INS_ZIP1:
  case AARCH64_INS_ZIP2:
  case AARCH64_INS_UZP1:
  case AARCH64_INS_UZP2: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    unsigned ElemSz = neonElemSize(ARM64.operands[0].vas);
    if (ElemSz == 0 || Dst.Size < ElemSz * 2) {
      // Unknown arrangement — fall back to copying the first source so the
      // value is at least defined rather than a placeholder zero.
      S.emit(NdOp::COPY, Dst, {A});
      break;
    }
    unsigned NLanes = Dst.Size / ElemSz;
    unsigned Half = NLanes / 2;
    auto extractElem = [&](const NdVar &Src, unsigned Idx) -> NdVar {
      NdVar E = S.makeTemp(ElemSz);
      S.emit(NdOp::SUBBYTES, E,
             {Src, NdVar::cst(static_cast<uint64_t>(Idx) * ElemSz, 4)});
      return E;
    };
    NdVar Acc;
    bool First = true;
    for (unsigned I = 0; I < NLanes; ++I) {
      bool FromB = false;
      unsigned SrcIdx = I;
      switch (Insn->id) {
      case AARCH64_INS_UZP1:
        FromB = (I >= Half);
        SrcIdx = 2 * (FromB ? I - Half : I);
        break;
      case AARCH64_INS_UZP2:
        FromB = (I >= Half);
        SrcIdx = 2 * (FromB ? I - Half : I) + 1;
        break;
      case AARCH64_INS_ZIP1:
        FromB = (I & 1);
        SrcIdx = I / 2;
        break;
      case AARCH64_INS_ZIP2:
        FromB = (I & 1);
        SrcIdx = Half + I / 2;
        break;
      case AARCH64_INS_TRN1:
        FromB = (I & 1);
        SrcIdx = I & ~1u;
        break;
      case AARCH64_INS_TRN2:
        FromB = (I & 1);
        SrcIdx = (I & ~1u) + 1;
        break;
      default:
        break;
      }
      NdVar E = extractElem(FromB ? B : A, SrcIdx);
      if (First) {
        Acc = E;
        First = false;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + ElemSz);
        S.emit(NdOp::CONCAT, Next, {E, Acc}); // E = high lane, Acc = low lanes
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // NEON EXT (extract from pair) — ext Vd, Vn, Vm, #index concatenates Vn:Vm
  // (Vn in the low bytes) and extracts a register-width window starting at byte
  // `index`.  Implemented as CONCAT(Vm,Vn) then SUBBYTES at byte `index`.
  // (Previously an intrinsic with no backend handler -> returned 0.)
  case AARCH64_INS_EXT: {
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]); // Vn -> low bytes
    NdVar B = L.operandRead(S, ARM64.operands[2]); // Vm -> high bytes
    unsigned RegBytes = Dst.Size;
    uint64_t Index = static_cast<uint64_t>(ARM64.operands[3].imm);
    // Normalize source operands to the register width.
    if (A.Size > RegBytes) {
      NdVar T = S.makeTemp(RegBytes);
      S.emit(NdOp::SUBBYTES, T, {A, NdVar::cst(0, 4)});
      A = T;
    }
    if (B.Size > RegBytes) {
      NdVar T = S.makeTemp(RegBytes);
      S.emit(NdOp::SUBBYTES, T, {B, NdVar::cst(0, 4)});
      B = T;
    }
    if (Index == 0) {
      S.emit(NdOp::COPY, Dst, {A});
      break;
    }
    if (Index >= RegBytes) {
      // Out of range (shouldn't happen for valid encodings) -> copy Vm.
      S.emit(NdOp::COPY, Dst, {B});
      break;
    }
    NdVar Combined = S.makeTemp(RegBytes * 2);
    S.emit(NdOp::CONCAT, Combined, {B, A}); // B high, A low
    S.emit(NdOp::SUBBYTES, Dst, {Combined, NdVar::cst(Index, 4)});
    break;
  }
  // NEON TBL/TBX — per-byte table lookup using a SELECT chain.
  //   TBL: result[i] = (idx[i] < table_len) ? table[idx[i]] : 0
  //   TBX: result[i] = (idx[i] < table_len) ? table[idx[i]] : old_dst[i]
  // The table may span 1-4 consecutive vector registers.  Capstone expands the
  // register list `{Vn.16b, Vn+1.16b, ...}` into separate operands, so the
  // index is always the *last* operand and the table registers are everything
  // between the destination and the index.
  case AARCH64_INS_TBL:
  case AARCH64_INS_TBX: {
    if (ARM64.op_count < 3)
      break;
    bool IsTbx = (Insn->id == AARCH64_INS_TBX);
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    unsigned IdxOp = ARM64.op_count - 1;
    NdVar Idx = L.operandRead(S, ARM64.operands[IdxOp]);
    // Concatenate every table register's bytes into one flat lookup table.
    std::vector<NdVar> TblBytes;
    for (unsigned R = 1; R < IdxOp; ++R) {
      NdVar T = L.operandRead(S, ARM64.operands[R]);
      for (unsigned J = 0; J < T.Size; ++J) {
        NdVar B = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, B, {T, NdVar::cst(J, 4)});
        TblBytes.push_back(B);
      }
    }
    unsigned TblLen = TblBytes.size();
    unsigned NBytes = Dst.Size;
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NBytes; ++I) {
      NdVar IdxByte = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, IdxByte, {Idx, NdVar::cst(I, 4)});
      // Default when no index matches: 0 (TBL) or the original dst byte (TBX).
      NdVar Res = S.makeTemp(1);
      if (IsTbx)
        S.emit(NdOp::SUBBYTES, Res, {OldDst, NdVar::cst(I, 4)});
      else
        S.emit(NdOp::COPY, Res, {NdVar::cst(0, 1)});
      for (unsigned J = 0; J < TblLen; ++J) {
        NdVar IsJ = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsJ, {IdxByte, NdVar::cst(J, 1)});
        NdVar NewRes = S.makeTemp(1);
        S.emit(NdOp::SELECT, NewRes, {IsJ, TblBytes[J], Res});
        Res = NewRes;
      }
      if (I == 0) {
        Acc = Res;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + 1);
        S.emit(NdOp::CONCAT, Next, {Res, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  default:
    return false;
  }
  return true;
}

} // namespace neverd
