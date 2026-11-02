//===- PortableX87Validation.h - Portable CFG and SSA admission -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Private validation shared by portable preparation and immutable
/// revalidation.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_MED_X86_PORTABLEX87VALIDATION_H
#define NEVERD_LIB_IR_MED_X86_PORTABLEX87VALIDATION_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/Support/Error.h"

namespace neverd::portable_x87 {

llvm::Error validateControlAndValues(const MedFunc &Function);
bool isIntegerOperation(NdOp Opcode);

} // namespace neverd::portable_x87

#endif // NEVERD_LIB_IR_MED_X86_PORTABLEX87VALIDATION_H
