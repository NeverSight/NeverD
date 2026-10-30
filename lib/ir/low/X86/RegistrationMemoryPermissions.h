//===- RegistrationMemoryPermissions.h - Private accesses -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Separate ordinary access permissions from values and frame storage.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONMEMORYPERMISSIONS_H
#define NEVERD_IR_LOW_X86_REGISTRATIONMEMORYPERMISSIONS_H

#include "neverd/ir/low/LowIR.h"

#include <cstddef>
#include <cstdint>

namespace neverd {
struct BinaryImage;
struct LowOp;
namespace registration_state {
class FrameTransfer;
}
namespace registration_abi {

/// Scalar effects that cannot themselves dispatch a fault or call user code.
/// Memory and calls require their own current-invocation proofs.
inline bool hasClosedRegistrationIntegerEffect(const LowOp &Op) {
  if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None)
    return false;
  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
  case NdOp::INT_MULT:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::BOOL_NOT:
  case NdOp::CONCAT:
  case NdOp::SUBBYTES:
  case NdOp::BRANCH:
  case NdOp::COND_BR:
  case NdOp::RETURN:
  case NdOp::INT_NEG2:
  case NdOp::POPCOUNT:
  case NdOp::LZCOUNT:
  case NdOp::SELECT:
  case NdOp::NOP:
    return true;
  default:
    return false;
  }
}

/// Only the ordinary entry frame, known Win32 thread fields and complete
/// mapped image ranges. Writes also require writable storage. No scalar,
/// frame cell, IAT disjointness or native mapping is produced.
bool hasClosedOrdinaryRegistrationMemoryAccess(
    const BinaryImage &Image, const LowOp &Op,
    const registration_state::FrameTransfer &Transfer,
    uint32_t CallerArgumentBytes, size_t &Work);

/// An ordinary returning helper may retain every existing import binding
/// only through closed integer effects. Calls compose their own receipts;
/// faults, FP/intrinsics and writes outside its current private frame do not
/// inherit the weaker integer-stack preservation policy.
bool preservesOrdinaryRegistrationBindings(
    const BinaryImage &Image, const LowOp &Op,
    const registration_state::FrameTransfer &Transfer,
    uint32_t CallerArgumentBytes, size_t &Work);

} // namespace registration_abi
} // namespace neverd

#endif // NEVERD_IR_LOW_X86_REGISTRATIONMEMORYPERMISSIONS_H
