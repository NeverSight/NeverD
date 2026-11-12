//===- MIPSFPAssembly.h - Native FP operand transport --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_MIPS_MIPSFPASSEMBLY_H
#define NEVERD_BACKEND_MIPS_MIPSFPASSEMBLY_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
namespace neverd::mips {
struct FPNativeAssembly {
  std::string Text;
  std::string Constraints;
  unsigned Pointers = 0;
  bool ClobbersAT = false;
  bool Read = false;
  bool Write = false;
  std::string llvmText() const {
    std::string S;
    for (char C : Text) {
      if (C == '$')
        S += '$';
      S += C == '%' ? '$' : C;
    }
    return S;
  }
  std::string cStatement(llvm::ArrayRef<std::string> Arguments,
                         llvm::StringRef Result = {}) const {
    std::string S;
    llvm::raw_string_ostream OS(S);
    OS << "__asm__ volatile(\"";
    OS.write_escaped(Text);
    OS << "\" : ";
    if (Read)
      OS << "\"=r\"(" << Result << ")";
    OS << " : ";
    for (unsigned I = 0; I < Arguments.size(); ++I) {
      if (I)
        OS << ", ";
      OS << "\"r\"(" << Arguments[I] << ")";
    }
    OS << " : \"memory\"";
    if (ClobbersAT)
      OS << ", \"$1\"";
    for (unsigned I = 1; I < Pointers; ++I)
      OS << ", \"$f" << 2 * (I - 1) << "\"";
    OS << ");";
    return S;
  }
};

} // namespace neverd::mips
#endif // NEVERD_BACKEND_MIPS_MIPSFPASSEMBLY_H
