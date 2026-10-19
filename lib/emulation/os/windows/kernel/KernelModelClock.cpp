//===- KernelModelClock.cpp - Read-only guest shared tick count
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Read-only guest shared tick count.
///

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error clockError(const char *Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

llvm::Expected<uint64_t> KernelModel::stallProcessor(uint32_t Microseconds) {
  if (!Microseconds)
    return uint64_t(0);
  // WDK ULONG is 32 bits. The virtual schedule chooses the requested lower
  // bound, independently of host speed and the boot clock increment.
  auto Target = Scheduler.computeDeadline(-int64_t(Microseconds) * 10);
  if (!Target)
    return Target.takeError();
  while (Scheduler.now100ns() < *Target) {
    auto Next = nextEventTime();
    const uint64_t Boundary = Next ? std::min(*Next, *Target) : *Target;
    const uint64_t Before = Scheduler.now100ns();
    if (auto E = processScheduledBoundary(Boundary, true))
      return E;
    // Delivery requires a safe guest boundary. Never spin the host if a
    // provider cannot progress while the current API owns the execution.
    if (Scheduler.now100ns() == Before && nextEventTime() == Next)
      return clockError(
          "processor stall requires an intervening guest callback");
  }
  return uint64_t(0);
}

llvm::Error KernelModel::initializeSharedClock() {
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return llvm::Error::success();
  if (auto E = Memory.map(SharedClockDataBase, profile::PageSize, Read))
    return E;
  SharedClockMapped = true;
  return synchronizeSharedClock();
}

llvm::Error KernelModel::synchronizeSharedClock() {
  if (!SharedClockMapped)
    return llvm::Error::success();
  auto Tick = Scheduler.queryTickCount();
  if (!Tick)
    return Tick.takeError();
  if (PublishedSharedTick == *Tick)
    return llvm::Error::success();
  std::array<uint8_t, SharedTickCountSize> Bytes{};
  llvm::support::endian::write32le(Bytes.data() + SharedTickLowOffset,
                                   uint32_t(*Tick));
  llvm::support::endian::write32le(Bytes.data() + SharedTickHigh1Offset,
                                   uint32_t(*Tick >> 32));
  llvm::support::endian::write32le(Bytes.data() + SharedTickHigh2Offset,
                                   uint32_t(*Tick >> 32));
  // A stopped-CPU owner update preserves guest read-only/NX permissions and
  // reaches the same RAM backing seen by authenticated physical aliases.
  if (auto E = Memory.writeBacking(SharedClockDataBase + SharedTickCountOffset,
                                   Bytes))
    return E;
  PublishedSharedTick = *Tick;
  return llvm::Error::success();
}

llvm::Error KernelModel::validateSharedClockAccess(uint64_t Address,
                                                   uint32_t Size,
                                                   bool IsWrite) const {
  if (!Size || Address >= SharedClockDataBase + profile::PageSize ||
      Address + Size <= SharedClockDataBase)
    return llvm::Error::success();
  if (!SharedClockMapped)
    return clockError("shared clock data requires system_clock_increment_100ns "
                      "and Windows 10.0.19041");
  const uint64_t Tick = SharedClockDataBase + SharedTickCountOffset;
  if (IsWrite || Address < Tick || Address >= Tick + SharedTickCountSize ||
      Size > Tick + SharedTickCountSize - Address)
    return clockError("guest access to opaque or read-only shared clock data");
  return llvm::Error::success();
}
} // namespace neverd::emulation
