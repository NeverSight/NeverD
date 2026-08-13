//===- SBFInterpreterDetail.h - Private SBF interpreter state ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The raw instruction and call-frame records, the wide arithmetic
/// primitives, and the layout and memory validation the SBF execution loop
/// runs before it starts. Shared between SBFInterpreterSupport.cpp, which
/// defines them, and SBFInterpreter.cpp, which runs the loop.
///
/// This header is an implementation detail of the sbf/runtime library and
/// should NOT be included by code outside lib/sbf/runtime/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_RUNTIME_SBFINTERPRETERDETAIL_H
#define NEVERD_SBF_RUNTIME_SBFINTERPRETERDETAIL_H

#include "neverd/sbf/runtime/SBFInterpreter.h"

#include "llvm/Support/Compiler.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace neverd::sbf {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE interpreter_detail {

struct RawInstruction {
  size_t Slot = 0;
  uint8_t RawOpcode = 0;
  uint8_t Dst = 0;
  uint8_t Src = 0;
  int16_t Offset = 0;
  int32_t Immediate = 0;
  const OpcodeInfo *Info = nullptr;
};

struct CallFrame {
  std::array<uint64_t, kCalleeSavedRegisterCount> SavedRegisters{};
  uint64_t FramePointer = 0;
  size_t ReturnSlot = 0;
};

int64_t signed64(uint64_t Value);
int32_t signed32(uint32_t Value);

uint32_t arithmeticShiftRight32(uint32_t Value, uint32_t Shift);
uint64_t arithmeticShiftRight64(uint64_t Value, uint64_t Shift);

uint64_t unsignedHighMultiply64(uint64_t Left, uint64_t Right);
uint64_t signedHighMultiply64(uint64_t Left, uint64_t Right);

/// Decode the raw encoding at \p Slot without consulting the analyzed LowIR.
RawInstruction decodeRaw(const SBFProgram &Program, size_t Slot);

/// Reject a program the raw interpreter cannot execute deterministically.
llvm::Error validateProgram(const SBFProgram &Program,
                            const InterpreterOptions &Options);

/// Whether \p Region maps \p Size bytes at \p Address, reporting where.
bool rangeContains(const MemoryRegion &Region, uint64_t Address, size_t Size,
                   size_t &Offset);

/// Reject an overlapping or misaligned VM memory mapping.
llvm::Error validateMemory(const std::vector<MemoryRegion> &Memory,
                           Version TheVersion);

/// Append the program's own data regions and its stack to \p Memory.
void appendProgramMemory(const SBFProgram &Program,
                         std::vector<MemoryRegion> &Memory);

/// The analyzed instruction at \p Slot, or null when the slot does not begin
/// a complete instruction.
const LowInstruction *findAnalyzedInstruction(const SBFProgram &Program,
                                              size_t Slot);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE interpreter_detail
} // namespace neverd::sbf

#endif // NEVERD_SBF_RUNTIME_SBFINTERPRETERDETAIL_H
