//===- RISCVOpcode.h - RISC-V instruction opcode fields -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Major opcodes of the physical and expanded integer instruction words.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_RISCV_RISCVOPCODE_H
#define NEVERD_LIB_LIFT_RISCV_RISCVOPCODE_H

#include <cstdint>

namespace neverd::riscv::opcode {
enum : uint32_t {
#define NEVERD_RISCV_OPCODE(Name, Value) Name = Value,
#include "RISCVIntegerOpcodes.def"
};
} // namespace neverd::riscv::opcode

#endif // NEVERD_LIB_LIFT_RISCV_RISCVOPCODE_H
