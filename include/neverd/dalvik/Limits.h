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

#ifndef NEVERD_DALVIK_LIMITS_H
#define NEVERD_DALVIK_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::dalvik {
// API 28 LOGGER_ENTRY_MAX_PAYLOAD, including the binary event's int32 tag.
inline constexpr unsigned kAndroidEventLogPayloadBytes = 4068;

// include/neverd/dalvik/Heap.h
inline constexpr uint64_t kHeapByteLimit = 8 * 1024 * 1024;
inline constexpr uint32_t kHeapObjectLimit = 4096;
// Conservative byte budget per retained ordinary instance-field map entry.
inline constexpr uint64_t kInstanceFieldStorageBytes = 64;

// Explicit framework observations; independent limits for distinct providers.
inline constexpr int kMaxAndroidSettingsValueBytes = 4096;
inline constexpr int kMaxAndroidApplicationInfoFieldBytes = 4096;
inline constexpr uint32_t kMaxAndroidApplicationSplitSources = 64;
inline constexpr int kMaxAndroidApplicationInfoQueries = 64;
inline constexpr int kMaxAndroidPackageResourcePathBytes = 4096;
inline constexpr int kMaxAndroidPackageInfoFieldBytes = 4096;
inline constexpr int kMaxAndroidPackageInfoQueries = 64;
inline constexpr int kMaxAndroidLocaleComponentBytes = 1024;
inline constexpr uint32_t kMaxAndroidClassLoaderPaths = 64;
inline constexpr uint32_t kMaxAndroidClassLoaderPathBytes = 4096;
inline constexpr uint32_t kMaxAndroidClassLoaderPathTotalBytes = 16384;
// Retained messages traversed or enqueued in one modeled queue.
inline constexpr uint32_t kMaxQueuedMessages = 1024;
inline constexpr uint32_t kMaxAndroidServiceCacheEntries = 256;
inline constexpr uint32_t kMaxAndroidBinderProxies = 256;
inline constexpr uint32_t kMaxAndroidServiceNameBytes = 4096;
inline constexpr uint32_t kMaxAndroidServiceLookupResults = 256;
inline constexpr uint32_t kMaxJavaURLCodeUnits = 4096;
inline constexpr uint32_t kMaxAndroidURLFailureHandlers = 2;
inline constexpr uint32_t kMaxAndroidURLHandlerMessageBytes = 4096;
// Conservative per-entry bookkeeping in addition to the retained state value.
inline constexpr uint64_t kFrameworkFieldStateOverheadBytes = 64;
inline constexpr uint64_t kAndroidApplicationInfoStateOverheadBytes =
    kFrameworkFieldStateOverheadBytes;

// lib/dalvik/AndroidFramework.cpp
inline constexpr int kMaxAndroidApplicationClassLookupFailures = 256;
inline constexpr int kMaxAndroidClassNameBytes = 1024;
inline constexpr int kMaxAndroidLookupMessageBytes = 4096;
inline constexpr int kMaxAndroidSharedLibraryPathBytes = 4095;

// lib/dalvik/AndroidVersion.cpp
inline constexpr int
    kAndroidVersionProgramAddAndroidVersionAddAddTextSizeLimit = 92;

// lib/dalvik/Budget.cpp
inline constexpr int kMaxInputWork = 20000000;
inline constexpr int kMinInputWork = 100000;
inline constexpr int kInputWorkPerByte = 4;
inline constexpr int kInputWorkSaturationBytes =
    kMaxInputWork / kInputWorkPerByte;

// lib/dalvik/Dex.cpp
inline constexpr int kDexBoundedWorkCountLimit = 128;
inline constexpr int kDexBoundedWorkStepsLimit = 128;
inline constexpr int kDexTakeSmallReadsLimit = 128;
inline constexpr int kDexQueryCopyWorkBytesLimit = 2048;
inline constexpr int kDexHeaderCountLimit = 32;
inline constexpr int kDexMutf8LimitMinArgument1 = 4096;
inline constexpr int kDexEncodedDepthLimit = 64;
inline constexpr int kDexAnnotationDepthLimit = 64;
inline constexpr int kDexInstructionsInitialMinArgument2 = 32;

// lib/dalvik/Library.cpp
inline constexpr int kLibraryExecuteLibraryNameBytesSizeLimit = 2048;

// lib/dalvik/Runtime.cpp
inline constexpr int kRuntimeRunNativeDepthLimit = 64;
inline constexpr int kRuntimeRunNativeRemainingStackLimit = 256;
inline constexpr int kRuntimeRunDepthLimit = 64;
inline constexpr int kRuntimeStateStartExecutionsSizeLimit = 64;
inline constexpr int kRuntimeProgramFindFieldDepthLimit = 64;
inline constexpr int kRuntimeProgramFindMethodFindFindDepthLimit = 64;
inline constexpr int kRuntimeProgramVirtualMethodDepthLimit = 64;
inline constexpr int kRuntimeProgramUnsupportedClassCheckDepthLimit = 64;

// lib/dalvik/Signature.cpp
inline constexpr int kSignatureTypeDepthLimit = 64;
inline constexpr int kSignatureEraseDepthLimit = 512;
inline constexpr int kSignatureSameTypeDepthLimit = 64;
inline constexpr int kSignatureSatisfiesDepthLimit = 64;
inline constexpr int kSignatureSatisfiesSeenSizeLimit = 1024;
inline constexpr int kSignaturePendingClassesSizeLimit = 64;
inline constexpr int kSignatureThrowableSeenSizeLimit = 64;

// lib/dalvik/Runtime.cpp
inline constexpr int kMaxRuntimeMethodHierarchyDepth = 64;

// lib/dalvik/Library.cpp
inline constexpr int kMaxJNIStringSearchWork = 1048576;

} // namespace neverd::limits::dalvik

#endif // NEVERD_DALVIK_LIMITS_H
