//===- X64CallContinuation.h - Conditional call relation ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Explicit raw normal-return relation and retained-event integrity.

#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64CALLCONTINUATION_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64CALLCONTINUATION_H

#include "../../core/RecoveryArchitecture.h"

#include <array>

namespace neverd::analysis {
class RecoveryDigest;
}

namespace neverd::analysis::x64 {

llvm::Error validateReturningCallDeclarations(const SpecializationOptions &O);
void appendReturningCallDeclaration(RecoveryDigest &H,
                                    const SpecializationX64ReturningCall &D);
std::array<uint64_t, 4>
returningCallDeclarationWords(const SpecializationX64ReturningCall &D);
RecoveryCallTransition
returningCallTransition(const SpecializationX64ReturningCall &D);
std::string returningCallDigest(const SpecializationReturningCall &Call);
llvm::Error validateReturningCalls(const SpecializationResult &Result,
                                   const SpecializationOptions &Options);

} // namespace neverd::analysis::x64

#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64CALLCONTINUATION_H
