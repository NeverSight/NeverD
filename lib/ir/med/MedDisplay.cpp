//===- MedDisplay.cpp - MedVar display helpers -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Human-readable display names for MedIR variables: maps register
/// offsets to architecture-specific register names, formats stack
/// slots, temporaries, parameters, and SSA versions.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd {

std::string MedVar::display() const {
  if (RenameTag >= 0)
    return "v" + std::to_string(RenameTag);

  if (Kind == Const)
    return "0x" + llvm::utohexstr(ConstVal);

  std::string Base;
  switch (Kind) {
  case Reg:
  case Flag: {
    const char *RN = nullptr;
    const auto &TRI = getTargetRegInfo(TheArch);
    if (TRI.GetRegName)
      RN = TRI.GetRegName(RegOff, Size);
    Base = (RN && RN[0] != '?') ? RN : "r0x" + llvm::utohexstr(RegOff);
    break;
  }
  case Stack:
    Base = "var_" + llvm::utohexstr(static_cast<uint64_t>(
                        StackOff < 0 ? -StackOff : StackOff));
    break;
  case Temp:
    Base = "t" + std::to_string(Id);
    break;
  case Param:
    Base = "arg" + std::to_string(Id);
    break;
  case RetVal:
    Base = "retval";
    break;
  default:
    break;
  }

  if (SSAVer > 0)
    return Base + "." + std::to_string(SSAVer);
  return Base;
}

} // namespace neverd
