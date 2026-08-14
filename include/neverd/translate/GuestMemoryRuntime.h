//===- GuestMemoryRuntime.h - Checked guest memory runtime -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines an owned, bounded guest-memory index with checked scalar accesses
/// and transactional instruction fetches.  Guest virtual addresses are keys
/// into that index and are never converted to host pointers.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_GUESTMEMORYRUNTIME_H
#define NEVERD_TRANSLATE_GUESTMEMORYRUNTIME_H

#include "neverd/translate/GuestState.h"
#include "neverd/translate/RuntimeABI.h"
#include "neverd/translate/TranslationResult.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace neverd::translate {

/// Scalar widths supported by the v1 little-endian helper ABI.  Values are
/// byte counts, making size arithmetic explicit at the checked boundary.
enum class GuestScalarWidth : uint8_t {
  I8 = 1,
  I16 = 2,
  I32 = 4,
  I64 = 8,
};

enum class GuestMemoryAccessStatus : uint8_t {
  Completed = 0,
  Fault = 1,
  SelfModification = 2,
};

struct GuestMemoryFault {
  RuntimeMemoryFaultKindV1 Kind = RuntimeMemoryFaultKindV1::None;
  MemoryFaultExit Exit;
  /// Exact byte count for the runtime operation.  AccessWidthBits mirrors this
  /// value when it fits the architecture-neutral TranslationResult payload.
  uint64_t AccessSize = 0;
  uint64_t ExpectedGeneration = 0;
  uint64_t ObservedGeneration = 0;
};

/// Typed result returned for every load, store, and generation check.  Value is
/// meaningful only for Completed loads/checks; the optional payload selected
/// by Status is the only valid exit payload.
struct GuestMemoryAccessResult {
  GuestMemoryAccessStatus Status = GuestMemoryAccessStatus::Completed;
  uint64_t Value = 0;
  std::optional<GuestMemoryFault> Fault;
  std::optional<SelfModificationExit> SelfModification;
};

/// One exact executable-memory fragment covered by an instruction fetch.
/// Bindings deliberately follow region boundaries even when adjacent regions
/// carry the same generation, so a dispatcher can revalidate every owner.
struct GuestExecutableRangeBinding {
  uint64_t Address = 0;
  uint64_t Size = 0;
  uint64_t Generation = 0;
};

/// Result of copying instruction bytes into caller-owned storage.  Bindings are
/// populated only on success; faults never expose a partial binding set.
struct GuestInstructionFetchResult {
  GuestMemoryAccessStatus Status = GuestMemoryAccessStatus::Completed;
  std::optional<GuestMemoryFault> Fault;
  std::vector<GuestExecutableRangeBinding> Bindings;
};

struct GuestMemoryRuntimeLimits {
  /// Zero is unbounded.  No implementation-selected finite cap is substituted.
  uint64_t MaxRegions = 0;
  uint64_t MaxBytes = 0;
};

struct GuestMemoryRuntimeConfig {
  CodeInvalidationPolicy CodeInvalidation =
      CodeInvalidationPolicy::InvalidateOnExecutableWrite;
  uint64_t InstructionBudget = 0;
  uint64_t BlockBudget = 0;
  GuestMemoryRuntimeLimits Limits;
};

enum class RuntimePollStatus : uint8_t {
  Continue = 0,
  Cancelled = 1,
  BudgetExhausted = 2,
};

struct RuntimePollResult {
  RuntimePollStatus Status = RuntimePollStatus::Continue;
  uint64_t InstructionCount = 0;
  uint64_t BlockCount = 0;
  std::optional<BudgetExit> Budget;
};

/// Checked runtime memory owned independently of the logical GuestState.
///
/// create() validates State and copies region bytes/metadata into a sorted
/// private index before returning.  Scalar accesses, generation validation,
/// poll(), and snapshotControlBlock() are confined to one execution thread.
/// requestCancellation() and cancellationRequested() are the only methods that
/// may be called concurrently; cancellation and accounting use atomics so a
/// snapshot cannot observe torn counters.
class GuestMemoryRuntime final {
public:
  static llvm::Expected<std::unique_ptr<GuestMemoryRuntime>>
  create(const GuestState &State, const GuestMemoryRuntimeConfig &Config = {});

  GuestMemoryRuntime(const GuestMemoryRuntime &) = delete;
  GuestMemoryRuntime &operator=(const GuestMemoryRuntime &) = delete;
  GuestMemoryRuntime(GuestMemoryRuntime &&) = delete;
  GuestMemoryRuntime &operator=(GuestMemoryRuntime &&) = delete;
  ~GuestMemoryRuntime() = default;

  GuestMemoryAccessResult loadScalar(uint64_t Address, GuestScalarWidth Width,
                                     uint32_t RequiredAlignment = 0);
  GuestMemoryAccessResult storeScalar(uint64_t Address, GuestScalarWidth Width,
                                      uint64_t Value,
                                      uint32_t RequiredAlignment = 0);

  /// Copy an exact instruction byte range after every covered region has been
  /// proven contiguous and executable.  Output is caller-owned and remains
  /// byte-for-byte unchanged on failure.  A successful result binds each
  /// covered region fragment to its current generation for exact dispatch-time
  /// revalidation.
  GuestInstructionFetchResult
  fetchInstructionBytes(uint64_t Address,
                        llvm::MutableArrayRef<uint8_t> Output);

  /// Validate the generation of executable memory immediately before a
  /// dispatch governed by ValidateBeforeDispatch.
  GuestMemoryAccessResult validateExecutableGeneration(uint64_t Address,
                                                       uint64_t Expected);

  /// Returns no value for an unmapped address.  It never exposes region bytes
  /// or a host address.
  std::optional<uint64_t> generationForAddress(uint64_t Address) const;

  RuntimePollResult poll(uint64_t InstructionDelta = 0,
                         uint64_t BlockDelta = 0);
  void requestCancellation() noexcept;
  bool cancellationRequested() const noexcept;

  /// Compose a closed v1 record for the outer dispatcher.  A pending
  /// cancellation wins over an earlier result.  ExpectedGeneration is applied
  /// only to a successful exit.  A generation-validation result is retained
  /// only when CurrentPC names the exact address checked by the last operation;
  /// validation then requires the expected and observed generations to match.
  /// Memory-fault snapshots preserve access, required alignment, and generation
  /// semantics through packRuntimeMemoryFaultDetailsV1().
  RuntimeControlBlockV1
  snapshotControlBlock(uint64_t CurrentPC = 0,
                       uint64_t ExpectedGeneration = 0) const noexcept;

  std::size_t regionCount() const noexcept { return Regions.size(); }
  uint64_t guestMemoryBytes() const noexcept { return GuestMemoryBytes; }
  CodeInvalidationPolicy codeInvalidationPolicy() const noexcept {
    return CodeInvalidation;
  }

private:
  struct Region {
    uint64_t Address = 0;
    uint64_t LastAddress = 0;
    MemoryPermission Permissions = MemoryPermission::None;
    uint64_t Generation = 0;
    std::vector<uint8_t> Bytes;
  };

  struct RegionSpan {
    std::size_t RegionIndex = 0;
    std::size_t Offset = 0;
    uint64_t Address = 0;
    uint64_t Size = 0;
  };

  struct AccessPlan {
    RuntimeMemoryFaultKindV1 Fault = RuntimeMemoryFaultKindV1::None;
    std::vector<RegionSpan> Spans;
  };

  GuestMemoryRuntime(std::vector<Region> Regions, uint16_t AddressWidth,
                     uint64_t GuestMemoryBytes,
                     const GuestMemoryRuntimeConfig &Config);

  std::optional<std::size_t> findRegionIndex(uint64_t Address) const;
  AccessPlan planAccess(uint64_t Address, uint64_t Size,
                        MemoryPermission RequiredPermission,
                        RuntimeMemoryFaultKindV1 ContinuationFault) const;

  GuestMemoryAccessResult fault(RuntimeMemoryFaultKindV1 Kind, uint64_t Address,
                                uint64_t AccessSize, MemoryAccessKind Access,
                                uint32_t RequiredAlignment,
                                uint64_t ExpectedGeneration = 0,
                                uint64_t ObservedGeneration = 0);
  GuestInstructionFetchResult fetchFault(RuntimeMemoryFaultKindV1 Kind,
                                         uint64_t Address, uint64_t AccessSize);
  void recordResult(const GuestMemoryAccessResult &Result);
  void recordPoll(const RuntimePollResult &Result);

  std::vector<Region> Regions;
  uint16_t AddressWidth = 0;
  uint64_t GuestMemoryBytes = 0;
  CodeInvalidationPolicy CodeInvalidation =
      CodeInvalidationPolicy::InvalidateOnExecutableWrite;
  uint64_t InstructionBudget = 0;
  uint64_t BlockBudget = 0;
  RuntimeControlBlockV1 Control;
  std::atomic<uint64_t> InstructionCount{0};
  std::atomic<uint64_t> BlockCount{0};
  std::atomic<bool> CancellationRequested{false};
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_GUESTMEMORYRUNTIME_H
