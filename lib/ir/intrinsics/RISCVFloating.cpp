//===- RISCVFloating.cpp - Scalar RISC-V floating instruction decoding --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/RISCV/RISCVFloating.h"

namespace neverd::riscv {
using V = FPValue;
static constexpr FPForm Forms[] = {
#define RISCV_FP_FORM(Id, Op, Text, Out, A, B, C, RM)                          \
  {FPOperation::Op, Text, V::Out, {V::A, V::B, V::C}, RM},
#include "neverd/ir/intrinsics/RISCV/RISCVFloating.def"
};
static_assert(std::size(Forms) == unsigned(FPFormID::Count));
const FPForm *floatingForm(uint64_t Selector, unsigned XLen) {
  if ((XLen != 4 && XLen != 8) || Selector / 8 >= std::size(Forms))
    return nullptr;
  const auto &Form = Forms[Selector / 8];
  const unsigned RM = Selector & 7;
  if ((Form.Rounded && !FPRoundingNames[RM]) || (!Form.Rounded && RM))
    return nullptr;
  const auto WideInt = [](V T) { return T == V::L || T == V::LU; };
  if (XLen == 4 && (WideInt(Form.Result) || WideInt(Form.Inputs[0])))
    return nullptr;
  return &Form;
}

std::optional<unsigned> decodeFloating(uint32_t W, unsigned XLen,
                                       const RISCVProfile &P) {
  using I = FPFormID;
  const unsigned Opcode = W & 127, Fmt = (W >> 25) & 3;
  const unsigned F3 = (W >> 12) & 7, Rs2 = (W >> 20) & 31;
  if (Fmt > 1 || (Fmt ? !P.DoubleFloat : !P.SingleFloat))
    return std::nullopt;
  I Base;
  if (Opcode == 0x53) {
    switch (W >> 27) {
    case 0:
      Base = I::AddS;
      break;
    case 1:
      Base = I::SubS;
      break;
    case 2:
      Base = I::MulS;
      break;
    case 3:
      Base = I::DivS;
      break;
    case 11:
      if (Rs2)
        return std::nullopt;
      Base = I::SqrtS;
      break;
    case 5:
      if (F3 > 1)
        return std::nullopt;
      Base = F3 ? I::MaxS : I::MinS;
      break;
    case 20:
      if (F3 > 2)
        return std::nullopt;
      Base = F3 == 0 ? I::LeS : F3 == 1 ? I::LtS : I::EqS;
      break;
    case 28:
      if (Rs2 || F3 != 1)
        return std::nullopt;
      Base = I::ClassS;
      break;
    case 24:
    case 26: {
      if (Rs2 > 3)
        return std::nullopt;
      const bool From = W >> 27 == 26;
      const I IDs[] = {From ? I::FromWS : I::ToWS, From ? I::FromWUS : I::ToWUS,
                       From ? I::FromLS : I::ToLS,
                       From ? I::FromLUS : I::ToLUS};
      Base = IDs[Rs2];
      break;
    }
    case 8:
      if (Rs2 != 1 - Fmt || !P.SingleFloat || !P.DoubleFloat)
        return std::nullopt;
      Base = I::ToS;
      break;
    default:
      return std::nullopt;
    }
  } else {
    switch (Opcode) {
    case 0x43:
      Base = I::MAddS;
      break;
    case 0x47:
      Base = I::MSubS;
      break;
    case 0x4b:
      Base = I::NMSubS;
      break;
    case 0x4f:
      Base = I::NMAddS;
      break;
    default:
      return std::nullopt;
    }
  }
  const unsigned ID = unsigned(Base) + Fmt;
  const unsigned Selector = 8 * ID + (Forms[ID].Rounded ? F3 : 0);
  return floatingForm(Selector, XLen) ? std::optional<unsigned>(Selector)
                                      : std::nullopt;
}
const char *FPControl::name() const {
  constexpr const char *Names[] = {"fflags", "frm", "fcsr"};
  return Names[CSR - 1];
}
const char *FPControl::mnemonic() const {
  constexpr const char *Names[] = {"csrr", "csrrw", "csrrs", "csrrc"};
  return Names[Operation];
}
} // namespace neverd::riscv
