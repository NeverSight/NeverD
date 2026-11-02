//===- X86LiftSystemState.cpp - Bounded system instruction lifting ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve selectors, conditional destinations, wait completion and FS/GS
/// base effects without substituting host values or ordinary memory accesses.
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/X86/X86SystemState.h"
#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/support/ISAEncoding.h"

#include <array>

namespace neverd {
namespace {

bool secureLaunchEncoding(const cs_insn *Insn, const cs_x86 &X, bool Mode64) {
  if (!Insn->detail || Insn->size < 3 || Insn->size > x86::kMaxInstructionLen ||
      X.op_count != 1 || X.operands[0].type != X86_OP_REG ||
      X.operands[0].reg != X86_REG_EAX || X.operands[0].size != 4 ||
      X.operands[0].access != CS_AC_READ || X.eflags || X.encoding.imm_size ||
      X.encoding.disp_size || X.encoding.imm_offset || X.encoding.disp_offset ||
      X.opcode[2] || X.opcode[3] || X.sib || X.disp || X.rex2 || X.sib_index ||
      X.sib_base || X.sib_scale || X.xop_cc || X.sse_cc || X.avx_cc ||
      X.avx_sae || X.avx_rm)
    return false;
  uint8_t Prefix[4]{}, Rex = 0;
  const unsigned Offset = Insn->size - 3;
  for (unsigned I = 0; I < Offset; ++I) {
    const uint8_t Byte = Insn->bytes[I];
    if (Mode64 && Byte >= 0x40 && Byte <= 0x4f) {
      Rex = Byte;
      continue;
    }
    Rex = 0;
    if (Byte == 0xf2 || Byte == 0xf3)
      Prefix[0] = Byte;
    else if (Byte == 0x66)
      Prefix[2] = Byte;
    else if (Byte == 0x67)
      Prefix[3] = Byte;
    else if (Byte == 0x26 || Byte == 0x2e || Byte == 0x36 || Byte == 0x3e ||
             Byte == 0x64 || Byte == 0x65)
      Prefix[1] = Byte;
    else
      return false;
  }
  for (unsigned I = 0; I < 4; ++I)
    // Capstone may omit an ignored REP/REPNE prefix from the normalized slot.
    if (Prefix[I] != X.prefix[I] && (I != 0 || X.prefix[I]))
      return false;
  const auto &Detail = *Insn->detail;
  return Insn->id == X86_INS_SKINIT && X.rex == Rex &&
         X.addr_size == (Mode64 ? (Prefix[3] ? 4 : 8) : (Prefix[3] ? 2 : 4)) &&
         Detail.regs_read_count == 1 && Detail.regs_read[0] == X86_REG_EAX &&
         Detail.regs_write_count == 0 && Detail.groups_count == 2 &&
         Detail.groups[0] == X86_GRP_PRIVILEGE &&
         Detail.groups[1] == X86_GRP_VM && Insn->bytes[Offset] == 0x0f &&
         Insn->bytes[Offset + 1] == 0x01 && Insn->bytes[Offset + 2] == 0xde &&
         X.opcode[0] == 0x0f && X.opcode[1] == 0x01 && X.modrm == 0xde &&
         X.encoding.modrm_offset == Offset + 2;
}

bool implicitMsrEncoding(const cs_insn *Insn, const cs_x86 &X, bool Mode64) {
  if (!Insn->detail || Insn->size < 3 || Insn->size > x86::kMaxInstructionLen ||
      X.op_count || X.eflags || X.encoding.imm_size || X.encoding.disp_size ||
      X.encoding.imm_offset || X.encoding.disp_offset || X.opcode[2] ||
      X.opcode[3] || Insn->detail->groups_count != 1 ||
      Insn->detail->groups[0] != X86_GRP_PRIVILEGE)
    return false;
  uint8_t Mandatory = 0;
  uint8_t Segment = 0, Rex = 0;
  bool OperandSize = false, AddressOverride = false;
  const auto OpcodeOffset = Insn->size - 3;
  for (unsigned I = 0; I < OpcodeOffset; ++I) {
    const uint8_t Byte = Insn->bytes[I];
    if (Mode64 && Byte >= 0x40 && Byte <= 0x4f) {
      Rex = Byte;
      continue;
    }
    Rex = 0;
    if (Byte == 0xf2 || Byte == 0xf3)
      Mandatory = Byte;
    else if (Byte == 0x66)
      OperandSize = true;
    else if (Byte == 0x67)
      AddressOverride = true;
    else if (Byte == 0x26 || Byte == 0x2e || Byte == 0x36 || Byte == 0x3e ||
             Byte == 0x64 || Byte == 0x65)
      Segment = Byte;
    else
      return false;
  }
  const auto Id = Mandatory == 0xf2   ? X86_INS_RDMSRLIST
                  : Mandatory == 0xf3 ? X86_INS_WRMSRLIST
                                      : X86_INS_WRMSRNS;
  const auto &Detail = *Insn->detail;
  return (Mandatory ? Mode64 : !OperandSize) && Insn->id == Id &&
         X.prefix[0] == Mandatory && X.prefix[1] == Segment &&
         X.prefix[2] == (OperandSize ? 0x66 : 0) &&
         X.prefix[3] == (AddressOverride ? 0x67 : 0) && X.rex == Rex &&
         X.addr_size ==
             (Mode64 ? (AddressOverride ? 4 : 8) : (AddressOverride ? 2 : 4)) &&
         Detail.regs_read_count == 3 &&
         Detail.regs_read[0] == (Mandatory ? X86_REG_RSI : X86_REG_EAX) &&
         Detail.regs_read[1] == (Mandatory ? X86_REG_RDI : X86_REG_EDX) &&
         Detail.regs_read[2] == (Mandatory ? X86_REG_RCX : X86_REG_ECX) &&
         Detail.regs_write_count == (Mandatory ? 1 : 0) &&
         (!Mandatory || Detail.regs_write[0] == X86_REG_RCX) &&
         Insn->bytes[OpcodeOffset] == 0x0f &&
         Insn->bytes[OpcodeOffset + 1] == 0x01 &&
         Insn->bytes[OpcodeOffset + 2] == 0xc6 && X.opcode[0] == 0x0f &&
         X.opcode[1] == 0x01 && X.modrm == 0xc6 &&
         X.encoding.modrm_offset == OpcodeOffset + 2;
}

bool generalRegister(const cs_x86_op &Operand, uint16_t Width) {
  if (Operand.type != X86_OP_REG || Operand.size != Width)
    return false;
  const auto R = mapCapstoneReg(static_cast<x86_reg>(Operand.reg));
  return R.Size == Width && x86reg::isGeneralRegOffset(R.Offset);
}

bool systemExtensionEncoding(const cs_insn *Insn, const cs_x86 &X,
                             bool Mode64) {
  if (!Insn->detail || Insn->size < 3 || Insn->size > x86::kMaxInstructionLen)
    return false;
  uint8_t Prefix[4]{}, Rex = 0;
  unsigned Offset = 0;
  for (; Offset < Insn->size && Insn->bytes[Offset] != 0x0f; ++Offset) {
    const auto B = Insn->bytes[Offset];
    if (Mode64 && B >= 0x40 && B <= 0x4f) {
      Rex = B;
      continue;
    }
    Rex = 0;
    if (B == 0xf2 || B == 0xf3)
      Prefix[0] = B;
    else if (B == 0x66)
      Prefix[2] = B;
    else if (B == 0x67)
      Prefix[3] = B;
    else if (B == 0x26 || B == 0x2e || B == 0x36 || B == 0x3e || B == 0x64 ||
             B == 0x65)
      Prefix[1] = B;
    else
      return false;
  }
  for (unsigned I = 0; I < 4; ++I)
    if (Prefix[I] != X.prefix[I])
      return false;
  if (Offset + 3 > Insn->size || X.rex != Rex || X.opcode[0] != 0x0f ||
      X.opcode[3] ||
      X.addr_size != (Mode64 ? (Prefix[3] ? 4 : 8) : (Prefix[3] ? 2 : 4)))
    return false;
  uint8_t Map = 0x01, ModRM = 0, Mask = 0xff, Mandatory = 0xf3;
  bool Privileged = false, Memory = false, Immediate = false;
  uint64_t Flags = 0;
  std::array<uint16_t, 3> Reads{}, Writes{};
  uint8_t ReadCount = 0, WriteCount = 0;
  switch (Insn->id) {
  case X86_INS_CLUI:
    ModRM = 0xee;
    break;
  case X86_INS_STUI:
    ModRM = 0xef;
    break;
  case X86_INS_TESTUI:
    ModRM = 0xed;
    Flags = X86_EFLAGS_MODIFY_CF | X86_EFLAGS_RESET_ZF | X86_EFLAGS_RESET_AF |
            X86_EFLAGS_RESET_OF | X86_EFLAGS_RESET_PF | X86_EFLAGS_RESET_SF;
    Writes[WriteCount++] = X86_REG_EFLAGS;
    break;
  case X86_INS_SENDUIPI:
    Map = 0xc7;
    ModRM = 0xf0;
    Mask = 0xf8;
    break;
  case X86_INS_LKGS:
    Map = 0x00;
    ModRM = 0x30;
    Mask = 0x38;
    Mandatory = 0xf2;
    Memory = (X.modrm & 0xc0) != 0xc0;
    Privileged = true;
    Writes[WriteCount++] = X86_REG_GS;
    break;
  case X86_INS_HRESET:
    Map = 0x3a;
    ModRM = 0xc0;
    Immediate = Privileged = true;
    Reads[ReadCount++] = X86_REG_EAX;
    break;
  case X86_INS_INVLPGB:
    if (X.addr_size != 4 && X.addr_size != 8)
      return false;
    Reads[ReadCount++] = X.addr_size == 8 ? X86_REG_RAX : X86_REG_EAX;
    Reads[ReadCount++] = X86_REG_EDX;
    Reads[ReadCount++] = X86_REG_ECX;
    [[fallthrough]];
  case X86_INS_TLBSYNC:
    ModRM = Insn->id == X86_INS_INVLPGB ? 0xfe : 0xff;
    Mandatory = 0;
    Privileged = true;
    if (Prefix[2])
      return false;
    break;
  default:
    return false;
  }
  if (!Mode64 && Insn->id != X86_INS_HRESET && Insn->id != X86_INS_INVLPGB &&
      Insn->id != X86_INS_TLBSYNC)
    return false;
  const unsigned ModRMOffset = Offset + (Immediate ? 3 : 2);
  if (ModRMOffset >= Insn->size || Prefix[0] != Mandatory ||
      X.opcode[1] != Map || Insn->bytes[Offset + 1] != Map ||
      X.opcode[2] != (Immediate ? 0xf0 : 0) ||
      (Immediate && Insn->bytes[Offset + 2] != 0xf0) ||
      X.encoding.modrm_offset != ModRMOffset ||
      Insn->bytes[ModRMOffset] != X.modrm || (X.modrm & Mask) != ModRM ||
      X.encoding.imm_size != (Immediate ? 1 : 0) ||
      X.encoding.imm_offset != (Immediate ? ModRMOffset + 1 : 0) ||
      (!Memory && (X.encoding.disp_size || X.encoding.disp_offset ||
                   Insn->size != ModRMOffset + (Immediate ? 2 : 1))) ||
      X.eflags != Flags)
    return false;
  const auto &D = *Insn->detail;
  if (D.groups_count != unsigned(Privileged) ||
      (Privileged && D.groups[0] != X86_GRP_PRIVILEGE) ||
      D.regs_read_count != ReadCount || D.regs_write_count != WriteCount)
    return false;
  for (unsigned I = 0; I < ReadCount; ++I)
    if (D.regs_read[I] != Reads[I])
      return false;
  for (unsigned I = 0; I < WriteCount; ++I)
    if (D.regs_write[I] != Writes[I])
      return false;
  if (Insn->id == X86_INS_SENDUIPI || (Insn->id == X86_INS_LKGS && !Memory)) {
    if (X.op_count != 1 || X.operands[0].type != X86_OP_REG ||
        mapCapstoneReg(static_cast<x86_reg>(X.operands[0].reg)).Offset !=
            x86reg::generalReg((X.modrm & 7) | ((Rex & 1) ? 8 : 0)))
      return false;
  }
  return true;
}

void copyFlag(X86Lifter::LiftState &S, NdVar Result, uint16_t Offset) {
  S.emit(NdOp::SUBBYTES, NdVar::reg(x86reg::ZF, 1),
         {Result, NdVar::scalar(Offset, 4)});
}

} // namespace

bool liftSystemState(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                     const cs_x86 &X86) {
  const bool Mode64 = L.targetArch() == Arch::X64;
  const uint16_t Word = Mode64 ? 8 : 4;
  switch (Insn->id) {
  case X86_INS_SKINIT:
    if (!secureLaunchEncoding(Insn, X86, Mode64))
      return false;
    // Native checks, reset and SLB reads belong to this terminating launch.
    // No old register, flag, stack or descriptor state survives in this CFG.
    S.emitIntrinsic(Intrinsic::X86Skinit, {}, {NdVar::reg(x86reg::RAX, 4)});
    return true;
  case X86_INS_CLUI:
  case X86_INS_STUI:
  case X86_INS_TESTUI:
  case X86_INS_TLBSYNC:
    if (!systemExtensionEncoding(Insn, X86, Mode64) || X86.op_count)
      return false;
    if (Insn->id == X86_INS_TESTUI) {
      S.emitIntrinsic(Intrinsic::X86TestUserInterrupt,
                      NdVar::reg(x86reg::CF, 1));
      for (const auto Flag :
           {x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF})
        S.emit(NdOp::COPY, NdVar::reg(Flag, 1), {NdVar::scalar(0, 1)});
    } else {
      S.emitIntrinsic(
          Insn->id == X86_INS_CLUI   ? Intrinsic::X86ClearUserInterrupt
          : Insn->id == X86_INS_STUI ? Intrinsic::X86SetUserInterrupt
                                     : Intrinsic::X86TlbSync,
          {});
    }
    return true;
  case X86_INS_SENDUIPI:
    if (!systemExtensionEncoding(Insn, X86, Mode64) || X86.op_count != 1 ||
        !generalRegister(X86.operands[0], 8) ||
        X86.operands[0].access != CS_AC_READ)
      return false;
    S.emitIntrinsic(Intrinsic::X86SendUserInterrupt, {},
                    {L.operandRead(S, X86.operands[0])});
    return true;
  case X86_INS_LKGS: {
    if (!systemExtensionEncoding(Insn, X86, Mode64) || X86.op_count != 1)
      return false;
    const auto &Op = X86.operands[0];
    const bool Memory = Op.type == X86_OP_MEM;
    if (Op.size != 2 || Op.access != CS_AC_READ ||
        Memory != ((X86.modrm & 0xc0) != 0xc0) ||
        (!Memory && !generalRegister(Op, 2)))
      return false;
    // The native instruction owns selector and descriptor reads, GS selector
    // updates and IA32_KERNEL_GS_BASE. The active GS base is unchanged.
    S.emitIntrinsic(
        Memory ? Intrinsic::X86LoadKernelGsMemory : Intrinsic::X86LoadKernelGs,
        {}, {Memory ? S.computeEA(Op) : L.operandRead(S, Op)},
        NdMemoryOrdering::None,
        Memory ? S.memoryAddressSpace(Op) : NdMemoryAddressSpace::Default);
    return true;
  }
  case X86_INS_HRESET:
    if (!systemExtensionEncoding(Insn, X86, Mode64) || X86.op_count != 1 ||
        X86.operands[0].type != X86_OP_IMM || X86.operands[0].size != 1 ||
        X86.operands[0].imm != Insn->bytes[Insn->size - 1])
      return false;
    S.emitIntrinsic(Intrinsic::X86HistoryReset, {},
                    {NdVar::reg(x86reg::RAX, 4)});
    return true;
  case X86_INS_INVLPGB:
    if (!systemExtensionEncoding(Insn, X86, Mode64) || X86.op_count)
      return false;
    S.emitIntrinsic(Intrinsic::X86InvalidateBroadcast, {},
                    {NdVar::reg(x86reg::RAX, X86.addr_size),
                     NdVar::reg(x86reg::RDX, 4), NdVar::reg(x86reg::RCX, 4)});
    return true;
  case X86_INS_WRMSRNS:
    if (!implicitMsrEncoding(Insn, X86, Mode64))
      return false;
    S.emitIntrinsic(Intrinsic::X86WriteMsrNonSerializing, {},
                    {NdVar::reg(x86reg::RCX, 4), NdVar::reg(x86reg::RAX, 4),
                     NdVar::reg(x86reg::RDX, 4)});
    return true;
  case X86_INS_RDMSRLIST:
  case X86_INS_WRMSRLIST:
    if (!implicitMsrEncoding(Insn, X86, Mode64))
      return false;
    // These are linear addresses even with 67 or a segment prefix. RCX is
    // bound read/write by the native executor, including fault/restart state.
    S.emitIntrinsic(Insn->id == X86_INS_RDMSRLIST ? Intrinsic::X86ReadMsrList
                                                  : Intrinsic::X86WriteMsrList,
                    NdVar::reg(x86reg::RCX, 8),
                    {NdVar::reg(x86reg::RSI, 8), NdVar::reg(x86reg::RDI, 8),
                     NdVar::reg(x86reg::RCX, 8)});
    return true;
  case X86_INS_RDPID:
    if (X86.op_count != 1 || !generalRegister(X86.operands[0], Word))
      return false;
    S.emitIntrinsic(Intrinsic::X86ReadPid, L.operandWrite(X86.operands[0]));
    return true;
  case X86_INS_RDFSBASE:
  case X86_INS_RDGSBASE:
  case X86_INS_WRFSBASE:
  case X86_INS_WRGSBASE: {
    if (!Mode64 || X86.op_count != 1 ||
        (X86.operands[0].size != 4 && X86.operands[0].size != 8) ||
        !generalRegister(X86.operands[0], X86.operands[0].size))
      return false;
    const bool Write =
        Insn->id == X86_INS_WRFSBASE || Insn->id == X86_INS_WRGSBASE;
    const Intrinsic Id =
        Insn->id == X86_INS_RDFSBASE   ? Intrinsic::X86ReadFsBase
        : Insn->id == X86_INS_RDGSBASE ? Intrinsic::X86ReadGsBase
        : Insn->id == X86_INS_WRFSBASE ? Intrinsic::X86WriteFsBase
                                       : Intrinsic::X86WriteGsBase;
    if (Write)
      S.emitIntrinsic(Id, NdVar(), {L.operandRead(S, X86.operands[0])});
    else
      S.emitIntrinsic(Id, L.operandWrite(X86.operands[0]));
    return true;
  }
  case X86_INS_CLTS:
    if (X86.op_count)
      return false;
    S.emitIntrinsic(Intrinsic::X86ClearTaskSwitched, NdVar());
    return true;
  case X86_INS_UMONITOR: {
    if (X86.op_count != 1 ||
        (S.AddressSize != 2 && S.AddressSize != 4 && S.AddressSize != 8) ||
        !generalRegister(X86.operands[0], S.AddressSize))
      return false;
    cs_x86_op Address{};
    Address.type = X86_OP_MEM;
    Address.size = 1;
    Address.mem.base = X86.operands[0].reg;
    Address.mem.scale = 1;
    Address.mem.segment = X86.prefix[1] == 0x64   ? X86_REG_FS
                          : X86.prefix[1] == 0x65 ? X86_REG_GS
                                                  : X86_REG_INVALID;
    S.emitIntrinsic(Intrinsic::X86UserMonitor, NdVar(), {S.computeEA(Address)},
                    NdMemoryOrdering::None, S.memoryAddressSpace(Address));
    return true;
  }
  case X86_INS_TPAUSE:
  case X86_INS_UMWAIT:
    if (X86.op_count != 1 || !generalRegister(X86.operands[0], 4))
      return false;
    S.emitIntrinsic(Insn->id == X86_INS_TPAUSE ? Intrinsic::X86TimedPause
                                               : Intrinsic::X86UserWait,
                    NdVar::reg(x86reg::CF, 1),
                    {L.operandRead(S, X86.operands[0]),
                     NdVar::reg(x86reg::RAX, 4), NdVar::reg(x86reg::RDX, 4)});
    for (const auto Flag :
         {x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF})
      S.emit(NdOp::COPY, NdVar::reg(Flag, 1), {NdVar::scalar(0, 1)});
    return true;
  case X86_INS_LAR:
  case X86_INS_LSL: {
    if (X86.op_count != 2)
      return false;
    const auto &Dst = X86.operands[0];
    const auto &Src = X86.operands[1];
    const uint16_t Width = Dst.size;
    if ((Width != 2 && Width != 4 && Width != 8) || Width > Word ||
        !generalRegister(Dst, Width) ||
        (Src.type != X86_OP_REG && Src.type != X86_OP_MEM) ||
        (Src.type == X86_OP_MEM && Src.size != 2) ||
        (Src.type == X86_OP_REG &&
         (Src.size < 2 || Src.size > Word || !generalRegister(Src, Src.size))))
      return false;
    const bool Memory = Src.type == X86_OP_MEM;
    const NdVar Result = S.makeTemp(2 * Width);
    const NdVar Selector =
        Memory ? S.computeEA(Src)
               : NdVar::reg(
                     mapCapstoneReg(static_cast<x86_reg>(Src.reg)).Offset, 2);
    const Intrinsic Id = Insn->id == X86_INS_LSL
                             ? Memory ? Intrinsic::X86LoadSegmentLimitMemory
                                      : Intrinsic::X86LoadSegmentLimit
                         : Memory ? Intrinsic::X86LoadAccessRightsMemory
                                  : Intrinsic::X86LoadAccessRights;
    S.emitIntrinsic(Id, Result, {Selector}, NdMemoryOrdering::None,
                    Memory ? S.memoryAddressSpace(Src)
                           : NdMemoryAddressSpace::Default);
    copyFlag(S, Result, Width);
    NdVar Value = S.makeTemp(Width);
    S.emit(NdOp::SUBBYTES, Value, {Result, NdVar::scalar(0, 4)});
    NdVar Destination = L.operandWrite(Dst);
    // Failure preserves the complete destination, including the high half of
    // a 32-bit destination in long mode. Only success performs zero extension.
    if (Mode64 && Width == 4) {
      const NdVar Extended = S.makeTemp(8);
      S.emit(NdOp::INT_ZEXT, Extended, {Value});
      Value = Extended;
      Destination.Size = 8;
    }
    S.emit(NdOp::SELECT, Destination,
           {NdVar::reg(x86reg::ZF, 1), Value, Destination});
    return true;
  }
  case X86_INS_ARPL: {
    if (Mode64 || X86.op_count != 2 || !generalRegister(X86.operands[1], 2))
      return false;
    const auto &Dst = X86.operands[0];
    if (Dst.size != 2 || (Dst.type != X86_OP_MEM && !generalRegister(Dst, 2)))
      return false;
    const NdVar Source = L.operandRead(S, X86.operands[1]);
    if (Dst.type == X86_OP_MEM) {
      S.emitIntrinsic(Intrinsic::X86AdjustRplMemory, NdVar::reg(x86reg::ZF, 1),
                      {S.computeEA(Dst), Source}, NdMemoryOrdering::None,
                      S.memoryAddressSpace(Dst));
    } else {
      const NdVar Result = S.makeTemp(4);
      S.emitIntrinsic(Intrinsic::X86AdjustRpl, Result,
                      {L.operandRead(S, Dst), Source});
      S.emit(NdOp::SUBBYTES, L.operandWrite(Dst),
             {Result, NdVar::scalar(0, 4)});
      copyFlag(S, Result, 2);
    }
    return true;
  }
  default:
    return false;
  }
}

} // namespace neverd
