//===- X86MpxEncoding.cpp - MPX encoding ownership ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate MPX prefixes, bounds registers and effective-address operands.
//===----------------------------------------------------------------------===//

#include "neverd/lift/X86/X86MpxEncoding.h"

#include "neverd/ir/X86/X86MpxContext.h"
#include "neverd/support/ISAEncoding.h"

#include <array>
#include <cstddef>

namespace neverd {

std::optional<X86MpxBranch> x86MpxBranchEffect(llvm::ArrayRef<uint8_t> Bytes,
                                               Arch Target) {
  if (Target != Arch::X86 && Target != Arch::X64)
    return std::nullopt;
  uint8_t Repeat = 0;
  size_t Offset = 0;
  for (; Offset < Bytes.size(); ++Offset) {
    const auto B = Bytes[Offset];
    if (B == 0xf0)
      return std::nullopt;
    if (B == 0xf2 || B == 0xf3) {
      if (Repeat)
        return std::nullopt;
      Repeat = B;
    } else if (B != 0x66 && B != 0x67 && B != 0x26 && B != 0x2e && B != 0x36 &&
               B != 0x3e && B != 0x64 && B != 0x65 &&
               !(Target == Arch::X64 && B >= 0x40 && B <= 0x4f)) {
      break;
    }
  }
  if (Offset == Bytes.size())
    return std::nullopt;
  const auto Opcode = Bytes[Offset++];
  if (Opcode == 0xeb || (Opcode >= 0xe0 && Opcode <= 0xe3) || Opcode == 0xca ||
      Opcode == 0xcb || Opcode == 0xcf ||
      (Target == Arch::X86 && (Opcode == 0x9a || Opcode == 0xea)))
    return X86MpxBranch::Preserve;
  bool Initialize = Opcode == 0xe8 || Opcode == 0xe9 || Opcode == 0xc2 ||
                    Opcode == 0xc3 || (Opcode >= 0x70 && Opcode <= 0x7f);
  if (Opcode == 0x0f && Offset < Bytes.size())
    Initialize = Bytes[Offset] >= 0x80 && Bytes[Offset] <= 0x8f;
  if (Opcode == 0xff && Offset < Bytes.size()) {
    const auto Extension = (Bytes[Offset] >> 3) & 7;
    if (Extension == 3 || Extension == 5)
      return X86MpxBranch::Preserve;
    Initialize = Extension == 2 || Extension == 4;
  }
  if (!Initialize)
    return std::nullopt;
  return Repeat == 0xf2 ? X86MpxBranch::Preserve : X86MpxBranch::Initialize;
}

namespace {

x86_reg segmentRegister(uint8_t Prefix) {
  switch (Prefix) {
  case 0x26:
    return X86_REG_ES;
  case 0x2e:
    return X86_REG_CS;
  case 0x36:
    return X86_REG_SS;
  case 0x3e:
    return X86_REG_DS;
  case 0x64:
    return X86_REG_FS;
  case 0x65:
    return X86_REG_GS;
  default:
    return X86_REG_INVALID;
  }
}

x86_reg generalRegister(unsigned Index, bool Mode64) {
  constexpr std::array<x86_reg, 16> Reg64 = {
      X86_REG_RAX, X86_REG_RCX, X86_REG_RDX, X86_REG_RBX,
      X86_REG_RSP, X86_REG_RBP, X86_REG_RSI, X86_REG_RDI,
      X86_REG_R8,  X86_REG_R9,  X86_REG_R10, X86_REG_R11,
      X86_REG_R12, X86_REG_R13, X86_REG_R14, X86_REG_R15};
  constexpr std::array<x86_reg, 8> Reg32 = {
      X86_REG_EAX, X86_REG_ECX, X86_REG_EDX, X86_REG_EBX,
      X86_REG_ESP, X86_REG_EBP, X86_REG_ESI, X86_REG_EDI};
  return Mode64 ? Reg64[Index] : Reg32[Index];
}

cs_x86_op registerOperand(x86_reg Register, unsigned Size, cs_ac_type Access) {
  cs_x86_op Operand{};
  Operand.type = X86_OP_REG;
  Operand.reg = Register;
  Operand.size = Size;
  Operand.access = Access;
  return Operand;
}

} // namespace

std::optional<X86MpxEncoding>
decodeX86MpxEncoding(llvm::ArrayRef<uint8_t> Bytes, Arch Target) {
  if (Target != Arch::X86 && Target != Arch::X64)
    return std::nullopt;
  Bytes = Bytes.take_front(x86::kMaxInstructionLen);
  X86MpxEncoding E;
  auto &X = E.Detail;
  const bool Mode64 = Target == Arch::X64;
  const unsigned WordBytes = Mode64 ? 8 : 4;
  size_t Offset = 0;
  while (Offset < Bytes.size()) {
    const auto B = Bytes[Offset];
    if (Mode64 && B >= 0x40 && B <= 0x4f) {
      X.rex = B;
      ++Offset;
      continue;
    }
    unsigned Group;
    if (B == 0xf0 || B == 0xf2 || B == 0xf3)
      Group = 0;
    else if (segmentRegister(B) != X86_REG_INVALID)
      Group = 1;
    else if (B == 0x66)
      Group = 2;
    else if (B == 0x67)
      Group = 3;
    else
      break;
    // Multiple prefixes in a group, or competing mandatory selectors, have
    // no qualified interpretation here. A legacy prefix cancels earlier REX.
    if (X.prefix[Group])
      return std::nullopt;
    X.prefix[Group] = B;
    X.rex = 0;
    ++Offset;
  }
  if (Offset + 3 > Bytes.size() || Bytes[Offset] != 0x0f ||
      (Bytes[Offset + 1] != 0x1a && Bytes[Offset + 1] != 0x1b) ||
      (X.prefix[2] && (X.prefix[0] == 0xf2 || X.prefix[0] == 0xf3)))
    return std::nullopt;

  const auto Repeat = X.prefix[0];
  const bool Locked = Repeat == 0xf0;
  const auto Mandatory = X.prefix[2] ? X.prefix[2] : Locked ? 0 : Repeat;
  const auto Opcode = Bytes[Offset + 1];
  X.opcode[0] = 0x0f;
  X.opcode[1] = Opcode;
  X.encoding.modrm_offset = Offset + 2;
  X.modrm = Bytes[Offset + 2];
  X.addr_size = WordBytes;
  Offset += 3;
  const unsigned Mod = X.modrm >> 6;
  const unsigned Reg = ((X.modrm >> 3) & 7) | (X.rex & 4 ? 8 : 0);
  const unsigned RM = (X.modrm & 7) | (X.rex & 1 ? 8 : 0);
  // Mandatory F2/F3 are instruction selectors, not repeat effects. Preserve
  // LOCK and 66/67/segment bytes in the ordinary Capstone prefix slots.
  if (!Locked)
    X.prefix[0] = 0;
  if (Mod == 3 && (!Mandatory || (Mandatory == 0xf3 && Opcode == 0x1b))) {
    if (Locked)
      return std::nullopt;
    E.Id = X86_INS_NOP;
    E.Mnemonic = "nop";
    E.Size = Offset;
    return E;
  }
  if (Reg >= X86MpxRegisterCount || (!Mode64 && Mod != 3 && X.prefix[3]))
    return std::nullopt;
  const bool Store = Opcode == 0x1b && (Mandatory == 0x66 || !Mandatory);
  if (Locked && !(Store && Mandatory == 0x66 && Mod != 3))
    return std::nullopt;
  if (Mandatory == 0x66) {
    E.Id = X86_INS_BNDMOV;
    E.Mnemonic = Locked ? "lock bndmov" : "bndmov";
  } else if (Mandatory == 0xf3) {
    E.Id = Opcode == 0x1a ? X86_INS_BNDCL : X86_INS_BNDMK;
    E.Mnemonic = Opcode == 0x1a ? "bndcl" : "bndmk";
  } else if (Mandatory == 0xf2) {
    E.Id = Opcode == 0x1a ? X86_INS_BNDCU : X86_INS_BNDCN;
    E.Mnemonic = Opcode == 0x1a ? "bndcu" : "bndcn";
  } else {
    E.Id = Store ? X86_INS_BNDSTX : X86_INS_BNDLDX;
    E.Mnemonic = Store ? "bndstx" : "bndldx";
  }
  const bool Check =
      E.Id == X86_INS_BNDCL || E.Id == X86_INS_BNDCU || E.Id == X86_INS_BNDCN;
  const bool Move = E.Id == X86_INS_BNDMOV;
  X.op_count = 2;
  X.operands[Store ? 1 : 0] =
      registerOperand(x86_reg(X86_REG_BND0 + Reg), 16,
                      Store || Check ? CS_AC_READ : CS_AC_WRITE);
  auto &Operand = X.operands[Store ? 0 : 1];
  if (Mod == 3) {
    if (Move && RM >= X86MpxRegisterCount)
      return std::nullopt;
    Operand = registerOperand(
        Move ? x86_reg(X86_REG_BND0 + RM) : generalRegister(RM, Mode64),
        Move ? 16 : WordBytes, Store ? CS_AC_WRITE : CS_AC_READ);
    E.Size = Offset;
    return E;
  }

  Operand.type = X86_OP_MEM;
  // Keep Capstone's address-expression operand shape. BNDMOV alone has a
  // direct memory footprint; checks/MK do not access memory and MIB describes
  // translation inputs, not a load/store at that effective address.
  Operand.size = Move ? 2 * WordBytes : 16;
  Operand.access = Store ? CS_AC_WRITE : CS_AC_READ;
  auto &Memory = Operand.mem;
  Memory.segment = segmentRegister(X.prefix[1]);
  Memory.scale = 1;
  const bool HasSIB = (X.modrm & 7) == 4;
  unsigned DispBytes = Mod == 1 ? 1 : Mod == 2 ? 4 : 0;
  if (HasSIB) {
    if (Offset == Bytes.size())
      return std::nullopt;
    X.sib = Bytes[Offset++];
    Memory.scale = 1 << (X.sib >> 6);
    const unsigned Index = ((X.sib >> 3) & 7) | (X.rex & 2 ? 8 : 0);
    if (Index != 4)
      Memory.index = generalRegister(Index, Mode64);
    if (Mod == 0 && (X.sib & 7) == 5)
      DispBytes = 4;
    else
      Memory.base = generalRegister((X.sib & 7) | (X.rex & 1 ? 8 : 0), Mode64);
    X.sib_index = Memory.index;
    X.sib_base = Memory.base;
    X.sib_scale = Memory.scale;
  } else if (Mod == 0 && (X.modrm & 7) == 5) {
    DispBytes = 4;
    if (Mode64) {
      if (!Move && !Check)
        return std::nullopt;
      Memory.base = X86_REG_RIP;
    }
  } else {
    Memory.base = generalRegister(RM, Mode64);
  }
  if (Offset + DispBytes > Bytes.size())
    return std::nullopt;
  if (DispBytes) {
    X.encoding.disp_offset = Offset;
    X.encoding.disp_size = DispBytes;
    uint32_t Value = 0;
    for (unsigned I = 0; I != DispBytes; ++I)
      Value |= uint32_t(Bytes[Offset + I]) << (8 * I);
    X.disp = DispBytes == 1 ? int64_t(static_cast<int8_t>(Value))
                            : int64_t(static_cast<int32_t>(Value));
    Memory.disp = X.disp;
  }
  E.Size = Offset + DispBytes;
  return E;
}

} // namespace neverd
