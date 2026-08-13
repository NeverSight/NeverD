//===- NeverDCAPIPlugin.h - C API plugin management ---------------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Discovery, lifecycle, and event dispatch for native and Python plugins.
/// Wraps PluginManager so tools never reference the internal C++ class.
///
/// All returned strings are heap-allocated via strdup(); callers must
/// free them with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_PLUGIN_H
#define NEVERD_SDK_CAPI_PLUGIN_H

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
// Plugin management
//
// Wraps PluginManager so tools never reference the internal C++ class.
// ===--------------------------------------------------------------------===//

/// Load all supported native (*.dylib / *.dll / *.so) and Python (*.py)
/// plugins from \p Dir in deterministic canonical-path order.
/// Returns the number of plugins successfully loaded.
NEVERD_API int neverd_plugins_load_dir(neverd_session_t Sess, const char *Dir);

/// Load one plugin file. Returns 1 on success or 0 on failure. The native
/// diagnostic is available through neverd_last_error().
NEVERD_API int neverd_plugins_load_file(neverd_session_t Sess,
                                        const char *Path);

/// Return a JSON array of loaded plugins:
///   [{"name":"…","version":"…","author":"…","description":"…",
///     "type":0,"kind":"python","path":"…"},…]
/// Caller frees with neverd_free_string().
NEVERD_API const char *neverd_plugins_list_json(neverd_session_t Sess);

/// Initialize all loaded plugins (calls each plugin's Init callback).
NEVERD_API void neverd_plugins_init(neverd_session_t Sess);

/// Terminate and unload all loaded plugins.
NEVERD_API void neverd_plugins_term(neverd_session_t Sess);

/// Run a specific plugin by name.  Returns the plugin's return code,
/// or -1 if the plugin was not found.
NEVERD_API int neverd_plugins_run(neverd_session_t Sess, const char *Name,
                                  int Arg);

/// Return the number of currently loaded plugins.
NEVERD_API int neverd_plugins_count(neverd_session_t Sess);

/// Dispatch an event to all loaded plugins.
/// Requires the caller to include NeverDPlugin.h for neverd_event_t.
NEVERD_API void neverd_plugins_dispatch_event(neverd_session_t Sess,
                                              const void *Event);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_PLUGIN_H
