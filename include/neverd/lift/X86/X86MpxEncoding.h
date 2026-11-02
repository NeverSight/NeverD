//===- X86MpxEncoding.h - MPX encoding ownership --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode MPX operands from the original bytes. The normalized address size
/// follows MPX, including ignored 67H in long mode, rather than ordinary MOV.
/// This describes an encoding, not enablement, memory permission or completion.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_X86_X86MPXENCODING_H
#define NEVERD_LIFT_X86_X86MPXENCODING_H

#include "neverd/Common.h"
#include "neverd/ir/X86/X86MpxContext.h"

#include "llvm/ADT/ArrayRef.h"

#include <capstone/capstone.h>
#include <cstdint>
#include <optional>

namespace neverd {

struct X86MpxEncoding {
  unsigned Id = X86_INS_INVALID;
  const char *Mnemonic = nullptr;
  uint16_t Size = 0;
  cs_x86 Detail{};
};

/// Read one complete, qualified MPX encoding from a byte stream. This also
/// recognizes the architectural register NOP forms of BNDLDX/STX and BNDMK.
/// Invalid banks, RIP-relative BNDMK/table forms, 16-bit memory addresses and
/// ambiguous mandatory-prefix combinations remain unqualified. REX.W does not
/// select data width; REX.B/X still extend GPRs, and REX.R cannot create BND4+.
/// MIB index/scale/displacement remain encoded operands: table execution must
/// ignore scale, and discard displacement when there is no base register.
std::optional<X86MpxEncoding>
decodeX86MpxEncoding(llvm::ArrayRef<uint8_t> Bytes, Arch Target);

/// Classify the bank effect of an already decoded, complete control transfer.
/// Null means this reader cannot authenticate that transfer. In particular an
/// unknown opcode must not silently acquire a preserve or initialize effect.
std::optional<X86MpxBranch> x86MpxBranchEffect(llvm::ArrayRef<uint8_t> Bytes,
                                               Arch Target);

} // namespace neverd

#endif // NEVERD_LIFT_X86_X86MPXENCODING_H
