//===- X86MpxExecution.h - Decoded MPX execution ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute one byte-authenticated MPX instruction with an explicit bounds bank,
/// operand snapshot and memory provider. Original branch completion and full
/// LowIR/MedIR/native state transport remain separate integration boundaries.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_X86_X86MPXEXECUTION_H
#define NEVERD_LIFT_X86_X86MPXEXECUTION_H

#include "neverd/Common.h"
#include "neverd/ir/X86/X86MpxMemory.h"

#include "llvm/ADT/ArrayRef.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd {

struct X86MpxExecutionInputs {
  /// Architectural encoding order: AX, CX, DX, BX, SP, BP, SI, DI, R8..R15.
  /// Unknown values are never treated as zero. i386 consumes only low words.
  std::array<std::optional<uint64_t>, 16> GPR{};
  /// Indexed by X86SegmentRegister. Needed only for table-pointer translation:
  /// all six segments in i386, FS/GS in long mode. BNDMOV delegates its
  /// complete segmented access to memory; checks and BNDMK never read a segment
  /// base.
  std::array<std::optional<uint64_t>, 6> SegmentBases{};
};

struct X86MpxExecutionResult {
  X86MpxResult Result = X86MpxResult::InvalidInput;
  uint16_t Size = 0;
};

/// Raw-byte admission uses decodeX86MpxEncoding's sole encoding authority.
/// GPRs and flags are not modified. The bounds bank and memory change according
/// to the supplied provider's completion; malformed encodings have no effects.
/// This executes a single instruction, not a function or an RTM/HLE region.
X86MpxExecutionResult
executeX86MpxInstruction(llvm::ArrayRef<uint8_t> Bytes, Arch Target,
                         va_t Address, X86MpxContext &Context,
                         const X86MpxExecutionInputs &Inputs,
                         X86MpxMemoryProvider &Memory);

} // namespace neverd

#endif // NEVERD_LIFT_X86_X86MPXEXECUTION_H
