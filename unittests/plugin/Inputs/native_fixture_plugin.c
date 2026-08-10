#include "neverd/sdk/NeverDPlugin.h"

static int fixtureInit(neverd_session_t Session) { return Session ? 0 : -1; }

static void fixtureTerm(void) {}

static int fixtureRun(neverd_session_t Session, int Arg) {
  return Session ? Arg + 7 : -1;
}

NEVERD_PLUGIN_EXPORT neverd_plugin_t neverd_plugin = {
    .Name = "Native Fixture",
    .Version = "1.2.3",
    .Author = "NeverD Tests",
    .Description = "Public plugin loading fixture",
    .Type = NEVERD_PLUGIN_GENERIC,
    .Init = fixtureInit,
    .Term = fixtureTerm,
    .Run = fixtureRun,
    .Event = 0,
};
