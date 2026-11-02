//===- X86AmxState.h - Architectural AMX source rendering -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared native bank operation rendering for HighC and LLVM-to-C.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86AMXSTATE_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86AMXSTATE_H

#include "neverd/ir/high/HighIR.h"

#include <optional>
#include <set>
#include <string>
#include <vector>
namespace llvm {
class CallInst;
class Function;
} // namespace llvm
namespace neverd {
std::string renderX86AmxExpression(Arch A, const HighExpr &E,
                                   const std::vector<std::string> &Arguments,
                                   const std::string &Type);
std::optional<std::string>
renderX86AmxCall(Arch A, const llvm::CallInst &Call,
                 const std::vector<std::string> &Arguments,
                 const std::string &Result = {}, bool ResultLive = false,
                 const std::string &Type = "unsigned _BitInt(512)");
bool isX86AmxNativeRowCall(const llvm::CallInst &Call, Arch A);
std::set<std::string> x86AmxFunctionFeatures(const HighFunc &Function);
std::set<std::string> x86AmxFunctionFeatures(const llvm::Function &Function);
} // namespace neverd
#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86AMXSTATE_H
