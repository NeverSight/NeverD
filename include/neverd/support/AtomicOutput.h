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

#include <filesystem>
#include <optional>
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

/// Platform-neutral seam for strict Windows publication. The callback selects
/// replacement or exclusive creation, performs exactly one namespace mutation,
/// and must never move the destination aside on failure.
struct ReplaceOperations {
  llvm::function_ref<std::error_code(llvm::StringRef, llvm::StringRef)> Replace;
};

enum class OutputMode { ReplaceExisting, CreateNew };

llvm::Error commitTemporaryOutput(llvm::StringRef TemporaryPath,
                                  llvm::StringRef OutputPath,
                                  const CommitOperations &Operations);

std::error_code
replaceTemporaryOutputWithoutMoveAside(llvm::StringRef TemporaryPath,
                                       llvm::StringRef OutputPath,
                                       const ReplaceOperations &Operations);

/// Close and atomically publish Temporary.  On Windows this retains the
/// candidate's native handle across close and issues one FileRenameInfo
/// rename request, so a locked destination fails without being moved aside.
/// CreateNew refuses existing destinations, using a no-replace rename on
/// Windows and an exclusive hard link on POSIX. Success is the namespace
/// linearization point; cleanup errors after POSIX publication are reported.
/// Optional permissions are applied after retiring delete-on-close and before
/// closing the candidate, since Windows refuses path metadata edits while the
/// file is delete-pending.
llvm::Error closeAndCommitTemporaryOutput(
    llvm::sys::fs::TempFile &Temporary, llvm::StringRef OutputPath,
    OutputMode Mode = OutputMode::ReplaceExisting,
    std::optional<std::filesystem::perms> Permissions = std::nullopt);

/// Close and remove Temporary while preserving useful close/remove context.
llvm::Error discardTemporaryOutput(llvm::sys::fs::TempFile &Temporary);

} // namespace neverd::support::atomic_output

#endif // NEVERD_SUPPORT_ATOMICOUTPUT_H
