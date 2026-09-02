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
#include "neverd/support/AtomicOutput.h"

#include "llvm/Support/Error.h"

namespace neverd::cli::detail {

// Compatibility aliases keep the private CLI surface stable while both the
// CLI and libneverd consume the one NeverDSupport implementation.
using AtomicOutputCommitOperations = support::atomic_output::CommitOperations;
using AtomicOutputReplaceOperations = support::atomic_output::ReplaceOperations;
using support::atomic_output::closeAndCommitTemporaryOutput;
using support::atomic_output::commitTemporaryOutput;
using support::atomic_output::discardTemporaryOutput;
using support::atomic_output::replaceTemporaryOutputWithoutMoveAside;

llvm::Error validateTranslationResultBindingV1(
    const neverd_translate_object_request_v1 &Request,
    const neverd_translate_object_result_v1 &Result);

} // namespace neverd::cli::detail

#endif // NEVERD_TOOLS_NEVERD_NEVERDTRANSLATEOUTPUT_H
