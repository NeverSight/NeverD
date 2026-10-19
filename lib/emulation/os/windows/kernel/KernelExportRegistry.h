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

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>

namespace neverd::emulation {
struct DriverOptions;
struct DriverImport;
struct ExecutionWatch;
class KernelReferenceImage;

class KernelExportRegistry {
public:
  enum class ExportKind {
    ModuleExport,
    ModuleData,
    ObjectTypeData,
    FrameworkFunction,
    DMAFunction,
    ProviderFunction,
    ProcessData,
    ProcessorData,
    NlsData,
    HalDispatchData,
    HalDispatchFunction
  };

  struct Export {
    std::string Name;
    std::string Module;
    uint64_t Address = 0;
    ExportKind Kind = ExportKind::ModuleExport;
    /// Guest framework globals, DMA adapter or PDO, never a host pointer.
    /// Zero for ordinary PE imports. The owning model checks its lifetime.
    uint64_t Binding = 0;
    bool isModuleSymbol() const {
      return Kind == ExportKind::ModuleExport || Kind == ExportKind::ModuleData;
    }
  };

  /// Bounded PE module basenames, folded case-insensitively. The kernel's
  /// multiprocessor spelling aliases ntoskrnl; other providers keep independent
  /// namespaces and unmodeled imports remain lazy traps. Loader validation
  /// shares this policy; accepting a module grants no execution semantics.
  static std::optional<std::string>
  canonicalImportModule(llvm::StringRef Module);
  /// Stable image identity shared by PE exports and module enumeration.
  static std::optional<uint64_t> moduleBase(llvm::StringRef Module);
  /// Service bodies are opaque, including unused addresses in their gates.
  static bool overlapsThunk(uint64_t Address, uint64_t Size = 1);
  /// Read-only kernel data is environment state, never a callable service gate.
  static bool overlapsData(uint64_t Address, uint64_t Size);
  /// Complete service regions, including identities registered after startup.
  /// Direct execution must stop before entering any byte of these bodies.
  static llvm::ArrayRef<ExecutionWatch> executionWatches();
  static bool matchesImportProvider(llvm::StringRef Requested,
                                    llvm::StringRef Provided);

  llvm::Error initialize(const DriverOptions &Options);
  /// The authenticated image owns every typed data inventory. Keep complete
  /// reference registration here so new data families cannot be omitted by
  /// individual callers. Explicit inventories below support malformed-input
  /// validation and synthetic reference construction.
  llvm::Error attachReference(const KernelReferenceImage &,
                              const DriverOptions &);
  llvm::Error attachReferenceImage(
      const std::map<std::string, uint64_t> &Names,
      const DriverOptions &Options,
      const std::map<std::string, uint64_t> &Data = {},
      const std::map<std::string, uint64_t> &ProcessData = {},
      const std::map<std::string, uint64_t> &ProcessorData = {},
      const std::map<std::string, uint64_t> &NlsData = {},
      const std::map<std::string, uint64_t> &HalData = {});
  llvm::Expected<uint64_t> insertHalDispatchFunction(uint64_t Table);
  llvm::Expected<uint64_t> bindImport(const DriverImport &Import);
  /// Pure lookup for a stopped mapping plan. Never allocates a new thunk or
  /// changes availability while a module transaction is being prepared.
  llvm::Expected<uint64_t> existingImport(const DriverImport &) const;
  /// Missing identity is distinct from explicit absence and invalid input.
  /// Never creates a thunk; used by the stopped module linker.
  llvm::Expected<std::optional<uint64_t>>
  findImport(const DriverImport &) const;
  std::set<std::string> importProviders() const;
  /// Kernel-only dynamic lookup, including kernel_exports scenario overrides.
  /// Static WDF imports and bound function tables never imply kernel presence.
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
  size_t ThunkCount = 0;
  bool ReferenceNamespace = false;
  uint64_t ReferenceHalCacheFunction = 0;
  std::map<Identity, uint64_t> Names;
  std::map<uint64_t, Export> Exports;
};
} // namespace neverd::emulation
#endif
