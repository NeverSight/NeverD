//===- MIPSScalarFPOperations.h - Scalar COP1 forms ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
#ifndef NEVERD_IR_INTRINSICS_MIPS_MIPSSCALARFPOPERATIONS_H
#define NEVERD_IR_INTRINSICS_MIPS_MIPSSCALARFPOPERATIONS_H
#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstdint>
#include <optional>

namespace neverd::mips {
struct ScalarFPOperation {
  uint16_t Key;
  uint8_t Arity, InputBytes, OutputBytes;
  const char *Mnemonic;
  constexpr bool writesCondition() const { return OutputBytes == 0; }
  constexpr bool hasFloatingResult() const {
    if (writesCondition())
      return false;
    if (Key & 0x8000)
      return true;
    const unsigned Format = Key >> 6, Function = Key & 63;
    // CVT.S/D also accepts W/L. Comparisons and conversions to W/L
    // write raw integer bits into the FPR and are not floating producers.
    return Function == 32 || Function == 33 ||
           ((Format == 16 || Format == 17) &&
            !(Function >= 8 && Function <= 15) && Function != 36 &&
            Function != 37);
  }
  uint32_t encoding() const {
    return Key & 0x8000 ? 0x4c000000u | (Key & 63)
                        : 0x44000000u | (uint32_t(Key >> 6) << 21) | (Key & 63);
  }
};
inline constexpr ScalarFPOperation ScalarFPOperations[] = {
#define NEVERD_MIPS_R6_FLOAT(Name, Id, Key, Arity, In, Out, Text)              \
  {Key, Arity, In, Out, Text},
#include "neverd/ir/intrinsics/MIPS/MIPSR6Float.def"
    {},
    {}, // Stable IDs reserved for CFC1/CTC1.
#define NEVERD_MIPS_R6_FLOAT(Name, Id, Key, Arity, In, Out, Text)              \
  {Key, Arity, In, Out, Text},
#include "neverd/ir/intrinsics/MIPS/MIPSLegacyFloat.def"
};
static_assert(sizeof(ScalarFPOperations) / sizeof(ScalarFPOperations[0]) ==
              unsigned(Intrinsic::MIPSFPNMSUB_D) -
                  unsigned(Intrinsic::MIPSFPADD_S) + 1);
constexpr bool isScalarFPControl(Intrinsic Id) {
  return Id == Intrinsic::MIPSReadFPControl ||
         Id == Intrinsic::MIPSWriteFPControl;
}
constexpr bool scalarFPControlSelector(unsigned Index, bool Write) {
  return (!Write && Index == 0) || Index == 25 || Index == 26 || Index == 28 ||
         Index == 31;
}
constexpr bool isScalarFPArithmetic(Intrinsic Id) {
  return (Id >= Intrinsic::MIPSFPADD_S && Id <= Intrinsic::MIPSFPCVT_D_L) ||
         (Id >= Intrinsic::MIPSFPABS_S && Id <= Intrinsic::MIPSFPNMSUB_D);
}
constexpr const ScalarFPOperation &scalarFPOperation(Intrinsic Id) {
  return ScalarFPOperations[unsigned(Id) - unsigned(Intrinsic::MIPSFPADD_S)];
}
constexpr bool scalarFPHasSelector(Intrinsic Id) {
  return isScalarFPControl(Id) ||
         (isScalarFPArithmetic(Id) && scalarFPOperation(Id).writesCondition());
}
constexpr bool scalarFPAvailable(Intrinsic Id, unsigned Revision) {
  if (isScalarFPControl(Id))
    return true;
  if (!isScalarFPArithmetic(Id))
    return false;
  if (Revision == 6)
    return Id < Intrinsic::MIPSFPABS_S;
  const auto &F = scalarFPOperation(Id);
  if (F.Key & 0x8000)
    return true;
  const unsigned Format = F.Key >> 6, Function = F.Key & 63;
  return ((Format == 16 || Format == 17) &&
          (Function <= 4 || Function == 5 || Function == 7 ||
           (Function >= 8 && Function <= 15) || Function == 21 ||
           Function == 22 || Function == 32 || Function == 33 ||
           Function == 36 || Function == 37 || Function >= 48)) ||
         ((Format == 20 || Format == 21) && (Function == 32 || Function == 33));
}
constexpr std::optional<Intrinsic> scalarFPArithmetic(uint32_t Word,
                                                      unsigned Revision = 6) {
  if (Word >> 26 != 17 && Word >> 26 != 19)
    return std::nullopt;
  Intrinsic Id;
  switch ((Word >> 26 == 19 ? 0x8000 : (Word >> 15) & 0x7c0) | (Word & 63)) {
#define NEVERD_MIPS_R6_FLOAT(Name, Value, Key, Arity, In, Out, Text)           \
  case Key:                                                                    \
    Id = Intrinsic::MIPSFP##Name;                                              \
    break;
#include "neverd/ir/intrinsics/MIPS/MIPSR6Float.def"
#define NEVERD_MIPS_R6_FLOAT(Name, Value, Key, Arity, In, Out, Text)           \
  case Key:                                                                    \
    Id = Intrinsic::MIPSFP##Name;                                              \
    break;
#include "neverd/ir/intrinsics/MIPS/MIPSLegacyFloat.def"
  default:
    return std::nullopt;
  }
  if (!scalarFPAvailable(Id, Revision))
    return std::nullopt;
  if (scalarFPOperation(Id).writesCondition() && (Word & 0xc0))
    return std::nullopt;
  if (scalarFPOperation(Id).Arity == 1 && (Word & 0x1f0000))
    return std::nullopt;
  return Id;
}
} // namespace neverd::mips
#endif // NEVERD_IR_INTRINSICS_MIPS_MIPSSCALARFPOPERATIONS_H
