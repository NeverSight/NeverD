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

#include <algorithm>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

struct EvexPackedCompareSpec {
  uint8_t Opcode = 0;
  uint16_t ElementSize = 0;
  bool W = false;
  bool Signed = false;
};

bool getEvexPackedCompareSpec(unsigned InsnId, EvexPackedCompareSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VPCMPB:
    Spec = {0x3f, 1, false, true};
    return true;
  case X86_INS_VPCMPUB:
    Spec = {0x3e, 1, false, false};
    return true;
  case X86_INS_VPCMPW:
    Spec = {0x3f, 2, true, true};
    return true;
  case X86_INS_VPCMPUW:
    Spec = {0x3e, 2, true, false};
    return true;
  case X86_INS_VPCMPD:
    Spec = {0x1f, 4, false, true};
    return true;
  case X86_INS_VPCMPUD:
    Spec = {0x1e, 4, false, false};
    return true;
  case X86_INS_VPCMPQ:
    Spec = {0x1f, 8, true, true};
    return true;
  case X86_INS_VPCMPUQ:
    Spec = {0x1e, 8, true, false};
    return true;
  default:
    return false;
  }
}

bool isVectorRegisterOfSize(const cs_x86_op &Operand, uint16_t Size) {
  if (Operand.type != X86_OP_REG || Operand.size != Size)
    return false;
  if (Size == 16)
    return Operand.reg >= X86_REG_XMM0 && Operand.reg <= X86_REG_XMM31;
  if (Size == 32)
    return Operand.reg >= X86_REG_YMM0 && Operand.reg <= X86_REG_YMM31;
  if (Size == 64)
    return Operand.reg >= X86_REG_ZMM0 && Operand.reg <= X86_REG_ZMM31;
  return false;
}

unsigned vectorRegisterIndex(const cs_x86_op &Operand) {
  if (Operand.size == 16)
    return static_cast<unsigned>(Operand.reg - X86_REG_XMM0);
  if (Operand.size == 32)
    return static_cast<unsigned>(Operand.reg - X86_REG_YMM0);
  return static_cast<unsigned>(Operand.reg - X86_REG_ZMM0);
}

x86_avx_bcast broadcastForLaneCount(unsigned LaneCount) {
  switch (LaneCount) {
  case 2:
    return X86_AVX_BCAST_2;
  case 4:
    return X86_AVX_BCAST_4;
  case 8:
    return X86_AVX_BCAST_8;
  case 16:
    return X86_AVX_BCAST_16;
  default:
    return X86_AVX_BCAST_INVALID;
  }
}

bool beginsWithPotentialEvexPrefix(const cs_insn *Insn) {
  if (!Insn)
    return false;
  size_t Offset = 0;
  while (Offset < Insn->size) {
    switch (Insn->bytes[Offset]) {
    case 0xf0:
    case 0xf2:
    case 0xf3:
    case 0x2e:
    case 0x36:
    case 0x3e:
    case 0x26:
    case 0x64:
    case 0x65:
    case 0x66:
    case 0x67:
      ++Offset;
      continue;
    default:
      return Insn->bytes[Offset] == 0x62;
    }
  }
  return false;
}

struct EvexFPCompareSpec {
  uint8_t Prefix = 0;
  uint16_t ElementSize = 0;
  bool W = false;
  bool Scalar = false;
};

bool getEvexFPCompareSpec(unsigned InsnId, EvexFPCompareSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VCMPPS:
    Spec = {0, 4, false, false};
    return true;
  case X86_INS_VCMPPD:
    Spec = {1, 8, true, false};
    return true;
  case X86_INS_VCMPSS:
    Spec = {2, 4, false, true};
    return true;
  case X86_INS_VCMPSD:
    Spec = {3, 8, true, true};
    return true;
  default:
    return false;
  }
}

bool liftEvexFPCompare(X86Lifter &L, X86Lifter::LiftState &S,
                       const cs_insn *Insn, const cs_x86 &X86) {
  EvexFPCompareSpec Spec;
  if (!Insn || !getEvexFPCompareSpec(Insn->id, Spec))
    return false;

  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x07) != 0x01 || (Encoding.P0 & 0x90) != 0x90 ||
      (Encoding.P1 & 0x87) !=
          static_cast<uint8_t>((Spec.W ? 0x80 : 0) | 0x04 | Spec.Prefix) ||
      Encoding.Opcode != 0xc2 || X86.encoding.imm_size != 1 ||
      X86.encoding.imm_offset != Insn->size - 1 || (Encoding.P2 & 0x80) != 0 ||
      X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const bool HasWriteMask =
      X86.op_count >= 4 && isX86OpmaskOperand(X86.operands[1]);
  const unsigned BaseOperandCount = HasWriteMask ? 4 : 3;
  const bool HasImmediate = X86.op_count == BaseOperandCount + 1 &&
                            X86.operands[X86.op_count - 1].type == X86_OP_IMM;
  if ((X86.op_count != BaseOperandCount && !HasImmediate) ||
      X86.op_count > BaseOperandCount + 1)
    return false;

  const unsigned FirstSourceIndex = HasWriteMask ? 2 : 1;
  const unsigned SecondSourceIndex = FirstSourceIndex + 1;
  const cs_x86_op &DestinationOperand = X86.operands[0];
  const cs_x86_op &FirstSourceOperand = X86.operands[FirstSourceIndex];
  const cs_x86_op &SecondSourceOperand = X86.operands[SecondSourceIndex];
  const uint8_t RawImmediate = Insn->bytes[Insn->size - 1];
  if (HasImmediate) {
    const cs_x86_op &ImmediateOperand = X86.operands[X86.op_count - 1];
    if (ImmediateOperand.size != 1 ||
        static_cast<uint8_t>(ImmediateOperand.imm) != RawImmediate)
      return false;
  }

  const bool MemoryForm = SecondSourceOperand.type == X86_OP_MEM;
  if (MemoryForm != ((Encoding.ModRM & 0xc0) != 0xc0))
    return false;
  // EVEX.P0.B4 extends an APX memory base, but cannot name a vector register
  // beyond ZMM31 in a ModRM register form.
  if (!MemoryForm && (Encoding.P0 & 0x08) != 0)
    return false;
  const bool EncodedB = (Encoding.P2 & 0x10) != 0;
  const bool Broadcast = MemoryForm && EncodedB;
  const bool SuppressExceptions = !MemoryForm && EncodedB;
  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  if ((MemoryForm && Spec.Scalar && EncodedB) ||
      (!Spec.Scalar && !SuppressExceptions && EncodedLength == 0x60) ||
      X86.avx_sae != SuppressExceptions)
    return false;

  // Packed register SAE has fixed 512-bit semantics and ignores encoded L'L.
  // Scalar forms always use XMM inputs and likewise ignore L'L.
  const uint16_t VectorSize =
      Spec.Scalar || SuppressExceptions
          ? static_cast<uint16_t>(Spec.Scalar ? 16 : 64)
          : static_cast<uint16_t>(EncodedLength == 0      ? 16
                                  : EncodedLength == 0x20 ? 32
                                                          : 64);
  const unsigned LaneCount = Spec.Scalar ? 1 : VectorSize / Spec.ElementSize;
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
  const uint16_t MemoryTupleSize =
      Spec.Scalar || Broadcast ? Spec.ElementSize : VectorSize;

  if (!isX86OpmaskOperand(DestinationOperand) ||
      DestinationOperand.size != MaskSize ||
      !isVectorRegisterOfSize(FirstSourceOperand, VectorSize) ||
      ((Encoding.ModRM >> 3) & 7) !=
          static_cast<unsigned>(DestinationOperand.reg - X86_REG_K0) ||
      decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
          vectorRegisterIndex(FirstSourceOperand))
    return false;

  if (MemoryForm) {
    if (SecondSourceOperand.size != MemoryTupleSize ||
        !validateCanonicalEvexMemoryTail(Insn, X86, Encoding,
                                         SecondSourceOperand, MemoryTupleSize,
                                         1))
      return false;
  } else if (!isVectorRegisterOfSize(SecondSourceOperand, VectorSize) ||
             decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
                 vectorRegisterIndex(SecondSourceOperand) ||
             !validateCanonicalEvexRegisterTail(Insn, X86, Encoding, 1)) {
    return false;
  }

  if (L.targetArch() == Arch::X86 &&
      (vectorRegisterIndex(FirstSourceOperand) >= 8 ||
       (!MemoryForm && vectorRegisterIndex(SecondSourceOperand) >= 8)))
    return false;

  const uint8_t EncodedMask = Encoding.P2 & 7;
  if (HasWriteMask) {
    const cs_x86_op &MaskOperand = X86.operands[1];
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(MaskOperand.reg));
    if (MaskOperand.reg == X86_REG_K0 || MaskOperand.size != MaskSize ||
        EncodedMask != MaskOperand.reg - X86_REG_K0 ||
        MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < MaskSize)
      return false;
  } else if (EncodedMask != 0) {
    return false;
  }

  const x86_avx_bcast ExpectedBroadcast =
      Broadcast ? broadcastForLaneCount(LaneCount) : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    if (&Operand == &SecondSourceOperand) {
      if (Operand.avx_bcast != ExpectedBroadcast)
        return false;
    } else if (Operand.avx_bcast != X86_AVX_BCAST_INVALID) {
      return false;
    }
    // A K-result compare always zeros masked destination bits; EVEX.z has no
    // independent architectural meaning for this instruction family.
    if (Operand.avx_zero_opmask)
      return false;
  }

  NdVar ActiveMask = NdVar::cst(
      LaneCount == 64 ? UINT64_MAX : ((UINT64_C(1) << LaneCount) - 1),
      MaskSize);
  if (HasWriteMask) {
    const RegInfo MaskInfo = mapCapstoneReg(
        static_cast<x86_reg>(X86.operands[1].reg));
    ActiveMask = NdVar::reg(MaskInfo.Offset, MaskSize);
  }
  if (Spec.Scalar && HasWriteMask) {
    NdVar LowBit = S.makeTemp(1);
    S.emit(NdOp::INT_AND, LowBit,
           {ActiveMask, NdVar::cst(1, ActiveMask.Size)});
    ActiveMask = LowBit;
  }

  const NdVar FirstSource = L.operandRead(S, FirstSourceOperand);
  const NdVar SecondSource =
      MemoryForm
          ? emitEvexMaskedMemoryLoad(S, SecondSourceOperand, ActiveMask,
                                     VectorSize, Spec.ElementSize,
                                     MemoryTupleSize, Broadcast)
          : L.operandRead(S, SecondSourceOperand);
  if (FirstSource.Size != VectorSize || SecondSource.Size != VectorSize ||
      ActiveMask.Size != MaskSize)
    return false;

  const uint8_t Control = makeX86FPCompareControl(
      Spec.ElementSize == 8, Spec.Scalar, SuppressExceptions);
  NdVar Compared = S.makeTemp(MaskSize);
  S.emitIntrinsic(Intrinsic::X86FPCompare, Compared,
                  {NdVar::cst(Control, 1), FirstSource, SecondSource, ActiveMask,
                   NdVar::cst(RawImmediate, 1)});

  const RegInfo DestinationInfo =
      mapCapstoneReg(static_cast<x86_reg>(DestinationOperand.reg));
  if (DestinationInfo.Offset == UINT64_C(0xffff) || DestinationInfo.Size != 8)
    return false;
  S.emit(NdOp::INT_ZEXT,
         NdVar::reg(DestinationInfo.Offset, DestinationInfo.Size), {Compared});
  return true;
}

} // namespace

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
    S.emit(NdOp::COPY, NdVar::reg(x86reg::AF, 1), {NdVar::cst(0, 1)});
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
    if (beginsWithPotentialEvexPrefix(Insn))
      return liftEvexFPCompare(L, S, Insn, X86);
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

  // AVX-512 packed integer compare into an opmask. Validate the complete EVEX
  // shape before emitting IR: the writemask controls both destination bits and
  // memory fault suppression, while a broadcast source performs at most one
  // scalar access when any selected lane is active.
  case X86_INS_VPCMP:
    return false;
  case X86_INS_VPCMPB:
  case X86_INS_VPCMPD:
  case X86_INS_VPCMPQ:
  case X86_INS_VPCMPUB:
  case X86_INS_VPCMPUD:
  case X86_INS_VPCMPUQ:
  case X86_INS_VPCMPW:
  case X86_INS_VPCMPUW: {
    EvexPackedCompareSpec Spec;
    if (!getEvexPackedCompareSpec(InsnId, Spec))
      return false;

    const bool HasWriteMask =
        X86.op_count >= 4 && isX86OpmaskOperand(X86.operands[1]);
    const unsigned BaseOperandCount = HasWriteMask ? 4 : 3;
    const bool HasImmediate = X86.op_count == BaseOperandCount + 1 &&
                              X86.operands[X86.op_count - 1].type == X86_OP_IMM;
    if ((X86.op_count != BaseOperandCount && !HasImmediate) || X86.avx_sae ||
        X86.avx_rm != X86_AVX_RM_INVALID)
      return false;

    CanonicalEvexEncodingInfo Encoding;
    if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
        (Encoding.P0 & 0x07) != 0x03 || (Encoding.P0 & 0x90) != 0x90 ||
        ((Encoding.P1 | 0x04) & 0x87) != (Spec.W ? 0x85 : 0x05) ||
        Encoding.Opcode != Spec.Opcode ||
        X86.encoding.imm_offset != Insn->size - 1 ||
        X86.encoding.imm_size != 1 || (Encoding.P2 & 0x80) != 0)
      return false;

    const uint8_t RawImmediate = Insn->bytes[Insn->size - 1];
    unsigned Predicate = 8;
    if (X86.avx_cc >= X86_AVX_CC_EQ && X86.avx_cc <= X86_AVX_CC_ORD)
      Predicate = static_cast<unsigned>(X86.avx_cc - X86_AVX_CC_EQ);
    else if (X86.avx_cc != X86_AVX_CC_INVALID)
      return false;
    if (HasImmediate) {
      const cs_x86_op &Immediate = X86.operands[X86.op_count - 1];
      if (Immediate.size != 1 ||
          static_cast<uint8_t>(Immediate.imm) != RawImmediate)
        return false;
      const unsigned ImmediatePredicate = RawImmediate & 7;
      if (Predicate != 8 && Predicate != ImmediatePredicate)
        return false;
      Predicate = ImmediatePredicate;
    }
    if (Predicate >= 8 || (RawImmediate & 7) != Predicate)
      return false;

    const cs_x86_op &DstOp = X86.operands[0];
    const cs_x86_op &AOp = X86.operands[HasWriteMask ? 2 : 1];
    const cs_x86_op &BOp = X86.operands[HasWriteMask ? 3 : 2];
    const uint8_t EncodedLength = Encoding.P2 & 0x60;
    if (EncodedLength == 0x60)
      return false;
    const uint16_t VectorSize = EncodedLength == 0      ? 16
                                : EncodedLength == 0x20 ? 32
                                                        : 64;
    const bool MemoryForm = BOp.type == X86_OP_MEM;
    const bool Broadcast = (Encoding.P2 & 0x10) != 0;
    if (!isX86OpmaskOperand(DstOp) ||
        !isVectorRegisterOfSize(AOp, VectorSize) ||
        ((Encoding.ModRM >> 3) & 7) !=
            static_cast<unsigned>(DstOp.reg - X86_REG_K0) ||
        decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
            vectorRegisterIndex(AOp))
      return false;

    if (MemoryForm) {
      if ((Encoding.ModRM & 0xc0) == 0xc0 ||
          (Broadcast && Spec.ElementSize < 4) ||
          BOp.size != (Broadcast ? Spec.ElementSize : VectorSize) ||
          !validateCanonicalEvexMemoryTail(
              Insn, X86, Encoding, BOp,
              Broadcast ? Spec.ElementSize : VectorSize, 1))
        return false;
    } else if (!isVectorRegisterOfSize(BOp, VectorSize) || Broadcast ||
               decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
                   vectorRegisterIndex(BOp) ||
               !validateCanonicalEvexRegisterTail(Insn, X86, Encoding, 1)) {
      return false;
    }

    const unsigned Lanes = VectorSize / Spec.ElementSize;
    const uint16_t MaskSize = static_cast<uint16_t>((Lanes + 7) / 8);
    if (DstOp.size != MaskSize ||
        (HasWriteMask && X86.operands[1].size != MaskSize))
      return false;

    const uint8_t EncodedMask = Encoding.P2 & 7;
    if (HasWriteMask) {
      const cs_x86_op &Mask = X86.operands[1];
      if (!isX86OpmaskOperand(Mask) || Mask.reg == X86_REG_K0 ||
          EncodedMask != static_cast<uint8_t>(Mask.reg - X86_REG_K0))
        return false;
    } else if (EncodedMask != 0) {
      return false;
    }

    const x86_avx_bcast ExpectedBroadcast =
        Broadcast ? broadcastForLaneCount(Lanes) : X86_AVX_BCAST_INVALID;
    if (BOp.avx_bcast != ExpectedBroadcast)
      return false;
    for (uint8_t Index = 0; Index < X86.op_count; ++Index) {
      const cs_x86_op &Operand = X86.operands[Index];
      if (&Operand != &BOp && Operand.avx_bcast != X86_AVX_BCAST_INVALID)
        return false;
      if (Operand.avx_zero_opmask)
        return false;
    }

    const RegInfo DestinationInfo =
        mapCapstoneReg(static_cast<x86_reg>(DstOp.reg));
    if (DestinationInfo.Size != 8)
      return false;
    const NdVar Dst = NdVar::reg(DestinationInfo.Offset, DestinationInfo.Size);
    NdVar A = L.operandRead(S, AOp);
    NdVar ActiveMask = NdVar::cst(
        Lanes == 64 ? UINT64_MAX : ((UINT64_C(1) << Lanes) - 1), MaskSize);
    if (HasWriteMask)
      ActiveMask = L.operandRead(S, X86.operands[1]);
    NdVar B = MemoryForm
                  ? emitEvexMaskedMemoryLoad(
                        S, BOp, ActiveMask, VectorSize, Spec.ElementSize,
                        Broadcast ? Spec.ElementSize : VectorSize, Broadcast)
                  : L.operandRead(S, BOp);
    if (A.Size != VectorSize || B.Size != VectorSize ||
        ActiveMask.Size != MaskSize)
      return false;

    NdVar Packed = NdVar::cst(0, MaskSize);
    for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
      NdVar LaneA = S.makeTemp(Spec.ElementSize);
      NdVar LaneB = S.makeTemp(Spec.ElementSize);
      S.emit(NdOp::SUBBYTES, LaneA,
             {A, NdVar::cst(Lane * Spec.ElementSize, 4)});
      S.emit(NdOp::SUBBYTES, LaneB,
             {B, NdVar::cst(Lane * Spec.ElementSize, 4)});
      NdVar Matches = S.makeTemp(1);
      switch (Predicate) {
      case 0: // EQ
        S.emit(NdOp::INT_EQUAL, Matches, {LaneA, LaneB});
        break;
      case 1: // LT
        S.emit(Spec.Signed ? NdOp::INT_SLESS : NdOp::INT_LESS, Matches,
               {LaneA, LaneB});
        break;
      case 2: // LE
        S.emit(Spec.Signed ? NdOp::INT_SLESSEQUAL : NdOp::INT_LESSEQUAL,
               Matches, {LaneA, LaneB});
        break;
      case 3: // FALSE
        S.emit(NdOp::COPY, Matches, {NdVar::cst(0, 1)});
        break;
      case 4: // NEQ
        S.emit(NdOp::INT_NOTEQUAL, Matches, {LaneA, LaneB});
        break;
      case 5: // NLT
        S.emit(Spec.Signed ? NdOp::INT_SLESSEQUAL : NdOp::INT_LESSEQUAL,
               Matches, {LaneB, LaneA});
        break;
      case 6: // NLE
        S.emit(Spec.Signed ? NdOp::INT_SLESS : NdOp::INT_LESS, Matches,
               {LaneB, LaneA});
        break;
      case 7: // TRUE
        S.emit(NdOp::COPY, Matches, {NdVar::cst(1, 1)});
        break;
      default:
        return false;
      }
      NdVar Bit = S.makeTemp(MaskSize);
      S.emit(NdOp::INT_ZEXT, Bit, {Matches});
      if (Lane != 0) {
        NdVar Shifted = S.makeTemp(MaskSize);
        S.emit(NdOp::INT_LEFT, Shifted, {Bit, NdVar::cst(Lane, MaskSize)});
        Bit = Shifted;
      }
      NdVar Next = S.makeTemp(MaskSize);
      S.emit(NdOp::INT_OR, Next, {Packed, Bit});
      Packed = Next;
    }
    if (HasWriteMask) {
      NdVar Masked = S.makeTemp(MaskSize);
      S.emit(NdOp::INT_AND, Masked, {Packed, ActiveMask});
      Packed = Masked;
    }
    if (Packed.Size == Dst.Size) {
      S.emit(NdOp::COPY, Dst, {Packed});
    } else {
      NdVar Cleared = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ZEXT, Cleared, {Packed});
      S.emit(NdOp::COPY, Dst, {Cleared});
    }
    break;
  }

  // PTEST / VPTEST — set ZF/CF based on bit test.
  case X86_INS_PTEST:
  case X86_INS_VPTEST: {
    if (X86.op_count < 2)
      return false;
    NdVar A = L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[1]);
    if (A.Size == 0 || A.Size != B.Size || A.Size % 8 != 0)
      return false;

    NdVar AllMaskedZero = NdVar::cst(1, 1);
    NdVar AllAndNotZero = NdVar::cst(1, 1);
    for (unsigned Offset = 0; Offset < A.Size; Offset += 8) {
      NdVar APart = S.makeTemp(8);
      NdVar BPart = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, APart, {A, NdVar::cst(Offset, 4)});
      S.emit(NdOp::SUBBYTES, BPart, {B, NdVar::cst(Offset, 4)});

      NdVar Masked = S.makeTemp(8);
      S.emit(NdOp::INT_AND, Masked, {APart, BPart});
      NdVar MaskedZero = S.makeTemp(1);
      S.emit(NdOp::INT_EQUAL, MaskedZero, {Masked, NdVar::cst(0, 8)});
      NdVar NextMaskedZero = S.makeTemp(1);
      S.emit(NdOp::BOOL_AND, NextMaskedZero, {AllMaskedZero, MaskedZero});
      AllMaskedZero = NextMaskedZero;

      NdVar NegA = S.makeTemp(8);
      S.emit(NdOp::INT_NOT, NegA, {APart});
      NdVar AndNot = S.makeTemp(8);
      S.emit(NdOp::INT_AND, AndNot, {NegA, BPart});
      NdVar AndNotZero = S.makeTemp(1);
      S.emit(NdOp::INT_EQUAL, AndNotZero, {AndNot, NdVar::cst(0, 8)});
      NdVar NextAndNotZero = S.makeTemp(1);
      S.emit(NdOp::BOOL_AND, NextAndNotZero, {AllAndNotZero, AndNotZero});
      AllAndNotZero = NextAndNotZero;
    }
    S.emit(NdOp::COPY, NdVar::reg(x86reg::ZF, 1), {AllMaskedZero});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {AllAndNotZero});
    for (uint64_t Flag : {x86reg::OF, x86reg::SF, x86reg::AF, x86reg::PF})
      S.emit(NdOp::COPY, NdVar::reg(Flag, 1), {NdVar::cst(0, 1)});
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
