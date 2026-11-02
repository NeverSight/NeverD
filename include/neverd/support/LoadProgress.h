//===- LoadProgress.h - Cooperative input preparation ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Borrowed progress and stop callbacks for synchronous input preparation.
/// Neither callbacks nor their contexts become part of the loaded image.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_LOADPROGRESS_H
#define NEVERD_SUPPORT_LOADPROGRESS_H

namespace neverd {

/// Unwinds synchronous preparation to its owning load boundary. Loader entry
/// points translate this to operation_canceled; the C SDK returns -2. No
/// partially prepared image may be published after this signal.
struct LoadCancelled final {
  static constexpr const char *Message = "input loading cancelled";
};

/// Optional load-time progress and cooperative cancellation. Callbacks must
/// not throw or reenter the loading object, and their contexts must outlive
/// the synchronous operation. Stop may run on joined parser workers and must
/// be thread-safe; it should latch a request until the operation returns.
/// Progress callbacks are serialized, including reports from PDB workers.
/// check()/report() throw LoadCancelled when a checkpoint observes a stop;
/// external I/O and individual parser operations may delay that checkpoint.
struct LoadProgress {
  using Fn = void (*)(void *User, const char *Phase, unsigned long long Done,
                      unsigned long long Total, const char *Detail);
  using StopFn = int (*)(void *User);
  Fn Callback = nullptr;
  void *User = nullptr;
  StopFn Stop = nullptr;
  void *StopUser = nullptr;

  explicit operator bool() const { return Callback || Stop; }

  void check() const {
    if (Stop && Stop(StopUser))
      throw LoadCancelled{};
  }

  void report(const char *Phase, unsigned long long Done,
              unsigned long long Total, const char *Detail = "") const {
    check();
    if (Callback)
      Callback(User, Phase, Done, Total, Detail ? Detail : "");
    check();
  }
};

} // namespace neverd

#endif // NEVERD_SUPPORT_LOADPROGRESS_H
