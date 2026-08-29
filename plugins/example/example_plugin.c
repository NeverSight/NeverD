//===- example_plugin.c - Minimal NeverD plugin example ---*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDPlugin.h"

#include <stdio.h>

static int exampleInit(neverd_session_t Session) {
  const char *Version = neverd_version();
  printf("[ExamplePlugin] initialized, NeverD %s\n",
         Version ? Version : "<unknown>");
  if (Version)
    neverd_free_string(Version);
  return 0;
}

static void exampleTerm(void) { printf("[ExamplePlugin] terminated\n"); }

static int exampleRun(neverd_session_t Session, int Arg) {
  int Count = neverd_func_count(Session);
  printf("[ExamplePlugin] binary has %d functions\n", Count);
  for (int I = 0; I < Count && I < 5; ++I) {
    const char *Name = neverd_func_name(Session, I);
    printf("  [%d] %s\n", I, Name ? Name : "<null>");
    if (Name)
      neverd_free_string(Name);
  }
  return 0;
}

static int exampleEvent(const neverd_event_t *Evt) {
  // The host invokes this callback through neverd_plugins_dispatch_event().
  // Evt and pointers in its payload are borrowed until this call returns.
  if (!Evt)
    return 0;

  if (Evt->Type == NEVERD_EVT_BINARY_LOADED) {
    const char *Path = Evt->Data.BinaryLoaded.Path;
    printf("[ExamplePlugin] binary loaded: %s\n", Path ? Path : "<unknown>");
  }
  return 0;
}

NEVERD_PLUGIN_EXPORT neverd_plugin_t neverd_plugin = {
    .Name = "Example Plugin",
    .Version = "1.0.0",
    .Author = "NeverD Team",
    .Description = "A minimal example plugin demonstrating the plugin API",
    .Type = NEVERD_PLUGIN_GENERIC,
    .Init = exampleInit,
    .Term = exampleTerm,
    .Run = exampleRun,
    .Event = exampleEvent,
};
