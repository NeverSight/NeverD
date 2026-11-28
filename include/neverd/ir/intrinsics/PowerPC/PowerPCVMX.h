//===- PowerPCVMX.h - VMX value types ---------------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_POWERPC_POWERPCVMX_H
#define NEVERD_IR_INTRINSICS_POWERPC_POWERPCVMX_H

#include "neverd/ir/NdTypes.h"
#include "neverd/lift/PowerPC/PowerPCVMXRegisters.h"

namespace neverd::ppc {
inline TypeRef byteVectorType() {
  return NdType::makeVector(NdType::makeInt(1, false), VectorBytes,
                            VectorBytes);
}
} // namespace neverd::ppc

#endif // NEVERD_IR_INTRINSICS_POWERPC_POWERPCVMX_H
