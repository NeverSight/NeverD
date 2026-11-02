//===- NeverDCAPIX87.h - x87 capability inventory -----------------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read-only x87 capability report shared by SDK, CLI, Python and UI clients.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_X87_H
#define NEVERD_SDK_CAPI_X87_H

#include "neverd/sdk/NeverDCAPISession.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Return the static, factorized x87 mode/ABI/state/consumer/backend matrix.
/// Numerical and architectural-state support are reported separately. Source
/// test references are not execution evidence. This performs no host probe,
/// loads no binary and requires no optional CPU-emulation build. The session
/// may be unloaded. Release the owned UTF-8 JSON with neverd_free_string().
/// NULL indicates an invalid session or failure; inspect neverd_last_error().
NEVERD_API const char *neverd_x87_capabilities_json(neverd_session_t Sess);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_X87_H
