//===- X86SystemRegister.h - CR/DR C expressions -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Both C projections preserve the owned native register access and effects.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86SYSTEMREGISTER_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86SYSTEMREGISTER_H

#include "neverd/backend/llvm/X86/X86SystemRegisterAsm.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace neverd {

// Clang declares several Microsoft CR/DR names without implementing them as
// builtins. Object format alone cannot authorize those spellings: an ordinary
// external call loses the architectural access. Select at C compilation time.
inline constexpr const char X86SystemRegisterCompilerDispatch[] =
    "#ifndef NEVERD_X86_SYSTEM_REGISTER\n"
    "#if defined(_MSC_VER) && !defined(__clang__)\n"
    "#define NEVERD_X86_SYSTEM_REGISTER(msvc, ...) msvc\n"
    "#else\n"
    "#define NEVERD_X86_SYSTEM_REGISTER(msvc, ...) __VA_ARGS__\n"
    "#endif\n"
    "#endif\n"
    "#ifndef NEVERD_X86_SYSTEM_REGISTER_NATIVE\n"
    "#if defined(__has_builtin)\n"
    "#if __has_builtin(__builtin_ia32_readcr) && "
    "__has_builtin(__builtin_ia32_writecr) && "
    "__has_builtin(__builtin_ia32_readdr) && "
    "__has_builtin(__builtin_ia32_writedr)\n"
    "#define NEVERD_X86_SYSTEM_REGISTER_NATIVE(native, ...) native\n"
    "#endif\n"
    "#endif\n"
    "#ifndef NEVERD_X86_SYSTEM_REGISTER_NATIVE\n"
    "#define NEVERD_X86_SYSTEM_REGISTER_NATIVE(native, ...) __VA_ARGS__\n"
    "#endif\n"
    "#endif\n\n";

inline std::string
renderX86SystemRegisterExpression(const X86SystemRegisterAsm &P, bool Microsoft,
                                  const std::vector<std::string> &Arguments) {
  if (Arguments.size() != (P.Write ? 1 : 0))
    throw std::invalid_argument("invalid x86 system register operand count");
  const std::string Type = "uint" + std::to_string(8 * P.Bytes) + "_t";
  std::string Native;
  if (P.Write)
    Native = "__asm__ volatile(\"mov %0, %" + P.Register + "\" : : \"r\"((" +
             Type + ")(" + Arguments[0] + ")) : \"cc\", \"memory\")";
  else
    Native = "({ " + Type +
             " neverd_system_register; __asm__ volatile(\"mov %" + P.Register +
             ", %0\" : \"=r\"(neverd_system_register) : : \"cc\", "
             "\"memory\"); neverd_system_register; })";
  if (Microsoft) {
    std::string Builtin = std::string(intrinsicCName(P.Id)) + "(";
    if (P.Debug)
      Builtin += std::to_string(P.Index) + (P.Write ? ", " : "");
    if (P.Write)
      Builtin += Arguments[0];
    Native = "NEVERD_X86_SYSTEM_REGISTER(" + Builtin + "), " + Native + ")";
  }
  if (P.Bytes != 8)
    return Native;
  const std::string Builtin =
      P.Builtin + "(" + std::to_string(P.Index) +
      (P.Write ? ", (" + Type + ")(" + Arguments[0] + ")" : "") + ")";
  return "NEVERD_X86_SYSTEM_REGISTER_NATIVE(" + Builtin + ", " + Native + ")";
}

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86SYSTEMREGISTER_H
