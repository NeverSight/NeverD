//===- X86ScalarSystem.h - Scalar MSR and port C expressions -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The same native builtin or owned assembly expression for both C routes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86SCALARSYSTEM_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86SCALARSYSTEM_H

#include "neverd/backend/llvm/X86/X86ScalarSystemNative.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace neverd {

inline constexpr const char X86ScalarSystemCompilerDispatch[] =
    "#ifndef NEVERD_X86_SCALAR_SYSTEM\n"
    "#if defined(__has_builtin)\n"
    "#if __has_builtin(__builtin_ia32_rdmsr) && "
    "__has_builtin(__builtin_ia32_wrmsr) && "
    "__has_builtin(__builtin_ia32_inbyte) && "
    "__has_builtin(__builtin_ia32_inword) && "
    "__has_builtin(__builtin_ia32_indword) && "
    "__has_builtin(__builtin_ia32_outbyte) && "
    "__has_builtin(__builtin_ia32_outword) && "
    "__has_builtin(__builtin_ia32_outdword)\n"
    "#define NEVERD_X86_SCALAR_SYSTEM(native, msvc, ...) native\n"
    "#endif\n"
    "#endif\n"
    "#ifndef NEVERD_X86_SCALAR_SYSTEM\n"
    "#if defined(_MSC_VER) && !defined(__clang__)\n"
    "#define NEVERD_X86_SCALAR_SYSTEM(native, msvc, ...) msvc\n"
    "#else\n"
    "#define NEVERD_X86_SCALAR_SYSTEM(native, msvc, ...) __VA_ARGS__\n"
    "#endif\n"
    "#endif\n"
    "#endif\n\n";

inline std::string
renderX86ScalarSystemExpression(const X86ScalarSystemNative &P,
                                const std::vector<std::string> &Arguments) {
  if (Arguments.size() != P.arguments())
    throw std::invalid_argument("invalid scalar x86 system operand count");
  const std::string Type = "uint" + std::to_string(8 * P.Bytes) + "_t";
  const std::string Selector =
      "(uint" + std::to_string(P.Port ? 16 : 32) + "_t)(" + Arguments[0] + ")";
  const std::string Value =
      P.Write ? "(" + Type + ")(" + Arguments[1] + ")" : "";
  const std::string Args = Selector + (P.Write ? ", " + Value : "");
  std::string Asm;
  if (P.Port) {
    if (P.Write)
      Asm = "__asm__ volatile(\"" + std::string(P.Mnemonic) +
            " %0, %1\" : : \"a\"(" + Value + "), \"Nd\"(" + Selector +
            ") : \"memory\")";
    else
      Asm = "({ " + Type + " neverd_port; __asm__ volatile(\"" + P.Mnemonic +
            " %1, %0\" : \"=a\"(neverd_port) : \"Nd\"(" + Selector +
            ") : \"memory\"); neverd_port; })";
  } else if (P.Write) {
    // Capture the C value once before splitting EDX:EAX.
    Asm = "({ uint64_t neverd_msr = " + Value +
          "; __asm__ volatile(\"wrmsr\" : : \"c\"(" + Selector +
          "), \"a\"((uint32_t)neverd_msr), "
          "\"d\"((uint32_t)(neverd_msr >> 32)) : \"memory\"); })";
  } else {
    Asm =
        "({ uint32_t neverd_msr_lo, neverd_msr_hi; "
        "__asm__ volatile(\"rdmsr\" : \"=a\"(neverd_msr_lo), "
        "\"=d\"(neverd_msr_hi) : \"c\"(" +
        Selector +
        ") : \"memory\"); ((uint64_t)neverd_msr_hi << 32) | neverd_msr_lo; })";
  }
  return "NEVERD_X86_SCALAR_SYSTEM(" + std::string(P.Builtin) + "(" + Args +
         "), " + P.Microsoft + "(" + Args + "), " + Asm + ")";
}

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86SCALARSYSTEM_H
