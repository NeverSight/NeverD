#include "neverd/sdk/NeverDPlugin.h"

NEVERD_PLUGIN_EXPORT neverd_plugin_t neverd_plugin = {
    .Name = 0,
    .Version = "1.0.0",
    .Author = "NeverD Tests",
    .Description = "Invalid descriptor fixture",
    .Type = NEVERD_PLUGIN_GENERIC,
    .Init = 0,
    .Term = 0,
    .Run = 0,
    .Event = 0,
};
