//===- X86DescriptorStateNative.h - Native descriptor APIs --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared native names and exact assembly fallback for descriptor operations.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86DESCRIPTORSTATENATIVE_H
#define NEVERD_BACKEND_LLVM_X86_X86DESCRIPTORSTATENATIVE_H

#include "neverd/backend/llvm/X86/X86MemoryAddressAsm.h"
#include "neverd/ir/intrinsics/X86/X86DescriptorState.h"

#include <optional>
#include <string>

namespace neverd {

struct X86DescriptorAddress {
  unsigned Bits;
  unsigned Prefix;
};

struct X86DescriptorStateNative {
  Intrinsic Id;
  Arch TargetArch;
  bool Memory;
  bool Verify;
  unsigned ResultBytes;
  unsigned OperandBytes;
  unsigned MemoryBytes;
  std::optional<X86DescriptorAddress> Address;
  std::string LLVMName;
  std::string Builtin;
  std::string Text;
  std::string Constraints;
  std::string InputConstraint;
  std::string Segment;
  std::string Mnemonic;

  unsigned arguments() const {
    return OperandBytes ? 1 + (Address ? 2 : 0) : 0;
  }
};

inline std::optional<X86DescriptorStateNative> x86DescriptorStateNative(
    Intrinsic Id, Arch TargetArch, bool Memory, unsigned ResultBytes,
    NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default,
    std::optional<X86DescriptorAddress> Address = std::nullopt) {
  const unsigned Inputs = Memory || !isX86SystemRegisterStore(Id) ? 2 : 1;
  const unsigned OperandBytes = Memory ? 8 : Inputs == 2 ? 2 : 0;
  if ((TargetArch != Arch::X86 && TargetArch != Arch::X64) ||
      !x86DescriptorStateShapeIsValid(Id, TargetArch, Inputs, ResultBytes,
                                      OperandBytes, Space) ||
      (Address && !Memory))
    return std::nullopt;
  const unsigned WordBits = TargetArch == Arch::X64 ? 64 : 32;
  if (!Address && Memory &&
      (Space != NdMemoryAddressSpace::Default || isX86DescriptorTable16(Id)))
    Address = X86DescriptorAddress{
        WordBits, Space == NdMemoryAddressSpace::X86FS   ? 0x64u
                  : Space == NdMemoryAddressSpace::X86GS ? 0x65u
                                                         : 0u};
  const char *Segment = "";
  if (Address) {
    if (Address->Bits != WordBits && Address->Bits != WordBits / 2)
      return std::nullopt;
    switch (Address->Prefix) {
    case 0:
      break;
    case 0x26:
      Segment = "es";
      break;
    case 0x2e:
      Segment = "cs";
      break;
    case 0x36:
      Segment = "ss";
      break;
    case 0x3e:
      Segment = "ds";
      break;
    case 0x64:
      Segment = "fs";
      break;
    case 0x65:
      Segment = "gs";
      break;
    default:
      return std::nullopt;
    }
  }
  X86DescriptorStateNative P{};
  P.Id = Id;
  P.TargetArch = TargetArch;
  P.Memory = Memory;
  P.Verify = isX86DescriptorVerify(Id);
  P.ResultBytes = P.Verify ? 1 : ResultBytes;
  P.OperandBytes = Memory ? WordBits / 8 : OperandBytes;
  P.MemoryBytes = isX86DescriptorTable(Id)  ? (TargetArch == Arch::X64 ? 10 : 6)
                  : Id == Intrinsic::Invlpg ? 1
                                            : 2;
  P.Address = Address;
  P.Segment = Segment;
  P.Mnemonic = intrinsicAsmMnemonic(Id);
  std::string Name = P.Mnemonic;
  if (isX86DescriptorTable16(Id)) {
    Name.pop_back(); // Assembly suffix 'w' becomes the API's explicit 16.
    Name += "16";
  }
  if (!isX86DescriptorTable(Id) && Id != Intrinsic::Invlpg) {
    Name += Memory ? ".mem" : ".reg";
    if (!Memory && isX86SystemRegisterStore(Id))
      Name += std::to_string(8 * ResultBytes);
  }
  if (Address)
    Name += ".address";
  P.Builtin = "__builtin_ia32_" + Name;
  for (char &C : P.Builtin)
    if (C == '.')
      C = '_';
  if (Address && TargetArch == Arch::X86)
    Name += "32";
  P.LLVMName = "llvm.x86." + Name;
  const auto Index = std::to_string(P.ResultBytes ? 1 : 0);
  std::string Operand = "$" + Index;
  if (Memory) {
    if (Address && Address->Bits != WordBits)
      Operand = "${" + Index + (Address->Bits == 16 ? ":w}" : ":k}");
    Operand =
        (P.Segment.empty() ? "" : "%" + P.Segment + ":") + "(" + Operand + ")";
  }
  P.Text = P.Mnemonic + " " + (P.OperandBytes ? Operand : "$0");
  if (P.Verify)
    P.Text += "\n\tsetz $0";
  if (P.ResultBytes)
    P.Constraints = P.Verify ? "=q," : "=r,";
  if (P.OperandBytes) {
    P.InputConstraint =
        Address ? x86MemoryAddressAsm(Address->Bits).LLVMConstraint : "r";
    P.Constraints += P.InputConstraint + ",";
  }
  P.Constraints += P.Verify ? "~{flags},~{memory}" : "~{memory}";
  return P;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86DESCRIPTORSTATENATIVE_H
