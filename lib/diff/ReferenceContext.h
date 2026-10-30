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

} // namespace neverd::diff

#endif // NEVERD_LIB_DIFF_REFERENCECONTEXT_H
