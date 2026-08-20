//===- NeverDCAPI.h - C API for NeverD decompiler -------------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Umbrella header for NeverD's pure C API.  External consumers include this
/// one file; it pulls in the domain headers the API is split across so no
/// include site has to know which one declares what.
///
/// All returned strings are heap-allocated via strdup(); callers free them
/// with neverd_free_string() unless a specific entry point documents its own
/// disposer.
///
///   - NeverDCAPITypes.h    -- shared handle / address / language types
///   - NeverDCAPISession.h  -- session lifecycle, metadata, errors, version
///   - NeverDCAPIDisasm.h   -- functions, bytes, disassembly, decompile, IR
///   - NeverDCAPIQuery.h    -- info panels, graphs, search, diff
///   - NeverDCAPISymbolic.h -- symbolic path exploration
///   - NeverDCAPIPatch.h    -- patching, pipeline, target config, benchmarks
///   - NeverDCAPIPersist.h  -- annotations and renames (JSON sidecar)
///   - NeverDCAPISigs.h     -- FLIRT signature matching and CRC16
///   - NeverDCAPIPlugin.h   -- plugin discovery, lifecycle, events
///   - NeverDCAPISimplify.h -- bitvector expression simplification
///   - NeverDCAPISynth.h    -- proof-gated expression synthesis
///   - NeverDCAPIOptimize.h -- transactional textual LLVM IR optimization
///   - NeverDCAPITranslate.h -- x86-64 to AArch64 relocatable objects
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_H
#define NEVERD_SDK_CAPI_H

#include "neverd/sdk/NeverDCAPIDisasm.h"
#include "neverd/sdk/NeverDCAPIOptimize.h"
#include "neverd/sdk/NeverDCAPIPatch.h"
#include "neverd/sdk/NeverDCAPIPersist.h"
#include "neverd/sdk/NeverDCAPIPlugin.h"
#include "neverd/sdk/NeverDCAPIQuery.h"
#include "neverd/sdk/NeverDCAPISafety.h"
#include "neverd/sdk/NeverDCAPISession.h"
#include "neverd/sdk/NeverDCAPISigs.h"
#include "neverd/sdk/NeverDCAPISimplify.h"
#include "neverd/sdk/NeverDCAPISymbolic.h"
#include "neverd/sdk/NeverDCAPISynth.h"
#include "neverd/sdk/NeverDCAPITranslate.h"
#include "neverd/sdk/NeverDCAPITypes.h"

#endif // NEVERD_SDK_CAPI_H
