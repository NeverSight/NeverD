//===- ExceptionModel.h - Exception dispatch model taxonomy ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Names the dispatch machinery a decoded exception record belongs to.  Shared
/// by the language model records in \ref LanguageEH.h and by the native
/// encoding taxonomy in \ref ExceptionEncoding.h, so it is kept on its own
/// rather than in either of them.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_EXCEPTIONMODEL_H
#define NEVERD_LOADER_EXCEPTIONMODEL_H

#include <cstdint>

namespace neverd {

/// Which dispatch machinery a record describes.  The model determines how a
/// handler is found at run time, and therefore which normalized fields carry
/// meaning: a Windows table model resolves handlers by address range, an
/// Itanium model resolves them by call-site region plus an action chain, a
/// registration model resolves them by walking a linked list the prologue
/// built on the stack, and Go resolves them from its own frame metadata.
enum class ExceptionModel : uint8_t {
  None,
  /// PE `.pdata`/`.xdata` on x64, ARM32, and ARM64.
  WindowsTable,
  /// x86-32 `EXCEPTION_REGISTRATION_RECORD` chain rooted at `FS:[0]`.
  WindowsRegistration,
  /// DWARF call frame information plus an Itanium LSDA.
  Itanium,
  /// Darwin `__unwind_info` compact unwind, optionally with an Itanium LSDA.
  CompactUnwind,
  /// Go runtime frame metadata (`pclntab` funcdata/pcdata).
  GoRuntime,
  /// ARM EHABI: a sorted `.ARM.exidx` index over the whole image, whose
  /// entries either encode a frame's unwinding inline or point into
  /// `.ARM.extab`.  A C++ frame's language data is appended to its
  /// `.ARM.extab` entry rather than given a section of its own, so an image
  /// can carry a full Itanium call-site table with no `.gcc_except_table`
  /// anywhere in it.
  ARMEHABI,
};

inline const char *getExceptionModelName(ExceptionModel Model) {
  switch (Model) {
  case ExceptionModel::None:
    return "none";
  case ExceptionModel::WindowsTable:
    return "windows-table";
  case ExceptionModel::WindowsRegistration:
    return "windows-registration";
  case ExceptionModel::Itanium:
    return "itanium";
  case ExceptionModel::CompactUnwind:
    return "compact-unwind";
  case ExceptionModel::GoRuntime:
    return "go-runtime";
  case ExceptionModel::ARMEHABI:
    return "arm-ehabi";
  }
  return "unknown";
}

} // namespace neverd

#endif // NEVERD_LOADER_EXCEPTIONMODEL_H
