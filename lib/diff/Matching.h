//===- Matching.h - Shared internals of the comparison ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Entry points the function matcher and the override editor share with the
/// structural delta.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_DIFF_MATCHING_H
#define NEVERD_LIB_DIFF_MATCHING_H

#include "neverd/diff/Comparison.h"
#include "neverd/diff/Control.h"
#include "neverd/diff/FunctionDelta.h"
#include "neverd/diff/Program.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>

namespace neverd::diff {

/// Stable mnemonic identities shared by matching and measurement validation.
std::vector<uint64_t> mnemonicKeys(const Program &P,
                                   const ComparisonControl &Control = {});

/// compareFunctions with each program's mnemonic keys (hashText of each
/// mnemonic) already computed.
FunctionDelta compareFunctionsWithKeys(
    const Program &Primary, uint32_t PF, const Program &Secondary, uint32_t SF,
    const Correspondence &Map, llvm::ArrayRef<uint64_t> PrimaryKeys,
    llvm::ArrayRef<uint64_t> SecondaryKeys,
    const ComparisonControl &Control = {},
    InstructionAlignmentProfile Alignment = CurrentInstructionAlignmentProfile);

/// Copies a delta's scores and tallies into its match.
void measure(FunctionMatch &M, const FunctionDelta &D);

} // namespace neverd::diff

#endif // NEVERD_LIB_DIFF_MATCHING_H
