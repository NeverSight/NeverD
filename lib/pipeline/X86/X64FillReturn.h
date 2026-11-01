//===- X64FillReturn.h - Checked fill-loop register results -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recover a physical call result from a complete, position-independent body.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_X86_X64FILLRETURN_H
#define NEVERD_LIB_PIPELINE_X86_X64FILLRETURN_H

namespace neverd {
struct BinaryImage;
struct MedFunc;

/// Keep the call and its effects. Define only its otherwise unavailable RCX
/// result, using the arguments of that occurrence and authenticated code.
void recoverX64FillReturns(MedFunc &Function, const BinaryImage &Image);
} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64FILLRETURN_H
