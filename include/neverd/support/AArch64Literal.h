//===- AArch64Literal.h - A64 PC-relative literal loads --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_AARCH64LITERAL_H
#define NEVERD_SUPPORT_AARCH64LITERAL_H

#include "neverd/Common.h"
#include "neverd/support/BranchEncoding.h"

namespace neverd::aarch64 {
struct LiteralLoad {
  va_t Address;
  unsigned Width;
};

/// Capstone's memory displacement can truncate an A64 VA to 32 bits.
inline std::optional<LiteralLoad> decodeLiteralLoad(uint32_t Word, va_t PC) {
  const auto Field = branch::A64LoadLiteralOffset;
  const va_t Address =
      PC + llvm::SignExtend64(Field.extract(Word), Field.Width) *
               branch::A64InstructionBytes;
#define A64_LITERAL_MASK(Value) constexpr uint32_t OpcodeMask = Value;
#include "neverd/support/AArch64Literal.def"
  switch (Word & OpcodeMask) {
#define A64_LITERAL_LOAD(Opcode, Bytes)                                        \
  case Opcode:                                                                 \
    return LiteralLoad{Address, Bytes};
#include "neverd/support/AArch64Literal.def"
  default:
    return std::nullopt;
  }
}
} // namespace neverd::aarch64
#endif
