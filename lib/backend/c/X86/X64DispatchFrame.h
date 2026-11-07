//===- X64DispatchFrame.h - C source for physical dispatch -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prints an explicit native bridge without inventing a dispatcher C call.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_BACKEND_C_X86_X64DISPATCHFRAME_H
#define NEVERD_LIB_BACKEND_C_X86_X64DISPATCHFRAME_H

#include "neverd/loader/X86/X64DispatchFrame.h"

#include "llvm/Support/raw_ostream.h"

namespace neverd::c_stub {

inline void writeDispatchFramePreamble(llvm::raw_ostream &OS) {
  OS << "/* neverd.native-bridge: physical Win64 dispatch frame.\n"
        " * Requires CPL0 and native linkage/dispatch symbols from the "
        "original\n"
        " * environment. Executes the original frame construction and "
        "selector\n"
        " * transfer without a C prologue or dispatcher call. Other incoming\n"
        " * registers remain live; the dispatcher owns execution and return.\n"
        " * This body supplies no portable C or exception-unwind contract. */\n"
        "#if !defined(__x86_64__)\n"
        "#error \"physical dispatch bridges require an x86-64 target\"\n"
        "#endif\n"
        "__attribute__((naked, ms_abi))\n";
}

inline void writeDispatchFrameBody(llvm::raw_ostream &OS,
                                   llvm::StringRef Assembly) {
  OS << "    __asm__(\"";
  OS.write_escaped(Assembly);
  OS << "\");\n}\n";
}

} // namespace neverd::c_stub

#endif // NEVERD_LIB_BACKEND_C_X86_X64DISPATCHFRAME_H
