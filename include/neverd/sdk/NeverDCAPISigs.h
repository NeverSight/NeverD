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

/// Match candidates and apply only independently evidenced names. Return the
/// candidate count (not the number of renamed functions), or -1 on failure.
NEVERD_API int neverd_apply_signatures(neverd_session_t Sess,
                                       const char *SigDir);
NEVERD_API int neverd_auto_apply_signatures(neverd_session_t Sess,
                                            const char *SigBaseDir);
NEVERD_API int neverd_apply_signature_file(neverd_session_t Sess,
                                           const char *SigPath);
NEVERD_API int neverd_sig_match_count(neverd_session_t Sess);
/// Candidate rows: addr, name, optional aliases, library, func_len, confirmed,
/// caller_confirmed. These check outgoing or incoming references respectively;
/// neither authorizes naming. Incoming rows name only caller-requested aliases.
/// auto_name is the independently admitted name at this address, or empty.
/// name_evidence is image_symbol, exception_personality, or none. An empty
/// auto_name carries candidate_reason=independent_identity_missing. All rows
/// at an address report the same decision; candidate aliases are not certified
/// by that decision. Consumers must use auto_name for identity-sensitive work.
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
