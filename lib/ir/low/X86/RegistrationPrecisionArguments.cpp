//===- RegistrationPrecisionArguments.cpp - Private CRT scalar packets ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Prove a valid precision invocation without granting memory preservation.
///
//===----------------------------------------------------------------------===//

#include "RegistrationPrecisionArguments.h"

#include "RegistrationABIPrivate.h"
#include "RegistrationCRTFrame.h"
#include "RegistrationFrame.h"
#include "RegistrationScalarPackets.h"

#include "neverd/ir/RegistrationCRTControl.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

namespace neverd::registration_abi {
using registration_state::FrameValue;

void PrecisionArguments::clear() {
  Words = {};
  Next = 0;
  Active = false;
}

bool PrecisionArguments::observe(
    const LowOp &Op, const registration_state::FrameTransfer &Transfer,
    size_t &Work) {
  if (Op.Opcode != NdOp::STORE) {
    if (Active && !chargeCalleeWork(Work, 1))
      return false;
    // Loads and FP/divide operations can dispatch a continuing handler. Even
    // an opaque read cannot preserve this invocation's argument snapshots.
    if (!isNontrappingFrameScalarOperation(Op))
      clear();
    return true;
  }
  if (!chargeCalleeWork(Work, Words.size() + 1))
    return false;
  const auto SP = Transfer.read(NdVar::reg(x86reg::RSP, 4));
  const auto Address =
      Op.NumInputs == 2 ? Transfer.read(Op.Inputs[0]) : FrameValue{};
  const auto Value =
      Op.NumInputs == 2 ? Transfer.read(Op.Inputs[1]) : FrameValue{};
  if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None || Op.NumInputs != 2 ||
      Op.Inputs[1].Size != 4 || !SP.Offset ||
      SP != FrameValue::frame(*SP.Offset) || !Address.Offset ||
      Address != FrameValue::frame(*Address.Offset) || !Value.Constant ||
      Value != FrameValue::constant(*Value.Constant) ||
      *Address.Offset < *SP.Offset || int64_t(*Address.Offset) + 4 > 0 ||
      int64_t(*Address.Offset) + 4 >
          int64_t(*SP.Offset) + int64_t(Words.size() * 4) ||
      (int64_t(*Address.Offset) - *SP.Offset) % 4) {
    clear();
    return true;
  }
  for (auto &Word : Words)
    if (Word && Word->Offset == *Address.Offset) {
      Word->Value = *Value.Constant;
      return true;
    }
  Words[Next] = Word{*Address.Offset, *Value.Constant};
  Next = (Next + 1) % Words.size();
  Active = true;
  return true;
}

bool PrecisionArguments::provesSuccessfulCall(
    const BinaryImage &Image, const LowOp &Op,
    const registration_state::FrameState &State, size_t &Work) const {
  if (!Active || !State.ImportBindingsIntact ||
      !chargeCalleeWork(Work, Words.size() * Words.size() + 1))
    return false;
  const auto &SP = State.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  if (!SP.Offset || SP != FrameValue::frame(*SP.Offset) || *SP.Offset > -12)
    return false;
  std::array<std::optional<uint32_t>, 3> Arguments;
  for (size_t I = 0; I != Arguments.size(); ++I)
    for (const auto &Word : Words)
      if (Word && int64_t(Word->Offset) == int64_t(*SP.Offset) + int64_t(I * 4))
        Arguments[I] = Word->Value;
  return Arguments[0] == 0 && Arguments[1] && Arguments[2] &&
         registration_crt::isX86ControlFPPrecisionSetting(*Arguments[1],
                                                          *Arguments[2]) &&
         registration_state::isCheckedControlFPCall(Image, Op, Work);
}

} // namespace neverd::registration_abi
