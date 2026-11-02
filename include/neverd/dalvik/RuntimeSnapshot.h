//===- RuntimeSnapshot.h - Read-only Java workload state --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#pragma once

#include "neverd/dalvik/AndroidApplication.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::mobile::dalvik {
struct AndroidServiceManagerState {
  bool initialized = false;
  uint32_t cache = 0, stat_logger = 0;
  bool cache_known = false;
  struct CacheEntry {
    uint32_t name = 0, binder = 0;
    bool operator==(const CacheEntry &) const = default;
  };
  std::vector<CacheEntry> entries;
  std::optional<uint64_t> manager;
  struct BinderProxy {
    uint32_t key = 0, identity = 0;
    std::optional<bool> warn_on_blocking;
    bool operator==(const BinderProxy &) const = default;
  };
  std::vector<BinderProxy> binder_proxies;
  bool operator==(const AndroidServiceManagerState &) const = default;
};
struct AndroidMessageState {
  uint32_t identity = 0;
  uint64_t target = 0, callback = 0, next = 0;
  int64_t when = 0;
  uint32_t flags = 0;
  bool operator==(const AndroidMessageState &) const = default;
};
struct AndroidMessageQueueState {
  uint32_t identity = 0;
  bool blocked = false;
  // False means a guest mutation made the finite linked-list view incomplete.
  // No message dispatch is inferred from any queue snapshot.
  bool complete = true;
  std::vector<AndroidMessageState> messages;
  bool operator==(const AndroidMessageQueueState &) const = default;
};
// These are model heap identities, not JNI references or guest addresses.
// Zero means not materialized. The declared phase is separate from lazy
// materialization and does not certify execution of an Application constructor.
struct AndroidFrameworkState {
  std::optional<AndroidApplicationSnapshot::Phase> declared_phase;
  std::optional<bool> application_loader_established;
  uint32_t activity_thread = 0, application = 0, context = 0;
  uint32_t boot_loader = 0, system_loader = 0, application_loader = 0;
  // Unknown/unread is distinct from a guest-written or declared null value.
  std::optional<uint64_t> instrumentation;
  // Present only for a materialized queue with explicitly known initial state.
  std::optional<AndroidMessageQueueState> main_message_queue;
  std::optional<AndroidServiceManagerState> service_manager;
  bool operator==(const AndroidFrameworkState &) const = default;
};

struct JavaClassState {
  std::string descriptor, source_id;
  std::optional<std::string> superclass;
  // Determined by Program ownership, never by the untrusted source_id label.
  bool supplied = false;
  enum class Initialization {
    NotStarted,
    Prepared,
    Initializing,
    Initialized,
    Erroneous
  } initialization = Initialization::NotStarted;
  uint32_t class_object = 0;
  // Ownership in the explicit Android loader projection, not evidence of an
  // APK/classpath on a real device. Absent when that ownership is
  // unestablished.
  std::optional<AndroidApplicationSnapshot::ClassLookupFailure::Loader>
      projected_loader;
  bool operator==(const JavaClassState &) const = default;
};

struct RuntimeSnapshot {
  // Only immutable Program declarations, in descriptor order. This is not a
  // complete boot classpath; library-only intrinsics/array types are excluded.
  std::vector<JavaClassState> classes;
  AndroidFrameworkState framework;
  bool terminal = false;
  bool operator==(const RuntimeSnapshot &) const = default;
};
} // namespace neverd::mobile::dalvik
