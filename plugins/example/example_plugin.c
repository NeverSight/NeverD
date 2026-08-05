//===- example_plugin.c - Minimal NeverD plugin example ---*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include <stdio.h>

#include "neverd/sdk/NeverDPlugin.h"

static int exampleInit(neverd_session_t Session) {
  printf("[ExamplePlugin] initialized, NeverD %s\n", neverd_version());
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
  if (Evt->Type == NEVERD_EVT_BINARY_LOADED)
    printf("[ExamplePlugin] binary loaded: %s\n", Evt->Data.BinaryLoaded.Path);
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
