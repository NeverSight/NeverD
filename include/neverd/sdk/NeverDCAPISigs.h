//===- NeverDCAPISigs.h - C API signature matching ----------------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// FLIRT signature application and match reporting, plus the CRC16 helper
/// used when building signatures.
///
/// All returned strings are heap-allocated via strdup(); callers must
/// free them with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_SIGS_H
#define NEVERD_SDK_CAPI_SIGS_H

#include "neverd/sdk/NeverDCAPITypes.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifdef NEVERD_EXPORTS
#define NEVERD_API __declspec(dllexport)
#else
#define NEVERD_API __declspec(dllimport)
#endif
#else
#define NEVERD_API __attribute__((visibility("default")))
#endif

// ===--------------------------------------------------------------------===//
// FLIRT signature matching
// ===--------------------------------------------------------------------===//

NEVERD_API int neverd_apply_signatures(neverd_session_t Sess,
                                       const char *SigDir);
NEVERD_API int neverd_auto_apply_signatures(neverd_session_t Sess,
                                            const char *SigBaseDir);
NEVERD_API int neverd_apply_signature_file(neverd_session_t Sess,
                                           const char *SigPath);
NEVERD_API int neverd_sig_match_count(neverd_session_t Sess);
NEVERD_API const char *neverd_sig_matches_json(neverd_session_t Sess);

// ===--------------------------------------------------------------------===//
// Signature generation utilities
// ===--------------------------------------------------------------------===//

/// Compute CRC16 over a byte buffer (FLIRT-compatible algorithm).
NEVERD_API unsigned short neverd_sig_compute_crc16(const unsigned char *Data,
                                                   int Length);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_SIGS_H
