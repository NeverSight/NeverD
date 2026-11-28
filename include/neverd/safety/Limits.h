//===- Limits.h - Component limits and defaults -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource ceilings, work budgets and defaults owned by this component.
/// Binary layouts and instruction/ABI constants remain with their models.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_LIMITS_H
#define NEVERD_SAFETY_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::safety {

// lib/safety/AllocLifetime.cpp
inline constexpr unsigned kAllocLifetimeMaxProofSteps = 4096;
// Bound same-block SSA walks for a byte range or returned value independently
// of function size. A longer chain retains an unknown proof.
inline constexpr unsigned kIndexedReadMaxValueSteps = 32;

// lib/safety/ArgSlicer.cpp
inline constexpr unsigned kArgSlicerMaxProofSteps = 4096;

// lib/safety/DarwinSanitizerPublicationAdapter.cpp
inline constexpr size_t kDarwinSanitizerPublicationAdapterIOChunkBytes =
    64 * 1024;

// lib/safety/InterprocReachability.cpp
inline constexpr unsigned kInterprocReachabilityDefaultMaxCallDepth = 64;

// lib/safety/ObjectModel.cpp
inline constexpr unsigned kObjectModelMaxProofSteps = 4096;

// lib/safety/ProcessInputReplay.h
/// Hard ceiling for literal process data retained by one report finding.
inline constexpr uint64_t kProcessInputReplayProcessInputReplayByteBudget =
    uint64_t(1) << 20;

// lib/safety/SourceSemantics.h
inline constexpr size_t kSourceSemanticsMaxFormatBytes = 4096;

} // namespace neverd::limits::safety

namespace neverd::safety::process_replay {

// include/neverd/safety/ProcessReplay.h
inline constexpr uint32_t kProcessReplayMaxArguments = 4096;
inline constexpr uint32_t kProcessReplayMaxEnvironmentEntries = 4096;
inline constexpr uint32_t kProcessReplayMaxResources = 16;
inline constexpr uint32_t kProcessReplayMaxEvents = 4096;
inline constexpr uint64_t kProcessReplayMaxLiteralBytes = uint64_t(1) << 20;
inline constexpr uint64_t kProcessReplayMaxReadRequestBytes = uint64_t(1) << 24;
inline constexpr uint64_t kProcessReplayMaxTotalReadRequestBytes = uint64_t(1)
                                                                   << 28;

// include/neverd/safety/ProcessReplayExecutor.h
inline constexpr uint64_t kProcessReplayExecutionMaxWallTimeMs = 10 * 60 * 1000;
inline constexpr uint64_t kProcessReplayExecutionMaxCpuTimeUs =
    10 * 60 * 1000 * 1000;
inline constexpr uint64_t kProcessReplayExecutionMaxAddressSpaceBytes =
    uint64_t{64} << 30;
inline constexpr uint64_t kProcessReplayExecutionMaxResidentMemoryBytes =
    uint64_t{64} << 30;
inline constexpr uint32_t kProcessReplayExecutionMaxProcesses = 1;
inline constexpr uint64_t kProcessReplayExecutionMaxTargetSiteVisits =
    uint64_t{1} << 20;
inline constexpr uint64_t kProcessReplayExecutionMaxControlEvents = uint64_t{1}
                                                                    << 20;
inline constexpr uint64_t kProcessReplayExecutionMaxProtocolBytes = uint64_t{1}
                                                                    << 28;
inline constexpr uint64_t kProcessReplayExecutionMaxStreamBytes = uint64_t{1}
                                                                  << 28;
inline constexpr uint64_t kProcessReplayExecutionMaxAggregateOutputBytes =
    uint64_t{1} << 29;
inline constexpr uint64_t kProcessReplayExecutionMaxPreviewBytes = uint64_t{1}
                                                                   << 16;
inline constexpr uint64_t kProcessReplayExecutionMaxBackendChunkBytes =
    uint64_t{1} << 20;

} // namespace neverd::safety::process_replay

namespace neverd::safety::sanitizer_publication_metadata {

// include/neverd/safety/SanitizerPublicationMetadata.h
inline constexpr uint32_t kSanitizerPublicationMetadataMaxRecords = 64;
inline constexpr uint32_t kSanitizerPublicationMetadataMaxActions = 64;
inline constexpr uint64_t kSanitizerPublicationMetadataMaxEncodedBytes =
    uint64_t{1} << 24;

} // namespace neverd::safety::sanitizer_publication_metadata

namespace neverd::limits::safety {

// include/neverd/safety/SafetyTypes.h
/// Default SAT-search ceiling used when a caller leaves SolverConflicts at
/// zero.  Safety analysis must remain bounded unless a future API exposes an
/// explicit unbounded mode.
inline constexpr uint64_t kSafetyTypesDefaultSafetySolverConflicts = uint64_t(1)
                                                                     << 18;

// lib/safety/AllocLifetime.cpp
inline constexpr int
    kAllocLifetimeMayBeStackAddressThroughReloadsMayBeDepthLimit = 64;
inline constexpr int kAllocLifetimeStackOffsetRecDepthLimit = 48;
inline constexpr int kAllocLifetimeMayBeStackAddressProveDepthLimit = 64;
inline constexpr int kAllocLifetimeBuildSummariesMaxChangesMaxArgument1 = 1024;
inline constexpr int kDefaultExplorationPaths = 64;
inline constexpr unsigned kDefaultExplorationSteps = (1u << 16);
inline constexpr int kDefaultExplorationLoopVisits = 3;

// lib/safety/ArgSlicer.cpp
inline constexpr int kArgSlicerRequiresPathValidationImplDepthLimit = 64;
inline constexpr int kArgSlicerStackOffsetDepthLimit = 48;
inline constexpr int kArgSlicerMayBeStackAddressImplDepthLimit = 64;
inline constexpr int kArgSlicerForwardPointeeOffsetDepthLimit = 48;
inline constexpr int kArgSlicerUpperBoundDepthLimit = 64;
inline constexpr int kArgSlicerClassifyDepthLimit = 64;

// lib/safety/ObjectModel.cpp
inline constexpr int kObjectModelExactDataIdentityDepthLimit = 32;
inline constexpr int kObjectModelScalarConstantValueDepthLimit = 64;
inline constexpr int kObjectModelConstantValueDepthLimit = 32;
inline constexpr int kObjectModelAllocationObjectDepthLimit = 32;
inline constexpr int kObjectModelMayBeStackAddressDepthLimit = 64;
inline constexpr int kObjectModelStackOffsetDepthLimit = 64;

// lib/safety/StackSlotFlow.h
inline constexpr unsigned kStackSlotFlowAliasesWholeFrameRemaining = 4096;

// lib/safety/DarwinSanitizerPublicationAdapter.cpp
inline constexpr int kMaxDarwinXattrReadAttempts = 4;

// lib/safety/DarwinSanitizerPublicationAdapter.cpp
inline constexpr int kMaxDarwinTempNameAttempts = 32;

} // namespace neverd::limits::safety

#endif // NEVERD_SAFETY_LIMITS_H
