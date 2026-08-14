//===- RuntimeSymbolRegistry.h - Closed runtime symbol table ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the host-side, exact-name symbol boundary used when generated
/// translation objects are linked in-process.  The registry never consults
/// process symbols or applies platform spelling rules.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_RUNTIMESYMBOLREGISTRY_H
#define NEVERD_TRANSLATE_RUNTIMESYMBOLREGISTRY_H

#include "neverd/translate/RuntimeHelpers.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace neverd::translate {

inline constexpr uint16_t kRuntimeSymbolRegistryVersionV1 = 1;

/// Stable failure categories for the version-1 runtime symbol registry.
/// Append new values without renumbering existing ones.
enum class RuntimeSymbolRegistryErrorCode : uint8_t {
  UnknownSymbol = 0,
  InvalidName = 1,
  DuplicateName = 2,
  BindingNotInABI = 3,
  MissingBinding = 4,
  HelperClassMismatch = 5,
  InvalidFunctionPointers = 6,
  NullAddress = 7,
  InvalidABISignature = 8,
};

class RuntimeSymbolRegistryError final
    : public llvm::ErrorInfo<RuntimeSymbolRegistryError> {
public:
  static char ID;

  RuntimeSymbolRegistryError(RuntimeSymbolRegistryErrorCode Reason,
                             std::string SymbolName = {});

  RuntimeSymbolRegistryErrorCode reason() const { return Reason; }
  llvm::StringRef symbolName() const { return SymbolName; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  RuntimeSymbolRegistryErrorCode Reason;
  std::string SymbolName;
};

/// One owned, native runtime symbol entry.  Entries contain an in-process
/// address and therefore must never be serialized or used as cache identity.
class RuntimeSymbolEntryV1 {
public:
  llvm::StringRef name() const { return Name; }
  RuntimeABIHelperClassV1 helperClass() const { return HelperClass; }
  llvm::orc::ExecutorAddr address() const { return Address; }

private:
  friend class RuntimeSymbolRegistryV1;

  RuntimeSymbolEntryV1(std::string Name, RuntimeABIHelperClassV1 HelperClass,
                       llvm::orc::ExecutorAddr Address)
      : Name(std::move(Name)), HelperClass(HelperClass), Address(Address) {}

  std::string Name;
  RuntimeABIHelperClassV1 HelperClass;
  llvm::orc::ExecutorAddr Address;
};

/// A complete, validated binding of the fixed version-1 runtime ABI.
///
/// Lookup is exact and confined to entries(); there is no fallback to the
/// current process, dynamic loader, prefix matching, or symbol normalization.
/// Entries are owned and sorted by their bytewise names.  identity() is a
/// versioned digest of the runtime ABI shape and deliberately excludes native
/// addresses so it is stable across ASLR, hosts, and locales.
class RuntimeSymbolRegistryV1 {
public:
  /// Construct the production registry from runtimeABIHelperBindingsV1().
  static llvm::Expected<RuntimeSymbolRegistryV1> create();

  /// Construct from a borrowed binding table, primarily for validation tests
  /// and explicitly provisioned hosts.  Bindings are consumed only during the
  /// call; the resulting registry owns all names and addresses.  This overload
  /// validates ABI shape but does not assert provenance of alternate helper
  /// implementations.  Production linking should use create().
  static llvm::Expected<RuntimeSymbolRegistryV1>
  create(llvm::ArrayRef<RuntimeABIHelperBindingV1> Bindings);

  /// Entries remain valid until this registry is moved from or destroyed.
  llvm::ArrayRef<RuntimeSymbolEntryV1> entries() const { return Entries; }

  /// Return sorted exact-name views.  The returned vector owns the views, not
  /// their bytes; the registry must remain alive and must not be moved while
  /// any returned StringRef is in use.
  std::vector<llvm::StringRef> names() const;

  /// Return the exact allowlist shape accepted by TranslationArtifactPolicyV1.
  /// Its lifetime contract is identical to names().
  std::vector<llvm::StringRef> artifactVerifierAllowlist() const;

  /// Resolve only a complete, byte-for-byte symbol name.
  llvm::Expected<llvm::orc::ExecutorAddr> lookup(llvm::StringRef Name) const;

  llvm::StringRef identity() const { return Identity; }

private:
  RuntimeSymbolRegistryV1(std::vector<RuntimeSymbolEntryV1> Entries,
                          std::string Identity)
      : Entries(std::move(Entries)), Identity(std::move(Identity)) {}

  std::vector<RuntimeSymbolEntryV1> Entries;
  std::string Identity;
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_RUNTIMESYMBOLREGISTRY_H
