//===- IntrinsicInstructionText.cpp - The instruction behind one ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/IntrinsicInstructionText.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd {

namespace {

std::string hexImmediate(uint64_t Value) {
  return "0x" + llvm::utohexstr(Value);
}

} // namespace

bool intrinsicCarriesInstructionOperand(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::Brk:
  case Intrinsic::Hlt_A64:
  case Intrinsic::Udf_A64:
  case Intrinsic::Hvc:
  case Intrinsic::Smc:
  case Intrinsic::Dmb:
  case Intrinsic::Dsb:
  case Intrinsic::Isb:
  case Intrinsic::ArmBkpt:
  case Intrinsic::ArmHlt:
  case Intrinsic::ArmUdf:
  case Intrinsic::ArmHvc:
  case Intrinsic::ArmSmc:
  case Intrinsic::ArmDmb:
  case Intrinsic::ArmDsb:
  case Intrinsic::ArmIsb:
  case Intrinsic::Ud0:
  case Intrinsic::Ud1:
    return true;
  default:
    return false;
  }
}

std::string armBarrierOptionText(uint64_t CRm) {
  switch (CRm) {
#define ARM_BARRIER_OPTION(CRM, NAME)                                          \
  case CRM:                                                                    \
    return NAME;
#include "neverd/ir/intrinsics/ARMBarrierOptions.def"
  default:
    return "#" + hexImmediate(CRm);
  }
}

std::optional<uint64_t> armBarrierOptionValue(llvm::StringRef Text) {
  Text = Text.trim();
#define ARM_BARRIER_OPTION(CRM, NAME)                                          \
  if (Text.equals_insensitive(NAME))                                           \
    return CRM;
#include "neverd/ir/intrinsics/ARMBarrierOptions.def"
  uint64_t Value = 0;
  if (Text.consume_front("#") && !Text.getAsInteger(0, Value) && Value <= 0xF)
    return Value;
  return std::nullopt;
}

std::string intrinsicInstructionText(Intrinsic Id,
                                     std::optional<uint64_t> Operand) {
  if (!Operand || !intrinsicCarriesInstructionOperand(Id))
    return {};
  // A barrier's C name (`__dmb`) is not its mnemonic.
  switch (Id) {
  case Intrinsic::Dmb:
  case Intrinsic::ArmDmb:
    return "dmb " + armBarrierOptionText(*Operand);
  case Intrinsic::Dsb:
  case Intrinsic::ArmDsb:
    return "dsb " + armBarrierOptionText(*Operand);
  case Intrinsic::Isb:
  case Intrinsic::ArmIsb:
    // SY is the only ISB option; the assembler spells it without one.
    return *Operand == 0xF ? std::string("isb")
                           : "isb #" + hexImmediate(*Operand);
  default:
    break;
  }
  const char *Mnemonic = intrinsicAsmMnemonic(Id);
  if (!Mnemonic)
    return {};
  switch (Id) {
  case Intrinsic::Ud0:
  case Intrinsic::Ud1:
    // The lifters keep the displacement of the form clang's trapping
    // sanitizers emit, `ud1 eax, [eax + kind]`, whose kind names the check
    // that failed.
    return std::string(Mnemonic) + " eax, dword ptr [eax + " +
           hexImmediate(*Operand) + "]";
  default:
    return std::string(Mnemonic) + " #" + hexImmediate(*Operand);
  }
}

} // namespace neverd
