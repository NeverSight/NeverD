//===- DriverInterrupts.h - Explicit interrupt scenario records -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource assignments and independent external interrupt pulses. These are
/// synthetic single-processor facts, never inferred from register writes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERINTERRUPTS_H
#define NEVERD_EMULATION_DRIVERINTERRUPTS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::emulation {

enum class DriverInterruptMode : uint32_t {
#define NEVERD_DRIVER_INTERRUPT_MODE(Name, Value, Spelling) Name = Value,
#include "neverd/emulation/DriverInterrupts.def"
#undef NEVERD_DRIVER_INTERRUPT_MODE
};

enum class DriverInterruptShare : uint8_t {
#define NEVERD_DRIVER_INTERRUPT_SHARE(Name, Value, Spelling) Name = Value,
#include "neverd/emulation/DriverInterrupts.def"
#undef NEVERD_DRIVER_INTERRUPT_SHARE
};

enum class DriverInterruptAction {
#define NEVERD_DRIVER_INTERRUPT_ACTION(Name, Spelling) Name,
#include "neverd/emulation/DriverInterrupts.def"
#undef NEVERD_DRIVER_INTERRUPT_ACTION
};

#define NEVERD_DRIVER_INTERRUPT_LIMIT(Name, Value)                             \
  inline constexpr size_t Name = Value;
#include "neverd/emulation/DriverInterrupts.def"
#undef NEVERD_DRIVER_INTERRUPT_LIMIT

/// Ordered raw/translated CM_RESOURCE_LIST interrupt descriptor pair. Group
/// zero and CPU zero are explicit provider facts. Memory descriptors precede
/// these descriptors in the same full list; neither vector nor level is
/// inferred from its counterpart.
struct DriverInterruptResource {
  std::string ID;
  uint32_t RawVector = 0;
  uint32_t RawLevel = 0;
  uint64_t RawAffinity = 0;
  uint32_t TranslatedVector = 0;
  uint32_t TranslatedLevel = 0;
  uint64_t TranslatedAffinity = 0;
  DriverInterruptMode Mode = DriverInterruptMode::Latched;
  DriverInterruptShare Share = DriverInterruptShare::DeviceExclusive;
  /// Explicit sampling period for a level-sensitive line; absent for pulses.
  std::optional<uint64_t> RetriggerAfter100ns;
};

struct DriverInterruptEvent {
  /// Deadline relative to successful source request submission. Delivery is
  /// at a supported callback boundary, including when this value is zero.
  uint64_t After100ns = 0;
  std::string DeviceID;
  std::string InterruptID;
  DriverInterruptAction Action = DriverInterruptAction::Pulse;
};

struct DriverInterruptHandlerResult {
  uint64_t InterruptObject = 0;
  uint64_t DeliveredAt100ns = 0;
  std::optional<uint64_t> ReturnedAt100ns;
  std::optional<uint8_t> ReturnValue;
  uint32_t DeliveryIndex = 0;
};

/// Independent observations, not IRPs or NTSTATUS completions. An armed event
/// survives source IRP completion and remains bound to its resource epoch.
struct DriverInterruptResult {
  uint32_t SourceRequestIndex = 0;
  uint32_t EventIndex = 0;
  std::string DeviceID;
  std::string InterruptID;
  uint64_t Epoch = 0;
  uint64_t DueAt100ns = 0;
  std::optional<uint64_t> OccurredAt100ns;
  std::optional<uint64_t> DeliveredAt100ns;
  std::optional<uint64_t> ReturnedAt100ns;
  std::optional<uint64_t> InterruptObject;
  /// Actual BOOLEAN low byte. Zero means unclaimed, not a scenario failure.
  std::optional<uint8_t> ReturnValue;
  std::optional<std::string> UndeliveredReason;
  std::vector<DriverInterruptHandlerResult> Handlers;
  DriverInterruptAction Action = DriverInterruptAction::Pulse;
};

} // namespace neverd::emulation

#endif // NEVERD_EMULATION_DRIVERINTERRUPTS_H
