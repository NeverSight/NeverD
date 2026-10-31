//===- ReferenceContext.h - Independently paired address uses --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Candidate function pairs supported by address-reference occurrences in a
/// previously settled correspondence. This does not establish data identity.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_DIFF_REFERENCECONTEXT_H
#define NEVERD_LIB_DIFF_REFERENCECONTEXT_H

#include "neverd/diff/Control.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace neverd::diff {

struct Program;
struct ProgramFeatures;

/// Complete blocks unique within independently paired complete callers align
/// call positions even when other blocks were added or reordered. All frozen
/// occurrences must agree; local call roles do not establish body identity.
std::vector<std::pair<uint32_t, uint32_t>>
blockCallCandidates(const Program &P, llvm::ArrayRef<uint32_t> PMatch,
                    llvm::ArrayRef<uint8_t> PTrusted, const Program &S,
                    llvm::ArrayRef<uint32_t> SMatch,
                    llvm::ArrayRef<uint8_t> STrusted,
                    const ComparisonControl &Control);

struct ReferenceCandidates {
  std::vector<std::pair<uint32_t, uint32_t>> Data;
  std::vector<std::pair<uint32_t, uint32_t>> Functions;
};

/// Returns unique candidates from address roles aligned in equal layouts of
/// independently matched witnesses. The supplied function maps are frozen for
/// this pass; proposed candidates never support their own evidence.
ReferenceCandidates
referenceCandidates(const Program &P, const ProgramFeatures &PF,
                    llvm::ArrayRef<uint32_t> PMatch, const Program &S,
                    const ProgramFeatures &SF, llvm::ArrayRef<uint32_t> SMatch,
                    const ComparisonControl &Control);

struct InitializerCandidates {
  std::vector<std::pair<uint32_t, uint32_t>> Contiguous;
  std::vector<std::pair<uint32_t, uint32_t>> Strided;
  std::vector<std::pair<uint32_t, uint32_t>> DataAddresses;
};

/// Complete initializer neighborhoods need distinct independent anchors and a
/// unique counterpart. Every aligned occurrence must agree before proposing an
/// unmatched function. Complete paired address uses can also bound intervals
/// with identical slot positions. This establishes neither object nor runtime
/// identity; proposals never bootstrap their own witnesses.
InitializerCandidates initializerCandidates(
    const Program &P, const ProgramFeatures &PF,
    llvm::ArrayRef<uint32_t> PMatch, llvm::ArrayRef<uint8_t> PTrusted,
    const Program &S, const ProgramFeatures &SF,
    llvm::ArrayRef<uint32_t> SMatch, llvm::ArrayRef<uint8_t> STrusted,
    const ComparisonControl &Control);

/// Complete callers can contradict a provisional body match. Returns review
/// alternatives with exactly one unmatched endpoint; WeakP/WeakS mark the
/// reviewable matches (NoIndex means protected or unmatched). No identity is
/// established and all observations use the same frozen correspondence.
std::vector<std::pair<uint32_t, uint32_t>> conflictingCallCandidates(
    const Program &P, const ProgramFeatures &PF,
    llvm::ArrayRef<uint32_t> PMatch, llvm::ArrayRef<uint32_t> WeakP,
    const Program &S, const ProgramFeatures &SF,
    llvm::ArrayRef<uint32_t> SMatch, llvm::ArrayRef<uint32_t> WeakS,
    const ComparisonControl &Control);

} // namespace neverd::diff

#endif // NEVERD_LIB_DIFF_REFERENCECONTEXT_H
