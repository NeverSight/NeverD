//===- X87PortableABI.cpp - Generated portable x87 signatures -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared runtime symbol and type authority for portable LLVM and C emission.
//===----------------------------------------------------------------------===//

#include "neverd/ir/X86/X87PortableABI.h"

#include "neverd/runtime/X87PortableRuntimeC.h"

#include "llvm/Support/ErrorHandling.h"

#include <array>

namespace neverd {
namespace {

template <typename T> constexpr X87PortableABIValueV1 valueKind();
template <> constexpr X87PortableABIValueV1 valueKind<void *>() {
  return X87PortableABIValueV1::Context;
}
template <> constexpr X87PortableABIValueV1 valueKind<uint32_t>() {
  return X87PortableABIValueV1::I32;
}
template <> constexpr X87PortableABIValueV1 valueKind<uint64_t>() {
  return X87PortableABIValueV1::I64;
}

template <typename Function> struct Signature;
template <typename Result, typename... Parameters>
struct Signature<Result (*)(Parameters...) noexcept> {
  static constexpr X87PortableABIValueV1 Return = valueKind<Result>();
  static constexpr std::array Arguments{valueKind<Parameters>()...};
};

const X87PortableHelperSignatureV1 Signatures[] = {
#define NEVERD_X87_PORTABLE_HELPER(Role, Name, Result, Parameters)             \
  {X87PortableHelperV1::Role, #Name, Signature<decltype(&Name)>::Return,       \
   Signature<decltype(&Name)>::Arguments,                                      \
   "extern " #Result " " #Name " " #Parameters ";"},
#include "neverd/ir/X86/X87PortableABI.def"
#undef NEVERD_X87_PORTABLE_HELPER
};

} // namespace

llvm::ArrayRef<X87PortableHelperSignatureV1> x87PortableHelperSignaturesV1() {
  return Signatures;
}

const X87PortableHelperSignatureV1 &
x87PortableHelperSignatureV1(X87PortableHelperV1 Role) {
  for (const auto &Signature : Signatures)
    if (Signature.Role == Role)
      return Signature;
  llvm_unreachable("unknown portable x87 runtime helper");
}

} // namespace neverd
