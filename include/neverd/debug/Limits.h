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

#ifndef NEVERD_DEBUG_LIMITS_H
#define NEVERD_DEBUG_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::debug {

// lib/debug/DWARFLoader.cpp
/// Records and arrays nested deeper than this are not laid out.
inline constexpr int kDWARFLoaderMaxLayoutDepth = 16;
/// More scalars than a layout lists: no convention passes a value with more
/// in registers, nor reads its scalars to place it.
inline constexpr size_t kDWARFLoaderMaxLayoutLeaves = 64;
/// More elements than an array's layout is computed for.
inline constexpr uint64_t kDWARFLoaderMaxLayoutElements = uint64_t(1) << 32;

// lib/debug/PDBLoader.cpp
/// Progress/checkpoint granularity, never a limit on accepted debug records.
inline constexpr uint32_t kPDBLoadPublicProgressRecords = 4096;
inline constexpr uint32_t kPDBLoadModuleProgressRecords = 16;
inline constexpr size_t kPDBLoaderPDBDebugContextImplLoadRecordParametersWork =
    200000;

// lib/debug/DWARFLoader.cpp
inline constexpr int
    kDWARFLoaderDWARFDebugContextImplRecordParameterDepthLimit = 32;
inline constexpr int
    kDWARFLoaderDWARFDebugContextImplRecordParameterQualifiedSizeLimit = 4096;
inline constexpr int
    kDWARFLoaderDWARFDebugContextImplConvertDwarfTypeDepthLimit = 16;
inline constexpr int
    kDWARFLoaderDWARFDebugContextImplResolveDeclaredTypeResolveDepthLimit = 32;
inline constexpr int
    kDWARFLoaderDWARFDebugContextImplClassifyReturnValueDepthLimit = 16;
inline constexpr int
    kDWARFLoaderDWARFDebugContextImplParseCompileUnitsWorkersObjectDataGetBufferSizeLimit =
        256 * 1024;

// lib/debug/PDB20Loader.cpp
inline constexpr int kPDB20LoaderResolveDepthLimit = 32;

// lib/debug/PDBLoader.cpp
inline constexpr int kPDBLoaderPDBDebugContextImplResolveIndexDepthLimit = 32;
inline constexpr int
    kPDBLoaderPDBDebugContextImplAttachDisplayFieldsDepthLimit = 32;
inline constexpr int kPDBLoaderPDBDebugContextImplVisitProcedureTypeDepthLimit =
    8;
inline constexpr int
    kPDBLoaderPDBDebugContextImplRecordParameterCGetNameSizeLimit = 8192;
inline constexpr int
    kPDBLoaderPDBDebugContextImplLoadRecordParametersAtSizeLimit = 64;

// lib/debug/SymbolStore.cpp
inline constexpr int kSymbolStorePdbSymbolRequestNameSizeLimit = 240;

// lib/debug/DWARFLoader.cpp
inline constexpr int kMaxDWARFUnwrapDepth = 32;

// lib/debug/PDBLoader.cpp
inline constexpr int kMaxPDBUnwrapDepth = 32;

// lib/debug/map/MapDebugContextBase.cpp
inline constexpr int kMaxMapFunctionGapBytes = 16 * 1024 * 1024;

// lib/debug/map/MapDebugContextBase.cpp
inline constexpr int kMaxNearestMapSymbolDistance = 256;

} // namespace neverd::limits::debug

#endif // NEVERD_DEBUG_LIMITS_H
