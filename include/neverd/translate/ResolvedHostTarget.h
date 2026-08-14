//===- ResolvedHostTarget.h - Deterministic host target identity -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_RESOLVEDHOSTTARGET_H
#define NEVERD_TRANSLATE_RESOLVEDHOSTTARGET_H

#include "neverd/translate/TranslationOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace neverd::translate {

/// Immutable, normalized host identity used by code generation and caches.
///
/// requestedTarget() preserves the caller's request verbatim.  The remaining
/// accessors describe the resolved target and therefore must be used for LLVM
/// target-machine creation, object validation, and cache lookup.
class ResolvedHostTarget final {
public:
  static constexpr uint32_t CacheIdentityVersion = 1;

  const HostTarget &requestedTarget() const { return RequestedTarget; }
  GuestArchitecture architecture() const { return Architecture; }
  llvm::StringRef triple() const { return Triple; }
  llvm::StringRef cpu() const { return CPU; }
  llvm::ArrayRef<std::string> features() const { return Features; }

  /// Returns a locale-independent, versioned text identity.  It identifies
  /// both the request kind and every resolved code-generation input.
  llvm::StringRef cacheKey() const { return CacheKey; }

  static llvm::Expected<ResolvedHostTarget>
  resolve(const TranslationOptions &Options);

private:
  ResolvedHostTarget(HostTarget RequestedTarget, GuestArchitecture Architecture,
                     std::string Triple, std::string CPU,
                     std::vector<std::string> Features, std::string CacheKey)
      : RequestedTarget(std::move(RequestedTarget)), Architecture(Architecture),
        Triple(std::move(Triple)), CPU(std::move(CPU)),
        Features(std::move(Features)), CacheKey(std::move(CacheKey)) {}

  HostTarget RequestedTarget;
  GuestArchitecture Architecture;
  std::string Triple;
  std::string CPU;
  std::vector<std::string> Features;
  std::string CacheKey;
};

llvm::Expected<ResolvedHostTarget>
resolveHostTarget(const TranslationOptions &Options);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_RESOLVEDHOSTTARGET_H
