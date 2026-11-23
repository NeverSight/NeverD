//===- Win64CallStackInput.cpp - Read one original CALL observation -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The V6 state and its stack belong to the observed original invocation.
/// Read its flat RAM or translate into the supplied immutable stack window.
//===----------------------------------------------------------------------===//

#include "Win64CallStackInput.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <stdexcept>

namespace neverd {
std::string win64PrecallParameters(Win64PrecallMemory Memory) {
  std::string Result = "const void *__neverd_precall";
  if (Memory == Win64PrecallMemory::WindowV6)
    Result +=
        ", uint64_t __neverd_stack_base, const void *__neverd_stack_bytes, "
        "uint64_t __neverd_stack_size";
  return Result;
}

void writeWin64PrecallReader(llvm::raw_ostream &OS, Win64PrecallMemory Memory,
                             const BinaryImage *Image) {
  using namespace analysis;
  if (!Image)
    throw std::invalid_argument(
        "original_call_state requires its linked image");
  static_assert(offsetof(X64ContinuationStateV6, Core) == 0 &&
                offsetof(X64ContinuationStateV5, Core) == 0 &&
                offsetof(X64ContinuationStateV4, Core) == 0 &&
                offsetof(X64ContinuationStateV2, Core) == 0);
  const auto &Registers = getTargetRegInfo(Arch::X64);
  const auto SP =
      offsetof(X64ContinuationStateV1, GPR) + Registers.StackPointer;
  OS << "/* One original pre-CALL observation per invocation. */\n"
        "static __inline__ __attribute__((__always_inline__)) uint64_t "
        "__neverd_read_precall_word(const void **cursor, "
        "uint64_t expected_pc, uint32_t offset";
  const bool Window = Memory == Win64PrecallMemory::WindowV6;
  if (Window)
    OS << ", uint64_t base, const void *bytes, uint64_t size";
  OS << ") {\n"
        "    const unsigned char *state = (const unsigned char *)*cursor;\n"
        "    uint64_t pc, rsp, word;\n"
        "    if (!state) __builtin_trap();\n"
        "    __builtin_memcpy(&pc, state + "
     << offsetof(X64ContinuationStateV1, PC)
     << ", sizeof(pc));\n"
        "#ifdef NEVERD_WIN64_PRECALL_LOAD_BASE\n"
        "    static const uint64_t load_base = "
        "NEVERD_WIN64_PRECALL_LOAD_BASE;\n"
        "    const uint64_t link_base = 0x"
     << llvm::utohexstr(Image->Base)
     << "ULL;\n"
        "    if (expected_pc < link_base || pc < load_base ||\n"
        "        pc - load_base != expected_pc - link_base) __builtin_trap();\n"
        "#else\n"
        "    if (pc != expected_pc) __builtin_trap();\n"
        "#endif\n"
        "    __builtin_memcpy(&rsp, state + "
     << SP
     << ", sizeof(rsp));\n"
        "    if (rsp > UINT64_MAX - (uint64_t)offset - (sizeof(word) - 1)) "
        "__builtin_trap();\n"
        "    uint64_t address = rsp + offset;\n";
  if (Window)
    OS << "    if (!bytes || address < base || size < sizeof(word) || "
          "address - base > size - sizeof(word)) __builtin_trap();\n"
          "    __builtin_memcpy(&word, (const unsigned char *)bytes + "
          "(address - base), sizeof(word));\n";
  else
    OS << "    __builtin_memcpy(&word, (const void *)(uintptr_t)address, "
          "sizeof(word));\n";
  OS << "    *cursor = 0;\n    return word;\n}\n";
}

std::string win64CallStackInputText(llvm::StringRef Site,
                                    llvm::StringRef Offset,
                                    Win64PrecallMemory Memory) {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  if (Memory == Win64PrecallMemory::None) {
    OS << "((uint64_t)NEVERD_WIN64_CALL_STACK_U64(" << Site << ", " << Offset
       << "))";
  } else {
    OS << "__neverd_read_precall_word(&__neverd_precall, " << Site << ", "
       << Offset;
    if (Memory == Win64PrecallMemory::WindowV6)
      OS << ", __neverd_stack_base, __neverd_stack_bytes, __neverd_stack_size";
    OS << ")";
  }
  return Result;
}
} // namespace neverd
