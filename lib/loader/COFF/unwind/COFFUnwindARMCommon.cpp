//===- COFFUnwindARMCommon.cpp - Shared ARM unwind helpers ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFUnwindARMDetail.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace neverd::coff_loader::arm_unwind_detail {

bool isMaskableRegister(uint16_t Reg) { return Reg < 32; }

void addRegister(UnwindOperation &Op, uint16_t Reg) {
  if (isMaskableRegister(Reg))
    Op.RegisterMask |= uint32_t(1) << Reg;
}

void setRegister(UnwindOperation &Op, UnwindRegisterClass Class,
                 uint16_t Reg) {
  Op.RegisterClass = Class;
  Op.Register = Reg;
  addRegister(Op, Reg);
}

void setRegisterPair(UnwindOperation &Op, UnwindRegisterClass Class,
                     uint16_t First, uint16_t Second) {
  Op.RegisterClass = Class;
  Op.Register = std::min(First, Second);
  addRegister(Op, First);
  addRegister(Op, Second);
}

void setRegisterRange(UnwindOperation &Op, UnwindRegisterClass Class,
                      uint16_t First, uint16_t Last) {
  Op.RegisterClass = Class;
  Op.Register = First;
  for (uint16_t Reg = First; Reg <= Last; ++Reg)
    addRegister(Op, Reg);
}

std::string atOffset(const char *What, size_t Offset) {
  return std::string(What) + " at unwind-code offset " + std::to_string(Offset);
}

} // namespace neverd::coff_loader::arm_unwind_detail
