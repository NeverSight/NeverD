//===- AndroidEventLog.h - API 28 event payload encoding --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared EventLogHelper/liblog list encoding for framework and JNI calls.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_DALVIK_ANDROID_EVENT_LOG_H
#define NEVERD_DALVIK_ANDROID_EVENT_LOG_H

#include "neverd/dalvik/Limits.h"
#include "neverd/dalvik/Runtime.h"

#include <array>
#include <cstdint>
#include <span>

namespace neverd::mobile::dalvik {
class AndroidEventLog {
public:
  explicit AndroidEventLog(uint32_t tag) : tag(tag) { storage[0] = 3; }
  void scalar(uint8_t type, uint64_t value);
  void string(std::span<const uint8_t> bytes);
  ExecutionResult string(const Object *object, const AdmitInstruction &admit);
  bool failed() const { return error != 0; }
  ExecutionResult write(const RuntimeServices &services,
                        const AdmitInstruction &admit);

private:
  // API 28 calloc storage includes four spare bytes beyond MAX_EVENT_PAYLOAD.
  // Truncated strings advance the cursor by their original bounded length.
  std::array<uint8_t, ::neverd::limits::dalvik::kAndroidEventLogPayloadBytes>
      storage{};
  uint32_t tag;
  unsigned position = 2, count = 0;
  int32_t error = 0;
};
} // namespace neverd::mobile::dalvik

#endif // NEVERD_DALVIK_ANDROID_EVENT_LOG_H
