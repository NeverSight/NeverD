//===- X86HypercallEncoding.h - Original hypercall bytes --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Qualify canonical VMCALL/VMMCALL bytes without inferring a hypervisor ABI.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_X86_X86HYPERCALLENCODING_H
#define NEVERD_LIFT_X86_X86HYPERCALLENCODING_H

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>

namespace neverd {

enum class X86HypercallKind : uint8_t { Vmcall, Vmmcall };

inline std::optional<X86HypercallKind>
x86HypercallEncoding(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() != 3 || Bytes[0] != 0x0f || Bytes[1] != 0x01)
    return std::nullopt;
  if (Bytes[2] == 0xc1)
    return X86HypercallKind::Vmcall;
  if (Bytes[2] == 0xd9)
    return X86HypercallKind::Vmmcall;
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_LIFT_X86_X86HYPERCALLENCODING_H
