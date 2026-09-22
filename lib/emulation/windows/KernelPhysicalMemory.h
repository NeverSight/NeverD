//===- KernelPhysicalMemory.h - RAM pages and allocation ownership --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Give existing guest RAM stable physical identities without copying bytes.
/// Exact allocation ranges and pinned views remain independent of shared pages.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELPHYSICALMEMORY_H
#define NEVERD_EMULATION_KERNELPHYSICALMEMORY_H

#include "../GuestMemory.h"

#include "neverd/emulation/DriverDMA.h"

#include <map>
#include <set>
#include <string>
#include <utility>

namespace neverd::emulation {
/// A bounded physical identity or pin resource could not be allocated. Callers
/// with a documented shortage result may translate only this error category.
class PhysicalMemoryLimitError
    : public llvm::ErrorInfo<PhysicalMemoryLimitError> {
public:
  static char ID;
  explicit PhysicalMemoryLimitError(std::string Message)
      : Message(std::move(Message)) {}
  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  std::string Message;
};
namespace physical {
#define NEVERD_KERNEL_PHYSICAL_VALUE(Name, Value)                              \
  constexpr uint64_t Name = Value;
#include "KernelPhysicalMemoryValues.def"
#undef NEVERD_KERNEL_PHYSICAL_VALUE
} // namespace physical

class KernelPhysicalMemory {
public:
  explicit KernelPhysicalMemory(GuestMemory &Memory) : Memory(Memory) {}
  KernelPhysicalMemory(const KernelPhysicalMemory &) = delete;
  KernelPhysicalMemory &operator=(const KernelPhysicalMemory &) = delete;
  struct Region {
    uint64_t Backing, Size;
  };
  struct Segment {
    uint64_t Physical, Backing, Length;
  };
  /// Returned metadata remains valid until this owner is retired.
  const Region *find(uint64_t Owner) const;
  llvm::Error canRegisterRegion(uint64_t Owner, uint64_t Backing,
                                uint64_t Size) const;
  /// Owner tokens and physical page identities are never recycled.
  llvm::Error registerRegion(uint64_t Owner, uint64_t Backing, uint64_t Size);
  /// IgnoredPin, when supplied, must be a live pin of this exact owner. This
  /// permits an owner to preflight release of its own mapping and then retire.
  llvm::Error canRetire(uint64_t Owner, uint64_t IgnoredPin = 0) const;
  /// Check every owner touched by an enclosing release span, including spans
  /// containing padding or several adjacent allocations. No owner is retired.
  llvm::Error canReleaseRange(uint64_t Backing, uint64_t Size,
                              uint64_t IgnoredPin = 0) const;
  llvm::Error retire(uint64_t Owner);
  /// Each segment is contained in one physical page and in the owner range.
  llvm::Expected<std::vector<Segment>> describe(uint64_t Owner, uint64_t Offset,
                                                uint64_t Length) const;
  llvm::Error canPin(uint64_t Owner, uint64_t Offset, uint64_t Length) const;
  llvm::Expected<uint64_t> pin(uint64_t Owner, uint64_t Offset,
                               uint64_t Length);
  /// Grow forward in place without releasing ownership or taking a second pin.
  llvm::Error canExtendPin(uint64_t Pin, uint64_t NewLength) const;
  llvm::Error extendPin(uint64_t Pin, uint64_t NewLength);
  llvm::Error unpin(uint64_t Pin);
  llvm::Error read(uint64_t Pin, uint64_t Offset,
                   llvm::MutableArrayRef<uint8_t> Bytes);
  llvm::Error write(uint64_t Pin, uint64_t Offset,
                    llvm::ArrayRef<uint8_t> Bytes);
  /// Physical identity alone never authorizes access to a retired byte slice.
  llvm::Expected<uint64_t> physicalAddress(uint64_t Backing) const;
  llvm::Expected<uint64_t> ownerForRange(uint64_t Backing, uint64_t Size) const;

private:
  struct PinRecord {
    uint64_t Owner, Offset, Length;
  };
  llvm::Expected<std::vector<uint64_t>>
  planRegion(uint64_t Owner, uint64_t Backing, uint64_t Size) const;
  llvm::Expected<uint64_t> viewBacking(uint64_t Owner, uint64_t Offset,
                                       uint64_t Length) const;
  llvm::Expected<uint64_t> pinBacking(uint64_t Pin, uint64_t Offset,
                                      uint64_t Length) const;
  GuestMemory &Memory;
  std::map<uint64_t, Region> Regions;
  std::map<uint64_t, uint64_t> OwnersByAddress;
  std::set<uint64_t> UsedOwners;
  std::map<uint64_t, uint64_t> Pages;
  std::map<uint64_t, PinRecord> Pins;
  uint64_t NextPin = 1;
};
} // namespace neverd::emulation
#endif // NEVERD_EMULATION_KERNELPHYSICALMEMORY_H
