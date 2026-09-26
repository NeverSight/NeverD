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

#include "neverd/emulation/DriverRegistry.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <map>

namespace neverd::emulation {
class GuestMemory;
class KernelModel;

class KernelRegistry {
public:
  explicit KernelRegistry(GuestMemory &Memory) : Memory(Memory) {}
  llvm::Error
  initialize(const std::optional<std::vector<DriverRegistryKey>> &Registry);
  llvm::Expected<uint64_t> call(const KernelModel &Model, llvm::StringRef Name,
                                llvm::ArrayRef<uint64_t> Arguments);
  std::optional<std::vector<DriverRegistryKey>> snapshot() const;
  bool hasOpenHandles() const { return !Handles.empty(); }
  bool ownsHandle(uint64_t Handle) const { return Handles.contains(Handle); }

private:
  struct Key {
    std::string Path;
    std::map<std::string, DriverRegistryValue> Values;
    bool Deleted = false;
    bool Volatile = false;
  };
  struct Handle {
    std::string Path;
    uint32_t Access;
  };
  GuestMemory &Memory;
  bool Configured = false;
  std::map<std::string, Key> Keys;
  std::map<uint64_t, Handle> Handles;
  uint64_t NextHandle = 0;
  size_t ValueCount = 0;
  size_t DataBytes = 0;

  llvm::Expected<uint64_t> open(const KernelModel &Model,
                                llvm::ArrayRef<uint64_t> Arguments,
                                bool Create);
  llvm::Expected<uint64_t> query(const KernelModel &Model, Key &Key,
                                 llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> set(const KernelModel &Model, Key &Key,
                               llvm::ArrayRef<uint64_t> Arguments);
};
} // namespace neverd::emulation

#endif // NEVERD_EMULATION_KERNELREGISTRY_H
