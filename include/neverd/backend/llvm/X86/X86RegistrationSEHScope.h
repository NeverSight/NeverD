//===- X86RegistrationSEHScope.h - Dynamic source scopes ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86REGISTRATIONSEHSCOPE_H
#define NEVERD_BACKEND_LLVM_X86_X86REGISTRATIONSEHSCOPE_H

#include <cstdint>
#include <limits>

namespace neverd::x86_registration {
/// A source block with several checked incoming states selects a scope from
/// its private logical try-level cell. This is never a native table index.
inline constexpr int32_t DynamicScopeLevel =
    std::numeric_limits<int32_t>::min();
} // namespace neverd::x86_registration

#endif // NEVERD_BACKEND_LLVM_X86_X86REGISTRATIONSEHSCOPE_H
