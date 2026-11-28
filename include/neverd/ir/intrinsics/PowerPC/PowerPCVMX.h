//===- PowerPCVMX.h - VMX value types ---------------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_POWERPC_POWERPCVMX_H
#define NEVERD_IR_INTRINSICS_POWERPC_POWERPCVMX_H

#include "neverd/ir/NdTypes.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/PowerPC/PowerPCVMXRegisters.h"

namespace neverd::ppc {
inline unsigned vmxAddLaneBytes(Intrinsic Id) {
  switch (Id) {
#define NEVERD_PPC_VMX(Name, Value, Word, Text, LaneBytes)                     \
  case Intrinsic::Name:                                                        \
    return LaneBytes;
#include "neverd/ir/intrinsics/PowerPC/PowerPCVMX.def"
  default:
    return 0;
  }
}

inline TypeRef vmxVectorType(unsigned LaneBytes) {
  return NdType::makeVector(NdType::makeInt(LaneBytes, false),
                            VectorBytes / LaneBytes, VectorBytes);
}

inline TypeRef byteVectorType() { return vmxVectorType(1); }
} // namespace neverd::ppc

#endif // NEVERD_IR_INTRINSICS_POWERPC_POWERPCVMX_H
