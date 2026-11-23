//===- Win64CallStackInput.h - Caller-supplied scalar C inputs --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Both ordinary C routes bind an original CALL-time value at its use. A
/// macro permits a scalar lvalue or an inlined per-invocation observation.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_WIN64CALLSTACKINPUT_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_WIN64CALLSTACKINPUT_H

#include "neverd/ir/X86/Win64PrecallMemory.h"
#include "neverd/ir/med/X86/Win64CallStackInput.h"

#include "llvm/ADT/StringExtras.h"

#include <string>

namespace llvm {
class raw_ostream;
}

namespace neverd {
struct BinaryImage;

inline constexpr char Win64CallStackInputPrelude[] =
    "/* Supply the original pre-CALL RSP slot for this dynamic invocation. */\n"
    "#ifndef NEVERD_WIN64_CALL_STACK_U64\n"
    "extern uint64_t neverd_win64_call_stack_u64(uint64_t call_pc, "
    "uint32_t rsp_offset);\n"
    "#define NEVERD_WIN64_CALL_STACK_U64(pc, offset) "
    "neverd_win64_call_stack_u64((pc), (offset))\n"
    "#endif\n";

/// These parameters belong to the separate source view, never the native ABI.
void writeWin64PrecallReader(llvm::raw_ostream &OS, Win64PrecallMemory Memory,
                             const BinaryImage *Image);
std::string win64PrecallParameters(Win64PrecallMemory Memory);
inline constexpr llvm::StringLiteral Win64PrecallSuffix = "_from_precall_v6";

std::string win64CallStackInputText(llvm::StringRef Site,
                                    llvm::StringRef Offset,
                                    Win64PrecallMemory Memory);
inline std::string win64CallStackInputText(Win64CallStackInput Input,
                                           Win64PrecallMemory Memory) {
  return win64CallStackInputText("0x" + llvm::utohexstr(Input.Site) + "ULL",
                                 std::to_string(Input.Offset) + "U", Memory);
}
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_WIN64CALLSTACKINPUT_H
