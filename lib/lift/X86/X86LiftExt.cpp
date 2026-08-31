//===- X86LiftExt.cpp - x86/x64 extension instruction lifter ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dispatches the x86/x64 extension instructions.  The BMI1/BMI2/ADX
/// bit-manipulation handlers are in X86LiftExtBMI.cpp; the privileged,
/// segment/descriptor, port-I/O and virtualization instructions stay here
/// because RSM reads the lifter's private target architecture.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/Support/Debug.h"

#include <optional>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

uint64_t gpr64Offset(unsigned Number) {
  if (Number < 16)
    return static_cast<uint64_t>(Number) * 8;
  return x86reg::extendedGeneralReg(Number - 16);
}

bool isExactGpr64Operand(const cs_x86_op &Operand, unsigned Number,
                         uint8_t Access) {
  if (Number >= 32 || Operand.type != X86_OP_REG || Operand.size != 8 ||
      Operand.access != Access)
    return false;
  const RegInfo Info = mapCapstoneReg(static_cast<x86_reg>(Operand.reg));
  return Info.Size == 8 && Info.Offset == gpr64Offset(Number);
}

uint32_t readImmediate32(const uint8_t *Bytes) {
  return static_cast<uint32_t>(Bytes[0]) |
         (static_cast<uint32_t>(Bytes[1]) << 8) |
         (static_cast<uint32_t>(Bytes[2]) << 16) |
         (static_cast<uint32_t>(Bytes[3]) << 24);
}

bool hasCanonicalRegisterOnlyDetail(const cs_x86 &X86) {
  return X86.encoding.disp_offset == 0 && X86.encoding.disp_size == 0 &&
         X86.disp == 0 && X86.sib == 0 &&
         X86.sib_base == X86_REG_INVALID &&
         X86.sib_index == X86_REG_INVALID && X86.sib_scale == 0;
}

/// Classify explicit-operand MSR encodings without weakening their execution
/// contract. Raw bytes and Capstone detail must agree exactly; the resulting
/// intrinsic is still opaque because only an authenticated architectural
/// environment can own feature, XCR0, CPL, bitmap, interception, validation,
/// and exception-order semantics.
std::optional<X86MsrAccessKind>
classifyExplicitMsr(const cs_insn *Insn, const cs_x86 &X86,
                    Arch TargetArch) {
  if (!Insn || !Insn->detail || TargetArch != Arch::X64 ||
      Insn->size == 0 || Insn->size > 15 || X86.op_count != 2 ||
      !hasCanonicalRegisterOnlyDetail(X86))
    return std::nullopt;

  std::optional<X86MsrAccessKind> Kind;
  unsigned BNumber = 0;
  unsigned RNumber = 0;
  uint32_t Immediate = 0;

  if (Insn->bytes[0] == 0x62 || X86.opcode[0] == 0x62) {
    CanonicalEvexEncodingInfo Encoding;
    if (!parseCanonicalEvexEncodingInfo(Insn, X86, TargetArch, Encoding))
      return std::nullopt;

    const uint8_t Map = Encoding.P0 & 7;
    const uint8_t Pp = Encoding.P1 & 3;
    const bool ImmediateForm = Map == 7;
    const bool Write = Pp == 2;
    if ((Map != 4 && Map != 7) || (Pp != 2 && Pp != 3) ||
        (Encoding.P1 & 0x7c) != 0x7c || Encoding.P2 != 0x08 ||
        (Encoding.ModRM & 0xc0) != 0xc0 ||
        (ImmediateForm && (Encoding.ModRM & 0x38) != 0))
      return std::nullopt;

    BNumber = ((~Encoding.P0 & 0x20) >> 2) |
              ((Encoding.P0 & 0x08) << 1) | (Encoding.ModRM & 7);
    if (ImmediateForm) {
      if (!validateCanonicalEvexRegisterTail(Insn, X86, Encoding, 4) ||
          X86.encoding.imm_offset != Encoding.Offset + 6 ||
          X86.encoding.imm_size != 4)
        return std::nullopt;
      Immediate = readImmediate32(&Insn->bytes[Encoding.Offset + 6]);

      if (Encoding.Opcode == 0xf6) {
        Kind = Write ? X86MsrAccessKind::WrmsrnsImmediate
                     : X86MsrAccessKind::RdmsrImmediate;
      } else if (Encoding.Opcode == 0xf8 &&
                 (Encoding.P1 & 0x80) == 0) {
        Kind = Write ? X86MsrAccessKind::UwrmsrEvexImmediate
                     : X86MsrAccessKind::UrdmsrEvexImmediate;
      } else {
        return std::nullopt;
      }
    } else {
      if (Encoding.Opcode != 0xf8 || (Encoding.P1 & 0x80) != 0 ||
          !validateCanonicalEvexRegisterTail(Insn, X86, Encoding) ||
          X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0)
        return std::nullopt;
      RNumber = ((~Encoding.P0 & 0x80) >> 4) |
                (~Encoding.P0 & 0x10) | ((Encoding.ModRM >> 3) & 7);
      Kind = Write ? X86MsrAccessKind::UwrmsrEvexRegister
                   : X86MsrAccessKind::UrdmsrEvexRegister;
    }
  } else if (Insn->bytes[0] == 0xc4 || X86.opcode[0] == 0xc4) {
    CanonicalVex3EncodingInfo Encoding;
    if (!parseCanonicalVex3EncodingInfo(Insn, X86, TargetArch, Encoding) ||
        (Encoding.P0 & 0x1f) != 7 || Encoding.Opcode != 0xf8 ||
        (Encoding.P1 & 0xfc) != 0x78 ||
        (Encoding.ModRM & 0xf8) != 0xc0 ||
        !validateCanonicalVex3RegisterTail(Insn, X86, Encoding, 4) ||
        X86.encoding.imm_offset != Encoding.Offset + 5 ||
        X86.encoding.imm_size != 4)
      return std::nullopt;

    const bool Write = (Encoding.P1 & 3) == 2;
    if (!Write && (Encoding.P1 & 3) != 3)
      return std::nullopt;
    BNumber = ((~Encoding.P0 & 0x20) >> 2) | (Encoding.ModRM & 7);
    Immediate = readImmediate32(&Insn->bytes[Encoding.Offset + 5]);
    Kind = Write ? X86MsrAccessKind::UwrmsrVexImmediate
                 : X86MsrAccessKind::UrdmsrVexImmediate;
  } else {
    size_t OpcodeOffset = 0;
    uint8_t SegmentPrefix = 0;
    uint8_t MandatoryPrefix = 0;
    uint8_t Rex = 0;
    bool OperandSize = false;
    bool AddressSize = false;
    while (OpcodeOffset < Insn->size &&
           Insn->bytes[OpcodeOffset] != 0x0f) {
      const uint8_t Prefix = Insn->bytes[OpcodeOffset];
      switch (Prefix) {
      case 0x26:
      case 0x2e:
      case 0x36:
      case 0x3e:
      case 0x64:
      case 0x65:
        if (SegmentPrefix != 0 || Rex != 0)
          return std::nullopt;
        SegmentPrefix = Prefix;
        break;
      case 0x66:
        if (OperandSize || Rex != 0)
          return std::nullopt;
        OperandSize = true;
        break;
      case 0x67:
        if (AddressSize || Rex != 0)
          return std::nullopt;
        AddressSize = true;
        break;
      case 0xf2:
      case 0xf3:
        if (MandatoryPrefix != 0 || Rex != 0)
          return std::nullopt;
        MandatoryPrefix = Prefix;
        break;
      default:
        if (Prefix < 0x40 || Prefix > 0x4f || Rex != 0)
          return std::nullopt;
        Rex = Prefix;
        break;
      }
      ++OpcodeOffset;
    }

    if (MandatoryPrefix != 0xf2 && MandatoryPrefix != 0xf3)
      return std::nullopt;
    if (Rex != 0 &&
        (OpcodeOffset == 0 || Insn->bytes[OpcodeOffset - 1] != Rex))
      return std::nullopt;
    if (OpcodeOffset + 4 != Insn->size ||
        Insn->bytes[OpcodeOffset] != 0x0f ||
        Insn->bytes[OpcodeOffset + 1] != 0x38 ||
        Insn->bytes[OpcodeOffset + 2] != 0xf8 ||
        (Insn->bytes[OpcodeOffset + 3] & 0xc0) != 0xc0)
      return std::nullopt;

    const uint8_t ModRM = Insn->bytes[OpcodeOffset + 3];
    if (X86.prefix[0] != MandatoryPrefix ||
        X86.prefix[1] != SegmentPrefix ||
        X86.prefix[2] != (OperandSize ? 0x66 : 0) ||
        X86.prefix[3] != (AddressSize ? 0x67 : 0) || X86.rex != Rex ||
        X86.opcode[0] != 0x0f || X86.opcode[1] != 0x38 ||
        X86.opcode[2] != 0xf8 || X86.opcode[3] != 0 ||
        X86.addr_size != (AddressSize ? 4 : 8) || X86.modrm != ModRM ||
        X86.encoding.modrm_offset != OpcodeOffset + 3 ||
        X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0)
      return std::nullopt;

    BNumber = (ModRM & 7) | ((Rex & 1) ? 8 : 0);
    RNumber = ((ModRM >> 3) & 7) | ((Rex & 4) ? 8 : 0);
    Kind = MandatoryPrefix == 0xf3
               ? X86MsrAccessKind::UwrmsrLegacyRegister
               : X86MsrAccessKind::UrdmsrLegacyRegister;
  }

  if (!Kind)
    return std::nullopt;
  const bool Write = x86MsrAccessIsWrite(*Kind);
  const bool ImmediateForm = x86MsrAccessHasImmediateSelector(*Kind);
  const cs_x86_op &BOperand = X86.operands[Write ? 1 : 0];
  const cs_x86_op &Selector = X86.operands[Write ? 0 : 1];
  if (!isExactGpr64Operand(BOperand, BNumber,
                           Write ? CS_AC_READ : CS_AC_WRITE))
    return std::nullopt;
  if (ImmediateForm) {
    if (Selector.type != X86_OP_IMM || Selector.size != 4 ||
        Selector.access != CS_AC_READ ||
        static_cast<uint32_t>(Selector.imm) != Immediate)
      return std::nullopt;
  } else if (!isExactGpr64Operand(Selector, RNumber, CS_AC_READ)) {
    return std::nullopt;
  }

  const bool Privileged =
      *Kind == X86MsrAccessKind::RdmsrImmediate ||
      *Kind == X86MsrAccessKind::WrmsrnsImmediate;
  if (Privileged) {
    if (Insn->detail->groups_count != 1 ||
        Insn->detail->groups[0] != X86_GRP_PRIVILEGE)
      return std::nullopt;
  } else if (Insn->detail->groups_count != 0) {
    return std::nullopt;
  }

  switch (*Kind) {
  case X86MsrAccessKind::RdmsrImmediate:
    return Insn->id == X86_INS_RDMSR ? Kind : std::nullopt;
  case X86MsrAccessKind::WrmsrnsImmediate:
    return Insn->id == X86_INS_WRMSRNS ? Kind : std::nullopt;
  case X86MsrAccessKind::UrdmsrLegacyRegister:
  case X86MsrAccessKind::UrdmsrVexImmediate:
  case X86MsrAccessKind::UrdmsrEvexRegister:
  case X86MsrAccessKind::UrdmsrEvexImmediate:
    return Insn->id == X86_INS_URDMSR ? Kind : std::nullopt;
  case X86MsrAccessKind::UwrmsrLegacyRegister:
  case X86MsrAccessKind::UwrmsrVexImmediate:
  case X86MsrAccessKind::UwrmsrEvexRegister:
  case X86MsrAccessKind::UwrmsrEvexImmediate:
    return Insn->id == X86_INS_UWRMSR ? Kind : std::nullopt;
  case X86MsrAccessKind::Count:
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace

bool X86Lifter::liftExt(LiftState &S, const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  auto LiftOuts = [&](Intrinsic Id, unsigned ElemSz) {
    const uint16_t AddrSz = S.AddressSize;
    bool Repeat = false;
    for (uint8_t Prefix : X86.prefix)
      Repeat |= Prefix == X86_PREFIX_REP || Prefix == X86_PREFIX_REPNE;
    NdVar Count =
        Repeat ? NdVar::reg(x86reg::RCX, AddrSz) : NdVar::scalar(1, AddrSz);
    NdVar Df = NdVar::reg(x86reg::DF, 1);
    S.emitIntrinsic(Id, NdVar(),
                    {NdVar::reg(x86reg::RSI, AddrSz), Count,
                     NdVar::reg(x86reg::RDX, 2), Df},
                    NdMemoryOrdering::None, stringSourceAddressSpace(X86));

    NdVar Bytes = S.makeTemp(AddrSz);
    S.emit(NdOp::INT_MULT, Bytes, {Count, NdVar::scalar(ElemSz, AddrSz)});
    NdVar NegBytes = S.makeTemp(AddrSz);
    S.emit(NdOp::INT_SUB, NegBytes, {NdVar::scalar(0, AddrSz), Bytes});
    NdVar Delta = S.makeTemp(AddrSz);
    S.emit(NdOp::SELECT, Delta, {Df, NegBytes, Bytes});
    NdVar NewSi = S.makeTemp(AddrSz);
    S.emit(NdOp::INT_ADD, NewSi, {NdVar::reg(x86reg::RSI, AddrSz), Delta});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::RSI, AddrSz), {NewSi});
    if (Repeat)
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RCX, AddrSz),
             {NdVar::scalar(0, AddrSz)});
  };
  auto LiftIns = [&](Intrinsic Id, unsigned ElemSz) {
    const uint16_t AddrSz = S.AddressSize;
    bool Repeat = false;
    for (uint8_t Prefix : X86.prefix)
      Repeat |= Prefix == X86_PREFIX_REP || Prefix == X86_PREFIX_REPNE;
    NdVar Count =
        Repeat ? NdVar::reg(x86reg::RCX, AddrSz) : NdVar::scalar(1, AddrSz);
    NdVar Df = NdVar::reg(x86reg::DF, 1);
    S.emitIntrinsic(Id, NdVar(),
                    {NdVar::reg(x86reg::RDI, AddrSz), Count,
                     NdVar::reg(x86reg::RDX, 2), Df});

    NdVar Bytes = S.makeTemp(AddrSz);
    S.emit(NdOp::INT_MULT, Bytes, {Count, NdVar::scalar(ElemSz, AddrSz)});
    NdVar NegBytes = S.makeTemp(AddrSz);
    S.emit(NdOp::INT_SUB, NegBytes, {NdVar::scalar(0, AddrSz), Bytes});
    NdVar Delta = S.makeTemp(AddrSz);
    S.emit(NdOp::SELECT, Delta, {Df, NegBytes, Bytes});
    NdVar NewDi = S.makeTemp(AddrSz);
    S.emit(NdOp::INT_ADD, NewDi, {NdVar::reg(x86reg::RDI, AddrSz), Delta});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::RDI, AddrSz), {NewDi});
    if (Repeat)
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RCX, AddrSz),
             {NdVar::scalar(0, AddrSz)});
  };
  switch (InsnId) {

  // --- SYSENTER / SYSEXIT / SYSRET ---
  case X86_INS_SYSENTER:
  case X86_INS_SYSEXIT:
  case X86_INS_SYSRET:
    S.emitIntrinsic(Intrinsic::Syscall);
    break;

  // --- RSM (return from SMM) ---
  case X86_INS_RSM: {
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    S.emit(NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, PtrSize)});
    break;
  }

  // --- INT1 (ICEBP) ---
  case X86_INS_INT1:
    S.emitIntrinsic(Intrinsic::Int1);
    break;

  // --- CLI / STI ---
  case X86_INS_CLI:
  case X86_INS_STI:
    S.emitIntrinsic(InsnId == X86_INS_CLI ? Intrinsic::Cli : Intrinsic::Sti);
    break;

  // --- UD0 / UD1 ---
  case X86_INS_UD0:
  case X86_INS_UD1:
    S.emitIntrinsic(InsnId == X86_INS_UD0 ? Intrinsic::Ud0 : Intrinsic::Ud1);
    break;

  // --- INSD / OUTSD ---
  case X86_INS_INSD:
    LiftIns(Intrinsic::Insd, 4);
    break;
  case X86_INS_OUTSD:
    LiftOuts(Intrinsic::Outsd, 4);
    break;

  // ========================================================================
  // Port I/O — IN/OUT.  Capture the port (imm8 const, or DX register) and the
  // accumulator (AL/AX/EAX, size from the operand) so codegen can re-emit a
  // valid `in`/`out`.  A bare `in`/`out` is rejected (too few operands).
  // ========================================================================
  case X86_INS_IN: {
    // in acc, port   — read I/O port into the accumulator (value-producing).
    uint16_t Sz = (X86.op_count >= 1 && X86.operands[0].type == X86_OP_REG)
                      ? static_cast<uint16_t>(X86.operands[0].size)
                      : 4;
    if (Sz != 1 && Sz != 2 && Sz != 4)
      Sz = 4;
    NdVar Port =
        (X86.op_count >= 2 && X86.operands[1].type == X86_OP_IMM)
            ? NdVar::cst(static_cast<uint64_t>(X86.operands[1].imm) & 0xFF, 1)
            : NdVar::reg(x86reg::RDX, 2);
    S.emitIntrinsic(Intrinsic::In, NdVar::reg(x86reg::RAX, Sz), {Port});
    break;
  }
  case X86_INS_OUT: {
    // out port, acc  — write the accumulator to an I/O port (side-effect).
    uint16_t Sz = (X86.op_count >= 2 && X86.operands[1].type == X86_OP_REG)
                      ? static_cast<uint16_t>(X86.operands[1].size)
                      : 4;
    if (Sz != 1 && Sz != 2 && Sz != 4)
      Sz = 4;
    NdVar Port =
        (X86.op_count >= 1 && X86.operands[0].type == X86_OP_IMM)
            ? NdVar::cst(static_cast<uint64_t>(X86.operands[0].imm) & 0xFF, 1)
            : NdVar::reg(x86reg::RDX, 2);
    S.emitIntrinsic(Intrinsic::Out, NdVar(),
                    {Port, NdVar::reg(x86reg::RAX, Sz)});
    break;
  }

  // ========================================================================
  // Descriptor-table loads/stores + INVLPG — capture the memory-address
  // operand (best-effort: base register, as CLFLUSH does) so codegen can
  // re-emit `mnemonic (addr)`.  A bare mnemonic would be rejected by the
  // assembler (too few operands).
  // ========================================================================
  case X86_INS_LGDT:
  case X86_INS_LIDT:
  case X86_INS_SGDT:
  case X86_INS_SIDT:
  case X86_INS_INVLPG: {
    Intrinsic Id;
    switch (InsnId) {
    case X86_INS_LGDT:
      Id = Intrinsic::Lgdt;
      break;
    case X86_INS_LIDT:
      Id = Intrinsic::Lidt;
      break;
    case X86_INS_SGDT:
      Id = Intrinsic::Sgdt;
      break;
    case X86_INS_SIDT:
      Id = Intrinsic::Sidt;
      break;
    default:
      Id = Intrinsic::Invlpg;
      break;
    }
    if (X86.op_count < 1 ||
        !S.emitMemoryIntrinsic(Id, X86.operands[0]))
      return false;
    break;
  }

  // ========================================================================
  // r/m16 system-register loads/stores: LLDT/LTR/LMSW read r/m16, while the
  // memory forms of SLDT/STR/SMSW write 16 bits.  Their register-destination
  // forms retain the decoded register width so r32/r64 receive the selector or
  // machine-status word zero-extended to the full destination.  A memory base
  // is captured as an 8-byte pointer input; a register destination becomes the
  // INTRINSIC output.
  // ========================================================================
  case X86_INS_LLDT:
  case X86_INS_LTR:
  case X86_INS_LMSW:
  case X86_INS_SLDT:
  case X86_INS_STR:
  case X86_INS_SMSW: {
    Intrinsic Id;
    switch (InsnId) {
    case X86_INS_LLDT:
      Id = Intrinsic::Lldt;
      break;
    case X86_INS_LTR:
      Id = Intrinsic::Ltr;
      break;
    case X86_INS_LMSW:
      Id = Intrinsic::Lmsw;
      break;
    case X86_INS_SLDT:
      Id = Intrinsic::Sldt;
      break;
    case X86_INS_STR:
      Id = Intrinsic::Str;
      break;
    default:
      Id = Intrinsic::Smsw;
      break;
    }
    bool IsStore = (InsnId == X86_INS_SLDT || InsnId == X86_INS_STR ||
                    InsnId == X86_INS_SMSW);
    if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_MEM) {
      if (!S.emitMemoryIntrinsic(Id, X86.operands[0]))
        return false;
    } else if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_REG) {
      auto RI = mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
      if (IsStore)
        S.emitIntrinsic(Id, NdVar::reg(RI.Offset, RI.Size), {});
      else
        S.emitIntrinsic(Id, NdVar(), {NdVar::reg(RI.Offset, 2)});
    } else {
      return false;
    }
    break;
  }

  // ========================================================================
  // Privileged / system instructions — I/O, MSRs, virtualization, etc.
  // ========================================================================
  case X86_INS_OUTSB:
    LiftOuts(Intrinsic::Outsb, 1);
    break;
  case X86_INS_OUTSW:
    LiftOuts(Intrinsic::Outsw, 2);
    break;
  case X86_INS_INSB:
    LiftIns(Intrinsic::Insb, 1);
    break;
  case X86_INS_INSW:
    LiftIns(Intrinsic::Insw, 2);
    break;
  // INVPCID retains its complete architectural inputs in one opaque effect.
  // In particular, do not emit an ordinary LOAD for the m128 descriptor: its
  // page fault follows feature, privilege, type and descriptor checks owned by
  // an authenticated architectural execution environment.
  case X86_INS_INVPCID: {
    const uint16_t TypeSize = TargetArch == Arch::X64   ? uint16_t{8}
                              : TargetArch == Arch::X86 ? uint16_t{4}
                                                        : uint16_t{0};
    if (TypeSize == 0 || X86.op_count != 2 ||
        X86.operands[0].type != X86_OP_REG ||
        X86.operands[0].size != TypeSize ||
        X86.operands[1].type != X86_OP_MEM || X86.operands[1].size != 16)
      return false;
    const cs_x86_op &Descriptor = X86.operands[1];

    // The invalidation environment, rather than ordinary LowIR, owns every
    // check and fault associated with the m128 descriptor.  computeEA emits
    // arithmetic (including a COPY for plain [reg]), which would place an
    // observable operation before the opaque invalidation.  A native-width,
    // base-only address already has an exact scalar NdVar representation, so
    // preserve it directly.  Indexed, displaced, RIP/EIP-relative,
    // relocated, and address-size-overridden forms need address computation;
    // fail closed until LowIR can carry such an expression atomically.
    if (TargetArch != Arch::X64 || S.AddressSize != 8 ||
        S.RelocatedDisplacement ||
        S.HasAmbiguousI386GOTOFFDisplacement ||
        X86.encoding.disp_offset != 0 || X86.encoding.disp_size != 0 ||
        X86.disp != 0 ||
        Descriptor.mem.base == X86_REG_INVALID ||
        Descriptor.mem.base == X86_REG_RIP ||
        Descriptor.mem.base == X86_REG_EIP ||
        Descriptor.mem.index != X86_REG_INVALID || Descriptor.mem.disp != 0)
      return false;
    const RegInfo DescriptorBase =
        mapCapstoneReg(static_cast<x86_reg>(Descriptor.mem.base));
    if (DescriptorBase.Size != 8)
      return false;

    const NdVar Type = operandRead(S, X86.operands[0]);
    if (!Type.isReg() || Type.Size != TypeSize)
      return false;
    S.emitIntrinsic(
        Intrinsic::X86Invalidate, NdVar(),
        {NdVar::reg(DescriptorBase.Offset, DescriptorBase.Size),
         NdVar::scalar(static_cast<uint64_t>(X86InvalidateKind::Invpcid), 1),
         Type},
        NdMemoryOrdering::None, LiftState::memoryAddressSpace(Descriptor));
    break;
  }
  // VMX invalidation requires VMCS/VMX state that LowIR does not model.  Keep
  // these instructions unlifted instead of collapsing them into INVPCID or a
  // generic system placeholder.
  case X86_INS_INVEPT:
  case X86_INS_INVVPID:
    return false;
  // Explicit-operand MSR forms retain their exact encoding family so an
  // authenticated executor can apply the correct feature, privilege, bitmap,
  // interception, and exception-order rules. The legacy operand-free RDMSR
  // remains on its original intrinsic.
  case X86_INS_RDMSR:
    if (X86.op_count == 0) {
      S.emitIntrinsic(Intrinsic::Rdmsr);
      break;
    }
    [[fallthrough]];
  case X86_INS_WRMSRNS:
  case X86_INS_URDMSR:
  case X86_INS_UWRMSR: {
    const std::optional<X86MsrAccessKind> Kind =
        classifyExplicitMsr(Insn, X86, TargetArch);
    if (!Kind)
      return false;

    const bool Write = x86MsrAccessIsWrite(*Kind);
    const bool ImmediateForm = x86MsrAccessHasImmediateSelector(*Kind);
    const cs_x86_op &SelectorOperand = X86.operands[Write ? 0 : 1];
    const NdVar Selector =
        ImmediateForm
            ? NdVar::scalar(static_cast<uint32_t>(SelectorOperand.imm), 4)
            : operandRead(S, SelectorOperand);
    const NdVar KindValue =
        NdVar::scalar(static_cast<uint64_t>(*Kind), 1);
    if (Write) {
      const NdVar Value = operandRead(S, X86.operands[1]);
      S.emitIntrinsic(Intrinsic::X86MsrAccess, NdVar(),
                      {KindValue, Selector, Value});
    } else {
      const RegInfo Destination = mapCapstoneReg(
          static_cast<x86_reg>(X86.operands[0].reg));
      S.emitIntrinsic(Intrinsic::X86MsrAccess,
                      NdVar::reg(Destination.Offset, Destination.Size),
                      {KindValue, Selector});
    }
    break;
  }
  case X86_INS_WRMSR:
  case X86_INS_RDPMC:
  case X86_INS_RDPID:
  case X86_INS_RDRAND:
  case X86_INS_RDSEED:
  case X86_INS_RDFSBASE:
  case X86_INS_RDGSBASE:
  case X86_INS_WRFSBASE:
  case X86_INS_WRGSBASE:
  case X86_INS_VMCALL:
  case X86_INS_VMMCALL:
  case X86_INS_VMRUN:
  case X86_INS_VMSAVE:
  case X86_INS_VMLOAD:
  case X86_INS_VMLAUNCH:
  case X86_INS_VMRESUME:
  case X86_INS_VMXOFF:
  case X86_INS_VMXON:
  case X86_INS_VMCLEAR:
  case X86_INS_VMPTRLD:
  case X86_INS_VMPTRST:
  case X86_INS_VMREAD:
  case X86_INS_VMWRITE:
  case X86_INS_VMFUNC:
  case X86_INS_STGI:
  case X86_INS_CLGI:
  case X86_INS_SKINIT:
  case X86_INS_HLT:
  case X86_INS_INVD:
  case X86_INS_WBINVD:
  case X86_INS_SWAPGS:
  case X86_INS_VERR:
  case X86_INS_VERW:
  case X86_INS_LAR:
  case X86_INS_LSL:
  case X86_INS_ARPL:
  case X86_INS_CLTS:
  case X86_INS_XSETBV:
  case X86_INS_MONITOR:
  case X86_INS_MWAIT:
  case X86_INS_MONITORX:
  case X86_INS_MWAITX:
  case X86_INS_GETSEC:
  case X86_INS_ENCLS:
  case X86_INS_ENCLU:
  case X86_INS_ENCLV:
  case X86_INS_TPAUSE:
  case X86_INS_UMONITOR:
  case X86_INS_UMWAIT: {
    Intrinsic Id;
    switch (InsnId) {
    case X86_INS_WRMSR:
      Id = Intrinsic::Wrmsr;
      break;
    case X86_INS_RDPMC:
      Id = Intrinsic::Rdpmc;
      break;
    case X86_INS_VMCALL:
      Id = Intrinsic::Vmcall;
      break;
    case X86_INS_VMMCALL:
      Id = Intrinsic::Vmmcall;
      break;
    case X86_INS_HLT:
      Id = Intrinsic::Hlt;
      break;
    case X86_INS_INVD:
      Id = Intrinsic::Invd;
      break;
    case X86_INS_WBINVD:
      Id = Intrinsic::Wbinvd;
      break;
    case X86_INS_SWAPGS:
      Id = Intrinsic::Swapgs;
      break;
    default:
      Id = Intrinsic::Hlt;
      break;
    }
    S.emitIntrinsic(Id);
    break;
  }

  case X86_INS_XSAVE:
  case X86_INS_XSAVEC:
  case X86_INS_XSAVES:
  case X86_INS_XSAVEOPT:
  case X86_INS_XRSTOR:
  case X86_INS_XRSTORS: {
    Intrinsic Id = Intrinsic::Xsave;
    switch (InsnId) {
    case X86_INS_XSAVEC:
      Id = Intrinsic::Xsavec;
      break;
    case X86_INS_XSAVES:
      Id = Intrinsic::Xsaves;
      break;
    case X86_INS_XSAVEOPT:
      Id = Intrinsic::Xsaveopt;
      break;
    case X86_INS_XRSTOR:
      Id = Intrinsic::Xrstor;
      break;
    case X86_INS_XRSTORS:
      Id = Intrinsic::Xrstors;
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

  default:
    return liftExtBMI(*this, S, Insn, X86);
  }
  return true;
}

} // namespace neverd
