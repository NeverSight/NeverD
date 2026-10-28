//===- RegistrationReadEffects.h - Private read permissions ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Separate ordinary read permissions from loaded values and frame storage.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONREADEFFECTS_H
#define NEVERD_IR_LOW_X86_REGISTRATIONREADEFFECTS_H

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
/// mapped image ranges. No scalar, frame cell or native mapping is produced.
bool hasClosedOrdinaryRegistrationRead(
    const BinaryImage &Image, const LowOp &Op,
    const registration_state::FrameTransfer &Transfer,
    uint32_t CallerArgumentBytes, size_t &Work);

} // namespace registration_abi
} // namespace neverd

#endif // NEVERD_IR_LOW_X86_REGISTRATIONREADEFFECTS_H
