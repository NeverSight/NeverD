//===- CRotateBuiltins.h - Fixed-width rotate fallbacks --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_CROTATEBUILTINS_H
#define NEVERD_BACKEND_C_RENDER_CROTATEBUILTINS_H

#include "llvm/ADT/StringRef.h"

namespace neverd {

struct CRotateBuiltin {
  unsigned Bytes;
  llvm::StringRef Preamble;
};

// The source assembler recognizes precisely the compatibility blocks written
// by the C emitter. Keep their complete spelling in one place.
inline constexpr CRotateBuiltin CRotateBuiltins[] = {
#define NEVERD_C_ROTATE_BUILTIN(Bytes, Bits, Mask, Type)                       \
  {Bytes, "#if defined(__GNUC__) && !defined(__clang__) && "                   \
          "!__has_builtin(__builtin_rotateleft" #Bits ")\n"                    \
          "#define __builtin_rotateleft" #Bits "(V, A) __extension__ ({ \\\n"  \
          "    __auto_type __neverd_rotate_value = (" Type ")(V); \\\n"        \
          "    unsigned int __neverd_rotate_amount = (A) & " #Mask "u; \\\n"   \
          "    (" Type ")(__neverd_rotate_amount ? "                           \
          "((__neverd_rotate_value << __neverd_rotate_amount) | "              \
          "(__neverd_rotate_value >> (" #Bits " - __neverd_rotate_amount))) "  \
          ": __neverd_rotate_value); })\n#endif\n\n"},
#include "neverd/backend/c/render/CRotateBuiltins.def"
};

} // namespace neverd

#endif
