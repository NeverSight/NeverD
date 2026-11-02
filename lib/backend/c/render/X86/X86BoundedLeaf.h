//===- X86BoundedLeaf.h - Bounded SGX/SMX C projection ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One validated state contract supplies both HighC and LLVM-to-C output.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86BOUNDEDLEAF_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86BOUNDEDLEAF_H

#include "neverd/backend/llvm/X86/X86BoundedLeaf.h"
#include "neverd/ir/high/HighIR.h"

#include <functional>
#include <optional>
#include <vector>

namespace llvm {
class CallInst;
}
namespace neverd {
std::optional<Intrinsic> x86BoundedLeafCallId(Arch A,
                                              const llvm::CallInst &Call);
std::string renderX86BoundedLeafExpression(
    Arch A, const HighExpr &E,
    const std::function<std::string(const HighExpr &)> &Expr,
    const std::function<std::string(const MedVar &)> &Var,
    const std::function<bool(const MedVar &)> &Alive);
std::optional<std::string>
renderX86BoundedLeafCall(Arch A, const llvm::CallInst &Call,
                         const std::string &Result, bool ResultLive,
                         const std::vector<std::string> &Arguments);
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86BOUNDEDLEAF_H
