//===- MIPSIntegerOpcodes.h - Classic MIPS opcode fields
//-------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Names of the numeric instruction fields shared with compressed expansion.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_MIPS_MIPSINTEGEROPCODES_H
#define NEVERD_LIB_LIFT_MIPS_MIPSINTEGEROPCODES_H

namespace neverd::mips {
namespace major {
enum : unsigned {
#define NEVERD_MIPS_MAJOR_OPCODE(Name, Value) Name = Value,
#include "MIPSIntegerOpcodes.def"
};
} // namespace major

namespace special {
enum : unsigned {
#define NEVERD_MIPS_SPECIAL_OPCODE(Name, Value) Name = Value,
#include "MIPSIntegerOpcodes.def"
};
} // namespace special

namespace special2 {
enum : unsigned {
#define NEVERD_MIPS_SPECIAL2_OPCODE(Name, Value) Name = Value,
#include "MIPSIntegerOpcodes.def"
};
} // namespace special2
} // namespace neverd::mips

#endif // NEVERD_LIB_LIFT_MIPS_MIPSINTEGEROPCODES_H
