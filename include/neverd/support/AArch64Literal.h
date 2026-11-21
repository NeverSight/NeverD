//===- AArch64Literal.h - A64 PC-relative literal loads --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_AARCH64LITERAL_H
#define NEVERD_SUPPORT_AARCH64LITERAL_H

#include "neverd/Common.h"
#include "neverd/support/ISAEncoding.h"

namespace neverd::aarch64 {
#define A64_LITERAL_MASK(Value)                                                \
  inline constexpr uint32_t LiteralOpcodeMask = Value;
#define A64_POINTER_LITERAL(Value)                                             \
  inline constexpr uint32_t PointerLiteralOpcode = Value;
#include "neverd/support/AArch64Literal.def"

inline bool isPointerLiteralLoad(uint32_t Word) {
  return (Word & LiteralOpcodeMask) == PointerLiteralOpcode;
}

/// The signed byte displacement between two addresses in the same image.
template <unsigned Bits>
inline std::optional<int64_t> addressDisplacement(va_t PC, va_t Target) {
  const int64_t Delta = int64_t(Target - PC);
  if ((Target < PC) != (Delta < 0) || !llvm::isInt<Bits>(Delta))
    return std::nullopt;
  return Delta;
}

/// Encode an LDR Xt literal without truncating its signed, scaled displacement.
inline std::optional<uint32_t> encodePointerLiteralLoad(uint32_t Rt, va_t PC,
                                                        va_t Slot) {
  constexpr auto Field = branch::A64LoadLiteralOffset;
  constexpr int64_t Scale = branch::A64InstructionBytes;
  constexpr unsigned Bits = Field.Width + llvm::ConstantLog2<Scale>();
  const auto Delta = addressDisplacement<Bits>(PC, Slot);
  if (Rt > kRdMask || !Delta || *Delta % Scale)
    return std::nullopt;
  return PointerLiteralOpcode | Rt |
         ((uint32_t(*Delta / Scale) << Field.Low) & Field.mask());
}

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
  switch (Word & LiteralOpcodeMask) {
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
