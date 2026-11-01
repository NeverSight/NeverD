//===- X86DivRem.h - Native quotient/remainder C projection ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One full-dividend operation, retaining both outputs and native divide
/// errors.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86DIVREM_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86DIVREM_H

#include "neverd/backend/llvm/X86/LLVMX86DivRem.h"

#include <array>
#include <string>

namespace neverd {

inline std::string renderX86DivRemCall(
    X86DivRemOperation Operation, const std::array<std::string, 3> &Args,
    const std::string &Result, const std::array<std::string, 4> &Locals) {
  const auto &[Q, R, D, AX] = Locals;
  const std::string Type = Operation.Bits == 8    ? "unsigned char"
                           : Operation.Bits == 16 ? "unsigned short"
                           : Operation.Bits == 32 ? "unsigned int"
                                                  : "unsigned long long";
  const std::string Builtin = std::string("__builtin_ia32_") +
                              (Operation.Signed ? "sdivrem" : "udivrem") +
                              std::to_string(Operation.Bits);
  std::string Text = "{\n  " + Type + " " + Q + " = (" + Type + ")(" + Args[0] +
                     "), " + R + " = (" + Type + ")(" + Args[1] + "), " + D +
                     " = (" + Type + ")(" + Args[2] + ");\n#if __has_builtin(" +
                     Builtin + ")\n";
  Text += "  " + Q + " = " + Builtin + "(" + Q + ", " + R + ", " + D + ", &" +
          R + ");\n#else\n";
  if (Operation.Bits == 8) {
    Text += "  unsigned short " + AX + " = (unsigned short)(" + Q +
            " | ((unsigned short)" + R +
            " << 8));\n"
            "  __asm__ volatile(\"" +
            std::string(x86DivRemMnemonic(Operation)) + " %1\" : \"+a\"(" + AX +
            ") : \"r\"(" + D +
            ") : \"cc\", \"memory\");\n"
            "  " +
            Q + " = (unsigned char)" + AX +
            ";\n"
            "  " +
            R + " = (unsigned char)(" + AX + " >> 8);\n";
  } else {
    Text += "  __asm__ volatile(\"" +
            std::string(x86DivRemMnemonic(Operation)) + " %2\" : \"+a\"(" + Q +
            "), \"+d\"(" + R + ") : \"r\"(" + D + ") : \"cc\", \"memory\");\n";
  }
  Text += "#endif\n";
  if (!Result.empty())
    Text += "  " + Result + ".field_0 = " + Q + ";\n  " + Result +
            ".field_1 = " + R + ";\n";
  return Text + "}\n";
}

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86DIVREM_H
