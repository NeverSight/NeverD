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

#ifndef NEVERD_PASS_LIMITS_H
#define NEVERD_PASS_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::pass {

// include/neverd/pass/ir/obf/InstSubstitutionPass.h
inline constexpr unsigned kDefaultInstructionSubstitutionRounds = 1;

// lib/pass/ir/simplify/common/SymSimplifyFinite.cpp
inline constexpr unsigned kSymSimplifyFiniteAnchorValuesMaxNodes = 32;
inline constexpr unsigned kSymSimplifyFiniteMaxEdges = 64;

// lib/pass/ir/simplify/common/ByteMemoryForwardingPass.cpp
inline constexpr int kByteMemoryForwardingPassAddressSizeLimit = 65536;

// lib/pass/ir/simplify/common/SymSimplifyPredicates.cpp
inline constexpr int kSymSimplifyPredicatesEnterDepthLimit = 128;

// lib/pass/ir/simplify/common/SymSimplifyTranslator.cpp
inline constexpr unsigned kSymSimplifyTranslatorSafeMaskedArithmeticRemaining =
    64;
inline constexpr unsigned kSymSimplifyTranslatorSafeMaskedArithmeticDepth = 8;
inline constexpr unsigned kSymSimplifyTranslatorSafeMaskedArithmeticMaxWidth =
    512;
inline constexpr unsigned kSymSimplifyTranslatorSafeMaskedOrRemaining = 64;

// lib/pass/ir/simplify/common/SymSimplifyFinite.cpp
inline constexpr int kSymSimplifyFiniteAtomicValuesVisitedLimit = 8;

} // namespace neverd::limits::pass

#endif // NEVERD_PASS_LIMITS_H
