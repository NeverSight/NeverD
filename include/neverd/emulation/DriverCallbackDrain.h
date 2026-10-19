//===- DriverCallbackDrain.h - Explicit foreground callback boundaries ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit foreground callback boundaries.
///

#ifndef NEVERD_EMULATION_DRIVERCALLBACKDRAIN_H
#define NEVERD_EMULATION_DRIVERCALLBACKDRAIN_H

#include "llvm/Support/Error.h"

#include <initializer_list>
#include <optional>
#include <string_view>

namespace neverd::emulation {
enum class DriverCallbackDrainPolicy { Complete, ForegroundReady };
constexpr std::string_view
callbackDrainPolicyName(DriverCallbackDrainPolicy P) {
  switch (P) {
  case DriverCallbackDrainPolicy::Complete:
    return "complete";
  case DriverCallbackDrainPolicy::ForegroundReady:
    return "foreground-ready";
  }
  return {};
}
constexpr std::optional<DriverCallbackDrainPolicy>
parseCallbackDrainPolicy(std::string_view Name) {
  for (auto P : {DriverCallbackDrainPolicy::Complete,
                 DriverCallbackDrainPolicy::ForegroundReady})
    if (callbackDrainPolicyName(P) == Name)
      return P;
  return std::nullopt;
}
inline llvm::Error
validateDriverCallbackDrainPolicy(std::optional<DriverCallbackDrainPolicy> P) {
  if (P && callbackDrainPolicyName(*P).empty())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "invalid callback_drain_policy");
  return llvm::Error::success();
}
constexpr bool
foregroundReadyCallbacks(std::optional<DriverCallbackDrainPolicy> P) {
  return P == DriverCallbackDrainPolicy::ForegroundReady;
}
} // namespace neverd::emulation
#endif
