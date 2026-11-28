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

#include <cstdint>
#include <optional>

namespace neverd::ppc {
enum class VMXOperation : uint8_t { Add, Sub };

struct VMXBinaryForm {
  uint8_t LaneBytes;
  VMXOperation Operation;
};

inline std::optional<VMXBinaryForm> vmxBinaryForm(Intrinsic Id) {
  switch (Id) {
#define NEVERD_PPC_VMX(Name, Value, Word, Text, Bytes, Operation)              \
  case Intrinsic::Name:                                                        \
    return VMXBinaryForm{Bytes, VMXOperation::Operation};
#include "neverd/ir/intrinsics/PowerPC/PowerPCVMX.def"
  default:
    return std::nullopt;
  }
}

inline TypeRef vmxVectorType(unsigned LaneBytes) {
  return NdType::makeVector(NdType::makeInt(LaneBytes, false),
                            VectorBytes / LaneBytes, VectorBytes);
}

inline TypeRef byteVectorType() { return vmxVectorType(1); }
} // namespace neverd::ppc

#endif // NEVERD_IR_INTRINSICS_POWERPC_POWERPCVMX_H
