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

#include "neverd/emulation/DriverDMA.h"
#include "neverd/emulation/GuestMemory.h"

#include <algorithm>
#include <map>
#include <optional>
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
  enum class CacheType { Cached, NonCached, WriteCombined };
  /// The allocation domain, not an inferred installed-RAM inventory. A caller
  /// must explicitly select this simulated domain before publishing it as RAM.
  static std::pair<uint64_t, uint64_t> identityWindow();
  explicit KernelPhysicalMemory(GuestMemory &Memory) : Memory(Memory) {}
  /// Configure before any owner, pin or ordinary guest frame is published.
  /// The default remains the original 1 MiB managed physical window.
  llvm::Error configureManagedCapacity(uint64_t Bytes);
  KernelPhysicalMemory(const KernelPhysicalMemory &) = delete;
  KernelPhysicalMemory &operator=(const KernelPhysicalMemory &) = delete;
  struct Region {
    uint64_t Backing, Size;
    bool OwnsPages = false;
    MemoryView Storage;
  };
  struct Segment {
    uint64_t Physical, Backing, Length;
  };
  /// Returned metadata remains valid until this owner is retired.
  const Region *find(uint64_t Owner) const;
  llvm::Error canRegisterRegion(uint64_t Owner, uint64_t Backing, uint64_t Size,
                                CacheType Cache = CacheType::Cached) const;
  /// Ordinary RAM owner tokens and physical page identities are not recycled.
  llvm::Error registerRegion(uint64_t Owner, uint64_t Backing, uint64_t Size,
                             CacheType Cache = CacheType::Cached);
  /// Select available pages from the explicit physical window. The returned
  /// page addresses are a pure plan; no owner, cache type or page is published.
  llvm::Expected<std::vector<uint64_t>>
  planAllocatedPages(uint64_t Low, uint64_t High, uint64_t Skip,
                     uint64_t PageCount, uint64_t ChunkPages = 0) const;
  /// Publish private canonical backing against exactly the selected free pages.
  /// Reserved, unowned paging frames may be rebound. The caller must reject
  /// outstanding virtual/physical views before assigning allocation backing.
  llvm::Error canRegisterAllocatedPages(uint64_t Owner, uint64_t Backing,
                                        llvm::ArrayRef<uint64_t> PhysicalPages,
                                        std::optional<CacheType> Cache) const;
  llvm::Error registerAllocatedPages(uint64_t Owner, uint64_t Backing,
                                     llvm::ArrayRef<uint64_t> PhysicalPages,
                                     std::optional<CacheType> Cache);
  /// Publish a previously unspecified cache attribute only after a mapping
  /// succeeds. Failed mapping admission must not choose a page's cache type.
  llvm::Error commitMappingCache(uint64_t Backing, uint64_t Size,
                                 CacheType Cache);
  /// Existing RAM pages retain their allocation's cache attribute, regardless
  /// of the valid attribute requested for a new MDL mapping.
  llvm::Expected<CacheType> cacheTypeForMapping(uint64_t Backing, uint64_t Size,
                                                CacheType Requested) const;
  /// IgnoredPin, when supplied, must be a live pin of this exact owner. This
  /// permits an owner to preflight release of its own mapping and then retire.
  llvm::Error canRetire(uint64_t Owner, uint64_t IgnoredPin = 0) const;
  /// Check every owner touched by an enclosing release span, including spans
  /// containing padding or several adjacent allocations. No owner is retired.
  llvm::Error canReleaseRange(uint64_t Backing, uint64_t Size,
                              uint64_t IgnoredPin = 0) const;
  /// Preflight a transaction that releases the listed live pins before any
  /// backing owners. Pins outside RetiringPins continue to protect every owner
  /// touched by Ranges. This does not unpin or retire anything.
  llvm::Error
  canReleaseRanges(llvm::ArrayRef<std::pair<uint64_t, uint64_t>> Ranges,
                   llvm::ArrayRef<uint64_t> RetiringPins) const;
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
  /// Residency is page-granular and shared by every live pin. This does not
  /// grant byte ownership or change the CPU permissions of the original VA.
  bool hasPinnedPages(uint64_t Backing, uint64_t Size) const;
  llvm::Error canUnpin(uint64_t Pin) const;
  llvm::Error unpin(uint64_t Pin);
  llvm::Error read(uint64_t Pin, uint64_t Offset,
                   llvm::MutableArrayRef<uint8_t> Bytes);
  llvm::Error write(uint64_t Pin, uint64_t Offset,
                    llvm::ArrayRef<uint8_t> Bytes);
  /// Physical identity alone never authorizes access to a retired byte slice.
  llvm::Expected<uint64_t> physicalAddress(uint64_t Backing) const;
  struct AddressTranslation {
    uint64_t Physical;
    std::vector<uint64_t> Owners;
  };
  /// Resolve the current RAM mapping against exact live owner slices. Aliases
  /// may share several owners; callers decide residency and access admission.
  /// This pure query neither pins pages nor grants access to their contents.
  llvm::Expected<AddressTranslation> resolveAddress(uint64_t Address) const;
  llvm::Expected<uint64_t> ownerForRange(uint64_t Backing, uint64_t Size) const;
  /// Extend frame identity to ordinary mapped RAM for the guest paging model.
  /// The original allocation window and exact owner/pin checks stay separate.
  llvm::Error enableGuestFrames(uint64_t Capacity);
  llvm::Expected<uint64_t> frameAddress(uint64_t Address);
  llvm::Expected<CacheType> frameCacheType(uint64_t Address) const;
  uint64_t frameGeneration() const { return FrameGeneration; }
  struct PhysicalViewPage {
    uint64_t Physical = 0, Backing = 0;
    std::optional<CacheType> Cache;
    std::vector<uint64_t> Owners;
    MemoryView Storage;
  };
  // Resolve only published guest PFNs to their current canonical RAM mapping.
  // No identity or zero-filled backing is invented for an unknown frame.
  llvm::Expected<std::vector<PhysicalViewPage>>
  physicalViewPages(uint64_t Address, uint64_t Size) const;
  uint64_t availablePins() const {
    return std::min<uint64_t>(physical::PinLimit - Pins.size(),
                              UINT64_MAX - NextPin);
  }
  uint64_t physicalRangeSize() const {
    return ManagedCapacity + GuestFrameCapacity;
  }

private:
  struct PinRecord {
    uint64_t Owner, Offset, Length;
  };
  using PageKey = std::pair<std::weak_ptr<MemoryRegion>, uint64_t>;
  struct PageKeyLess {
    bool operator()(const PageKey &A, const PageKey &B) const {
      if (A.first.owner_before(B.first))
        return true;
      if (B.first.owner_before(A.first))
        return false;
      return A.second < B.second;
    }
  };
  struct PageRecord {
    uint64_t Physical;
    std::optional<CacheType> Cache;
    bool Reusable = false;
    bool Managed = true;
    uint64_t BackingPage = 0;
  };
  llvm::Expected<std::vector<uint64_t>>
  planRegion(uint64_t Owner, uint64_t Backing, uint64_t Size, CacheType Cache,
             bool RebindUnmanaged = false) const;
  llvm::Expected<Region>
  planAllocatedRegion(uint64_t Owner, uint64_t Backing,
                      llvm::ArrayRef<uint64_t> PhysicalPages,
                      std::optional<CacheType> Cache) const;
  llvm::Expected<uint64_t> viewBacking(uint64_t Owner, uint64_t Offset,
                                       uint64_t Length) const;
  llvm::Expected<uint64_t> pinOffset(uint64_t Pin, uint64_t Offset,
                                     uint64_t Length) const;
  llvm::Expected<PageKey> pageKey(uint64_t Address) const;
  PageKey pageKey(const Region &Region, uint64_t Address) const;
  bool containsPage(const MemoryView &View, const PageKey &Key) const;
  size_t managedPageCount() const;
  using Owners = std::multimap<uint64_t, uint64_t>;
  const Owners *activeOwners() const;
  GuestMemory &Memory;
  std::map<uint64_t, Region> Regions;
  std::map<std::shared_ptr<const void>, Owners,
           std::owner_less<std::shared_ptr<const void>>>
      OwnersBySpace;
  std::set<uint64_t> UsedOwners;
  std::map<PageKey, PageRecord, PageKeyLess> Pages;
  std::map<uint64_t, PinRecord> Pins;
  uint64_t NextPin = 1;
  uint64_t ManagedCapacity = physical::PhysicalSize;
  bool ManagedCapacityConfigured = false;
  uint64_t GuestFrameCapacity = 0;
  uint64_t FrameGeneration = 0;
  uint64_t NextGuestFrame = physical::PhysicalBase + physical::PhysicalSize;
};
} // namespace neverd::emulation
#endif // NEVERD_EMULATION_KERNELPHYSICALMEMORY_H
