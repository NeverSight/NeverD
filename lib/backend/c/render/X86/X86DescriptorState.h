//===- X86DescriptorState.h - Descriptor C expressions -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native builtin selection and instruction-owned memory for both C routes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86DESCRIPTORSTATE_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86DESCRIPTORSTATE_H

#include "neverd/backend/llvm/X86/X86DescriptorStateNative.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace neverd {

inline constexpr const char X86DescriptorStateCompilerDispatch[] =
    "#ifndef NEVERD_X86_DESCRIPTOR_STATE\n"
    "#if defined(__has_builtin)\n"
    "#if __has_builtin(__builtin_ia32_lgdt_address) && "
    "__has_builtin(__builtin_ia32_verr_reg) && "
    "__has_builtin(__builtin_ia32_sldt_reg64)\n"
    "#define NEVERD_X86_DESCRIPTOR_STATE(native, msvc, ...) native\n"
    "#endif\n"
    "#endif\n"
    "#ifndef NEVERD_X86_DESCRIPTOR_STATE\n"
    "#if defined(_MSC_VER) && !defined(__clang__)\n"
    "#define NEVERD_X86_DESCRIPTOR_STATE(native, msvc, ...) msvc\n"
    "#else\n"
    "#define NEVERD_X86_DESCRIPTOR_STATE(native, msvc, ...) __VA_ARGS__\n"
    "#endif\n"
    "#endif\n"
    "#endif\n\n";

inline std::string
renderX86DescriptorStateExpression(const X86DescriptorStateNative &P,
                                   const std::vector<std::string> &Args,
                                   bool NativeDispatch = true) {
  if (Args.size() != unsigned(P.OperandBytes != 0))
    throw std::invalid_argument("invalid descriptor state C operand count");
  const unsigned WordBits = P.TargetArch == Arch::X64 ? 64 : 32;
  const auto Type = [](unsigned Bytes) {
    return "uint" + std::to_string(Bytes * 8) + "_t";
  };
  std::string Input;
  if (P.OperandBytes) {
    Input = "(" + Type(P.OperandBytes) + ")(" + Args[0] + ")";
    if (P.Memory)
      Input = "(" + Type(P.OperandBytes) + ")(uintptr_t)(" + Args[0] + ")";
  }
  std::string Temporary = "neverd_descriptor";
  while (Input.find(Temporary) != std::string::npos)
    Temporary += "_";
  const std::string Index = P.ResultBytes ? "1" : "0";
  std::string Operand = "%" + Index;
  if (P.Memory) {
    if (P.Address && P.Address->Bits != WordBits)
      Operand = std::string(P.Address->Bits == 16 ? "%w" : "%k") + Index;
    Operand =
        (P.Segment.empty() ? "" : "%%" + P.Segment + ":") + "(" + Operand + ")";
  }
  std::string Asm = "__asm__ volatile(\"" + P.Mnemonic + " " +
                    (P.OperandBytes ? Operand : "%0");
  if (P.Verify)
    Asm += "\\n\\tsetz %0";
  Asm += "\" : ";
  if (P.ResultBytes)
    Asm += std::string(P.Verify ? "\"=q\"" : "\"=r\"") + "(" + Temporary + ")";
  Asm += " : ";
  if (P.OperandBytes) {
    const auto *Constraint =
        P.Address ? x86MemoryAddressAsm(P.Address->Bits).GNUConstraint : "r";
    Asm += std::string("\"") + Constraint + "\"(" + Input + ")";
  }
  Asm += P.Verify ? " : \"cc\", \"memory\")" : " : \"memory\")";
  if (P.ResultBytes)
    Asm = "({ " + Type(P.ResultBytes) + " " + Temporary + "; " + Asm + "; " +
          Temporary + "; })";
  if (P.TargetArch == Arch::X86 || !NativeDispatch)
    return Asm;
  std::string Native = P.Builtin;
  std::string NativeArgs = Input;
  if (P.Memory) {
    if (!P.Address)
      Native += "_address";
    NativeArgs += ", " +
                  std::to_string(P.Address ? P.Address->Bits : WordBits) +
                  ", " + std::to_string(P.Address ? P.Address->Prefix : 0);
  }
  Native += "(" + NativeArgs + ")";
  std::string Microsoft = Asm;
  if (P.Memory && !P.Address) {
    const char *Name = P.Id == Intrinsic::Sidt     ? "__sidt"
                       : P.Id == Intrinsic::Lidt   ? "__lidt"
                       : P.Id == Intrinsic::Invlpg ? "__invlpg"
                                                   : nullptr;
    if (Name)
      Microsoft = std::string(Name) + "((void *)(uintptr_t)(" + Args[0] + "))";
  }
  return "NEVERD_X86_DESCRIPTOR_STATE(" + Native + ", " + Microsoft + ", " +
         Asm + ")";
}

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86DESCRIPTORSTATE_H
