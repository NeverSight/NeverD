//===- X86LegacyPortIO.h - Protected port instruction grammar --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compose legacy segments and register progress with shared port effects.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X86LEGACYPORTIO_H
#define NEVERD_LIB_TRANSLATE_X86_X86LEGACYPORTIO_H

#include "X86LegacyEncoding.h"

#include "neverd/translate/X86/X86LegacyExecution.h"
#include "neverd/translate/X86/X86PortIO.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::translate::detail {

struct X86LegacyPortInstruction {
  unsigned Bytes = 1, AddressBytes = 4;
  bool Input = false, String = false, Repeat = false, Locked = false;
  X86SegmentRegister Segment = X86SegmentRegister::DS;
  std::optional<uint8_t> ImmediatePort;
};

std::optional<X86LegacyPortInstruction>
decodeLegacyPortIO(llvm::ArrayRef<uint8_t> Bytes, bool Code32);

llvm::Expected<X86PortIOResult>
executeLegacyPortIO(X86LegacyExecutionState &Staged, X86PagedMemory &Memory,
                    const X86LegacyPortInstruction &Instruction,
                    uint32_t Fallthrough,
                    const X86LegacyExecutionOptions &Options);

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYPORTIO_H
