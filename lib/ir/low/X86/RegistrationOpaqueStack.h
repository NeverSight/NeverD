//===- RegistrationOpaqueStack.h - Call-independent exits ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove explicit return-stack restoration without a nested call contract.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONOPAQUESTACK_H
#define NEVERD_IR_LOW_X86_REGISTRATIONOPAQUESTACK_H

#include "neverd/Common.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd {
struct BinaryImage;
struct LowFunc;

namespace registration_abi {

/// A plain PE32 body may discard every nested call's unknown stack effect by
/// explicitly restoring a nonvolatile register anchor. Follow every physical
/// edge and return without using import identities, saved memory or inferred
/// callee cleanup. This grants only the ordinary returning ESP adjustment;
/// memory, import preservation, return values and native EH remain separate.
std::optional<uint32_t> projectOpaqueCallReturnStack(const BinaryImage &Image,
                                                     const LowFunc &Function,
                                                     size_t &Work);

} // namespace registration_abi
} // namespace neverd

#endif // NEVERD_IR_LOW_X86_REGISTRATIONOPAQUESTACK_H
