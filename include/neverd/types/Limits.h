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

#ifndef NEVERD_TYPES_LIMITS_H
#define NEVERD_TYPES_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::types {

// include/neverd/types/TypeFit.h
inline constexpr size_t kTypeFitLimit = 50;

// lib/types/DWARFTypeImport.cpp
/// How deep a type expression nests: pointers, arrays, qualifiers and the
/// anonymous records defined in place.
inline constexpr unsigned kDWARFTypeImportMaxDepth = 64;

// lib/types/TypeFit.cpp
/// How deep a walk follows records held in records and typedefs of
/// typedefs before it stops, as one that holds itself would never end.
inline constexpr unsigned kTypeFitMaxDepth = 64;
/// How many places in a record a pointer to one of its members is tried at.
inline constexpr size_t kTypeFitMaxShifts = 4096;

// lib/types/CTypeParser.cpp
inline constexpr int kCTypeParserExpandDepthLimit = 16;
inline constexpr int kCTypeParserPassingDepthLimit = 16;

// lib/types/TypeImport.cpp
/// Nesting of inline type expressions. Named recursion uses a worklist.
inline constexpr unsigned kPrototypeTypeImportMaxDepth = 64;

// lib/types/CTypeRender.cpp
inline constexpr int kCTypeRenderTypedefRecordDepthLimit = 16;

// lib/types/CTypeRender.cpp
inline constexpr int kMaxCTypeDefinitionHops = 64;

// lib/types/DWARFTypeImport.cpp
inline constexpr int kMaxInferredRecordAlignment = 4096;

// lib/types/PDBTypeImport.cpp
inline constexpr int kPDBTypeImportImporterFieldsListsLimit = 4096;

// lib/types/PythonTypeRender.cpp
inline constexpr int kPythonTypeRenderPythonWriterTypedefinitionHopLimit = 64;

// lib/types/TypeFit.cpp
inline constexpr int kMaxFuzzyPatternBytes = 64;

// lib/types/TypeFit.cpp
inline constexpr int kMaxFuzzyCandidateBytes = 1024;

} // namespace neverd::limits::types

#endif // NEVERD_TYPES_LIMITS_H
