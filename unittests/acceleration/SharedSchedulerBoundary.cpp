//===- SharedSchedulerBoundary.cpp - Scheduler library boundaries
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Submits tasks through a component archive, shared owner or SDK plugin.
///
//===----------------------------------------------------------------------===//

#include "neverd/support/Parallel.h"

extern "C"
#if defined(_WIN32) && defined(NEVERD_TEST_SHARED_BOUNDARY)
    __declspec(dllexport)
#endif
    void NEVERD_TEST_SCHEDULER_ENTRY(unsigned Count, void (*Body)(void *),
                                     void *Context) {
  neverd::runTaskWorkers(Count, Body, Context);
}
