//===- NeverDPlugin.h - Plugin SDK for NeverD -------------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Public header for writing NeverD plugins.
// Plugins are shared libraries (.dylib/.dll/.so) that export a
// `neverd_plugin` symbol of type neverd_plugin_t.
//
// Pure C API — safe across any compiler/CRT combination.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_PLUGIN_H
#define NEVERD_SDK_PLUGIN_H

#include "neverd/sdk/NeverDCAPI.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  NEVERD_PLUGIN_GENERIC = 0,
  NEVERD_PLUGIN_LOADER = 1,
  NEVERD_PLUGIN_PROCESSOR = 2,
  NEVERD_PLUGIN_UI = 3,
} neverd_plugin_type_t;

typedef enum {
  NEVERD_EVT_BINARY_LOADED = 1,
  NEVERD_EVT_BINARY_CLOSING = 2,
  NEVERD_EVT_FUNC_SELECTED = 3,
  NEVERD_EVT_ADDR_CHANGED = 4,
  NEVERD_EVT_ANALYSIS_DONE = 5,
  NEVERD_EVT_PATCH_APPLIED = 6,
} neverd_event_type_t;

typedef struct {
  neverd_event_type_t Type;
  neverd_session_t Session;
  union {
    struct {
      const char *Path;
    } BinaryLoaded;
    struct {
      neverd_va_t Addr;
      const char *Name;
    } FuncSelected;
    struct {
      neverd_va_t Addr;
    } AddrChanged;
    struct {
      const char *OutputPath;
      int CodeSize;
    } PatchApplied;
  } Data;
} neverd_event_t;

typedef int (*neverd_plugin_init_fn)(neverd_session_t Session);
typedef void (*neverd_plugin_term_fn)(void);
typedef int (*neverd_plugin_run_fn)(neverd_session_t Session, int Arg);
typedef int (*neverd_plugin_event_fn)(const neverd_event_t *Event);

typedef struct {
  const char *Name;
  const char *Version;
  const char *Author;
  const char *Description;
  neverd_plugin_type_t Type;

  neverd_plugin_init_fn Init;
  neverd_plugin_term_fn Term;
  neverd_plugin_run_fn Run;
  neverd_plugin_event_fn Event;
} neverd_plugin_t;

#ifdef _WIN32
#define NEVERD_PLUGIN_EXPORT __declspec(dllexport)
#else
#define NEVERD_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_PLUGIN_H
