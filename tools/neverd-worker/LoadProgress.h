//===- LoadProgress.h - Bounded progress publication -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Request-local presentation cadence; cancellation belongs to the loader.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_WORKER_LOADPROGRESS_H
#define NEVERD_TOOLS_NEVERD_WORKER_LOADPROGRESS_H

#include "ResourceLimits.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace neverd::worker {

class LoadProgress {
public:
  using Clock = std::chrono::steady_clock;

  bool publish(std::string_view phase, std::uint64_t done, std::uint64_t total,
               std::string_view detail, Clock::time_point now = Clock::now()) {
    if (sent_ && done != 0 && (!total || done < total) && phase == phase_ &&
        detail == detail_ &&
        now - last_ < std::chrono::milliseconds(
                          ::neverd::limits::worker::kLoadProgressIntervalMs))
      return false;
    phase_ = phase;
    detail_ = detail;
    last_ = now;
    sent_ = true;
    return true;
  }

private:
  std::string phase_, detail_;
  Clock::time_point last_{};
  bool sent_ = false;
};

} // namespace neverd::worker

#endif // NEVERD_TOOLS_NEVERD_WORKER_LOADPROGRESS_H
