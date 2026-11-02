//===- X87GuardedValueRender.h - Guarded native x87 C assembly --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Render the same guarded native value assembly in HighC and LLVM-C.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_X86_X87GUARDEDVALUERENDER_H
#define NEVERD_BACKEND_C_RENDER_X86_X87GUARDEDVALUERENDER_H

#include "neverd/backend/llvm/X86/LLVMX87GuardedValueAsm.h"

#include <array>
#include <string>

namespace neverd {

inline std::string
renderX87GuardedValueAsm(Intrinsic Id,
                         const std::array<std::string, 4> &Arguments) {
  const std::string Assembly = x87GuardedValueAsm(Id);
  std::string Result = "__asm__ volatile(\"";
  for (size_t I = 0; I < Assembly.size(); ++I) {
    switch (Assembly[I]) {
    case '$':
      if (++I == Assembly.size())
        llvm::report_fatal_error("invalid guarded x87 assembly substitution");
      Result += Assembly[I] == '$' ? "$" : "%" + Assembly.substr(I, 1);
      break;
    case '%':
      Result += "%%";
      break;
    case '\n':
      Result += "\\n";
      break;
    case '\t':
      Result += "\\t";
      break;
    default:
      Result += Assembly[I];
    }
  }
  Result += "\" :: ";
  for (size_t I = 0; I != Arguments.size(); ++I)
    Result += (I ? ", " : "") + std::string("\"r\"(") + Arguments[I] + ")";
  return Result + " : \"eax\", \"cc\", " +
         x87StateCClobbers(X87StateEffect::Reset).str() + ")";
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_X86_X87GUARDEDVALUERENDER_H
