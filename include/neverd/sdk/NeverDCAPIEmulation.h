//===- NeverDCAPIEmulation.h - Bounded driver execution C API -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Owned JSON reports for bounded driver initialization and lifecycle
/// scenarios.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_EMULATION_H
#define NEVERD_SDK_CAPI_EMULATION_H

#include "neverd/sdk/NeverDCAPISession.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// v1 requires struct_size == sizeof(neverd_driver_options_v1). All budgets
/// must be positive. NULL Options selects 100000 instructions, 64 MiB of guest
/// memory, 10000 recorded events, 5000 ms, and service name "NeverDDriver".
typedef struct neverd_driver_options_v1 {
  size_t struct_size;
  uint64_t instruction_limit;
  uint64_t memory_limit;
  uint64_t event_limit;
  uint64_t timeout_milliseconds;
  /// Optional NUL-terminated service name; see the profile's ASCII
  /// restrictions. NULL selects the default.
  const char *service_name;
} neverd_driver_options_v1;

enum {
#define NEVERD_DRIVER_SCENARIO_LIMIT(Name, CName, Value) CName = Value,
#include "neverd/emulation/DriverScenarioLimits.def"
#undef NEVERD_DRIVER_SCENARIO_LIMIT
};

/// Execute a fresh complete image using the bounded x64 WDM initialization
/// profile. A non-NULL, nonempty Path enters strict execution preflight
/// directly and does not require a loaded session. NULL Path uses the loaded
/// session's file path. Neither route runs the IR pipeline or modifies the
/// session image. The file must remain available and unchanged during the call.
/// Returns owned JSON, including partial observations on execution stops.
/// A "returned" stop does not imply successful NTSTATUS or a working driver.
/// NULL means invalid session/options, setup failure, or a disabled feature;
/// query neverd_last_error(Sess) for a non-NULL session. Free returned JSON
/// with neverd_free_string(). Requires NEVERD_ENABLE_DRIVER_EMULATION at build
/// time.
NEVERD_API const char *
neverd_emulate_driver_json(neverd_session_t Sess, const char *Path,
                           const neverd_driver_options_v1 *Options);

/// Execute a strict JSON scenario over the v1 budget/service options. Path and
/// ownership rules match neverd_emulate_driver_json. ScenarioJSON is required,
/// NUL-terminated UTF-8 JSON, limited to NEVERD_DRIVER_SCENARIO_JSON_LIMIT
/// bytes. Supported root keys: load_address (0x string), unload (boolean),
/// requests, and kernel_exports (routine names mapped to availability
/// booleans). Each request supplies kind
/// (create/ioctl/read/write/cleanup/close), optional device and optional file
/// (u32 identity, default zero). CREATE without a device selects the sole live
/// device; later requests use their file's device. IOCTL requires code (u32 or
/// 0x string) and accepts input (hex bytes), output_size, and direct_input (the
/// separate direct IOCTL buffer's initial hex bytes). READ accepts output_size
/// and byte_offset; WRITE accepts input and byte_offset. Offsets default to
/// zero, accept integers or 0x strings, and the entire transfer must fit
/// nonnegative signed 64-bit file offsets. Unknown keys and malformed/excessive
/// requests are errors. The original v1 entry point remains
/// initialization-only.
NEVERD_API const char *
neverd_emulate_driver_scenario_json(neverd_session_t Sess, const char *Path,
                                    const char *ScenarioJSON,
                                    const neverd_driver_options_v1 *Options);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_EMULATION_H
