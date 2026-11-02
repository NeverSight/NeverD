//===- AArch64ExclusiveC.h - Shared exclusive access C text -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Both C projections write an AArch64 exclusive load or store (LDXR, LDAXR,
/// STXR, STLXR and their pair forms) as the Clang and GCC builtin of the same
/// width and order, which selects the same instruction.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64EXCLUSIVEC_H
#define NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64EXCLUSIVEC_H

#include "llvm/ADT/StringRef.h"

#include <string>

namespace neverd {

/// The C type of an exclusive access of \p Bytes, spelling a pair's 128 bits
/// as \p Pair; null for a width no exclusive instruction accesses.
inline const char *aarch64ExclusiveAccessType(unsigned Bytes,
                                              const char *Pair) {
  switch (Bytes) {
  case 1:
    return "uint8_t";
  case 2:
    return "uint16_t";
  case 4:
    return "uint32_t";
  case 8:
    return "uint64_t";
  case 16:
    return Pair;
  default:
    return nullptr;
  }
}

/// The exclusive load of a \p Type at the integer \p Address.
inline std::string aarch64ExclusiveLoadC(bool Acquire, llvm::StringRef Type,
                                         llvm::StringRef Address) {
  return std::string(Acquire ? "__builtin_arm_ldaex" : "__builtin_arm_ldrex") +
         "((const volatile " + Type.str() + " *)(uintptr_t)(" + Address.str() +
         "))";
}

/// The exclusive store of \p Value as a \p Type at the integer \p Address:
/// zero where it stored.
inline std::string aarch64ExclusiveStoreC(bool Release, llvm::StringRef Type,
                                          llvm::StringRef Value,
                                          llvm::StringRef Address) {
  return std::string(Release ? "__builtin_arm_stlex" : "__builtin_arm_strex") +
         "((" + Type.str() + ")(" + Value.str() + "), (volatile " + Type.str() +
         " *)(uintptr_t)(" + Address.str() + "))";
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64EXCLUSIVEC_H
