//===- X86LiftLegacyExt.cpp - x86 minor ISA extension lifter --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Small, mostly vendor-specific extensions: 3DNow!, TBM,
/// CET, MOVDIRI/MOVDIR64B, PTWRITE, VIA PadLock, the 64-bit
/// xsave variants, GFNI, LWP, CLAC/STAC, CLZERO, the bare
/// prefix opcodes, MMX/SSE4a data moves and the remaining
/// system and MXCSR instructions.
///
//===----------------------------------------------------------------------===//

#include "X86FPEncoding.h"
#include "X86LiftDetail.h"
#include "X86LiftTBM.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/X86/X86SegmentedBlock.h"
#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/support/ISAEncoding.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

bool liftCacheZero(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X) {
  if (!Insn->detail || Insn->size < 3 || Insn->size > x86::kMaxInstructionLen ||
      X.op_count || X.eflags || X.encoding.imm_size || X.encoding.imm_offset ||
      X.encoding.disp_size || X.encoding.disp_offset ||
      Insn->detail->regs_read_count || Insn->detail->regs_write_count ||
      Insn->detail->groups_count)
    return false;
  const bool Mode64 = L.targetArch() == Arch::X64;
  uint8_t Segment = 0, Rex = 0;
  bool OperandOverride = false, AddressOverride = false;
  const auto OpcodeOffset = Insn->size - 3;
  for (unsigned I = 0; I < OpcodeOffset; ++I) {
    const uint8_t Byte = Insn->bytes[I];
    if (Mode64 && Byte >= 0x40 && Byte <= 0x4f) {
      Rex = Byte;
      continue;
    }
    Rex = 0;
    if (Byte == 0x66)
      OperandOverride = true;
    else if (Byte == 0x67)
      AddressOverride = true;
    else if (Byte == 0x3e || Byte == 0x64 || Byte == 0x65)
      Segment = Byte;
    else
      // Other segment identities and refining prefixes need their own
      // architectural contract; do not silently turn them into default DS.
      return false;
  }
  const unsigned AddressSize =
      Mode64 ? (AddressOverride ? 4 : 8) : (AddressOverride ? 2 : 4);
  if (X.prefix[0] || X.prefix[1] != Segment ||
      X.prefix[2] != (OperandOverride ? 0x66 : 0) ||
      X.prefix[3] != (AddressOverride ? 0x67 : 0) || X.rex != Rex ||
      X.addr_size != AddressSize || S.AddressSize != AddressSize ||
      X.opcode[0] != 0x0f || X.opcode[1] != 0x01 || X.opcode[2] ||
      X.opcode[3] || X.modrm != 0xfc ||
      X.encoding.modrm_offset != OpcodeOffset + 2 ||
      Insn->bytes[OpcodeOffset] != 0x0f ||
      Insn->bytes[OpcodeOffset + 1] != 0x01 ||
      Insn->bytes[OpcodeOffset + 2] != 0xfc)
    return false;
  // Capstone supplies no explicit operand or register-read entry. The
  // address-size-selected AX/EAX/RAX is nevertheless the instruction input.
  NdVar Address = NdVar::reg(x86reg::RAX, AddressSize);
  if (AddressSize != 8) {
    const auto Extended = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, Extended, {Address});
    Address = Extended;
  }
  const auto Space = Segment == 0x64   ? NdMemoryAddressSpace::X86FS
                     : Segment == 0x65 ? NdMemoryAddressSpace::X86GS
                                       : NdMemoryAddressSpace::Default;
  S.emitIntrinsic(Intrinsic::Clzero, {}, {Address}, NdMemoryOrdering::None,
                  Space);
  return true;
}

bool liftSegmentedBlock32(X86Lifter &L, X86Lifter::LiftState &S,
                          const cs_insn *Insn, const cs_x86 &X86,
                          X86SegmentedBlockKind Kind) {
  FPEncodingInfo E;
  const unsigned PP = Kind == X86SegmentedBlockKind::DirectStore ? 1
                      : Kind == X86SegmentedBlockKind::Enqueue   ? 3
                                                                 : 2;
  // The pinned handwritten ENQ decoder retains REP in prefix[0] and all
  // three opcode bytes. Authenticate that detail before adapting it to the
  // generated legacy layout consumed by the common byte/detail validator.
  cs_x86 Detail = X86;
  if (Kind != X86SegmentedBlockKind::DirectStore) {
    const auto Prefix = Kind == X86SegmentedBlockKind::Enqueue ? 0xf2 : 0xf3;
    if (X86.prefix[0] != Prefix || X86.opcode[0] != 0x0f ||
        X86.opcode[1] != 0x38 || X86.opcode[2] != 0xf8 || X86.opcode[3] != 0)
      return false;
    Detail.prefix[0] = 0;
    Detail.opcode[1] = 0xf8;
    Detail.opcode[2] = 0;
  }
  if (!parseFPEncoding(Insn, Detail, Arch::X86, false, 2, PP, E) ||
      E.Opcode != 0xf8 || X86.op_count != 2 ||
      !matchesFPGPROperand(X86.operands[0], E.gprRegIndex(), E.AddressSize) ||
      X86.operands[1].type != X86_OP_MEM || X86.operands[1].size != 64 ||
      !validateFPEncodingTail(Insn, Detail, E, X86.operands[1]))
    return false;
  const auto &Memory = X86.operands[1];
  X86BlockSegment Segment;
  switch (Memory.mem.segment) {
  case X86_REG_ES:
    Segment = X86BlockSegment::ES;
    break;
  case X86_REG_CS:
    Segment = X86BlockSegment::CS;
    break;
  case X86_REG_SS:
    Segment = X86BlockSegment::SS;
    break;
  case X86_REG_DS:
    Segment = X86BlockSegment::DS;
    break;
  case X86_REG_FS:
    Segment = X86BlockSegment::FS;
    break;
  case X86_REG_GS:
    Segment = X86BlockSegment::GS;
    break;
  case X86_REG_INVALID:
    Segment = Memory.mem.base == X86_REG_BP || Memory.mem.base == X86_REG_EBP ||
                      Memory.mem.base == X86_REG_ESP
                  ? X86BlockSegment::SS
                  : X86BlockSegment::DS;
    break;
  default:
    return false;
  }
  const auto SourceEA = S.computeEA(Memory, true, true);
  auto Source = S.makeTemp(4);
  S.emit(NdOp::SUBBYTES, Source, {SourceEA, NdVar::scalar(0, 1)});
  auto Destination = L.operandRead(S, X86.operands[0]);
  if (Destination.Size == 2) {
    auto Extended = S.makeTemp(4);
    S.emit(NdOp::INT_ZEXT, Extended, {Destination});
    Destination = Extended;
  }
  const auto Selector =
      x86SegmentedBlockSelector(Kind, Segment, E.AddressSize == 2);
  const bool HasFlag = x86SegmentedBlockHasFlag(Selector);
  S.emitIntrinsic(Intrinsic::X86SegmentedBlockMemory,
                  HasFlag ? NdVar::reg(x86reg::ZF, 1) : NdVar{},
                  {NdVar::scalar(Selector, 2), Source, Destination});
  if (HasFlag)
    for (uint64_t Flag :
         {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::SF, x86reg::OF})
      S.emit(NdOp::COPY, NdVar::reg(Flag, 1), {NdVar::scalar(0, 1)});
  return true;
}

bool beginsWithPotentialEvexPrefix(const cs_insn *Insn) {
  if (!Insn)
    return false;
  size_t Offset = 0;
  while (Offset < Insn->size) {
    const uint8_t Byte = Insn->bytes[Offset];
    if (Byte == 0x62)
      return true;
    if (Byte != 0x26 && Byte != 0x2e && Byte != 0x36 && Byte != 0x3e &&
        Byte != 0x64 && Byte != 0x65 && Byte != 0x66 && Byte != 0x67 &&
        Byte != 0xf0 && Byte != 0xf2 && Byte != 0xf3)
      return false;
    ++Offset;
  }
  return false;
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

bool isEvexGfniCandidate(const cs_insn *Insn, const cs_x86 &X86) {
  return beginsWithPotentialEvexPrefix(Insn) || X86.opcode[0] == 0x62 ||
         (X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]));
}

bool liftEvexGfni(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                  const cs_x86 &X86, Intrinsic Id, bool IsMul) {
  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding))
    return false;

  const uint8_t ExpectedMap = IsMul ? 2 : 3;
  const uint8_t ExpectedP1 = IsMul ? 0x05 : 0x85;
  const uint8_t ExpectedOpcode = Id == Intrinsic::Gf2p8AffineQb ? 0xce : 0xcf;
  if ((Encoding.P0 & 0x08) != 0 || (Encoding.P0 & 0x07) != ExpectedMap ||
      (Encoding.P1 & 0x87) != ExpectedP1 || Encoding.Opcode != ExpectedOpcode ||
      (Encoding.P2 & 0x60) == 0x60 || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const bool HasMask = X86.op_count == (IsMul ? 4 : 5);
  if ((!HasMask && X86.op_count != (IsMul ? 3 : 4)) ||
      (HasMask && !isX86OpmaskOperand(X86.operands[1])))
    return false;

  const unsigned Source1Index = HasMask ? 2 : 1;
  const unsigned Source2Index = Source1Index + 1;
  const unsigned ImmediateIndex = Source2Index + 1;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &Source1 = X86.operands[Source1Index];
  const cs_x86_op &Source2 = X86.operands[Source2Index];
  const bool RegisterSource = Source2.type == X86_OP_REG;
  const bool MemorySource = Source2.type == X86_OP_MEM;
  const bool Broadcast = MemorySource && (Encoding.P2 & 0x10) != 0;
  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  const uint16_t VectorSize = EncodedLength == 0      ? 16
                              : EncodedLength == 0x20 ? 32
                                                      : 64;
  if (!isVectorRegisterOfSize(Destination, VectorSize) ||
      !isVectorRegisterOfSize(Source1, VectorSize) ||
      (!RegisterSource && !MemorySource) ||
      (RegisterSource && !isVectorRegisterOfSize(Source2, VectorSize)) ||
      (MemorySource && Source2.size != (Broadcast ? 8 : VectorSize)) ||
      (IsMul && Broadcast))
    return false;

  const unsigned DestinationRegister = vectorRegisterIndex(Destination);
  const unsigned Source1Register = vectorRegisterIndex(Source1);
  if (Encoding.regIndex() != DestinationRegister ||
      Encoding.vvvvIndex() != Source1Register ||
      (L.targetArch() == Arch::X86 &&
       (DestinationRegister >= 8 || Source1Register >= 8)))
    return false;
  if (RegisterSource) {
    const unsigned Source2Register = vectorRegisterIndex(Source2);
    if ((Encoding.ModRM & 0xc0) != 0xc0 || (Encoding.P2 & 0x10) != 0 ||
        Encoding.rmIndex() != Source2Register ||
        (L.targetArch() == Arch::X86 && Source2Register >= 8))
      return false;
  } else if ((Encoding.ModRM & 0xc0) == 0xc0) {
    return false;
  }

  const size_t TrailingBytes = IsMul ? 0 : 1;
  if (IsMul) {
    if (X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
        (RegisterSource
             ? !validateCanonicalEvexRegisterTail(Insn, X86, Encoding)
             : !validateCanonicalEvexMemoryTail(Insn, X86, Encoding, Source2,
                                                VectorSize)))
      return false;
  } else {
    const cs_x86_op &Immediate = X86.operands[ImmediateIndex];
    if (X86.encoding.imm_size != 1 ||
        X86.encoding.imm_offset != Insn->size - 1 ||
        Immediate.type != X86_OP_IMM || Immediate.size != 1 ||
        static_cast<uint8_t>(Immediate.imm) != Insn->bytes[Insn->size - 1] ||
        (RegisterSource
             ? !validateCanonicalEvexRegisterTail(Insn, X86, Encoding,
                                                  TrailingBytes)
             : !validateCanonicalEvexMemoryTail(
                   Insn, X86, Encoding, Source2,
                   Broadcast ? uint16_t{8} : VectorSize, TrailingBytes)))
      return false;
  }

  const uint8_t EncodedMask = Encoding.P2 & 7;
  const bool EncodedZero = (Encoding.P2 & 0x80) != 0;
  const uint16_t MaskSize = VectorSize / 8;
  const uint64_t ActiveBits =
      VectorSize == 64 ? UINT64_MAX : ((UINT64_C(1) << VectorSize) - 1);
  NdVar ActiveMask = NdVar::cst(ActiveBits, MaskSize);
  if (HasMask) {
    const cs_x86_op &Mask = X86.operands[1];
    const RegInfo MaskInfo = mapCapstoneReg(static_cast<x86_reg>(Mask.reg));
    if (Mask.reg == X86_REG_K0 ||
        EncodedMask != static_cast<uint8_t>(Mask.reg - X86_REG_K0) ||
        Mask.size != MaskSize || MaskInfo.Offset == UINT64_C(0xffff) ||
        MaskInfo.Size < MaskSize ||
        EncodedZero != static_cast<bool>(Mask.avx_zero_opmask))
      return false;
    ActiveMask = NdVar::reg(MaskInfo.Offset, MaskSize);
  } else if (EncodedMask != 0 || EncodedZero) {
    return false;
  }

  const x86_avx_bcast ExpectedBroadcast = VectorSize == 16   ? X86_AVX_BCAST_2
                                          : VectorSize == 32 ? X86_AVX_BCAST_4
                                                             : X86_AVX_BCAST_8;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const x86_avx_bcast OperandBroadcast = X86.operands[Index].avx_bcast;
    if ((Broadcast && Index == Source2Index
             ? OperandBroadcast != ExpectedBroadcast
             : OperandBroadcast != X86_AVX_BCAST_INVALID) ||
        (X86.operands[Index].avx_zero_opmask && (!HasMask || Index != 1)))
      return false;
  }

  NdVar Left = L.operandRead(S, Source1);
  NdVar Right;
  if (RegisterSource) {
    Right = L.operandRead(S, Source2);
  } else if (IsMul) {
    Right = emitEvexMaskedMemoryLoad(S, Source2, ActiveMask, VectorSize, 1,
                                     VectorSize, false);
  } else if (Broadcast) {
    Right = emitEvexMaskedMemoryLoad(S, Source2, NdVar::cst(1, 1), VectorSize,
                                     8, 8, true);
  } else {
    Right = L.operandRead(S, Source2);
  }
  if (Left.Size != VectorSize || Right.Size != VectorSize)
    return false;
  NdVar Raw = S.makeTemp(VectorSize);
  if (IsMul) {
    S.emitIntrinsic(Id, Raw, {Left, Right});
  } else {
    const uint8_t Immediate =
        static_cast<uint8_t>(X86.operands[ImmediateIndex].imm);
    S.emitIntrinsic(Id, Raw, {Left, Right, NdVar::cst(Immediate, 1)});
  }

  if (HasMask)
    return emitMaskedVectorResult(L, S, Destination, X86.operands[1], Raw, 1);
  S.emit(NdOp::COPY, L.operandWrite(Destination), {Raw});
  return true;
}

} // namespace

bool liftLegacyExt(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // 3DNow! requires packed lanes, its own floating-point rules and the MMX/
  // x87 state transition. Neither scalar FLOAT_* nor COPY is that contract.
  // ========================================================================
  case X86_INS_PFADD:
  case X86_INS_PFACC:
  case X86_INS_PFNACC:
  case X86_INS_PFPNACC:
  case X86_INS_PFSUB:
  case X86_INS_PFSUBR:
  case X86_INS_PFMUL:
  case X86_INS_PFMAX:
  case X86_INS_PFMIN:
  case X86_INS_PFCMPEQ:
  case X86_INS_PFCMPGE:
  case X86_INS_PFCMPGT:
  case X86_INS_PFRCP:
  case X86_INS_PFRCPIT1:
  case X86_INS_PFRCPIT2:
  case X86_INS_PFRSQIT1:
  case X86_INS_PFRSQRT:
  case X86_INS_PF2ID:
  case X86_INS_PF2IW:
  case X86_INS_PI2FD:
  case X86_INS_PI2FW:
  case X86_INS_PSWAPD:
  case X86_INS_PMULHRW:
  case X86_INS_PAVGUSB:
    throw UnliftedInstruction(Insn->address, Insn->mnemonic, Insn->op_str);

  // ========================================================================
  // TBM — encoding, results and flags have one family owner.
  // ========================================================================
  case X86_INS_BLCFILL:
  case X86_INS_BLCI:
  case X86_INS_BLCIC:
  case X86_INS_BLCMSK:
  case X86_INS_BLCS:
  case X86_INS_BLSFILL:
  case X86_INS_BLSIC:
  case X86_INS_T1MSKC:
  case X86_INS_TZMSK:
    return tbm::lift(L, S, Insn);

  // ========================================================================
  // CET — Control-flow Enforcement (ENDBR, shadow stack, etc.).
  // ========================================================================
  case X86_INS_ENDBR32:
  case X86_INS_ENDBR64:
    S.emit(NdOp::NOP, {}, {});
    break;

  case X86_INS_INCSSPD:
  case X86_INS_INCSSPQ:
    S.emitIntrinsic(Intrinsic::CetIncSsp);
    break;
  case X86_INS_RDSSPD:
  case X86_INS_RDSSPQ: {
    const auto Refuse = [&]() -> bool {
      throw UnliftedInstruction(Insn->address, Insn->mnemonic, Insn->op_str);
    };
    const bool X64 = L.targetArch() == Arch::X64;
    const unsigned Width = InsnId == X86_INS_RDSSPQ ? 8 : 4;
    if ((L.targetArch() != Arch::X86 && !X64) || Insn->size == 0 ||
        Insn->size > 15 || (!X64 && Width == 8) || X86.op_count != 1 ||
        X86.operands[0].type != X86_OP_REG || X86.operands[0].size != Width)
      return Refuse();
    size_t Offset = 0;
    bool Repeat = false;
    uint8_t Rex = 0;
    while (Offset < Insn->size) {
      const uint8_t Byte = Insn->bytes[Offset];
      if (Byte == 0xf3) {
        Repeat = true;
        Rex = 0;
      } else if (Byte == 0x26 || Byte == 0x2e || Byte == 0x36 || Byte == 0x3e ||
                 Byte == 0x64 || Byte == 0x65 || Byte == 0x66 || Byte == 0x67) {
        Rex = 0;
      } else if (X64 && (Byte & 0xf0) == 0x40) {
        Rex = Byte;
      } else {
        break;
      }
      ++Offset;
    }
    if (!Repeat || Offset + 3 != Insn->size || Insn->bytes[Offset] != 0x0f ||
        Insn->bytes[Offset + 1] != 0x1e ||
        (Insn->bytes[Offset + 2] & 0xf8) != 0xc8 ||
        Width != (X64 && (Rex & 8) ? 8U : 4U))
      return Refuse();
    const unsigned Register =
        (Insn->bytes[Offset + 2] & 7) | ((Rex & 1) ? 8 : 0);
    const RegInfo Reg =
        mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
    if (Reg.Offset != x86reg::generalReg(Register) || Reg.Size != Width)
      return Refuse();
    const NdVar Full = NdVar::reg(Reg.Offset, X64 ? 8 : 4);
    S.emitIntrinsic(Intrinsic::CetRdSsp, Full, {Full, NdVar::cst(Width, 1)});
    break;
  }
  case X86_INS_SAVEPREVSSP:
    S.emitIntrinsic(Intrinsic::CetSaveprevssp);
    break;
  case X86_INS_RSTORSSP:
    S.emitIntrinsic(Intrinsic::CetRstorssp);
    break;
  case X86_INS_WRSSD:
  case X86_INS_WRSSQ:
  case X86_INS_WRUSSD:
  case X86_INS_WRUSSQ: {
    const bool Is64 = InsnId == X86_INS_WRSSQ || InsnId == X86_INS_WRUSSQ;
    const uint16_t Width = Is64 ? 8 : 4;
    if (X86.op_count != 2 || X86.operands[0].type != X86_OP_MEM ||
        X86.operands[1].type != X86_OP_REG || X86.operands[0].size != Width ||
        X86.operands[1].size != Width)
      return false;

    const NdVar Address = S.computeEA(X86.operands[0]);
    const NdVar Source = L.operandRead(S, X86.operands[1]);
    if (Address.Size != 8 || Source.Size != Width)
      return false;
    const NdMemoryAddressSpace AddressSpace =
        S.memoryAddressSpace(X86.operands[0]);
    // The opaque CET effect owns enablement, privilege, page type and
    // alignment checks in architectural order.  Moving alignment ahead of it
    // would expose #GP before the higher-priority #UD/CPL checks.
    S.emitIntrinsic(InsnId == X86_INS_WRSSD || InsnId == X86_INS_WRSSQ
                        ? Intrinsic::CetWrss
                        : Intrinsic::CetWruss,
                    NdVar(), {Address, Source}, NdMemoryOrdering::None,
                    AddressSpace);
    break;
  }
  case X86_INS_SETSSBSY:
    S.emitIntrinsic(Intrinsic::CetSetssbsy);
    break;
  case X86_INS_CLRSSBSY:
    S.emitIntrinsic(Intrinsic::CetClrssbsy);
    break;

  // MOVDIRI — scalar direct store.
  case X86_INS_MOVDIRI: {
    FPEncodingInfo Encoding;
    if (!parseFPEncoding(Insn, X86, L.targetArch(), false, 2, 0, Encoding) ||
        Encoding.Opcode != 0xf9 || X86.op_count != 2)
      return false;
    const uint16_t Width = Encoding.W ? 8 : 4;
    if (X86.operands[0].type != X86_OP_MEM || X86.operands[0].size != Width ||
        !matchesFPGPROperand(X86.operands[1], Encoding.gprRegIndex(), Width) ||
        !validateFPEncodingTail(Insn, X86, Encoding, X86.operands[0]))
      return false;
    const NdVar Address = S.computeEA(X86.operands[0]);
    const NdVar Source = L.operandRead(S, X86.operands[1]);
    if (Address.Size != 8 || Source.Size != Width)
      return false;
    // A plain STORE loses the WC transaction and write-completion atomicity.
    // Keep the actual memory operation, including unaligned stores, native.
    S.emitIntrinsic(Intrinsic::X86DirectStore, {}, {Address, Source},
                    NdMemoryOrdering::None,
                    S.memoryAddressSpace(X86.operands[0]));
    break;
  }

  // MOVDIR64B reads the complete m512 source before issuing one aligned,
  // 64-byte-atomic direct store to the address held in the register operand.
  // The register is an address source, not a destination register.
  case X86_INS_MOVDIR64B: {
    if (L.targetArch() == Arch::X86)
      return liftSegmentedBlock32(L, S, Insn, X86,
                                  X86SegmentedBlockKind::DirectStore);
    FPEncodingInfo Encoding;
    if (L.targetArch() != Arch::X64 ||
        !parseFPEncoding(Insn, X86, L.targetArch(), false, 2, 1, Encoding) ||
        Encoding.Opcode != 0xf8 || X86.op_count != 2 ||
        X86.operands[0].type != X86_OP_REG ||
        X86.operands[1].type != X86_OP_MEM || X86.operands[1].size != 64 ||
        !matchesFPGPROperand(X86.operands[0], Encoding.gprRegIndex(),
                             Encoding.AddressSize) ||
        !validateFPEncodingTail(Insn, X86, Encoding, X86.operands[1]))
      return false;

    NdVar Address = L.operandRead(S, X86.operands[0]);
    if (Address.Size != 4 && Address.Size != 8)
      return false;
    if (Address.Size != 8) {
      NdVar ExtendedAddress = S.makeTemp(8);
      S.emit(NdOp::INT_ZEXT, ExtendedAddress, {Address});
      Address = ExtendedAddress;
    }

    const NdVar SourceAddress = S.computeEA(X86.operands[1]);
    if (SourceAddress.Size != 8)
      return false;
    // The instruction owns feature checks, the complete source read, alignment
    // and the indivisible direct store. A masked ordinary store cannot express
    // its write-completion atomicity or memory-type protocol.
    S.emitIntrinsic(Intrinsic::X86DirectStore64B, {}, {SourceAddress, Address},
                    NdMemoryOrdering::None,
                    S.memoryAddressSpace(X86.operands[1]));
    break;
  }

  // ENQCMD/ENQCMDS own their complete ordered system-memory transaction.
  // Keeping the source as an address (rather than an eager LOAD plus a byte
  // snapshot) lets an authenticated concrete executor enforce the
  // PASID/CPL check before the 64-byte source read, then validate the command
  // header and portal before returning the enqueue completion in ZF.
  case X86_INS_ENQCMD:
  case X86_INS_ENQCMDS: {
    if (L.targetArch() == Arch::X86)
      return liftSegmentedBlock32(
          L, S, Insn, X86,
          InsnId == X86_INS_ENQCMD ? X86SegmentedBlockKind::Enqueue
                                   : X86SegmentedBlockKind::EnqueueSupervisor);
    if (L.targetArch() != Arch::X64 ||
        (S.AddressSize != 4 && S.AddressSize != 8) || X86.op_count != 2 ||
        X86.operands[0].type != X86_OP_REG ||
        X86.operands[1].type != X86_OP_MEM || X86.operands[1].size != 64)
      return false;

    NdVar PortalAddress = L.operandRead(S, X86.operands[0]);
    if (X86.operands[0].size != S.AddressSize ||
        PortalAddress.Size != S.AddressSize)
      return false;
    if (S.AddressSize == 4) {
      const NdVar ExtendedPortal = S.makeTemp(8);
      S.emit(NdOp::INT_ZEXT, ExtendedPortal, {PortalAddress});
      PortalAddress = ExtendedPortal;
    }
    const NdVar CommandAddress = S.computeEA(X86.operands[1]);
    if (PortalAddress.Size != 8 || CommandAddress.Size != 8)
      return false;
    const NdMemoryAddressSpace SourceAddressSpace =
        S.memoryAddressSpace(X86.operands[1]);
    S.emitIntrinsic(InsnId == X86_INS_ENQCMD ? Intrinsic::Enqcmd
                                             : Intrinsic::Enqcmds,
                    NdVar::reg(x86reg::ZF, 1), {CommandAddress, PortalAddress},
                    NdMemoryOrdering::None, SourceAddressSpace);
    for (uint64_t Flag :
         {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::SF, x86reg::OF})
      S.emit(NdOp::COPY, NdVar::reg(Flag, 1), {NdVar::scalar(0, 1)});
    break;
  }

  // PT — Processor Trace.
  case X86_INS_PTWRITE:
    S.emitIntrinsic(Intrinsic::Ptwrite, NdVar());
    break;

  // ========================================================================
  // VIA PadLock — hardware crypto acceleration (XCRYPT*, XSHA*, MONTMUL,
  // XSTORE).
  // ========================================================================
  case X86_INS_XCRYPTECB:
  case X86_INS_XCRYPTCBC:
  case X86_INS_XCRYPTCTR:
  case X86_INS_XCRYPTCFB:
  case X86_INS_XCRYPTOFB:
  case X86_INS_XSHA1:
  case X86_INS_XSHA256:
  case X86_INS_MONTMUL:
  case X86_INS_XSTORE: {
    Intrinsic Id;
    switch (InsnId) {
    case X86_INS_XCRYPTECB:
      Id = Intrinsic::Xcryptecb;
      break;
    case X86_INS_XCRYPTCBC:
      Id = Intrinsic::Xcryptcbc;
      break;
    case X86_INS_XCRYPTCTR:
      Id = Intrinsic::Xcryptctr;
      break;
    case X86_INS_XCRYPTCFB:
      Id = Intrinsic::Xcryptcfb;
      break;
    case X86_INS_XCRYPTOFB:
      Id = Intrinsic::Xcryptofb;
      break;
    case X86_INS_XSHA1:
      Id = Intrinsic::Xsha1;
      break;
    case X86_INS_XSHA256:
      Id = Intrinsic::Xsha256;
      break;
    case X86_INS_MONTMUL:
      Id = Intrinsic::Montmul;
      break;
    case X86_INS_XSTORE:
      Id = Intrinsic::Xstore;
      break;
    default:
      Id = Intrinsic::Xcryptecb;
      break;
    }
    S.emitIntrinsic(Id);
    for (uint64_t RO : {x86reg::RSI, x86reg::RDI, x86reg::RCX}) {
      S.emit(NdOp::COPY, NdVar::reg(RO, 8), {S.makeTemp(8)});
    }
    break;
  }

  // ========================================================================
  // xsave 64-bit variants.
  // ========================================================================
  case X86_INS_XSAVE64:
  case X86_INS_XRSTOR64:
  case X86_INS_XSAVES64:
  case X86_INS_XRSTORS64:
  case X86_INS_XSAVEC64:
  case X86_INS_XSAVEOPT64: {
    Intrinsic Id = Intrinsic::Xsave64;
    switch (InsnId) {
    case X86_INS_XRSTOR64:
      Id = Intrinsic::Xrstor64;
      break;
    case X86_INS_XSAVES64:
      Id = Intrinsic::Xsaves64;
      break;
    case X86_INS_XRSTORS64:
      Id = Intrinsic::Xrstors64;
      break;
    case X86_INS_XSAVEC64:
      Id = Intrinsic::Xsavec64;
      break;
    case X86_INS_XSAVEOPT64:
      Id = Intrinsic::Xsaveopt64;
      break;
    default:
      break;
    }
    if (X86.op_count < 1 ||
        !S.emitMemoryIntrinsic(
            Id, X86.operands[0],
            {NdVar::reg(x86reg::RAX, 4), NdVar::reg(x86reg::RDX, 4)}))
      return false;
    break;
  }

  // x87 misc — FNINIT/FNCLEX already handled above in x87 block.

  // ========================================================================
  // GFNI — Galois Field instructions.
  // ========================================================================
  case X86_INS_GF2P8AFFINEINVQB:
  case X86_INS_GF2P8AFFINEQB:
  case X86_INS_GF2P8MULB:
  case X86_INS_VGF2P8AFFINEINVQB:
  case X86_INS_VGF2P8AFFINEQB:
  case X86_INS_VGF2P8MULB: {
    const bool IsVex = InsnId == X86_INS_VGF2P8AFFINEINVQB ||
                       InsnId == X86_INS_VGF2P8AFFINEQB ||
                       InsnId == X86_INS_VGF2P8MULB;
    const bool IsMul =
        InsnId == X86_INS_GF2P8MULB || InsnId == X86_INS_VGF2P8MULB;
    Intrinsic Id = IsMul ? Intrinsic::Gf2p8MulB
                         : (InsnId == X86_INS_GF2P8AFFINEINVQB ||
                                    InsnId == X86_INS_VGF2P8AFFINEINVQB
                                ? Intrinsic::Gf2p8AffineInvQb
                                : Intrinsic::Gf2p8AffineQb);
    if (IsVex && isEvexGfniCandidate(Insn, X86))
      return liftEvexGfni(L, S, Insn, X86, Id, IsMul);

    const unsigned RequiredOps = IsMul ? (IsVex ? 3 : 2) : (IsVex ? 4 : 3);
    if (X86.op_count < RequiredOps)
      break;

    NdVar Dst = L.operandWrite(X86.operands[0]);
    const unsigned SrcIdx = IsVex ? 1 : 0;
    NdVar Src1 = L.operandRead(S, X86.operands[SrcIdx]);
    NdVar Src2 = L.operandRead(S, X86.operands[SrcIdx + 1]);

    if (IsMul) {
      S.emitIntrinsic(Id, Dst, {Src1, Src2});
      break;
    }

    if (X86.operands[X86.op_count - 1].type != X86_OP_IMM)
      break;
    uint8_t Imm = static_cast<uint8_t>(X86.operands[X86.op_count - 1].imm);
    S.emitIntrinsic(Id, Dst, {Src1, Src2, NdVar::cst(Imm, 1)});
    break;
  }

  // ========================================================================
  // LWP — Lightweight Profiling (AMD).
  // ========================================================================
  case X86_INS_LLWPCB:
    S.emitIntrinsic(Intrinsic::Llwpcb, NdVar());
    break;
  case X86_INS_SLWPCB:
    S.emitIntrinsic(Intrinsic::Slwpcb);
    break;
  case X86_INS_LWPINS:
    S.emitIntrinsic(Intrinsic::Lwpins);
    break;
  case X86_INS_LWPVAL:
    S.emitIntrinsic(Intrinsic::Lwpval);
    break;

  // CLAC / STAC — supervisor mode access control.
  case X86_INS_CLAC:
    S.emitIntrinsic(Intrinsic::Clac, NdVar());
    break;
  case X86_INS_STAC:
    S.emitIntrinsic(Intrinsic::Stac, NdVar());
    break;

  // CLZERO — zero cache line (AMD Zen).
  case X86_INS_CLZERO:
    return liftCacheZero(L, S, Insn, X86);

  // XTEST already handled in TSX block.

  // VPCLMUL variants that are not the base VPCLMULQDQ are rare but exist
  // in some Capstone builds. Already covered via VPCLMULQDQ above.

  // ========================================================================
  // Prefixes — not standalone instructions; emit NOP.
  // ========================================================================
  case X86_INS_DATA16:
  case X86_INS_LOCK:
  case X86_INS_REP:
  case X86_INS_REPNE:
  case X86_INS_REX64:
    S.emit(NdOp::NOP, {}, {});
    break;

  // ========================================================================
  // SSE/MMX data moves — treat as COPY to preserve dataflow.
  // ========================================================================
  case X86_INS_MOVDQ2Q:
  case X86_INS_MOVQ2DQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case X86_INS_PSHUFW: {
    if (X86.op_count != 3 || X86.operands[2].type != X86_OP_IMM)
      return false;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Pshufw, Dst,
                    {Src, NdVar::cst(X86.operands[2].imm & 0xff, 1)});
    break;
  }

  // ========================================================================
  // SSE4a (AMD): EXTRQ / INSERTQ → intrinsic (hard to model per-bit).
  // ========================================================================
  case X86_INS_EXTRQ:
    S.emitIntrinsic(Intrinsic::Extrq);
    break;
  case X86_INS_INSERTQ:
    S.emitIntrinsic(Intrinsic::Insertq);
    break;

  // ========================================================================
  // System instructions.
  // ========================================================================
  case X86_INS_CLDEMOTE:
    S.emitIntrinsic(Intrinsic::Cldemote, NdVar());
    break;
  case X86_INS_INVLPGA:
    return liftVirtualization(L, S, Insn, X86);
  case X86_INS_PCONFIG:
    S.emitIntrinsic(Intrinsic::Pconfig);
    break;
  case X86_INS_RDPKRU:
    S.emitIntrinsic(Intrinsic::Rdpkru);
    S.emit(NdOp::COPY, NdVar::reg(x86reg::RAX, 4), {S.makeTemp(4)});
    break;
  case X86_INS_WRPKRU:
    S.emitIntrinsic(Intrinsic::Wrpkru, NdVar());
    break;
  case X86_INS_SYSEXITQ:
  case X86_INS_SYSRETQ:
    return false; // Requires the complete nonlocal machine-state boundary.
  case X86_INS_WBNOINVD:
    S.emitIntrinsic(Intrinsic::Wbnoinvd, NdVar());
    break;

  // ========================================================================
  // AVX/AVX-512 control: VLDMXCSR / VSTMXCSR.
  // ========================================================================
  case X86_INS_VLDMXCSR:
  case X86_INS_VSTMXCSR:
    if (X86.op_count < 1 ||
        !S.emitMemoryIntrinsic(InsnId == X86_INS_VLDMXCSR ? Intrinsic::Ldmxcsr
                                                          : Intrinsic::Stmxcsr,
                               X86.operands[0]))
      return false;
    break;

  default:
    return false;
  }
  return true;
}

} // namespace neverd
