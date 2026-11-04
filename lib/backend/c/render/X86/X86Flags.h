//===- X86Flags.h - Exact-width FLAGS C expressions ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prefer native word intrinsics; preserve the caller's red zone in GNU asm.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86FLAGS_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86FLAGS_H

#include "neverd/backend/llvm/X86/X86FlagsNative.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace neverd {

inline constexpr const char X86Flags16CompilerDispatch[] =
    "#ifndef NEVERD_X86_FLAGS16\n"
    "#if defined(__has_builtin)\n"
    "#if __has_builtin(__builtin_ia32_readeflags_u16) && "
    "__has_builtin(__builtin_ia32_writeeflags_u16)\n"
    "#define NEVERD_X86_FLAGS16(native, ...) native\n"
    "#endif\n"
    "#endif\n"
    "#ifndef NEVERD_X86_FLAGS16\n"
    "#if defined(__GNUC__) || defined(__clang__)\n"
    "#define NEVERD_X86_FLAGS16(native, ...) __VA_ARGS__\n"
    "#else\n"
    "#error Word FLAGS requires native x86 intrinsics or GNU assembly\n"
    "#endif\n"
    "#endif\n"
    "#endif\n"
    "#if defined(__x86_64__)\n"
    "#define NEVERD_X86_FLAGS16_ENTER \"leaq -128(%%rsp), %%rsp\\n\\t\"\n"
    "#define NEVERD_X86_FLAGS16_LEAVE \"leaq 128(%%rsp), %%rsp\\n\\t\"\n"
    "#else\n"
    "#define NEVERD_X86_FLAGS16_ENTER \"\"\n"
    "#define NEVERD_X86_FLAGS16_LEAVE \"\"\n"
    "#endif\n\n";

inline std::string
renderX86FlagsExpression(const X86FlagsNative &P,
                         const std::vector<std::string> &Arguments) {
  if (Arguments.size() != unsigned(P.write()))
    throw std::invalid_argument("invalid x86 FLAGS operand count");
  const std::string Native =
      std::string(P.Builtin) + "(" +
      (P.write()
           ? "(uint" + std::to_string(8 * P.Bytes) + "_t)(" + Arguments[0] + ")"
           : "") +
      ")";
  if (P.Bytes != 2)
    return Native;
  // LEA leaves flags intact and makes the temporary push safe even when the
  // compiler holds live objects in the SysV red zone. The argument is read
  // once.
  const std::string Assembly =
      P.write()
          ? "({ uint16_t neverd_flags = (uint16_t)(" + Arguments[0] +
                "); __asm__ volatile(NEVERD_X86_FLAGS16_ENTER "
                "\"pushw %0\\n\\tpopfw\\n\\t\" NEVERD_X86_FLAGS16_LEAVE : : "
                "\"r\"(neverd_flags) : \"memory\", \"cc\"); })"
          : "({ uint16_t neverd_flags; __asm__ volatile("
            "NEVERD_X86_FLAGS16_ENTER \"pushfw\\n\\tpopw %0\\n\\t\" "
            "NEVERD_X86_FLAGS16_LEAVE : \"=r\"(neverd_flags) : : \"memory\"); "
            "neverd_flags; })";
  return "NEVERD_X86_FLAGS16(" + Native + ", " + Assembly + ")";
}

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86FLAGS_H
