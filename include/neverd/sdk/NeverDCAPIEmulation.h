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
/// requests, registry, pnp_devices, and kernel_exports (routine names mapped
/// to availability booleans). Each ordinary request supplies kind
/// (create/ioctl/read/write/cleanup/close), optional device or configured
/// device_id, and optional file (u32 identity, default zero). CREATE without
/// a device selector selects the sole live
/// device; later requests use their file's device. IOCTL requires code (u32 or
/// 0x string) and accepts input (hex bytes), output_size, and direct_input (the
/// separate direct IOCTL buffer's initial hex bytes). READ accepts output_size
/// and byte_offset; WRITE accepts input and byte_offset. Offsets default to
/// zero, accept integers or 0x strings, and the entire transfer must fit
/// nonnegative signed 64-bit file offsets. Forwarded CREATE/CLEANUP/CLOSE
/// requests require an explicit bus_completion with a final status. A positive
/// delay_100ns is accepted for WDM file forwarding, automatic KMDF forwarding,
/// and synchronous, callback-based asynchronous, or SEND_AND_FORGET CREATE
/// sends. Synchronous sends suspend the guest caller until the lower response
/// or relative timeout. Automatic KMDF forwarding retains the WDM IRP/file
/// through cleanup and destruction callbacks required during completion.
/// External WDF references preserve context but do not extend the lifetime
/// of a completed WDM file.
/// Configured pnp_devices require a
/// unique case-sensitive ASCII id, bus="resource_free" or "register_bank",
/// initial_device_power=
/// "D0" and initial_system_power="working". The register_bank bus additionally
/// requires nonempty combined resources/interrupts arrays. Each memory resource
/// id/raw_start/translated_start/length
/// and registers array is explicit; each register requires offset/width/access/
/// value. The bounded fixed assignments use exact aligned 1/2/4-byte read_only
/// or read_write registers. Physical addresses are synthetic, never host
/// memory. Register initial values persist across unmap/stop/restart.
/// Resource-free devices omit both arrays. Each interrupt requires id,
/// independent raw_vector/raw_level/raw_affinity and
/// translated_vector/translated_level/ translated_affinity, mode="latched", and
/// share="device_exclusive". Only CPU0/group0 and translated DIRQL3..12 are
/// supported. READ/WRITE/IOCTL may specify interrupt_events with explicit
/// after_100ns/device_id/interrupt_id; successful submission binds each pulse
/// to its connected resource epoch. Delivery is cooperative at callback
/// boundaries, including zero delay; source request completion does not cancel
/// a pulse. Reports keep interrupt BOOLEAN observations independently of
/// IRP/NTSTATUS results. A kind="pnp" request requires device_id, minor
/// (start/query_remove/cancel_remove/remove/query_stop/stop/
/// cancel_stop/surprise_removal), and
/// bus_completion with explicit final status (u32 or 0x string) and optional
/// nonnegative delay_100ns measured from provider receipt. Stop, cancel-stop,
/// surprise-removal, cancel-remove and remove require status zero. Query-stop
/// status 0x119 requires unmodeled resource requery and is rejected. PnP
/// requests forbid device, file, transfer and
/// cancellation fields. Optional initial_reported_device_power (D0/D3) is a
/// separate per-device notification fact; its absence does not imply D0.
/// Optional requested_device_power is a per-PDO FIFO of explicit device-power
/// responses, consumed only by matching guest PoRequestPowerIrp calls. Both
/// those entries and kind="power" requests require minor (query/set),
/// power_type (device/system), power_state (D0/D3 or working/sleeping3),
/// power_action (none/sleep), system_context (opaque u32 or 0x string) and
/// bus_completion. Power requests require device_id and forbid file, transfer
/// and cancellation fields. System query to working is unsupported. Child
/// power requests have independent report rows with origin="PoRequestPowerIrp"
/// and response_index; unused responses do not create observations. At most
/// NEVERD_DRIVER_SCENARIO_POWER_RESPONSE_LIMIT response entries are accepted
/// across all PDOs. Unknown keys and malformed/excessive requests are
/// errors. The original v1 entry point remains initialization-only.
NEVERD_API const char *
neverd_emulate_driver_scenario_json(neverd_session_t Sess, const char *Path,
                                    const char *ScenarioJSON,
                                    const neverd_driver_options_v1 *Options);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_EMULATION_H
