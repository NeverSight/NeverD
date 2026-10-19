//===- DriverThreadNotifications.h - Modeled thread lifecycle delivery ----===//
// NeverD Decompiler
///
/// \file
/// Modeled thread lifecycle delivery.
///

#ifndef NEVERD_EMULATION_DRIVERTHREADNOTIFICATIONS_H
#define NEVERD_EMULATION_DRIVERTHREADNOTIFICATIONS_H
#include <cstdint>
#include <vector>
namespace neverd::emulation {
struct DriverThreadNotificationResult {
  uint32_t ProcessID = 4;
  uint64_t ThreadID = 0, ThreadObject = 0;
  uint32_t ActorProcessID = 4;
  // Internal execution identity, deliberately distinct from the native CID.
  uint64_t ActorThreadKey = 0;
  bool Create = true, Completed = false;
  std::vector<uint64_t> DeliveredRoutines;
};
} // namespace neverd::emulation
#endif
