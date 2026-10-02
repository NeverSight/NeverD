//===- NeverDCAPIProcess.h - Explicit guest process execution ----*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_SDK_CAPI_PROCESS_H
#define NEVERD_SDK_CAPI_PROCESS_H
#include "neverd/sdk/NeverDCAPISession.h"

#ifdef __cplusplus
extern "C" {
#endif
enum {
#define NEVERD_PROCESS_REPORT_LIMIT(Name, CName, Value) CName = Value,
#include "neverd/emulation/ProcessReport.def"
#undef NEVERD_PROCESS_REPORT_LIMIT
};

/// Execute Path under linux-elf64-v1 or android-aarch64-api28-v1.
/// Both strings are required and nonempty. The session's loaded analysis image
/// is neither required nor changed. Requires CPU or driver emulation enabled.
/// OptionsJSON is NULL for defaults, or a NUL-terminated UTF-8 JSON object of
/// at most NEVERD_PROCESS_OPTIONS_JSON_LIMIT bytes. Optional keys: backend,
/// instruction_limit, event_limit, timeout_microseconds, memory_limit,
/// stack_size, output_limit, instruction_quantum, arguments and environment.
/// Limits must be positive integers; arguments/environment are string arrays.
/// Android uses an "android" object instead: entry_symbol or entry_address,
/// scalar arguments, load_bias, properties, memory, read_memory, initialize,
/// trace_limit, and an explicit libraries/symbols catalogue.
/// See docs/android-native-emulation.md for the wire format.
/// Unknown fields, invalid values, unavailable backends and unsupported image
/// layouts fail before entry. The host environment is never inherited.
///
/// The owned JSON report distinguishes guest exit_status from incomplete
/// execution; Android native returns use stop_reason=returned and return_value.
/// stdout_hex/stderr_hex preserve arbitrary bytes; addresses,
/// service numbers and raw register/result bits are hexadecimal strings.
/// Release with neverd_free_string(). NULL means setup/API failure; inspect
/// neverd_last_error(Sess). Guest faults and limits return a report, not NULL.
NEVERD_API const char *neverd_emulate_process_json(neverd_session_t Sess,
                                                   const char *Path,
                                                   const char *Profile,
                                                   const char *OptionsJSON);
#ifdef __cplusplus
}
#endif
#endif
