//===- X87PortableABI.h - Generated portable x87 signatures -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact generated-call signatures derived from the runtime C declarations.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X87PORTABLEABI_H
#define NEVERD_IR_X86_X87PORTABLEABI_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>

namespace neverd {

inline constexpr uint32_t kX87PortableExecutionABIVersion = 1;

enum class X87PortableHelperV1 {
#define NEVERD_X87_PORTABLE_HELPER(Role, Name, Result, Parameters) Role,
#include "neverd/ir/X86/X87PortableABI.def"
#undef NEVERD_X87_PORTABLE_HELPER
};

enum class X87PortableABIValueV1 { Context, I32, I64 };

struct X87PortableHelperSignatureV1 {
  X87PortableHelperV1 Role;
  llvm::StringRef Name;
  X87PortableABIValueV1 Result;
  llvm::ArrayRef<X87PortableABIValueV1> Parameters;
  llvm::StringRef CDeclaration;
};

/// Exact types are derived from the same C declarations used by the runtime.
/// Consumers never infer a declaration from source ABI or observed arguments.
llvm::ArrayRef<X87PortableHelperSignatureV1> x87PortableHelperSignaturesV1();
const X87PortableHelperSignatureV1 &
x87PortableHelperSignatureV1(X87PortableHelperV1 Role);

} // namespace neverd

#endif // NEVERD_IR_X86_X87PORTABLEABI_H
