//===- KernelModel.h - Bounded Windows environment ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded Windows environment model.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELMODEL_H
#define NEVERD_EMULATION_KERNELMODEL_H
#include "../GuestMemory.h"

#include "neverd/emulation/DriverSession.h"

#include <map>
#include <optional>
namespace neverd::emulation {
struct DriverImage;
class KernelModel {
public:
  KernelModel(GuestMemory &Memory, DriverResult &Result)
      : Memory(Memory), Result(Result) {}
  llvm::Error initialize(const DriverImage &Image,
                         const DriverOptions &Options);
  /// End a normally returned DriverEntry, regardless of its NTSTATUS. The
  /// borrowed RegistryPath record and buffer expire before any later callback.
  llvm::Error finishEntry();
  uint64_t driverObject() const { return DriverObject; }
  uint64_t registryPath() const { return RegistryPath; }
  /// Exact supported function imports. Unknown imports are never bound as code.
  static std::optional<unsigned> argumentCount(const std::string &Name);
  llvm::Expected<uint64_t> call(const std::string &Name,
                                llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error snapshot();
  struct Invocation {
    uint64_t PC = 0;
    uint64_t Argument0 = 0;
    uint64_t Argument1 = 0;
  };
  /// Construct one synchronous guest IRP and retain its object state until
  /// finishRequest. Request errors do not launch a callback.
  llvm::Expected<Invocation> beginRequest(const DriverRequest &Request);
  llvm::Error finishRequest(uint32_t DispatchStatus);
  llvm::Expected<Invocation> beginUnload();
  llvm::Error finishUnload();
  /// Reject CPU accesses whose environment semantics this profile does not own.
  llvm::Error validateGuestAccess(uint64_t Address, uint32_t Size,
                                  bool IsWrite) const;

private:
  GuestMemory &Memory;
  DriverResult &Result;
  uint64_t DriverObject = 0;
  uint64_t RegistryPath = 0;
  uint64_t DriverExtension = 0;
  bool EntryFinished = false;
  uint64_t NextAllocation = 0;
  uint64_t AllocationEnd = 0;
  struct PoolAllocation {
    uint64_t Size;
    uint32_t Tag;
  };
  std::map<uint64_t, PoolAllocation> Allocations;
  std::map<uint64_t, uint64_t> ArenaAllocations;
  std::map<uint64_t, DriverDevice> Devices;
  std::map<uint64_t, uint64_t> DeviceSizes;
  std::map<uint64_t, uint64_t> FreedRanges;
  mutable std::array<bool, 28 * 8> DispatchBytesWritten{};
  std::map<std::string, std::string> SymbolicLinks;
  enum class FileState { Closed, Opening, Open, Cleaned };
  FileState OpenState = FileState::Closed;
  uint64_t FileObject = 0;
  uint64_t FileDevice = 0;
  bool Unloading = false;
  bool Unloaded = false;
  struct ActiveRequest {
    DriverRequestKind Kind;
    size_t ResultIndex;
    uint64_t IRP = 0;
    uint64_t Stack = 0;
    uint64_t SystemBuffer = 0;
    uint64_t UserBuffer = 0;
    uint64_t BufferSize = 0;
    uint64_t SecurityContext = 0;
    uint32_t OutputSize = 0;
    bool Completed = false;
    mutable std::array<bool, 16> IOStatusWritten{};
  };
  std::optional<ActiveRequest> Request;
  llvm::Error completeRequest(uint64_t IRP, uint8_t PriorityBoost);
  llvm::Error validateIOAccess(uint64_t Address, uint32_t Size,
                               bool IsWrite) const;
  llvm::Expected<uint64_t> allocate(uint64_t Size, uint64_t Alignment = 16);
  llvm::Expected<uint64_t> makeUnicodeString(const std::string &Text);
  llvm::Expected<std::string> readObjectName(uint64_t Address);
  llvm::Expected<uint64_t> createDevice(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error deleteDevice(uint64_t Address);
};
} // namespace neverd::emulation
#endif
