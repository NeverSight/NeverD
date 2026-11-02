//===- X86RepState.h - REP completion in generated C ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Both C routes use the same native builtin and older-compiler fallback.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86REPSTATE_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86REPSTATE_H

#include "neverd/backend/llvm/X86/X86RepStateAsm.h"
#include "neverd/ir/high/HighIR.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace llvm {
class CallInst;
}

namespace neverd {
std::optional<Intrinsic> x86RepNativeId(std::string_view Name, Arch A);
std::optional<X86RepAsm> x86RepCallPlan(Arch A, const llvm::CallInst &Call);
std::string
renderX86RepExpression(Arch A, const HighExpr &E, const std::string &ResultType,
                       const std::function<std::string(const HighExpr &)> &Expr,
                       const std::function<std::string(const MedVar &)> &Var,
                       const std::function<bool(const MedVar &)> &Alive);
std::optional<std::string>
renderX86RepCall(Arch A, const llvm::CallInst &Call, const std::string &Result,
                 bool ResultLive, const std::vector<std::string> &Arguments);
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86REPSTATE_H
