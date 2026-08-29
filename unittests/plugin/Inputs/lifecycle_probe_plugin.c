//===- lifecycle_probe_plugin.c - Native plugin lifecycle probe -*- C -*-===//

#include "neverd/sdk/NeverDPlugin.h"

#include <stdio.h>
#include <stdlib.h>

static void recordLifecycle(const char *Event) {
  const char *Path = getenv("NEVERD_LIFECYCLE_PROBE_TRACE");
  if (!Path || !Path[0])
    return;

  FILE *Trace = fopen(Path, "ab");
  if (!Trace)
    return;
  fputs(Event, Trace);
  fputc('\n', Trace);
  fclose(Trace);
}

static int lifecycleInit(neverd_session_t Session) {
  (void)Session;
  const char *Fail = getenv("NEVERD_LIFECYCLE_PROBE_FAIL_INIT");
  if (Fail && Fail[0] && Fail[0] != '0') {
    recordLifecycle("init:failed");
    return 41;
  }
  recordLifecycle("init:success");
  return 0;
}

static void lifecycleTerm(void) { recordLifecycle("term"); }

NEVERD_PLUGIN_EXPORT neverd_plugin_t neverd_plugin = {
    .Name = "Lifecycle Probe",
    .Version = "1.0.0",
    .Author = "NeverD Tests",
    .Description = "Records native plugin initialization and termination",
    .Type = NEVERD_PLUGIN_GENERIC,
    .Init = lifecycleInit,
    .Term = lifecycleTerm,
    .Run = NULL,
    .Event = NULL,
};
