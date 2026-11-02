//===- AArch64ReturnProtectionC.h - Shared signed frame policy -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Both C projections retain the native compiler's return-signing policy.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64RETURNPROTECTIONC_H
#define NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64RETURNPROTECTIONC_H

#include "neverd/ir/med/AArch64/AArch64ReturnProtection.h"

#include <string>

namespace neverd {

#define NEVERD_A64_RETURN_C_TEXT(Name, Text)                                   \
  inline constexpr char Name[] = Text;
#include "neverd/backend/c/render/AArch64/AArch64ReturnProtectionC.def"

inline std::string
aarch64ReturnProtectionCTarget(const AArch64ReturnProtection &Protection) {
  std::string Target = kReturnProtectionCTarget;
  if (Protection.Leaf)
    Target += kReturnProtectionCLeaf;
  if (Protection.BKey)
    Target += kReturnProtectionCBKey;
  return Target;
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64RETURNPROTECTIONC_H
