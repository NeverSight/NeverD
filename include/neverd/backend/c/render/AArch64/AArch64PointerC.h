//===- AArch64PointerC.h - Pointer result statements -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// GNU statement expressions need an explicit discard when only their
/// instruction's effects survive. Both source routes retain the operation.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64POINTERC_H
#define NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64POINTERC_H

#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"
#include "neverd/ir/intrinsics/AArch64/AArch64CPA.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLoad.h"
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.h"

namespace neverd {

#define NEVERD_A64_POINTER_C_TEXT(Name, Text)                                  \
  inline constexpr char Name[] = Text;
#include "neverd/backend/c/render/AArch64/AArch64PointerC.def"

inline bool aarch64PointerCRequiresDiscard(Intrinsic Id) {
  return isAArch64PAuthIntrinsic(Id) || aarch64CPAOperation(Id).has_value() ||
         isAArch64PAuthLoadIntrinsic(Id) ||
         isAArch64AddressMemoryIntrinsic(Id) ||
         isAArch64WritebackMemoryIntrinsic(Id);
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64POINTERC_H
