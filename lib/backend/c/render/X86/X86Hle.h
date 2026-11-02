//===- X86Hle.h - HLE source adapters -------------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// HighC state adaptation and exact LLVM native-call admission.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86HLE_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86HLE_H

#include "neverd/Common.h"

#include <string>
#include <vector>

namespace llvm {
class CallBase;
}

namespace neverd {
struct HighExpr;

std::string renderX86HleExpression(Arch TargetArch, const HighExpr &Expression,
                                   const std::vector<std::string> &Arguments);
std::string renderX86HleNativeCall(Arch TargetArch, const llvm::CallBase &Call,
                                   bool ResultLive,
                                   const std::vector<std::string> &Arguments);

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86HLE_H
