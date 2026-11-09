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
    "#ifndef NEVERD_X86_TLBSYNC\n"
    "#if defined(__has_builtin)\n"
    "#if __has_builtin(__builtin_ia32_tlbsync_control64) || "
    "((defined(__i386__) || defined(_M_IX86)) && "
    "__has_builtin(__builtin_ia32_tlbsync_control32))\n"
    "#define NEVERD_X86_TLBSYNC(native, ...) native\n"
    "#endif\n"
    "#endif\n"
    "#ifndef NEVERD_X86_TLBSYNC\n"
    "#define NEVERD_X86_TLBSYNC(native, ...) __VA_ARGS__\n"
    "#endif\n"
    "#endif\n\n"
    "#ifndef NEVERD_X86_INVLPGB32\n"
    "#if defined(__has_builtin)\n"
    "#if __has_builtin(__builtin_ia32_invlpgb32)\n"
    "#define NEVERD_X86_INVLPGB32(native, ...) native\n"
    "#endif\n"
    "#endif\n"
    "#ifndef NEVERD_X86_INVLPGB32\n"
    "#define NEVERD_X86_INVLPGB32(native, ...) __VA_ARGS__\n"
    "#endif\n"
    "#endif\n\n"
    "#ifndef NEVERD_X86_SKINIT\n"
    "#if defined(__has_builtin)\n"
    "#if __has_builtin(__builtin_ia32_skinit)\n"
    "#define NEVERD_X86_SKINIT(native, ...) native\n"
    "#endif\n"
    "#endif\n"
    "#ifndef NEVERD_X86_SKINIT\n"
    "#define NEVERD_X86_SKINIT(native, ...) __VA_ARGS__\n"
    "#endif\n"
    "#endif\n\n"
    "#ifndef NEVERD_X86_WRMSRNS\n"
    "#if defined(__has_builtin)\n"
    "#if __has_builtin(__builtin_ia32_wrmsrns)\n"
    "#define NEVERD_X86_WRMSRNS(native, ...) native\n"
    "#endif\n"
    "#endif\n"
    "#ifndef NEVERD_X86_WRMSRNS\n"
    "#define NEVERD_X86_WRMSRNS(native, ...) __VA_ARGS__\n"
    "#endif\n"
    "#endif\n\n"
    "#ifndef NEVERD_X86_SYSTEM_STATE\n"
    "#if defined(__has_builtin)\n"
    "#if __has_builtin(__builtin_ia32_clts) && "
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
    "#endif\n\n"
    "#ifndef NEVERD_X86_ARPL\n"
    "#if defined(__has_builtin)\n"
    "#if __has_builtin(__builtin_ia32_arpl_reg) && "
    "__has_builtin(__builtin_ia32_arpl_mem) && "
    "__has_builtin(__builtin_ia32_arpl_mem_address32)\n"
    "#define NEVERD_X86_ARPL(native, ...) native\n"
    "#endif\n"
    "#endif\n"
    "#ifndef NEVERD_X86_ARPL\n"
    "#define NEVERD_X86_ARPL(native, ...) __VA_ARGS__\n"
    "#endif\n"
    "#endif\n\n"
    "#ifndef NEVERD_X86_HRESET\n"
    "#if defined(__NEVERSIGHT_X86_HRESET__) && "
    "__NEVERSIGHT_X86_HRESET__ >= 1\n"
    "#define NEVERD_X86_HRESET(native, ...) native\n"
    "#elif defined(__has_builtin)\n"
    "#if __has_builtin(__builtin_ia32_hreset)\n"
    "#define NEVERD_X86_HRESET(native, ...) native\n"
    "#endif\n"
    "#endif\n"
    "#ifndef NEVERD_X86_HRESET\n"
    "#define NEVERD_X86_HRESET(native, ...) __VA_ARGS__\n"
    "#endif\n"
    "#endif\n\n";

/// Compiler builtins and inline assembly need no intrinsic header declaration.
/// Only the named header APIs selected by the renderer require one.
inline bool x86SystemStateUsesCHeader(Intrinsic Id, NdMemoryAddressSpace Space,
                                      bool MsvcIntrinsics) {
  return Id == Intrinsic::Xabort ||
         (MsvcIntrinsics && Space == NdMemoryAddressSpace::Default &&
          x86SystemStateContract(Id).Kind == X86SystemStateKind::ShadowWrite);
}

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
