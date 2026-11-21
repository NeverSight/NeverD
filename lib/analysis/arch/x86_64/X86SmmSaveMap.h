//===- X86SmmSaveMap.h - KVM SMM processor save maps ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Serialize and restore KVM's SMRAM layout selected by CPUID long-mode
/// support.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86SMMSAVEMAP_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86SMMSAVEMAP_H

#include "neverd/analysis/arch/x86_64/X86ModeExecution.h"

#include <array>

namespace neverd::analysis::detail {

using X86SmmSaveImage = std::array<uint8_t, 512>;

X86SmmSaveImage saveX86SmmState(const X86ModeState &State,
                                const translate::X86PagingState &Controls,
                                bool LongModeCapable);
void restoreX86SmmState(const X86SmmSaveImage &Image, X86ModeState &State,
                        translate::X86PagingState &Controls,
                        bool LongModeCapable);

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86SMMSAVEMAP_H
