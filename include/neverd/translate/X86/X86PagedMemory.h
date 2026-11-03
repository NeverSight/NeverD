//===- X86PagedMemory.h - Restartable x86 RAM operands ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compose architectural page translation with exact physical RAM accesses.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_X86PAGEDMEMORY_H
#define NEVERD_TRANSLATE_X86_X86PAGEDMEMORY_H

#include "neverd/ir/X86/X86LegacySegments.h"
#include "neverd/translate/GuestMemoryRuntime.h"
#include "neverd/translate/X86/X86Paging.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace neverd::translate {

struct X86SegmentedAccess {
  X86SegmentRegister Segment;
  X86LegacySegmentCache Cache;
  uint32_t Offset;
  MemoryAccessKind Kind;
  uint8_t CPL;
  std::optional<X86FullSegmentLimitChoice> FullLimitChoice;
};

/// Owned physical ordinary RAM and explicitly bound paging controls for one
/// confined CPU. Physical regions use Read/Write backing permissions; Execute
/// belongs to guest paging and is rejected on the physical backing. This
/// component has no translated-code cache or MMIO and performs fresh walks.
///
/// Loads/fetches publish output only after every page and byte succeeds. Stores
/// preflight every physical fragment before publishing any operand byte. Page
/// table A/D effects are not rolled back on a later fault. This is a primitive
/// for a restartable operand, not a claim that REP, gather/scatter, XSAVE or
/// other partially completing instructions are whole-instruction atomic.
class X86PagedMemory final {
public:
  static llvm::Expected<std::unique_ptr<X86PagedMemory>>
  create(const GuestState &PhysicalRAM, const X86PagingFeatures &Features,
         const X86PagingState &Controls,
         const GuestMemoryRuntimeLimits &Limits = {});

  const X86PagingState &controls() const { return Controls; }
  const X86PagingFeatures &features() const { return Features; }
  /// Replaces an already architecturally validated register bank. A guest MOV
  /// CR instruction owns transition checks and transactional PDPTE reload.
  llvm::Error bindControls(const X86PagingState &State);
  GuestMemoryRuntime &physicalMemory() { return *Physical; }

  llvm::Expected<X86MemoryAccessResult>
  load(const X86LinearAccess &Access, llvm::MutableArrayRef<uint8_t> Output);
  llvm::Expected<X86MemoryAccessResult> store(const X86LinearAccess &Access,
                                              llvm::ArrayRef<uint8_t> Input);

  /// Protected legacy 16/32-bit operands with already loaded caches. Segment
  /// checks precede all page walks; linear wrap still retains whole-operand
  /// publication. Alignment, instruction commit and exception delivery remain
  /// the caller's owners. No selector reload or GDT lookup is inferred.
  llvm::Expected<X86MemoryAccessResult>
  loadSegment(const X86SegmentedAccess &Access,
              llvm::MutableArrayRef<uint8_t> Output);
  llvm::Expected<X86MemoryAccessResult>
  storeSegment(const X86SegmentedAccess &Access, llvm::ArrayRef<uint8_t> Input);

  /// A successful plan retains translations and physical write permission.
  /// Commit before rebinding controls or mutating physical RAM; page-table A/D
  /// effects of preparation remain visible even if the instruction faults.
  struct PreparedStore;
  llvm::Expected<PreparedStore> prepareStore(const X86LinearAccess &Access,
                                             uint64_t Bytes);
  llvm::Expected<PreparedStore>
  prepareSegmentStore(const X86SegmentedAccess &Access, uint64_t Bytes);
  /// Read the exact fragments of a successful write plan for a read/modify/
  /// write instruction. This retains write-fault classification and does not
  /// walk again through page-table bytes changed by preparation. Physical
  /// read failures remain backing stops. Commit/lifetime rules are as above.
  llvm::Expected<X86MemoryAccessResult>
  loadPreparedStore(const PreparedStore &Store,
                    llvm::MutableArrayRef<uint8_t> Output);
  void commitStore(const PreparedStore &Store, llvm::ArrayRef<uint8_t> Input);

private:
  struct Fragment {
    uint64_t PhysicalAddress;
    uint64_t Size;
  };
  struct Plan {
    X86MemoryAccessResult Result;
    std::vector<Fragment> Fragments;
  };

public:
  struct PreparedStore : X86MemoryAccessResult {
  private:
    friend class X86PagedMemory;
    std::vector<Fragment> Fragments;
    const X86PagedMemory *Owner = nullptr;
    uint64_t Bytes = 0;
  };

private:
  X86PagedMemory(std::unique_ptr<GuestMemoryRuntime> Physical,
                 const X86PagingFeatures &Features,
                 const X86PagingState &Controls);
  llvm::Expected<Plan> plan(const X86LinearAccess &Access, uint64_t Bytes);

  std::unique_ptr<GuestMemoryRuntime> Physical;
  X86PagingFeatures Features;
  X86PagingState Controls;
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86PAGEDMEMORY_H
