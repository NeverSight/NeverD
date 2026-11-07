//===- X86SegmentAsm.h - C text for x86 segment selector moves --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared native builtin dispatch and GNU fallback for segment selector moves.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_X86_X86SEGMENTASM_H
#define NEVERD_BACKEND_C_RENDER_X86_X86SEGMENTASM_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"

#include <optional>
#include <string>
#include <vector>

namespace llvm {
class CallBase;
} // namespace llvm

namespace neverd {

inline constexpr const char X86SegmentMoveCompilerDispatch[] =
    "#ifndef NEVERD_X86_SEGMENT_MOVE\n"
    "#if defined(__has_builtin)\n"
    "#if __has_builtin(__builtin_ia32_readseg) && "
    "__has_builtin(__builtin_ia32_readseg_mem) && "
    "__has_builtin(__builtin_ia32_writeseg) && "
    "__has_builtin(__builtin_ia32_writeseg_mem)\n"
    "#define NEVERD_X86_SEGMENT_MOVE(native, ...) native\n"
    "#endif\n"
    "#endif\n"
    "#ifndef NEVERD_X86_SEGMENT_MOVE\n"
    "#define NEVERD_X86_SEGMENT_MOVE(native, ...) __VA_ARGS__\n"
    "#endif\n"
    "#endif\n\n";

/// The selector named by its AT&T register name, without the `%` sigil.
std::string x86SegmentReadText(llvm::StringRef Name);
std::string x86SegmentWriteText(llvm::StringRef Name, llvm::StringRef Value);

bool isX86SegmentMoveNativeName(llvm::StringRef Name);
/// Validate the native call or the precise old-SDK assembly contract.
std::optional<std::string>
renderX86SegmentMoveCall(Arch TheArch, const llvm::CallBase &Call,
                         const std::string &Result, bool ResultLive,
                         const std::vector<std::string> &Arguments);

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_X86_X86SEGMENTASM_H
