//===- BinaryContinuationInstructions.h - Original native spans -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original instruction reads shared with the strict binary recovery provider.

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_BINARYCONTINUATIONINSTRUCTIONS_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_BINARYCONTINUATIONINSTRUCTIONS_H

#include "neverd/analysis/InterpreterSpecialization.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <vector>

namespace neverd {
struct BinaryImage;
namespace analysis {

llvm::Expected<std::vector<SpecializationInstruction>>
readBinaryX64ContinuationInstructions(const BinaryImage &Image,
                                      llvm::ArrayRef<va_t> Addresses);

} // namespace analysis
} // namespace neverd

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_BINARYCONTINUATIONINSTRUCTIONS_H
