//===- ExceptionEdge.h - IR-level exceptional edges ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The exceptional control-flow transfer NeverD's IR keeps separate from an
/// ordinary CFG edge, and the taxonomy of what each one is for.  These are the
/// only exception records a CFG consumer has to understand; everything else in
/// this family describes the tables they were recovered from.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_EXCEPTIONEDGE_H
#define NEVERD_LOADER_EXCEPTIONEDGE_H

#include "neverd/Common.h"

#include <cstdint>

namespace neverd {

enum class ExceptionalEdgeKind : uint8_t {
  SEHFilter,
  SEHHandler,
  SEHFinally,
  CxxCleanup,
  CxxCatch,
  /// Itanium landing pad that only runs destructors and resumes unwinding.
  ItaniumCleanupPad,
  /// Itanium landing pad that may stop the exception for a typed catch.
  ItaniumCatchPad,
  /// Itanium landing pad reached because an exception specification was
  /// violated, which calls `std::unexpected`/`std::terminate`.
  ItaniumSpecPad,
  /// Go `deferreturn` re-entry: the runtime resumes the frame here after
  /// running its deferred calls during a panic.
  GoDeferReturn,
  /// Go `recover` continuation.
  GoRecover,
  /// Delphi `try..finally` cleanup body.
  DelphiFinally,
  /// Delphi `try..except` body that catches anything, which is also what a
  /// `safecall` wrapper's automatic handler reaches.
  DelphiExcept,
  /// One `except on <class> do` arm.
  DelphiOnException,
  Unknown,
};

inline const char *getExceptionalEdgeKindName(ExceptionalEdgeKind Kind) {
  switch (Kind) {
  case ExceptionalEdgeKind::SEHFilter:
    return "seh-filter";
  case ExceptionalEdgeKind::SEHHandler:
    return "seh-handler";
  case ExceptionalEdgeKind::SEHFinally:
    return "seh-finally";
  case ExceptionalEdgeKind::CxxCleanup:
    return "cxx-cleanup";
  case ExceptionalEdgeKind::CxxCatch:
    return "cxx-catch";
  case ExceptionalEdgeKind::ItaniumCleanupPad:
    return "itanium-cleanup";
  case ExceptionalEdgeKind::ItaniumCatchPad:
    return "itanium-catch";
  case ExceptionalEdgeKind::ItaniumSpecPad:
    return "itanium-spec";
  case ExceptionalEdgeKind::GoDeferReturn:
    return "go-deferreturn";
  case ExceptionalEdgeKind::GoRecover:
    return "go-recover";
  case ExceptionalEdgeKind::DelphiFinally:
    return "delphi-finally";
  case ExceptionalEdgeKind::DelphiExcept:
    return "delphi-except";
  case ExceptionalEdgeKind::DelphiOnException:
    return "delphi-on";
  case ExceptionalEdgeKind::Unknown:
    return "unknown";
  }
  return "unknown";
}

/// IR-level exceptional transfer kept separate from ordinary CFG edges.  In a
/// successor list BlockId is the target block; in a predecessor list it is the
/// source block.  A value of -1 denotes a valid target outside the current
/// function/funclet while TargetVA retains the exact destination.
struct ExceptionalEdge {
  int BlockId = -1;
  va_t TargetVA = 0;
  ExceptionalEdgeKind Kind = ExceptionalEdgeKind::Unknown;
  uint32_t RegionIndex = 0;
  int32_t State = -1;

  bool operator==(const ExceptionalEdge &Other) const {
    return BlockId == Other.BlockId && TargetVA == Other.TargetVA &&
           Kind == Other.Kind && RegionIndex == Other.RegionIndex &&
           State == Other.State;
  }
};

} // namespace neverd

#endif // NEVERD_LOADER_EXCEPTIONEDGE_H
