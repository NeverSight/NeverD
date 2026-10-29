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

#include <cstddef>
#include <cstdint>

namespace neverd {
struct BinaryImage;
struct LowOp;
namespace registration_state {
class FrameTransfer;
}
namespace registration_abi {

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
