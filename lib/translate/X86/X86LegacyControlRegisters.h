//===- X86LegacyControlRegisters.h - Protected MOV-CR ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original control-register grammar and staged paging transitions.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X86LEGACYCONTROLREGISTERS_H
#define NEVERD_LIB_TRANSLATE_X86_X86LEGACYCONTROLREGISTERS_H

#include "X86LegacyEncoding.h"

#include "neverd/translate/X86/X86LegacyExecution.h"

#include <cstdint>
#include <optional>

namespace neverd::translate::detail {

struct X86LegacyControlInstruction {
  uint8_t Control, General;
  bool Write, Locked;
};

inline std::optional<X86LegacyControlInstruction>
decodeLegacyControlRegister(llvm::ArrayRef<uint8_t> Bytes, bool Code32) {
  X86LegacyEncoding R(Bytes, Code32);
  if (R.byte() != 0x0f)
    return std::nullopt;
  const auto Opcode = R.byte();
  if (Opcode != 0x20 && Opcode != 0x22)
    return std::nullopt;
  // MOV-CR ignores mod: even r/m=4 or 5 consumes no SIB/displacement.
  const auto ModRM = R.byte();
  if (!R.done())
    return std::nullopt;
  return X86LegacyControlInstruction{uint8_t((ModRM >> 3) & 7),
                                     uint8_t(ModRM & 7), Opcode == 0x22,
                                     R.Group1Prefix == 0xf0};
}

llvm::Error validateLegacyControlContinuation(
    const X86LegacyExecutionState &State, const X86PagedMemory &Memory,
    const X86LegacyExecutionOptions &Options,
    std::optional<llvm::ArrayRef<uint8_t>> Instruction = std::nullopt);
uint64_t legacyControlInstructionKey(const X86LegacyExecutionState &State);

llvm::Error
validateLegacyControlBinding(const X86PagedMemory &Memory,
                             const X86LegacyControlRegisterBinding &Binding);

/// The caller commits these staged banks only on success, before post-step
/// events. Physical PDPTE reads never produce linear data breakpoints.
llvm::Expected<X86MemoryAccessResult> executeLegacyControlRegister(
    X86LegacyExecutionState &Next, X86PagingState &Controls,
    X86PagedMemory &Memory, const X86LegacyControlInstruction &Instruction,
    llvm::ArrayRef<uint8_t> Bytes, const X86LegacyExecutionOptions &Options);

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYCONTROLREGISTERS_H
