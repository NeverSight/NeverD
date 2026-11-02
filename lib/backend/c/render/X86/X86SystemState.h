//===- X86SystemState.h - System instruction C rendering -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared HighC and LLVM C rendering of owned system assembly contracts.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86SYSTEMSTATE_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86SYSTEMSTATE_H

#include "neverd/backend/llvm/X86/X86SystemStateNative.h"

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

inline constexpr const char X86SystemStateCompilerDispatch[] =
    "#ifndef NEVERD_X86_SYSTEM_STATE\n"
    "#if defined(__has_builtin)\n"
    "#if __has_builtin(__builtin_ia32_clts) && "
    "__has_builtin(__builtin_ia32_skinit) && "
    "__has_builtin(__builtin_ia32_wrmsrns) && "
    "__has_builtin(__builtin_ia32_rdmsrlist) && "
    "__has_builtin(__builtin_ia32_wrmsrlist) && "
    "__has_builtin(__builtin_ia32_lkgs_reg) && "
    "__has_builtin(__builtin_ia32_lkgs_mem) && "
    "__has_builtin(__builtin_ia32_lar_reg16) && "
    "__has_builtin(__builtin_ia32_lsl_mem64) && "
    "__has_builtin(__builtin_ia32_lar_mem16_address) && "
    "__has_builtin(__builtin_ia32_lsl_mem64_address) && "
    "__has_builtin(__builtin_ia32_lkgs_address) && "
    "__has_builtin(__builtin_ia32_invlpgb_addr32) && "
    "__has_builtin(__builtin_ia32_tlbsync)\n"
    "#define NEVERD_X86_SYSTEM_STATE(native, ...) native\n"
    "#endif\n"
    "#endif\n"
    "#ifndef NEVERD_X86_SYSTEM_STATE\n"
    "#define NEVERD_X86_SYSTEM_STATE(native, ...) __VA_ARGS__\n"
    "#endif\n"
    "#endif\n\n";

bool isX86SystemStateNativeName(llvm::StringRef Name);

std::optional<std::string>
renderX86SystemStateNativeCall(Arch TargetArch, const llvm::CallBase &Call,
                               const std::string &Result, bool ResultLive,
                               const std::vector<std::string> &Arguments);

std::string
renderX86SystemStateExpression(Arch TargetArch, const HighExpr &Expression,
                               const std::vector<std::string> &Arguments,
                               bool MsvcIntrinsics, bool &HasCIntrinsics);

std::optional<std::string>
renderX86SystemStateCall(Arch TargetArch, const llvm::CallInst &Call,
                         const std::string &Result, bool ResultLive,
                         const std::vector<std::string> &Arguments);

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86SYSTEMSTATE_H
