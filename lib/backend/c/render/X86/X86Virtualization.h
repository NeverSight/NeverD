//===- X86Virtualization.h - Shared architectural asm source --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// C rendering and exact native/inline-assembly admission for VMX/SVM.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86VIRTUALIZATION_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86VIRTUALIZATION_H

#include "neverd/backend/llvm/X86/X86VirtualizationNative.h"

#include "llvm/ADT/StringRef.h"

#include <optional>
#include <string>
#include <vector>

namespace llvm {
class CallBase;
class CallInst;
} // namespace llvm

namespace neverd {
struct HighExpr;

inline constexpr const char X86VirtualizationCompilerDispatch[] =
    "#ifndef NEVERD_X86_VIRTUALIZATION\n"
    "#if defined(__has_builtin)\n"
    "#if __has_builtin(__builtin_ia32_vmwrite) && "
    "__has_builtin(__builtin_ia32_vmread_reg) && "
    "__has_builtin(__builtin_ia32_vmread_address) && "
    "__has_builtin(__builtin_ia32_vmload_addr32) && "
    "__has_builtin(__builtin_ia32_clgi)\n"
    "#define NEVERD_X86_VIRTUALIZATION(native, ...) native\n"
    "#endif\n"
    "#endif\n"
    "#ifndef NEVERD_X86_VIRTUALIZATION\n"
    "#define NEVERD_X86_VIRTUALIZATION(native, ...) __VA_ARGS__\n"
    "#endif\n"
    "#endif\n\n";

bool isX86VirtualizationNativeName(llvm::StringRef Name);
bool isX86KvmHypercallNativeName(llvm::StringRef Name);

std::string
renderX86VirtualizationExpression(Arch TargetArch, const HighExpr &Expression,
                                  const std::vector<std::string> &Arguments);

std::optional<std::string>
renderX86VirtualizationCall(Arch TargetArch, const llvm::CallInst &Call,
                            const std::string &Result, bool ResultLive,
                            const std::vector<std::string> &Arguments);

std::optional<std::string>
renderX86VirtualizationNativeCall(Arch TargetArch, const llvm::CallBase &Call,
                                  const std::string &Result, bool ResultLive,
                                  const std::vector<std::string> &Arguments);

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86VIRTUALIZATION_H
