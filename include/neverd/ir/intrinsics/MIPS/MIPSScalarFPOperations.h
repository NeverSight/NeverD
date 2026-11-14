//===- MIPSScalarFPOperations.h - Scalar COP1 forms ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
#ifndef NEVERD_IR_INTRINSICS_MIPS_MIPSSCALARFPOPERATIONS_H
#define NEVERD_IR_INTRINSICS_MIPS_MIPSSCALARFPOPERATIONS_H
#include "neverd/ir/intrinsics/Intrinsics.h"

#include <array>
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
// COP1 formats 16..21 occupy six 64-function rows; COP1X uses one row.
// Keep dispatch under 1 KiB and derive every entry from the semantic catalog.
inline constexpr auto ScalarFPDecode = [] {
  std::array<uint16_t, 7 * 64> Result{};
#define NEVERD_MIPS_R6_FLOAT(Name, Value, Key, Arity, In, Out, Text)           \
  Result[(Key & 0x8000) ? 6 * 64 + (Key & 63) : Key - 0x400] = Value;
#include "neverd/ir/intrinsics/MIPS/MIPSR6Float.def"
#define NEVERD_MIPS_R6_FLOAT(Name, Value, Key, Arity, In, Out, Text)           \
  Result[(Key & 0x8000) ? 6 * 64 + (Key & 63) : Key - 0x400] = Value;
#include "neverd/ir/intrinsics/MIPS/MIPSLegacyFloat.def"
  return Result;
}();
constexpr std::optional<Intrinsic> scalarFPArithmetic(uint32_t Word,
                                                      unsigned Revision = 6) {
  const unsigned Opcode = Word >> 26, Format = (Word >> 21) & 31;
  if ((Opcode != 17 && Opcode != 19) ||
      (Opcode == 17 && (Format < 16 || Format > 21)))
    return std::nullopt;
  const unsigned Row = Opcode == 19 ? 6 : Format - 16;
  const auto Value = ScalarFPDecode[Row * 64 + (Word & 63)];
  if (!Value)
    return std::nullopt;
  const auto Id = static_cast<Intrinsic>(Value);
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
