//===- X86Sgx.h - SGX source completion ----------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Render the shared SGX native register and flag contract in both C routes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86SGX_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86SGX_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/X86/X86Sgx.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class CallInst;
}

namespace neverd {

std::string
renderX86SgxExpression(Arch A, const HighExpr &E,
                       const std::function<std::string(const HighExpr &)> &Expr,
                       const std::function<std::string(const MedVar &)> &Var,
                       const std::function<bool(const MedVar &)> &Alive);

std::optional<std::string>
renderX86SgxCall(Arch A, const llvm::CallInst &Call, const std::string &Result,
                 bool ResultLive, const std::vector<std::string> &Arguments);

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86SGX_H
