//===- X86FourSource.h - Four-source C projection ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check complete native and owned-assembly contracts before source projection.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86FOURSOURCE_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86FOURSOURCE_H

#include "neverd/backend/llvm/X86/X86FourSourceNative.h"
#include "neverd/ir/high/HighIR.h"

#include <optional>
#include <string>
#include <vector>

namespace llvm {
class CallInst;
}
namespace neverd {
inline constexpr const char X86FourSourceCompilerDispatch[] =
    "#ifndef NEVERD_X86_FOUR_SOURCE\n"
    "#if defined(__NEVERC_X86_FOUR_SOURCE__) && __NEVERC_X86_FOUR_SOURCE__ >= "
    "1\n"
    "#define NEVERD_X86_FOUR_SOURCE(native, ...) native\n"
    "#else\n#define NEVERD_X86_FOUR_SOURCE(native, ...) __VA_ARGS__\n"
    "#endif\n#endif\n"
    "#ifndef NEVERD_X86_FOUR_SOURCE_STACK\n"
    "#if defined(__NEVERC_X86_FOUR_SOURCE__) && "
    "__NEVERC_X86_FOUR_SOURCE__ >= 1 && "
    "defined(__NEVERC_X86_NATIVE_MEMORY_STACK_REFERENCE__) && "
    "__NEVERC_X86_NATIVE_MEMORY_STACK_REFERENCE__ >= 1\n"
    "#define NEVERD_X86_FOUR_SOURCE_STACK(native, ...) native\n"
    "#else\n#define NEVERD_X86_FOUR_SOURCE_STACK(native, ...) __VA_ARGS__\n"
    "#endif\n#endif\n";
X86FourSourcePlan x86FourSourceHighPlan(Arch A, const HighExpr &E);
std::optional<X86FourSourcePlan> x86FourSourceCallPlan(Arch A,
                                                       const llvm::CallInst &C);
std::string renderX86FourSourceExpression(Arch A, const HighExpr &E,
                                          const std::vector<std::string> &Args,
                                          const std::string &Type);
std::optional<std::string> renderX86FourSourceCall(
    Arch A, const llvm::CallInst &Call, const std::vector<std::string> &Args,
    const std::string &Result, bool Live, const std::string &Type);
} // namespace neverd
#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86FOURSOURCE_H
