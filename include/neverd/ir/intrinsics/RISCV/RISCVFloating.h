//===- RISCVFloating.h - Scalar F/D instruction forms --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_RISCV_RISCVFLOATING_H
#define NEVERD_IR_INTRINSICS_RISCV_RISCVFLOATING_H

#include "neverd/lift/RISCV/RISCVProfile.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd::riscv {
enum class FPValue { N, S, D, W, WU, L, LU, X };
enum class FPOperation {
  Add,
  Sub,
  Mul,
  Div,
  Sqrt,
  MAdd,
  MSub,
  NMSub,
  NMAdd,
  Min,
  Max,
  Eq,
  Lt,
  Le,
  Class,
  ToW,
  ToWU,
  ToL,
  ToLU,
  FromW,
  FromWU,
  FromL,
  FromLU,
  Convert
};
enum class FPFormID : unsigned {
#define RISCV_FP_FORM(Id, ...) Id,
#include "neverd/ir/intrinsics/RISCV/RISCVFloating.def"
  Count
};
struct FPForm {
  FPOperation Operation;
  const char *Mnemonic;
  FPValue Result;
  std::array<FPValue, 3> Inputs;
  bool Rounded;

  unsigned arity() const {
    return Inputs[2] != FPValue::N ? 3 : Inputs[1] != FPValue::N ? 2 : 1;
  }
};
inline bool fpRegister(FPValue V) { return V == FPValue::S || V == FPValue::D; }
inline unsigned fpBytes(FPValue V, unsigned XLen) {
  if (V == FPValue::X)
    return XLen;
  return V == FPValue::D || V == FPValue::L || V == FPValue::LU ? 8 : 4;
}
inline constexpr std::array<const char *, 8> FPRoundingNames{
    "rne", "rtz", "rdn", "rup", "rmm", nullptr, nullptr, "dyn"};
/// Selector is the form ID followed by the three-bit rounding field.
const FPForm *floatingForm(uint64_t Selector, unsigned XLen);
std::optional<unsigned> decodeFloating(uint32_t Word, unsigned XLen,
                                       const RISCVProfile &Profile);

/// CSR 1/2/3 selects fflags/frm/fcsr. Operation 0 is a read, 1 a swap,
/// 2 sets bits and 3 clears bits. Immediate sources are ordinary constants.
struct FPControl {
  unsigned CSR, Operation;
  const char *name() const;
  const char *mnemonic() const;
  unsigned mask() const { return CSR == 1 ? 31 : CSR == 2 ? 7 : 255; }
  unsigned shift() const { return CSR == 2 ? 5 : 0; }
};
inline std::optional<FPControl> fpControl(uint64_t Selector) {
  if (Selector < 4 || Selector > 15)
    return std::nullopt;
  return FPControl{unsigned(Selector >> 2), unsigned(Selector & 3)};
}
} // namespace neverd::riscv
#endif
