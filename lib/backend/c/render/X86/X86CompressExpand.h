//===- X86CompressExpand.h - Compact memory C projection ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Both source routes preserve one native memory instruction.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86COMPRESSEXPAND_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86COMPRESSEXPAND_H
#include "neverd/backend/llvm/X86/X86CompressExpandNative.h"
#include "neverd/ir/high/HighIR.h"
namespace llvm {
class CallInst;
}
namespace neverd {
inline constexpr const char X86CompactCompilerDispatch[] =
    "#ifndef NEVERD_X86_COMPACT\n"
    "#if defined(__NEVERC_X86_COMPRESS_EXPAND__) && "
    "__NEVERC_X86_COMPRESS_EXPAND__ >= 1\n"
    "#define NEVERD_X86_COMPACT(native, ...) native\n"
    "#else\n#define NEVERD_X86_COMPACT(native, ...) "
    "__VA_ARGS__\n#endif\n#endif\n"
    "#ifndef NEVERD_X86_COMPACT_STACK\n"
    "#if defined(__NEVERC_X86_COMPRESS_EXPAND__) && "
    "__NEVERC_X86_COMPRESS_EXPAND__ >= 1 && "
    "defined(__NEVERC_X86_NATIVE_MEMORY_STACK_REFERENCE__) && "
    "__NEVERC_X86_NATIVE_MEMORY_STACK_REFERENCE__ >= 1\n"
    "#define NEVERD_X86_COMPACT_STACK(native, ...) native\n"
    "#else\n#define NEVERD_X86_COMPACT_STACK(native, ...) __VA_ARGS__\n"
    "#endif\n#endif\n";
X86CompactPlan x86CompactHighPlan(Arch A, const HighExpr &E);
std::vector<std::string> x86CompactSourceFeatures(const X86CompactPlan &P);
std::optional<X86CompactPlan> x86CompactCallPlan(Arch A,
                                                 const llvm::CallInst &Call);
std::string renderX86CompactExpression(Arch A, const HighExpr &E,
                                       const std::vector<std::string> &Args,
                                       const std::string &Type);
std::optional<std::string> renderX86CompactCall(
    Arch A, const llvm::CallInst &Call, const std::vector<std::string> &Args,
    const std::string &Result, bool Live, const std::string &Type);
} // namespace neverd
#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86COMPRESSEXPAND_H
