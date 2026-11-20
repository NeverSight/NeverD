//===- MachOPageAddress.h - AArch64 source data addresses -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decode a contiguous ADRP and unshifted 64-bit ADD which materialize a
/// byte address in the source or installed Mach-O image.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOPAGEADDRESS_H
#define NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOPAGEADDRESS_H

#include "neverd/decode/Decoder.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/MathExtras.h"

#include <optional>

namespace neverd {
struct MachOPageAddress {
  va_t Target;
  aarch64_reg PageRegister;
  aarch64_reg ResultRegister;
};

inline std::optional<MachOPageAddress>
decodeMachOPageAddress(Decoder &Decode, llvm::ArrayRef<uint8_t> Bytes,
                       va_t PC) {
  constexpr auto Word = aarch64::kInsnSize;
  DecodedInsn I{};
  if (Bytes.size() < 2 * Word ||
      Decode.decodeOne(Bytes.data(), Word, PC, I) != int(Word) ||
      I.Id != AARCH64_INS_ADRP || !I.Raw || !I.Raw->detail)
    return std::nullopt;
  const auto &Page = I.Raw->detail->aarch64;
  if (Page.op_count != 2 || Page.operands[0].type != AARCH64_OP_REG ||
      Page.operands[1].type != AARCH64_OP_IMM)
    return std::nullopt;
  const auto Register = Page.operands[0].reg;
  const va_t Base = Page.operands[1].imm;
  if (Decode.decodeOne(Bytes.data() + Word, Word, PC + Word, I) != int(Word) ||
      I.Id != AARCH64_INS_ADD || !I.Raw || !I.Raw->detail)
    return std::nullopt;
  const auto &Add = I.Raw->detail->aarch64;
  if (Add.op_count != 3 || Add.operands[0].type != AARCH64_OP_REG ||
      Add.operands[1].type != AARCH64_OP_REG ||
      Add.operands[1].reg != Register ||
      Add.operands[2].type != AARCH64_OP_IMM || Add.operands[2].shift.value ||
      !llvm::isUInt<branch::A64AddImm12.Width>(Add.operands[2].imm))
    return std::nullopt;
  // A W result loses the high address bits. SP has different register-field
  // semantics from XZR, which can be the destination of ADRP.
  const auto RD = Add.operands[0].reg;
  if (!((RD >= AARCH64_REG_X0 && RD <= AARCH64_REG_X28) ||
        RD == AARCH64_REG_FP || RD == AARCH64_REG_LR))
    return std::nullopt;
  return MachOPageAddress{Base + static_cast<va_t>(Add.operands[2].imm),
                          Register, RD};
}
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOPAGEADDRESS_H
