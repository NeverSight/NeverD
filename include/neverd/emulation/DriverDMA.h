//===- DriverDMA.h - Explicit DMA capabilities and transactions -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Device logical address domains and independent external DMA observations.
/// Addresses refer to modeled guest storage, never host physical memory.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERDMA_H
#define NEVERD_EMULATION_DRIVERDMA_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::emulation {

enum class DriverDmaDirection : uint8_t {
#define NEVERD_DRIVER_DMA_DIRECTION(Name, Spelling) Name,
#include "neverd/emulation/DriverDMA.def"
#undef NEVERD_DRIVER_DMA_DIRECTION
};

#define NEVERD_DRIVER_DMA_LIMIT(Name, Value)                                   \
  inline constexpr size_t Name = Value;
#define NEVERD_DRIVER_DMA_ADDRESS(Name, Value)                                 \
  inline constexpr uint64_t Name = Value;
#include "neverd/emulation/DriverDMA.def"
#undef NEVERD_DRIVER_DMA_LIMIT
#undef NEVERD_DRIVER_DMA_ADDRESS

struct DriverDmaConfig {
  uint32_t AddressBits = 0;
  uint32_t MaximumLength = 0;
  uint32_t MapRegisters = 0;
  uint32_t Alignment = 0;
  /// Each PDO has an independent logical domain; equal apertures on different
  /// PDOs do not alias or conflict. Neither address is a CPU virtual address.
  uint64_t LogicalBase = 0;
  uint64_t LogicalLength = 0;
  bool ScatterGather = false;
};

struct DriverDmaEvent {
  /// Relative to successful source request submission, in virtual 100 ns units.
  uint64_t After100ns = 0;
  std::string DeviceID;
  uint64_t LogicalAddress = 0;
  /// Direction is from the device's perspective: ReadMemory reads guest RAM.
  DriverDmaDirection Direction = DriverDmaDirection::ReadMemory;
  uint32_t Length = 0;
  /// Required exact bytes for WriteMemory; empty for ReadMemory.
  std::vector<uint8_t> Data{};
};

/// DMA observations are independent of source IRP completion and never imply
/// a device interrupt or vendor register protocol.
struct DriverDmaResult {
  uint32_t SourceRequestIndex = 0;
  uint32_t EventIndex = 0;
  std::string DeviceID;
  uint64_t Epoch = 0;
  uint64_t DueAt100ns = 0;
  uint64_t LogicalAddress = 0;
  uint32_t Length = 0;
  DriverDmaDirection Direction = DriverDmaDirection::ReadMemory;
  std::optional<uint64_t> OccurredAt100ns;
  std::optional<uint64_t> CompletedAt100ns;
  std::optional<uint64_t> Mapping;
  std::optional<uint64_t> Adapter;
  std::optional<std::string> FailureReason;
  /// Bytes actually observed by the transaction; no fabricated read data.
  std::vector<uint8_t> Data{};
};

} // namespace neverd::emulation

#endif // NEVERD_EMULATION_DRIVERDMA_H
