//===- MedCallConventionAAPCS64.cpp - AArch64 AAPCS64 call arguments -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AAPCS64 passes integer arguments in X0-X7 and floating-point ones in V0-V7,
// each class counting its own registers, then the stack; X8 carries the
// address of an indirectly returned aggregate.  Apple's variant has its own
// entry (MedCallConventionDarwinAArch64.cpp).  The routines of the Itanium
// C++ ABI's runtime and unwinder take the parameters it declares.
//
//===----------------------------------------------------------------------===//

#include "../MedCallConventionItanium.h"

#include "neverd/ir/med/MedCallConvention.h"

namespace neverd {

extern const CallArgumentConvention AAPCS64CallArguments;
const CallArgumentConvention AAPCS64CallArguments = {
    .TheArch = Arch::AArch64,
    .ImportArgumentsFromPrototype = true,
    .PrototypeArgCount = itaniumRuntimeArgCount,
    .ArgumentsFromCallSetup = true,
    .FormattedCallArguments = true,
};

} // namespace neverd
