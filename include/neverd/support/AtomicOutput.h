//===- AtomicOutput.h - Single-mutation output publication -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared close-before-rename protocol for publishing a same-directory
/// temporary file without ever moving an existing destination aside.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_ATOMICOUTPUT_H
#define NEVERD_SUPPORT_ATOMICOUTPUT_H

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"

#include <system_error>

namespace neverd::support::atomic_output {

/// Filesystem boundary for the continuously protected close-before-rename
/// protocol.  Tests inject failures here without relying on platform-specific
/// disk behavior.
struct CommitOperations {
  /// Close must preserve cleanup established by RegisterSignalCleanup.
  llvm::function_ref<llvm::Error()> Close;
  llvm::function_ref<std::error_code(llvm::StringRef, llvm::StringRef)> Rename;
  /// Close and remove the candidate without changing the destination.
  llvm::function_ref<llvm::Error(llvm::StringRef)> Discard;
  /// Ensure cleanup is active before Close, reusing existing protection when
  /// TempFile already installed it.
  llvm::function_ref<llvm::Error(llvm::StringRef)> RegisterSignalCleanup;
  llvm::function_ref<void(llvm::StringRef)> UnregisterSignalCleanup;
};

/// Platform-neutral seam for the strict Windows replacement operation.  The
/// callback must perform exactly one replace-if-exists namespace mutation and
/// must never move the destination aside on failure.
struct ReplaceOperations {
  llvm::function_ref<std::error_code(llvm::StringRef, llvm::StringRef)> Replace;
};

llvm::Error commitTemporaryOutput(llvm::StringRef TemporaryPath,
                                  llvm::StringRef OutputPath,
                                  const CommitOperations &Operations);

std::error_code
replaceTemporaryOutputWithoutMoveAside(llvm::StringRef TemporaryPath,
                                       llvm::StringRef OutputPath,
                                       const ReplaceOperations &Operations);

/// Close and atomically publish Temporary.  On Windows this retains the
/// candidate's native handle across close and issues one FileRenameInfo
/// replace-if-exists request, so a locked destination fails without being
/// moved aside.  Success is the namespace linearization point.
llvm::Error closeAndCommitTemporaryOutput(llvm::sys::fs::TempFile &Temporary,
                                          llvm::StringRef OutputPath);

/// Close and remove Temporary while preserving useful close/remove context.
llvm::Error discardTemporaryOutput(llvm::sys::fs::TempFile &Temporary);

} // namespace neverd::support::atomic_output

#endif // NEVERD_SUPPORT_ATOMICOUTPUT_H
