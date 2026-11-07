//===- HostCompiler.h - What the host test compiler builds --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Tests that build generated C or IR on the host depend on the host and on its
/// Clang: x86 instructions need an x86 host, and Apple Clang can lag the LLVM
/// that NeverD links. The same output stays covered on a host that can build
/// it, so a test skips with the reason instead of reporting a product failure.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_HOSTCOMPILER_H
#define NEVERD_UNITTESTS_HOSTCOMPILER_H

#include "TestProcess.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace neverd::test {

/// Whether this host builds and runs x86 code.
inline constexpr bool hostRunsX86() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
  return true;
#else
  return false;
#endif
}

/// Whether C of any architecture names x86 instructions: only an x86 compiler
/// accepts these headers and helpers.
inline bool namesX86Instructions(std::string_view C) {
  for (std::string_view Mark :
       {"<immintrin.h>", "<x86intrin.h>", "neverd_x86_"})
    if (C.find(Mark) != std::string_view::npos)
      return true;
  return false;
}

/// The reason to skip C of unknown architecture on this host, or nothing when
/// it can build and run here.
inline std::optional<std::string> cannotRunC(std::string_view C) {
  if (hostRunsX86() || !namesX86Instructions(C))
    return std::nullopt;
  return std::string(
      "the C retains x86 instructions; running it requires an x86 host");
}

/// As cannotRunC, for C known to come from an x86 image, whose inline assembly
/// is x86 assembly too.
inline std::optional<std::string> cannotRunX86C(std::string_view C) {
  if (hostRunsX86() ||
      (!namesX86Instructions(C) && C.find("__asm__") == std::string_view::npos))
    return std::nullopt;
  return std::string(
      "the C retains x86 instructions; running it requires an x86 host");
}

/// Whether a cross build failed only because this host has no C library headers
/// for its target. A Linux host's /usr/include serves its cross builds too, so
/// there the same message is a real failure.
inline bool lacksCrossHeaders(std::string_view Diagnostics) {
#ifdef __linux__
  (void)Diagnostics;
  return false;
#else
  return Diagnostics.find("file not found") != std::string_view::npos;
#endif
}

/// Whether \p Compiler builds \p Source, given in the language \p Extension
/// names, with \p Arguments. Silent, and false when anything goes wrong.
inline bool compilerAccepts(llvm::StringRef Compiler, llvm::StringRef Extension,
                            llvm::StringRef Source,
                            std::initializer_list<llvm::StringRef> Arguments) {
  llvm::SmallString<128> Input, Output;
  int Descriptor = -1;
  if (llvm::sys::fs::createTemporaryFile("neverd-compiler-probe", Extension,
                                         Descriptor, Input))
    return false;
  {
    llvm::raw_fd_ostream OS(Descriptor, /*shouldClose=*/true);
    OS << Source;
  }
  if (llvm::sys::fs::createTemporaryFile("neverd-compiler-probe", "o",
                                         Output)) {
    (void)llvm::sys::fs::remove(Input);
    return false;
  }
  std::vector<llvm::StringRef> Command = {Compiler};
  Command.insert(Command.end(), Arguments);
  Command.insert(Command.end(), {"-c", Input, "-o", Output});
  const std::optional<llvm::StringRef> Quiet[] = {
      llvm::StringRef(), llvm::StringRef(), llvm::StringRef()};
  const int Status =
      llvm::sys::ExecuteAndWait(Compiler, Command, std::nullopt, Quiet, 60);
  (void)llvm::sys::fs::remove(Input);
  (void)llvm::sys::fs::remove(Output);
  return Status == 0;
}

/// Whether \p Compiler parses the textual IR the linked LLVM prints.
inline bool compilerReadsCurrentIR(llvm::StringRef Compiler) {
  return compilerAccepts(Compiler, "ll",
                         "define i1 @f(ptr captures(none) %p, i32 %a) {\n"
                         "  %c = icmp samesign ult i32 %a, 1\n"
                         "  ret i1 %c\n"
                         "}\n",
                         {"-Wno-override-module"});
}

/// Whether \p Compiler supports the _BitInt widths LLVM C spells wide values
/// in.
inline bool compilerHasWideBitInt(llvm::StringRef Compiler) {
  return compilerAccepts(Compiler, "c", "unsigned _BitInt(129) value;\n", {});
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_HOSTCOMPILER_H
