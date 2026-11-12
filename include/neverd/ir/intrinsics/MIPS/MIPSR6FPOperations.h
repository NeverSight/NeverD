//===- MIPSR6FPOperations.h - Release 6 scalar COP1 forms ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_INTRINSICS_MIPS_MIPSR6FPOPERATIONS_H
#define NEVERD_IR_INTRINSICS_MIPS_MIPSR6FPOPERATIONS_H
#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstdint>
#include <optional>

namespace neverd::mips {
struct R6FPOperation {
  uint16_t Key;
  uint8_t Arity, InputBytes, OutputBytes;
  const char *Mnemonic;
  constexpr bool hasFloatingResult() const {
    const unsigned Format = Key >> 6, Function = Key & 63;
    // CVT.S/D also accepts W/L. Comparisons and conversions to W/L
    // write raw integer bits into the FPR and are not floating producers.
    return Function == 32 || Function == 33 ||
           ((Format == 16 || Format == 17) &&
            !(Function >= 8 && Function <= 15) && Function != 36 &&
            Function != 37);
  }
  uint32_t encoding() const {
    return 0x44000000u | (uint32_t(Key >> 6) << 21) | (Key & 63);
  }
};
inline constexpr R6FPOperation R6FPOperations[] = {
#define NEVERD_MIPS_R6_FLOAT(Name, Id, Key, Arity, In, Out, Text)              \
  {Key, Arity, In, Out, Text},
#include "neverd/ir/intrinsics/MIPS/MIPSR6Float.def"
};
static_assert(sizeof(R6FPOperations) / sizeof(R6FPOperations[0]) ==
              unsigned(Intrinsic::MIPSFPCVT_D_L) -
                  unsigned(Intrinsic::MIPSFPADD_S) + 1);
constexpr bool isR6FPControl(Intrinsic Id) {
  return Id == Intrinsic::MIPSReadFPControl ||
         Id == Intrinsic::MIPSWriteFPControl;
}
constexpr bool r6FPControlSelector(unsigned Index, bool Write) {
  return (!Write && Index == 0) || Index == 25 || Index == 26 || Index == 28 ||
         Index == 31;
}
constexpr bool isR6FPArithmetic(Intrinsic Id) {
  return Id >= Intrinsic::MIPSFPADD_S && Id <= Intrinsic::MIPSFPCVT_D_L;
}
constexpr const R6FPOperation &r6FPOperation(Intrinsic Id) {
  return R6FPOperations[unsigned(Id) - unsigned(Intrinsic::MIPSFPADD_S)];
}
constexpr std::optional<Intrinsic> r6FPArithmetic(uint32_t Word) {
  if (Word >> 26 != 17)
    return std::nullopt;
  Intrinsic Id;
  switch (((Word >> 15) & 0x7c0) | (Word & 63)) {
#define NEVERD_MIPS_R6_FLOAT(Name, Value, Key, Arity, In, Out, Text)           \
  case Key:                                                                    \
    Id = Intrinsic::MIPSFP##Name;                                              \
    break;
#include "neverd/ir/intrinsics/MIPS/MIPSR6Float.def"
  default:
    return std::nullopt;
  }
  if (r6FPOperation(Id).Arity == 1 && (Word & 0x1f0000))
    return std::nullopt;
  return Id;
}
} // namespace neverd::mips
#endif // NEVERD_IR_INTRINSICS_MIPS_MIPSR6FPOPERATIONS_H
