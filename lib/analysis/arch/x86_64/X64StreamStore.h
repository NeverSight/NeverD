//===- X64StreamStore.h - Retained streaming stores ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact default-space native store and fence shapes for raw x64 regions.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64STREAMSTORE_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64STREAMSTORE_H

#include "../../core/RecoveryArchitecture.h"

namespace neverd::analysis::x64::streamstore {
bool isOperation(const LowOp &Op);
bool isFence(const LowOp &Op);
bool supportsFence(const LowOp &Op, const SpecializationOptions &Options);
} // namespace neverd::analysis::x64::streamstore

#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64STREAMSTORE_H
