//===- X86NativeFeatures.h - Native C feature ownership ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Select compiler APIs and merge their required features once per function.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86NATIVEFEATURES_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86NATIVEFEATURES_H

#include "neverd/ir/high/HighIR.h"

#include <string>

namespace llvm {
class Function;
}
namespace neverd {
std::string x86NativeFunctionPreamble(const HighFunc &Function,
                                      Arch TargetArch);
std::string x86NativeFunctionPreamble(const llvm::Function &Function,
                                      Arch TargetArch);
} // namespace neverd
#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86NATIVEFEATURES_H
