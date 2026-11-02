//===- AArch64DecodeContract.h - Operand detail ownership ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// AArch64 raw identity and operand availability belong to one decoded
/// occurrence. Classification establishes raw bytes without operand authority.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_AARCH64_AARCH64DECODECONTRACT_H
#define NEVERD_DECODE_AARCH64_AARCH64DECODECONTRACT_H

#include "capstone/capstone.h"

#include <cstdint>

namespace neverd {

struct AArch64DecodeOccurrence {
  const cs_insn *Instruction;
  const cs_detail *Operands;
  uint64_t DecoderIdentity;
  uint64_t Generation;
  uint32_t Word;
  bool HasOperandDetail;
};

namespace aarch64_decode {

#define NEVERD_AARCH64_DECODE_DIAGNOSTIC(Name, Text)                           \
  inline constexpr const char *Name = Text;
#include "neverd/decode/AArch64/AArch64DecodeContract.def"

} // namespace aarch64_decode
} // namespace neverd

#endif // NEVERD_DECODE_AARCH64_AARCH64DECODECONTRACT_H
