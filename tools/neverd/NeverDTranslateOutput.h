//===- NeverDTranslateOutput.h - Translation output support ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private contracts used by the translate-object command and its focused
/// tests.  Nothing in this header is part of libneverd's public ABI.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_NEVERDTRANSLATEOUTPUT_H
#define NEVERD_TOOLS_NEVERD_NEVERDTRANSLATEOUTPUT_H

#include "neverd/sdk/NeverDCAPITranslate.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"

#include <system_error>

namespace neverd::cli::detail {

/// Filesystem boundary for the continuously protected close-before-rename
/// commit protocol.  Tests inject failures here without relying on
/// platform-specific disk behavior.
struct AtomicOutputCommitOperations {
  /// Close must preserve cleanup established by RegisterSignalCleanup.
  llvm::function_ref<llvm::Error()> Close;
  llvm::function_ref<std::error_code(llvm::StringRef, llvm::StringRef)> Rename;
  /// Close and remove the candidate without changing the destination.
  llvm::function_ref<llvm::Error(llvm::StringRef)> Discard;
  /// Ensure cleanup is active before Close, reusing existing protection when
  /// the temporary-file implementation already installed it.
  llvm::function_ref<llvm::Error(llvm::StringRef)> RegisterSignalCleanup;
  llvm::function_ref<void(llvm::StringRef)> UnregisterSignalCleanup;
};

/// Platform-neutral seam for the strict Windows replace protocol.  The one
/// operation must atomically rename the candidate with replace-if-exists
/// semantics and must not move an existing destination aside on failure.
struct AtomicOutputReplaceOperations {
  llvm::function_ref<std::error_code(llvm::StringRef, llvm::StringRef)> Replace;
};

llvm::Error validateTranslationResultBindingV1(
    const neverd_translate_object_request_v1 &Request,
    const neverd_translate_object_result_v1 &Result);

llvm::Error
commitTemporaryOutput(llvm::StringRef TemporaryPath, llvm::StringRef OutputPath,
                      const AtomicOutputCommitOperations &Operations);

std::error_code replaceTemporaryOutputWithoutMoveAside(
    llvm::StringRef TemporaryPath, llvm::StringRef OutputPath,
    const AtomicOutputReplaceOperations &Operations);

llvm::Error closeAndCommitTemporaryOutput(llvm::sys::fs::TempFile &Temporary,
                                          llvm::StringRef OutputPath);

llvm::Error discardTemporaryOutput(llvm::sys::fs::TempFile &Temporary);

} // namespace neverd::cli::detail

#endif // NEVERD_TOOLS_NEVERD_NEVERDTRANSLATEOUTPUT_H
