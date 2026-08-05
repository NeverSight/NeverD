//===- Diagnostic.h - Serialized diagnostic output -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Diagnostic helpers for code that can run inside a parallel region.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_DIAGNOSTIC_H
#define NEVERD_SUPPORT_DIAGNOSTIC_H

#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <mutex>

namespace neverd {

/// Process-wide lock serializing writes to the stderr diagnostic stream.
inline std::mutex &diagnosticMutex() {
  static std::mutex M;
  return M;
}

/// Holds \c diagnosticMutex() for the lifetime of one diagnostic statement.
///
/// \c llvm::errs() carries mutable stream state and \c WithColor emits the
/// severity prefix, the message and the color escapes as separate writes, so
/// two threads reaching the same diagnostic interleave their output and race on
/// that state.  This wrapper takes the lock before the prefix is written and
/// releases it when the full expression ends, which makes the whole
/// `<<` chain one atomic unit:
///
/// \code
///   syncWarning() << "phase: " << Name << " failed\n";
/// \endcode
///
/// Only diagnostics reachable from a parallel region need it; the serial phases
/// keep using \c llvm::WithColor directly.  Nothing streamed into it may emit a
/// diagnostic of its own — the mutex is not recursive.
class SyncDiag {
public:
  enum class Level { Warning, Error };

  explicit SyncDiag(Level L) : Lock(diagnosticMutex()), OS(open(L)) {}

  SyncDiag(const SyncDiag &) = delete;
  SyncDiag &operator=(const SyncDiag &) = delete;

  template <typename T> SyncDiag &operator<<(const T &Val) {
    OS << Val;
    return *this;
  }

private:
  static llvm::raw_ostream &open(Level L) {
    return L == Level::Error ? llvm::WithColor::error()
                             : llvm::WithColor::warning();
  }

  // Declaration order is load-bearing: members initialize in declaration order,
  // so the lock is held before open() writes the severity prefix, and releases
  // last on destruction.
  std::lock_guard<std::mutex> Lock;
  llvm::raw_ostream &OS;
};

/// Thread-safe replacement for `llvm::WithColor::warning()`.
inline SyncDiag syncWarning() { return SyncDiag(SyncDiag::Level::Warning); }

/// Thread-safe replacement for `llvm::WithColor::error()`.
inline SyncDiag syncError() { return SyncDiag(SyncDiag::Level::Error); }

} // namespace neverd

#endif // NEVERD_SUPPORT_DIAGNOSTIC_H
