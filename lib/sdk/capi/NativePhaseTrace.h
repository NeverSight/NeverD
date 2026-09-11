//===- NativePhaseTrace.h - Bounded opt-in native phase diagnostics ------===//
#ifndef NEVERD_SDK_CAPI_NATIVE_PHASE_TRACE_H
#define NEVERD_SDK_CAPI_NATIVE_PHASE_TRACE_H

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>

namespace neverd::sdk {

class NativePhaseTrace {
public:
  enum class Phase { SessionLoad, ObjCExport, Pipeline };

private:
  using Clock = std::chrono::steady_clock;
  using Milliseconds = std::chrono::milliseconds;
  Phase Current;
  unsigned Iteration;
  bool Enabled = false;
  bool Finished = false;
  Clock::time_point Started;

  const char *name() const noexcept {
    switch (Current) {
    case Phase::SessionLoad:
      return "session_load";
    case Phase::ObjCExport:
      return "objc_export";
    case Phase::Pipeline:
      return "pipeline";
    }
    return "unknown";
  }

  Milliseconds::rep elapsed() const noexcept {
    const int SavedErrno = errno;
    const auto Elapsed =
        std::chrono::duration_cast<Milliseconds>(Clock::now() - Started)
            .count();
    errno = SavedErrno;
    return Elapsed;
  }

  void record(const char *Event, Milliseconds::rep Elapsed) noexcept {
    const int SavedErrno = errno;
    try {
      llvm::SmallString<192> Line;
      llvm::raw_svector_ostream OS(Line);
      OS << "[neverd-child-phase] phase=" << name() << " event=" << Event
         << " iteration=" << Iteration << " elapsed_ms=" << Elapsed << '\n';
      llvm::raw_fd_ostream Sink(2, /*shouldClose=*/false, /*unbuffered=*/true);
      llvm::scope_exit ClearError([&Sink]() noexcept { Sink.clear_error(); });
      Sink.write(Line.data(), Line.size());
      Sink.flush();
    } catch (...) {
      // Formatting and ordinary stream errors do not replace analysis errors.
    }
    errno = SavedErrno;
  }

public:
  explicit NativePhaseTrace(Phase Current, unsigned Iteration = 0) noexcept
      : Current(Current), Iteration(Iteration) {
    const int SavedErrno = errno;
    const char *Value = std::getenv("NEVERD_NATIVE_PHASES");
    Enabled = Value && Value[0] == '1' && Value[1] == '\0';
    if (Enabled) {
      Started = Clock::now();
      record("begin", 0);
    }
    errno = SavedErrno;
  }
  NativePhaseTrace(const NativePhaseTrace &) = delete;
  NativePhaseTrace &operator=(const NativePhaseTrace &) = delete;

  ~NativePhaseTrace() noexcept {
    if (Enabled && !Finished)
      record("aborted", elapsed());
  }

  void finish(bool Success) noexcept {
    if (!Enabled || Finished)
      return;
    Finished = true;
    record(Success ? "completed" : "failed", elapsed());
  }
};

} // namespace neverd::sdk
#endif
