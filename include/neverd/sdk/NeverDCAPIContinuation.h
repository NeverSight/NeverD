//===- NeverDCAPIContinuation.h - Explicit x64 state C ------------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Experimental state-source export, separate from ordinary function C.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_NEVERDCAPICONTINUATION_H
#define NEVERD_SDK_NEVERDCAPICONTINUATION_H

#include "neverd/sdk/NeverDCAPISession.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Export selected x64 instruction regions under an explicit kernel-events-v6
/// execution domain. RequestJSON is a size-delimited UTF-8 object; it selects
/// either instructions or entries, mxcsr_mask and paging_address_bits. Native
/// timestamp, prefetch, speculation MSR, descriptor and page-table bindings are
/// individually opt-in. UseLLVM is exactly 0 (HighC) or 1 (LLVM C).
///
/// The returned C includes its state layout and a bounded resume driver. It
/// does not use an ordinary function ABI or execute the loaded image. CALL,
/// RET, privilege return and exception entry stop at environment boundaries.
/// An unimplemented native request leaves the instruction unexecuted.
///
/// On success, ManifestJSON receives an independently owned JSON manifest of
/// exact original bytes, input bindings, state offsets and the C source hash.
/// If non-NULL, it is cleared before any failure. Free both returned strings
/// with neverd_free_string(). NULL source indicates failure; inspect
/// neverd_last_error(). No implicit ordinary-decompile fallback is enabled.
NEVERD_API const char *
neverd_x64_continuation_source_v1(neverd_session_t Sess,
                                  const char *RequestJSON, size_t RequestSize,
                                  int UseLLVM, const char **ManifestJSON);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_NEVERDCAPICONTINUATION_H
