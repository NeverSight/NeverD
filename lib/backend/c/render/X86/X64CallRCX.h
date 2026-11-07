//===- X64CallRCX.h - Native Win64 call C projection ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One checked assembly contract serves both source routes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X64CALLRCX_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X64CALLRCX_H

#include "neverd/backend/llvm/X86/X64CallRCXNative.h"
#include "neverd/ir/high/HighIR.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class CallInst;
}
namespace neverd {
bool isX64CallRCX(Arch A, const llvm::CallInst &Call);
std::string renderX64CallRCXExpression(
    Arch A, const HighExpr &E,
    const std::function<std::string(const HighExpr &)> &Expr,
    const std::function<std::string(const MedVar &)> &Var,
    const std::function<bool(const MedVar &)> &Alive);
std::optional<std::string>
renderX64CallRCX(Arch A, const llvm::CallInst &Call, const std::string &Result,
                 bool ResultLive, const std::vector<std::string> &Arguments);
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X64CALLRCX_H
