//===- KernelRegistry.h - Session-local NT registry handles ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded concrete registry and per-handle capabilities.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELREGISTRY_H
#define NEVERD_EMULATION_KERNELREGISTRY_H

#include "neverd/emulation/DriverRegistryHives.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <map>
#include <memory>

namespace neverd::emulation {
class GuestMemory;
class KernelModel;

class KernelRegistry {
public:
  KernelRegistry(KernelModel &Owner, GuestMemory &Memory)
      : Owner(Owner), Memory(Memory) {}
  KernelRegistry(const KernelRegistry &) = delete;
  KernelRegistry &operator=(const KernelRegistry &) = delete;
  llvm::Error
  initialize(const std::optional<std::vector<DriverRegistryKey>> &Registry,
             std::optional<uint64_t> SystemTime100ns = std::nullopt,
             const std::optional<std::vector<DriverRegistryHive>> &Hives = {});
  llvm::Expected<uint64_t> call(llvm::StringRef Name,
                                llvm::ArrayRef<uint64_t> Arguments);
  std::optional<std::vector<DriverRegistryKey>> snapshot() const;
  bool hasOpenHandles() const;
  const std::optional<std::vector<DriverRegistryHiveState>> &
  hiveSnapshot() const {
    return Hives;
  }
  bool ownsHandle(uint64_t Handle) const;
  bool isConfigured() const { return Configured; }
  /// Incarnation identity, shared by handles and never reused after deletion.
  std::optional<uint64_t> keyIdentity(uint64_t Handle) const;
  // External subsystem bindings borrow the same key incarnation and live
  // values. The caller must provide registry callback continuations or reject
  // filters.
  llvm::Expected<std::optional<uint64_t>>
  providerKey(llvm::StringRef Path) const;
  struct ProviderValue {
    uint32_t Status = 0;
    std::optional<DriverRegistryValue> Value;
  };
  llvm::Expected<ProviderValue> providerValue(uint64_t Identity,
                                              llvm::StringRef Name) const;

private:
  friend class KernelModel;
  llvm::Error checkHandleCapacity() const;
  void retireObject(uint64_t Object) { KeyBodies.erase(Object); }
  llvm::Expected<uint64_t> queryObjectName(KernelModel &Model, uint64_t Object,
                                           uint64_t Buffer, uint32_t Length,
                                           uint64_t ReturnLength,
                                           bool RequiredLength);

  struct Key {
    std::string Path;
    std::map<std::string, DriverRegistryValue> Values;
    // Windows value indices follow the live value list, including insertion
    // order and deletion compaction, independently of case-insensitive lookup.
    std::vector<std::string> ValueOrder;
    bool Deleted = false;
    bool Volatile = false;
    std::optional<uint64_t> LastWriteTime100ns;
    // The kernel retains these maxima until the corresponding list is empty.
    uint32_t MaxNameLength = 0;
    uint32_t MaxValueNameLength = 0;
    uint32_t MaxValueDataLength = 0;
    uint64_t Identity = 0;
    std::optional<DriverRegistryKeyInformation> Information;
    std::optional<std::vector<std::string>> SubkeyOrder;
  };
  KernelModel &Owner;
  GuestMemory &Memory;
  bool Configured = false;
  bool ObservedEnumeration = false;
  // Live names and per-open key bodies share the key data. Deleted entries
  // leave Keys immediately; existing bodies retain their old data identity.
  // Pointer/handle counts and retirement belong only to KernelModel.
  std::map<std::string, std::shared_ptr<Key>> Keys;
  std::map<uint64_t, std::shared_ptr<Key>> KeyBodies;
  std::optional<std::vector<DriverRegistryHiveState>> Hives;
  uint64_t NextKeyIdentity = 1;
  size_t ValueCount = 0;
  size_t DataBytes = 0;
  std::optional<uint64_t> SystemTime100ns;

  llvm::Expected<std::optional<uint64_t>>
  mutationTime(const KernelModel &Model) const;
  llvm::Expected<uint64_t> queryKey(const KernelModel &Model, const Key &Entry,
                                    uint32_t Class, uint64_t Buffer,
                                    uint32_t Length, uint64_t ResultLength);
  llvm::Expected<uint64_t> enumerateKey(const KernelModel &Model,
                                        llvm::StringRef Path,
                                        llvm::ArrayRef<uint64_t> Arguments);

  llvm::Expected<uint64_t>
  open(KernelModel &Model, llvm::ArrayRef<uint64_t> Arguments, bool Create);
  llvm::Expected<uint64_t> query(const KernelModel &Model, Key &Key,
                                 llvm::ArrayRef<uint64_t> Arguments,
                                 bool Enumerate);
  llvm::Expected<uint64_t> set(const KernelModel &Model, Key &Key,
                               llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> enumerate(const KernelModel &, Key &,
                                     llvm::ArrayRef<uint64_t>);
  static DriverRegistryKey keySnapshot(const Key &);
  std::vector<DriverRegistryKey> commitSnapshot(llvm::StringRef Root) const;
  llvm::Expected<uint64_t> flush(const Key &);
};
} // namespace neverd::emulation

#endif // NEVERD_EMULATION_KERNELREGISTRY_H
