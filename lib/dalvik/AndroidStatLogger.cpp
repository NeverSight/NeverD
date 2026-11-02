//===- AndroidStatLogger.cpp - Shared duration statistics ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// API 28 statistics over retained Java arrays and the shared boot clock.
//===----------------------------------------------------------------------===//
#include "AndroidFramework.h"

#include "llvm/Support/Endian.h"

#include <bit>
#include <optional>
#include <utility>

namespace neverd::mobile::dalvik {
ExecutionResult AndroidFramework::recordStatistic(
    Heap &heap, const Program &program, uint64_t receiver, int32_t event,
    uint64_t start, const AdmitInstruction &admit) {
  constexpr auto Type = "Lcom/android/internal/util/StatLogger;";
  struct Stop {
    ExecutionResult result;
  };
  auto take = [](ExecutionResult result) {
    if (result.stop != ExecutionStop::Returned)
      throw Stop{std::move(result)};
    return result.value;
  };
  auto tick = [&] {
    take({admit(), 0, {}, "Android statistics work budget exhausted"});
  };
  auto fail = [](const char *exception) -> uint64_t {
    throw Stop{{ExecutionStop::Thrown, 0, exception,
                "Android statistics field access failed"}};
  };
  const auto logger = heap.get(receiver);
  auto field = [&](const char *name, const char *type) -> uint64_t & {
    tick();
    const auto *member = program.findField({Type, name, type}, false);
    if (!logger->Fields.contains(member))
      throw Stop{{ExecutionStop::Unsupported,
                  0,
                  {},
                  "Android statistics field state is not supplied"}};
    return logger->Fields.at(member);
  };
  // Field typing and primitive-array storage belong to Runtime and Heap.
  // Read/write each selected element in Java order, including partial effects
  // when a later array or clock access fails.
  auto element = [&](const char *name, bool wide,
                     std::optional<uint64_t> write = {}) -> uint64_t {
    const auto array = heap.get(field(name, wide ? "[J" : "[I"));
    if (!array)
      return fail("java/lang/NullPointerException");
    if (uint32_t(event) >= array->Length)
      return fail("java/lang/ArrayIndexOutOfBoundsException");
    auto *bytes = array->Bytes.data() + uint64_t(event) * (wide ? 8 : 4);
    if (write) {
      if (wide)
        llvm::support::endian::write64le(bytes, *write);
      else
        llvm::support::endian::write32le(bytes, uint32_t(*write));
    }
    return wide ? llvm::support::endian::read64le(bytes)
                : llvm::support::endian::read32le(bytes);
  };
  auto signedLong = [](uint64_t value) {
    return std::bit_cast<int64_t>(value);
  };
  try {
    if (!field("mLock", "Ljava/lang/Object;"))
      return {ExecutionStop::Thrown, 0, "java/lang/NullPointerException",
              "null Android statistics lock"};
    tick();
    const uint64_t duration =
        take(readClock(ElapsedRealtimeNanos)) / 1000 - start;
    if (event < 0 ||
        event >= std::bit_cast<int32_t>(uint32_t(field("SIZE", "I"))))
      return {ExecutionStop::Unsupported,
              0,
              {},
              "Android StatLogger invalid-event logging is not modeled"};
    element("mCountStats", false, element("mCountStats", false) + 1);
    element("mDurationStats", true, element("mDurationStats", true) + duration);
    if (signedLong(element("mMaxDurationStats", true)) < signedLong(duration))
      element("mMaxDurationStats", true, duration);
    tick();
    const uint64_t now = take(readClock(ElapsedRealtime));
    if (signedLong(now) > signedLong(field("mNextTickTime", "J"))) {
      const auto count = uint32_t(element("mCallsPerSecond", false));
      if (std::bit_cast<int32_t>(uint32_t(element(
              "mMaxCallsPerSecond", false))) < std::bit_cast<int32_t>(count))
        element("mMaxCallsPerSecond", false, count);
      const auto elapsed = element("mDurationPerSecond", true);
      if (signedLong(element("mMaxDurationPerSecond", true)) <
          signedLong(elapsed))
        element("mMaxDurationPerSecond", true, elapsed);
      element("mCallsPerSecond", false, 0);
      element("mDurationPerSecond", true, 0);
      field("mNextTickTime", "J") = now + 1000;
    }
    element("mCallsPerSecond", false, element("mCallsPerSecond", false) + 1);
    element("mDurationPerSecond", true,
            element("mDurationPerSecond", true) + duration);
    return {ExecutionStop::Returned, duration};
  } catch (Stop &stop) {
    return std::move(stop.result);
  }
}
} // namespace neverd::mobile::dalvik
