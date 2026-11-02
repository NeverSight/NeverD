//===- X86APXAtomic.h - APX atomic C projection ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Both C routes consume the native APX transaction contract.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86APXATOMIC_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86APXATOMIC_H

#include "neverd/backend/llvm/X86/X86APXAtomic.h"
#include "neverd/ir/high/HighIR.h"

#include <vector>

namespace llvm {
class CallInst;
}
namespace neverd {
inline constexpr const char X86APXAtomicCompilerDispatch[] =
    "#ifndef NEVERD_X86_APX_ATOMIC\n"
    "#if defined(__NEVERC_X86_APX_ATOMIC__) && __NEVERC_X86_APX_ATOMIC__ >= 1\n"
    "#define NEVERD_X86_APX_ATOMIC(native, ...) native\n"
    "#else\n"
    "#define NEVERD_X86_APX_ATOMIC(native, ...) __VA_ARGS__\n"
    "#endif\n#endif\n"
    "#ifndef NEVERD_X86_LEGACY_ATOMIC\n"
    "#if defined(__NEVERC_X86_LEGACY_ATOMIC__) && __NEVERC_X86_LEGACY_ATOMIC__ "
    ">= 1\n"
    "#define NEVERD_X86_LEGACY_ATOMIC(native, ...) native\n"
    "#else\n"
    "#define NEVERD_X86_LEGACY_ATOMIC(native, ...) __VA_ARGS__\n"
    "#endif\n#endif\n"
    "#ifndef NEVERD_X86_APX_ATOMIC_STACK\n"
    "#if defined(__NEVERC_X86_NATIVE_MEMORY_STACK_REFERENCE__) && "
    "__NEVERC_X86_NATIVE_MEMORY_STACK_REFERENCE__ >= 1\n"
    "#define NEVERD_X86_APX_ATOMIC_STACK(native, ...) "
    "NEVERD_X86_APX_ATOMIC(native, __VA_ARGS__)\n"
    "#else\n"
    "#define NEVERD_X86_APX_ATOMIC_STACK(native, ...) __VA_ARGS__\n"
    "#endif\n#endif\n"
    "#ifndef NEVERD_X86_LEGACY_ATOMIC_STACK\n"
    "#if defined(__NEVERC_X86_NATIVE_MEMORY_STACK_REFERENCE__) && "
    "__NEVERC_X86_NATIVE_MEMORY_STACK_REFERENCE__ >= 1\n"
    "#define NEVERD_X86_LEGACY_ATOMIC_STACK(native, ...) "
    "NEVERD_X86_LEGACY_ATOMIC(native, __VA_ARGS__)\n"
    "#else\n"
    "#define NEVERD_X86_LEGACY_ATOMIC_STACK(native, ...) __VA_ARGS__\n"
    "#endif\n#endif\n";

std::optional<X86APXAtomicPlan>
x86APXAtomicCallPlan(Arch A, const llvm::CallInst &Call);
std::string
renderX86APXAtomicExpression(Arch A, const HighExpr &E,
                             const std::vector<std::string> &Arguments);
std::optional<std::string>
renderX86APXAtomicCall(Arch A, const llvm::CallInst &Call,
                       const std::string &Result, bool ResultLive,
                       const std::vector<std::string> &Arguments);
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86APXATOMIC_H
