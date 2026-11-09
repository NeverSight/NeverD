//===- MedCallConventionMIPS.cpp - MIPS o32/n64 integer call arguments ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// o32 uses four 32-bit argument registers and aligned pairs for 64-bit
/// integers; n64 uses eight 64-bit carriers. TargetRegInfo owns the distinct
/// stack bases. FP and aggregate contracts require separate admission.

#include "neverd/ir/med/MedCallConvention.h"

namespace neverd {

extern const CallArgumentConvention MIPSO32CallArguments;
extern const CallArgumentConvention MIPSELO32CallArguments;
extern const CallArgumentConvention MIPSN64CallArguments;
extern const CallArgumentConvention MIPSELN64CallArguments;

const CallArgumentConvention MIPSO32CallArguments = {
    .TheArch = Arch::MIPS32,
    .ImportArgumentsFromPrototype = true,
    .PairAlignedWideArguments = true,
    .IntegerParametersUseFullRegisters = true,
    .ArgumentsFromCallSetup = true,
};
const CallArgumentConvention MIPSELO32CallArguments = {
    .TheArch = Arch::MIPS32EL,
    .ImportArgumentsFromPrototype = true,
    .PairAlignedWideArguments = true,
    .IntegerParametersUseFullRegisters = true,
    .ArgumentsFromCallSetup = true,
};
const CallArgumentConvention MIPSN64CallArguments = {
    .TheArch = Arch::MIPS64,
    .ImportArgumentsFromPrototype = true,
    .IntegerParametersUseFullRegisters = true,
    .ArgumentsFromCallSetup = true,
};
const CallArgumentConvention MIPSELN64CallArguments = {
    .TheArch = Arch::MIPS64EL,
    .ImportArgumentsFromPrototype = true,
    .IntegerParametersUseFullRegisters = true,
    .ArgumentsFromCallSetup = true,
};

} // namespace neverd
