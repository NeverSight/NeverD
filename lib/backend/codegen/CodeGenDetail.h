//===- CodeGenDetail.h - Shared codegen target-feature helpers --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal helpers shared between the architecture-generic codegen driver
/// (CodeGen.cpp) and the per-target feature detectors
/// (CodeGenX86.cpp, CodeGenAArch64.cpp, CodeGenARM.cpp).
///
/// This header is an implementation detail of the codegen library and should
/// NOT be included by code outside lib/backend/codegen/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_CODEGENDETAIL_H
#define NEVERD_BACKEND_CODEGEN_CODEGENDETAIL_H

#include <set>
#include <string>
#include <utility>

namespace neverd {

/// Synthetic marker inserted into the scanned-name set when the module uses
/// native half-precision (`half`) arithmetic (emitted as ops, not intrinsics),
/// so a per-target feature detector can gate the fp16 feature on it.  Not a
/// real symbol name.
inline constexpr const char *kUsesHalfMarker = "__nd_uses_half";

/// True if any scanned intrinsic/function name contains \p Pat.
inline bool anyContains(const std::set<std::string> &Names, const char *Pat) {
  for (const auto &N : Names)
    if (N.find(Pat) != std::string::npos)
      return true;
  return false;
}

// Architecture-specific target {CPU, feature-string} builders, keyed off the
// set of intrinsic/function names referenced by the module.  CodeGen.cpp scans
// the module once and dispatches to one of these by target architecture.

/// x86/x86-64 SSE/AVX/AES/SHA feature detection (CodeGenX86.cpp).
std::pair<std::string, std::string>
detectTargetFeaturesX86(const std::set<std::string> &Names);

/// AArch64 NEON/crypto/SVE/CRC/PAuth/MTE feature detection
/// (CodeGenAArch64.cpp).
std::pair<std::string, std::string>
detectTargetFeaturesAArch64(const std::set<std::string> &Names);

/// ARM32 VFP/NEON/crypto feature detection (CodeGenARM.cpp).
std::pair<std::string, std::string>
detectTargetFeaturesARM(const std::set<std::string> &Names);

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_CODEGENDETAIL_H
