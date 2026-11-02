//===- ReferenceContext.h - Independently paired address uses --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Candidate function pairs supported by address-reference occurrences in a
/// previously settled correspondence, including mapped entry-call roles.
/// This does not establish data identity or whole-function correspondence.
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
/// occurrences must agree before proposing automatic admission.
/// Supplying weak maps instead retains conflicting review roles, including
/// targets consumed by a provisional match and observed blocks of partial
/// callers. Unknown instructions, data or transfers still reject the block;
/// these review proposals cannot establish body identity.
std::vector<std::pair<uint32_t, uint32_t>> blockCallCandidates(
    const Program &P, llvm::ArrayRef<uint32_t> PMatch,
    llvm::ArrayRef<uint8_t> PTrusted, const Program &S,
    llvm::ArrayRef<uint32_t> SMatch, llvm::ArrayRef<uint8_t> STrusted,
    const ComparisonControl &Control, llvm::ArrayRef<uint32_t> WeakP = {},
    llvm::ArrayRef<uint32_t> WeakS = {});

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

struct NeighborhoodReviewCandidate {
  uint32_t Primary, Secondary;
  /// All observed ordered calls agree on distinct independently mapped targets.
  bool OrderedCallees = false;
  /// Independent caller and callee neighborhoods both agree.
  bool BothDirections = false;
};

/// Independent frozen neighbors can expose a competing body when a weak match
/// has a conflicting neighborhood. Caller roles require at least two new
/// independent witnesses and retain every observed source caller. These are
/// review proposals, including incomplete native bodies; they never establish
/// whole-function identity. Callee anchors cannot be marked incomplete.
std::vector<NeighborhoodReviewCandidate> neighborhoodReviewCandidates(
    const Program &P, const ProgramFeatures &PF,
    llvm::ArrayRef<uint32_t> PMatch, llvm::ArrayRef<uint32_t> WeakP,
    llvm::ArrayRef<uint8_t> PTrusted, const Program &S,
    const ProgramFeatures &SF, llvm::ArrayRef<uint32_t> SMatch,
    llvm::ArrayRef<uint32_t> WeakS, llvm::ArrayRef<uint8_t> STrusted,
    const ComparisonControl &Control);

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

/// Unmatched modeled callers can expose a possible inline boundary in a weak
/// helper match. Complete ordered entry calls must bind distinct already mapped
/// targets outside the proposed pair. These are review proposals only; the
/// comparison owner must also compare instruction coverage and CFG evidence.
std::vector<std::pair<uint32_t, uint32_t>> inlineCallerCandidates(
    const Program &P, const ProgramFeatures &PF,
    llvm::ArrayRef<uint32_t> PMatch, llvm::ArrayRef<uint32_t> WeakP,
    const Program &S, const ProgramFeatures &SF,
    llvm::ArrayRef<uint32_t> SMatch, const ComparisonControl &Control);

/// Complete callers can contradict a provisional body match. Returns review
/// alternatives with exactly one unmatched endpoint; WeakP/WeakS mark the
/// reviewable matches (NoIndex means protected or unmatched). No identity is
/// established and all observations use the same frozen correspondence.
/// Split receives conflicting observations separately; the comparison owner
/// requires preserved CFGs or greater competing instruction coverage for them.
std::vector<std::pair<uint32_t, uint32_t>> conflictingCallCandidates(
    const Program &P, const ProgramFeatures &PF,
    llvm::ArrayRef<uint32_t> PMatch, llvm::ArrayRef<uint32_t> WeakP,
    const Program &S, const ProgramFeatures &SF,
    llvm::ArrayRef<uint32_t> SMatch, llvm::ArrayRef<uint32_t> WeakS,
    const ComparisonControl &Control,
    std::vector<std::pair<uint32_t, uint32_t>> *Split = nullptr);

} // namespace neverd::diff

#endif // NEVERD_LIB_DIFF_REFERENCECONTEXT_H
