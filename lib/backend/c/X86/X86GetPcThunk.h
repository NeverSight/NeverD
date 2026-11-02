//===- X86GetPcThunk.h - C projection of i386 get-PC helpers ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_BACKEND_C_X86_X86GETPCTHUNK_H
#define NEVERD_LIB_BACKEND_C_X86_X86GETPCTHUNK_H
#include "neverd/loader/X86/X86GetPcThunk.h"

#include "llvm/Support/raw_ostream.h"

namespace neverd::c_stub {
/// The helper \p Name, with \p Symbol above it: the comment that names the
/// symbol its identifier is spelled from (`__x86.get_pc_thunk.bx`), if any.
inline void writeGetPcThunk(llvm::raw_ostream &OS, llvm::StringRef Name,
                            llvm::StringRef ReturnType, unsigned Register,
                            llvm::StringRef Symbol = {}) {
  if (!Symbol.empty())
    OS << "/* " << Symbol << " */\n";
  OS << "/* i386 get-PC helper: preserves every other register and flags. */\n"
        "#if !defined(__i386__)\n"
        "#error \"i386 get-PC helpers require an i386 target\"\n"
        "#endif\n"
        "__attribute__((naked)) "
     << ReturnType << " " << Name
     << "(void) {\n"
        "    __asm__(\"";
  OS.write_escaped(x86GetPcThunkAssembly(Register));
  OS << "\");\n}\n";
}
} // namespace neverd::c_stub
#endif
