//===- KernelExportRegistry.h - Guest kernel export identity --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A session-owned export namespace shared by static and dynamic resolution.
/// Availability is independent of whether an API has an execution model.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELEXPORTREGISTRY_H
#define NEVERD_EMULATION_KERNELEXPORTREGISTRY_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>

namespace neverd::emulation {
struct DriverOptions;
struct DriverImport;

class KernelExportRegistry {
public:
  enum class ExportKind {
    ModuleExport,
    FrameworkFunction,
    DMAFunction,
    ProviderFunction
  };

  struct Export {
    std::string Name;
    std::string Module;
    uint64_t Address = 0;
    ExportKind Kind = ExportKind::ModuleExport;
    /// Guest framework globals, DMA adapter or PDO, never a host pointer.
    /// Zero for ordinary PE imports. The owning model checks its lifetime.
    uint64_t Binding = 0;
  };

  /// Recognized PE import providers, folded case-insensitively. The kernel's
  /// multiprocessor spelling aliases ntoskrnl; HAL and WDFLDR keep their own
  /// namespaces.
  /// Returned names have static storage. Loader validation shares this policy.
  static std::optional<llvm::StringRef>
  canonicalImportModule(llvm::StringRef Module);
  /// Stable image identity shared by PE exports and module enumeration.
  static std::optional<uint64_t> moduleBase(llvm::StringRef Module);
  /// Service bodies are opaque, including unused addresses in their gates.
  static bool overlapsThunk(uint64_t Address, uint64_t Size = 1);

  llvm::Error initialize(const DriverOptions &Options);
  llvm::Expected<uint64_t> bindImport(const DriverImport &Import);
  /// Kernel/HAL dynamic lookup, including kernel_exports scenario overrides.
  /// Static WDF imports and bound function tables never imply system presence.
  /// Conflicting identities are refused rather than selecting a guessed owner.
  llvm::Expected<uint64_t> resolve(llvm::StringRef Name) const;
  /// Register one stable table thunk per binding and exact function spelling.
  /// Registration conveys identity only; unknown functions retain lazy traps
  /// and must not acquire guessed argument counts or execution semantics.
  /// Thunks are never recycled when the framework later unbinds a driver.
  llvm::Expected<uint64_t> insertFrameworkFunction(uint64_t BindingIdentity,
                                                   llvm::StringRef Name);
  /// DMA table identities are scoped to their adapter and remain traps after
  /// retirement. They never become dynamically discoverable kernel exports.
  llvm::Expected<uint64_t> insertDMAFunction(uint64_t Adapter,
                                             llvm::StringRef Name);
  /// Provider callbacks are scoped to their PDO and never kernel exports.
  llvm::Expected<uint64_t> insertProviderFunction(uint64_t PDO,
                                                  llvm::StringRef Name);
  /// Remaining stable identities, excluding the reserved return sentinel.
  size_t availableThunkCount() const;
  const Export *lookup(uint64_t Address) const;
  const std::map<uint64_t, Export> &entries() const { return Exports; }

private:
  using Identity = std::tuple<std::string, std::string, uint64_t>;
  llvm::Expected<uint64_t> insert(llvm::StringRef Module, llvm::StringRef Name,
                                  ExportKind Kind = ExportKind::ModuleExport,
                                  uint64_t Binding = 0);
  bool Initialized = false;
  std::map<Identity, uint64_t> Names;
  std::map<uint64_t, Export> Exports;
};
} // namespace neverd::emulation
#endif
