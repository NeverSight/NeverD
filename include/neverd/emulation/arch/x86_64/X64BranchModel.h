//===- X64BranchModel.h - Immutable relative-branch model ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Immutable x64 relative-branch decoding models shared by execution backends.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_ARCH_X86_64_X64BRANCHMODEL_H
#define NEVERD_EMULATION_ARCH_X86_64_X64BRANCHMODEL_H

namespace neverd::emulation {
/// Relative-branch decoding rules, not a complete CPU or CPUID identity.
enum class X64BranchModel {
#define NEVERD_X64_BRANCH_MODEL(Name, Mode) Name,
#include "neverd/emulation/arch/x86_64/X64BranchModels.def"
#undef NEVERD_X64_BRANCH_MODEL
};
} // namespace neverd::emulation
#endif // NEVERD_EMULATION_ARCH_X86_64_X64BRANCHMODEL_H
