//===- X86BitTest.h - Native memory bit C projection ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve signed bit addressing, carry, complete operand access and faults.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86BITTEST_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86BITTEST_H

#include "neverd/backend/llvm/X86/LLVMX86BitTest.h"

#include <array>
#include <string>

namespace neverd {

inline std::string renderX86BitTestCall(
    X86BitTestOperation Operation, const std::array<std::string, 2> &Args,
    const std::string &Result, const std::array<std::string, 3> &Locals) {
  const auto &[Base, Index, Carry] = Locals;
  const std::string Type = Operation.Bits == 16   ? "short"
                           : Operation.Bits == 32 ? "int"
                                                  : "long long";
  const std::string Suffix = Operation.Bits == 16   ? "w"
                             : Operation.Bits == 32 ? "l"
                                                    : "q";
  const std::string Builtin = std::string("__builtin_ia32_") +
                              Operation.Mnemonic + "_mem" +
                              std::to_string(Operation.Bits);
  std::string Text =
      "{\n  void *" + Base + " = (void *)(" + Args[0] + ");\n  " + Type + " " +
      Index + " = (" + Type + ")(" + Args[1] + ");\n  unsigned char " + Carry +
      ";\n#if __has_builtin(" + Builtin + ")\n  " + Carry + " = " + Builtin +
      "(" + Base + ", " + Index + ");\n#else\n  __asm__ volatile(\"" +
      Operation.Mnemonic + Suffix + " %2,(%1)\" : \"=@ccc\"(" + Carry +
      ") : \"r\"(" + Base + "), \"r\"(" + Index +
      ") : \"cc\", \"memory\");\n#endif\n";
  if (!Result.empty())
    Text += "  " + Result + " = " + Carry + ";\n";
  return Text + "}\n";
}

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86BITTEST_H
