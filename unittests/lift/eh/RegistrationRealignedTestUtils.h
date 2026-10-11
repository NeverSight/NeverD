//===- RegistrationRealignedTestUtils.h - x86 aligned frame fixture -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_REGISTRATIONREALIGNEDTESTUTILS_H
#define NEVERD_UNITTESTS_REGISTRATIONREALIGNEDTESTUTILS_H

#include "neverd/ir/low/LowIR.h"

#include <initializer_list>

namespace neverd::registration_test {

void emit(LowBlock &Block, va_t Address, NdOp Opcode, NdVar Output,
          std::initializer_list<NdVar> Inputs,
          NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default);
LowFunc makeRealignedRegistrationFrame();

} // namespace neverd::registration_test

#endif
