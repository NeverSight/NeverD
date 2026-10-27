//===- X87ContextRender.h - Shared complete x87 C transport -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Render exactly the owned LLVM operation, with its operands captured once.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_C_RENDER_X86_X87CONTEXTRENDER_H
#define NEVERD_BACKEND_C_RENDER_X86_X87CONTEXTRENDER_H

#include "neverd/backend/llvm/X86/LLVMX87ContextAsm.h"

#include <vector>

namespace neverd {
inline std::string x87ContextCAsm(const X87ContextAsmIdentity &Id) {
  std::string Text = x87ContextAsm(Id.Phase, Id.Selector, Id.Space), Out;
  for (size_t I = 0; I < Text.size(); ++I) {
    const char C = Text[I];
    if (C == '\n')
      Out += "\\n";
    else if (C == '\t')
      Out += "\\t";
    else if (C == '$' && I + 1 < Text.size() && Text[I + 1] == '$') {
      Out += '$';
      ++I;
    } else if (C == '$' && I + 1 < Text.size() && Text[I + 1] >= '0' &&
               Text[I + 1] <= '3') {
      Out += '%';
    } else if (C == '%')
      Out += "%%";
    else
      Out += C;
  }
  return Out;
}

inline std::string renderX87ContextAsm(const X87ContextAsmIdentity &Id,
                                       const std::vector<std::string> &Args) {
  const bool Step = Id.Phase == X87ContextAsmPhase::Step;
  const bool Restore = Id.Phase == X87ContextAsmPhase::Restore ||
                       Id.Phase == X87ContextAsmPhase::RestoreHost;
  std::string Text;
  if (Step)
    Text = "do { register void *x87_memory_operand __asm__(\"r10\") = "
           "(void *)(uintptr_t)(" +
           Args[2] + "); ";
  Text += "__asm__ volatile(\"" + x87ContextCAsm(Id) + "\" :: ";
  for (unsigned I = 0; I < Args.size(); ++I) {
    if (I)
      Text += ", ";
    if (Step && I == 2)
      Text += "\"r\"(x87_memory_operand)";
    else
      Text += "\"r\"(" + Args[I] + ")";
  }
  Text += " : \"memory\"";
  if (Step)
    Text += ", \"rax\", \"r11\", \"cc\"";
  if (Step || Restore) {
    Text += ", \"st\", \"st(1)\", \"st(2)\", \"st(3)\", \"st(4)\", \"st(5)\", "
            "\"st(6)\", \"st(7)\"";
    for (unsigned I = 0; I < 8; ++I)
      Text += ", \"mm" + std::to_string(I) + "\"";
    for (unsigned I = 0; I < 16; ++I)
      Text += ", \"xmm" + std::to_string(I) + "\"";
  }
  Text += ");";
  if (Step)
    Text += " } while (0);";
  return Text;
}
} // namespace neverd
#endif
