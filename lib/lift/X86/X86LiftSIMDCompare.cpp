//===- X86LiftSIMDCompare.cpp - x86/x64 SIMD compare lifter ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSE/AVX comparison instructions: packed integer equality and
/// greater-than, predicated packed and scalar float compares,
/// ordered/unordered scalar compares that write EFLAGS, the
/// AVX-512 mask-producing compares, PTEST, and the SSE4.2
/// packed string compares.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftSIMDCompare(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                     const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // VPCMPEQ{B,W,D,Q} — per-lane equality comparison, result is all-1s or 0.
  case X86_INS_VPCMPEQB:
  case X86_INS_VPCMPEQW:
  case X86_INS_VPCMPEQD:
  case X86_INS_VPCMPEQQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    unsigned LaneSz = 1;
    switch (InsnId) {
    case X86_INS_VPCMPEQW:
      LaneSz = 2;
      break;
    case X86_INS_VPCMPEQD:
      LaneSz = 4;
      break;
    case X86_INS_VPCMPEQQ:
      LaneSz = 8;
      break;
    default:
      break;
    }
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
        NdVar Eq = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, Eq, {La, Lb});
        NdVar Mask = S.makeTemp(LaneSz);
        uint64_t AllOnes = (LaneSz == 8) ? 0xFFFFFFFFFFFFFFFFULL
                                         : ((1ULL << (LaneSz * 8)) - 1);
        S.emit(NdOp::SELECT, Mask,
               {Eq, NdVar::cst(AllOnes, LaneSz), NdVar::cst(0, LaneSz)});
        if (I == 0) {
          Acc = Mask;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Mask, Acc});
          Acc = Next;
        }
      }
      return Acc;
    };
    NdVar LoHalf = BuildHalf(0);
    NdVar HiHalf = BuildHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // SSE float compare with predicate (CMPPS/CMPPD).
  // CMPSS/CMPSD are handled by the dual-purpose dispatcher in LiftString.
  // VEX versions (VCMPPS/VCMPPD) are handled separately.
  case X86_INS_CMPPS:
  case X86_INS_CMPPD: {
    if (X86.op_count < 2)
      break;
    bool IsVEX = false;
    bool IsPD = (InsnId == X86_INS_CMPPD);
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = IsVEX ? L.operandRead(S, X86.operands[1])
                    : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[IsVEX ? 2 : 1]);
    uint8_t Pred = 0;
    bool FoundImm = false;
    for (uint8_t N = 0; N < X86.op_count; ++N) {
      if (X86.operands[N].type == X86_OP_IMM) {
        Pred = static_cast<uint8_t>(X86.operands[N].imm) & 7;
        FoundImm = true;
      }
    }
    if (!FoundImm && Insn->size >= 1)
      Pred = Insn->bytes[Insn->size - 1] & 7;
    unsigned LaneSz = IsPD ? 8 : 4;
    unsigned NLanes = Dst.Size / LaneSz;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar La = S.makeTemp(LaneSz);
      NdVar Lb = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
      S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
      NdVar Cmp = S.makeTemp(1);
      bool Neg = false;
      switch (Pred) {
      default:
      case 0:
        S.emit(NdOp::FLOAT_EQUAL, Cmp, {La, Lb});
        break;
      case 1:
        S.emit(NdOp::FLOAT_LESS, Cmp, {La, Lb});
        break;
      case 2:
        S.emit(NdOp::FLOAT_LESSEQUAL, Cmp, {La, Lb});
        break;
      case 3: { // UNORD: isNaN(a) || isNaN(b) — must inspect BOTH operands
        NdVar NanA = S.makeTemp(1), NanB = S.makeTemp(1);
        S.emit(NdOp::FLOAT_ISNAN, NanA, {La});
        S.emit(NdOp::FLOAT_ISNAN, NanB, {Lb});
        S.emit(NdOp::BOOL_OR, Cmp, {NanA, NanB});
        break;
      }
      case 4:
        S.emit(NdOp::FLOAT_NOTEQUAL, Cmp, {La, Lb});
        break;
      case 5:
        S.emit(NdOp::FLOAT_LESS, Cmp, {La, Lb});
        Neg = true;
        break;
      case 6:
        S.emit(NdOp::FLOAT_LESSEQUAL, Cmp, {La, Lb});
        Neg = true;
        break;
      case 7: { // ORD: !(isNaN(a) || isNaN(b)) — must inspect BOTH operands
        NdVar NanA = S.makeTemp(1), NanB = S.makeTemp(1);
        S.emit(NdOp::FLOAT_ISNAN, NanA, {La});
        S.emit(NdOp::FLOAT_ISNAN, NanB, {Lb});
        S.emit(NdOp::BOOL_OR, Cmp, {NanA, NanB});
        Neg = true;
        break;
      }
      }
      if (Neg) {
        NdVar NotCmp = S.makeTemp(1);
        S.emit(NdOp::BOOL_NOT, NotCmp, {Cmp});
        Cmp = NotCmp;
      }
      NdVar Mask = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_ZEXT, Mask, {Cmp});
      NdVar AllOnes = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_NEG2, AllOnes, {Mask});
      if (I == 0) {
        Acc = AllOnes;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {AllOnes, Acc});
        Acc = Next;
      }
    }
    if (Acc.Size < Dst.Size) {
      NdVar Wide = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ZEXT, Wide, {Acc});
      S.emit(NdOp::COPY, Dst, {Wide});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }

  // PCMPISTRI/PCMPESTRI (index→ECX) and PCMPISTRM/PCMPESTRM (mask→XMM0) —
  // SSE4.2 string compare.  The index/mask result maps to the real LLVM
  // pcmp*str* intrinsic; CF/ZF/SF/OF come from per-flag intrinsics (the
  // comparison clears AF/PF).  Explicit-length forms read the string lengths
  // from EAX (first string) and EDX (second string).
  case X86_INS_PCMPISTRI:
  case X86_INS_PCMPESTRI:
  case X86_INS_PCMPISTRM:
  case X86_INS_PCMPESTRM: {
    if (X86.op_count < 3)
      break;
    bool IsExplicit =
        (InsnId == X86_INS_PCMPESTRI || InsnId == X86_INS_PCMPESTRM);
    bool IsIndex = (InsnId == X86_INS_PCMPISTRI || InsnId == X86_INS_PCMPESTRI);
    NdVar A = L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[1]);
    NdVar Imm = NdVar::cst(X86.operands[2].imm & 0xFF, 1);
    NdVar La = NdVar::reg(x86reg::RAX, 4);
    NdVar Lb = NdVar::reg(x86reg::RDX, 4);
    NdVar Out =
        IsIndex ? NdVar::reg(x86reg::RCX, 4) : NdVar::reg(x86reg::XMM0, 16);
    Intrinsic FlagId;
    if (IsExplicit) {
      S.emitIntrinsic(IsIndex ? Intrinsic::Pcmpestri : Intrinsic::Pcmpestrm,
                      Out, {A, La, B, Lb, Imm});
      FlagId = Intrinsic::PcmpestrFlag;
    } else {
      S.emitIntrinsic(IsIndex ? Intrinsic::Pcmpistri : Intrinsic::Pcmpistrm,
                      Out, {A, B, Imm});
      FlagId = Intrinsic::PcmpistrFlag;
    }
    static constexpr struct {
      uint64_t Sel;
      uint64_t Reg;
    } StatusFlags[] = {
        {0, x86reg::CF}, {1, x86reg::ZF}, {2, x86reg::SF}, {3, x86reg::OF}};
    uint64_t ImmVal = X86.operands[2].imm & 0xFF;
    for (const auto &F : StatusFlags) {
      NdVar Bit = S.makeTemp(1);
      // Pack the control imm (bits 0-7) and the flag selector (bits 8-9) into a
      // single operand so the explicit form stays within the 6-input INTRINSIC
      // limit (A/LA/B/LB/immsel + the intrinsic code).
      NdVar ImmSel = NdVar::cst(ImmVal | (F.Sel << 8), 2);
      if (IsExplicit)
        S.emitIntrinsic(FlagId, Bit, {A, La, B, Lb, ImmSel});
      else
        S.emitIntrinsic(FlagId, Bit, {A, B, ImmSel});
      S.emit(NdOp::COPY, NdVar::reg(F.Reg, 1), {Bit});
    }
    S.emit(NdOp::COPY, NdVar::reg(x86reg::AF, 1), {NdVar::cst(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::PF, 1), {NdVar::cst(0, 1)});
    break;
  }

  // PCMPEQQ / VPCMPEQQ — per-lane qword equal compare (SSE4.1 / AVX).
  case X86_INS_PCMPEQQ:
  case X86_INS_PCMPGTQ: {
    if (X86.op_count < 2)
      break;
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    unsigned LaneSz = 8;
    if (Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {DstR, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Cmp = S.makeTemp(1);
        if (InsnId == X86_INS_PCMPEQQ)
          S.emit(NdOp::INT_EQUAL, Cmp, {La, Lb});
        else
          S.emit(NdOp::INT_SLESS, Cmp, {Lb, La});
        NdVar AllOnes = NdVar::cst(~uint64_t(0), LaneSz);
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, Lane, {Cmp, AllOnes, NdVar::cst(0, LaneSz)});
        if (I == 0) {
          Acc = Lane;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Lane, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Cmp = S.makeTemp(1);
      if (InsnId == X86_INS_PCMPEQQ)
        S.emit(NdOp::INT_EQUAL, Cmp, {DstR, Src});
      else
        S.emit(NdOp::INT_SLESS, Cmp, {Src, DstR});
      S.emit(
          NdOp::SELECT, Dst,
          {Cmp, NdVar::cst(~uint64_t(0), Dst.Size), NdVar::cst(0, Dst.Size)});
    }
    break;
  }

  // VCMPxx / VCOMISS / VCOMISD / VUCOMISS / VUCOMISD — vector/scalar fp
  // compare. Capstone has both the generic VCMP* (with predicate as
  // immediate operand) AND pseudo-mnemonic VCMP ids (capstone renders
  // these with a suffix like "vcmpgt_oqps" but the underlying ID is the
  // catch-all X86_INS_VCMP). We include all variants.
  case X86_INS_VCOMISS:
  case X86_INS_VCOMISD:
  case X86_INS_VUCOMISS:
  case X86_INS_VUCOMISD: {
    if (X86.op_count < 2)
      break;
    NdVar Lhs = L.operandRead(S, X86.operands[0]);
    NdVar Rhs = L.operandRead(S, X86.operands[X86.op_count - 1]);
    // VCOMISS/VUCOMISS compare the low single (4B), the *SD forms the low
    // double (8B).  operandRead returns the whole XMM for a register operand;
    // extract the scalar so the float emitter infers the right precision (a *SS
    // compare left 16B wide reads 8 bytes as a double and inverts the sign
    // test).
    unsigned ScalarSz =
        (InsnId == X86_INS_VCOMISS || InsnId == X86_INS_VUCOMISS) ? 4 : 8;
    if (Lhs.Size > ScalarSz) {
      NdVar T = S.makeTemp(ScalarSz);
      S.emit(NdOp::SUBBYTES, T, {Lhs, NdVar::cst(0, 4)});
      Lhs = T;
    }
    if (Rhs.Size > ScalarSz) {
      NdVar T = S.makeTemp(ScalarSz);
      S.emit(NdOp::SUBBYTES, T, {Rhs, NdVar::cst(0, 4)});
      Rhs = T;
    }
    // An unordered (NaN) compare sets ZF=PF=CF=1.  Bare ordered relations leave
    // ZF/CF 0 on NaN, which makes the SETA/SETAE ordered idioms (!CF&&!ZF /
    // !CF) wrongly read true, so OR the unordered predicate into ZF and CF.
    NdVar NanA = S.makeTemp(1);
    NdVar NanB = S.makeTemp(1);
    S.emit(NdOp::FLOAT_ISNAN, NanA, {Lhs});
    S.emit(NdOp::FLOAT_ISNAN, NanB, {Rhs});
    NdVar Unord = S.makeTemp(1);
    S.emit(NdOp::BOOL_OR, Unord, {NanA, NanB});
    NdVar Eq = S.makeTemp(1);
    S.emit(NdOp::FLOAT_EQUAL, Eq, {Lhs, Rhs});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::ZF, 1), {Eq, Unord});
    NdVar Lt = S.makeTemp(1);
    S.emit(NdOp::FLOAT_LESS, Lt, {Lhs, Rhs});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::CF, 1), {Lt, Unord});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::PF, 1), {Unord});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::cst(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::SF, 1), {NdVar::cst(0, 1)});
    break;
  }
  // VEX FP compare (3-operand: dst, src1, src2 [, imm]).  The predicate lives
  // in the trailing immediate byte; for non-NaN inputs predicate & 7 selects
  // the relation (the 32 AVX predicates are four NaN/signaling variants of the
  // same eight relations).  Packed forms fill the result per lane; scalar forms
  // compare lane 0 and keep src1's upper lanes (VEX scalar semantics).
  case X86_INS_VCMP:
  case X86_INS_VCMPSS:
  case X86_INS_VCMPSD:
  case X86_INS_VCMPPS:
  case X86_INS_VCMPPD: {
    if (X86.op_count < 3)
      break;
    bool IsWide = (InsnId == X86_INS_VCMPPD || InsnId == X86_INS_VCMPSD);
    bool IsScalar = (InsnId == X86_INS_VCMPSS || InsnId == X86_INS_VCMPSD);
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    uint8_t Pred = 0;
    bool FoundImm = false;
    int SrcIdx = X86.op_count - 1;
    for (uint8_t N = 0; N < X86.op_count; ++N)
      if (X86.operands[N].type == X86_OP_IMM) {
        Pred = static_cast<uint8_t>(X86.operands[N].imm) & 7;
        FoundImm = true;
      }
    if (FoundImm)
      SrcIdx = X86.op_count - 2;
    else if (Insn->size >= 1)
      Pred = Insn->bytes[Insn->size - 1] & 7;
    NdVar B = L.operandRead(S, X86.operands[SrcIdx]);
    unsigned LaneSz = IsWide ? 8 : 4;
    unsigned NLanes = IsScalar ? 1 : (Dst.Size / LaneSz);
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar La = S.makeTemp(LaneSz);
      NdVar Lb = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
      S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
      NdVar Cmp = S.makeTemp(1);
      bool Neg = false;
      switch (Pred) {
      default:
      case 0:
        S.emit(NdOp::FLOAT_EQUAL, Cmp, {La, Lb});
        break;
      case 1:
        S.emit(NdOp::FLOAT_LESS, Cmp, {La, Lb});
        break;
      case 2:
        S.emit(NdOp::FLOAT_LESSEQUAL, Cmp, {La, Lb});
        break;
      case 3: { // UNORD: isNaN(a) || isNaN(b) — must inspect BOTH operands
        NdVar NanA = S.makeTemp(1), NanB = S.makeTemp(1);
        S.emit(NdOp::FLOAT_ISNAN, NanA, {La});
        S.emit(NdOp::FLOAT_ISNAN, NanB, {Lb});
        S.emit(NdOp::BOOL_OR, Cmp, {NanA, NanB});
        break;
      }
      case 4:
        S.emit(NdOp::FLOAT_NOTEQUAL, Cmp, {La, Lb});
        break;
      case 5:
        S.emit(NdOp::FLOAT_LESS, Cmp, {La, Lb});
        Neg = true;
        break;
      case 6:
        S.emit(NdOp::FLOAT_LESSEQUAL, Cmp, {La, Lb});
        Neg = true;
        break;
      case 7: { // ORD: !(isNaN(a) || isNaN(b)) — must inspect BOTH operands
        NdVar NanA = S.makeTemp(1), NanB = S.makeTemp(1);
        S.emit(NdOp::FLOAT_ISNAN, NanA, {La});
        S.emit(NdOp::FLOAT_ISNAN, NanB, {Lb});
        S.emit(NdOp::BOOL_OR, Cmp, {NanA, NanB});
        Neg = true;
        break;
      }
      }
      if (Neg) {
        NdVar NotCmp = S.makeTemp(1);
        S.emit(NdOp::BOOL_NOT, NotCmp, {Cmp});
        Cmp = NotCmp;
      }
      NdVar Mask = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_ZEXT, Mask, {Cmp});
      NdVar AllOnes = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_NEG2, AllOnes, {Mask});
      if (I == 0) {
        Acc = AllOnes;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {AllOnes, Acc});
        Acc = Next;
      }
    }
    if (IsScalar && Dst.Size > LaneSz) {
      NdVar Hi = S.makeTemp(Dst.Size - LaneSz);
      S.emit(NdOp::SUBBYTES, Hi, {A, NdVar::cst(LaneSz, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, Acc});
    } else if (Acc.Size < Dst.Size) {
      NdVar Wide = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ZEXT, Wide, {Acc});
      S.emit(NdOp::COPY, Dst, {Wide});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }

  // Generic VPCMP with predicate (AVX-512). Capstone 6 maps predicate-
  // suffixed vpcmp* mnemonics to X86_INS_VPCMP /
  // X86_INS_VPCMPB/D/Q/W/UB/UD/UQ/UW.
  case X86_INS_VPCMP:
  case X86_INS_VPCMPB:
  case X86_INS_VPCMPD:
  case X86_INS_VPCMPQ:
  case X86_INS_VPCMPW:
  case X86_INS_VPCMPUB:
  case X86_INS_VPCMPUD:
  case X86_INS_VPCMPUQ:
  case X86_INS_VPCMPUW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(
        S, X86.operands[X86.op_count >= 4 ? 2 : X86.op_count - 1]);
    S.emit(NdOp::INT_EQUAL, Dst, {A, B});
    break;
  }

  // PTEST / VPTEST — set ZF/CF based on bit test.
  case X86_INS_PTEST:
  case X86_INS_VPTEST: {
    if (X86.op_count < 2)
      break;
    NdVar A = L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[1]);
    NdVar Masked = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, Masked, {A, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Masked, NdVar::cst(0, A.Size)});
    NdVar AndNot = S.makeTemp(A.Size);
    NdVar NegA = S.makeTemp(A.Size);
    S.emit(NdOp::INT_NOT, NegA, {A});
    S.emit(NdOp::INT_AND, AndNot, {NegA, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
           {AndNot, NdVar::cst(0, A.Size)});
    break;
  }

  // AVX packed integer compare — per-lane greater-than.
  case X86_INS_VPCMPGTB:
  case X86_INS_VPCMPGTW:
  case X86_INS_VPCMPGTD:
  case X86_INS_VPCMPGTQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    unsigned LaneSz = 1;
    switch (InsnId) {
    case X86_INS_VPCMPGTW:
      LaneSz = 2;
      break;
    case X86_INS_VPCMPGTD:
      LaneSz = 4;
      break;
    case X86_INS_VPCMPGTQ:
      LaneSz = 8;
      break;
    default:
      break;
    }
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
        NdVar Gt = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, Gt, {Lb, La});
        NdVar Mask = S.makeTemp(LaneSz);
        uint64_t AllOnes = (LaneSz == 8) ? 0xFFFFFFFFFFFFFFFFULL
                                         : ((1ULL << (LaneSz * 8)) - 1);
        S.emit(NdOp::SELECT, Mask,
               {Gt, NdVar::cst(AllOnes, LaneSz), NdVar::cst(0, LaneSz)});
        if (I == 0) {
          Acc = Mask;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Mask, Acc});
          Acc = Next;
        }
      }
      return Acc;
    };
    NdVar LoHalf = BuildHalf(0);
    NdVar HiHalf = BuildHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
