//===- RegistrationCRTControl.h - CRT control-word contracts -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Share the CRT control-word fields and invocation validity rules.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_REGISTRATIONCRTCONTROL_H
#define NEVERD_IR_REGISTRATIONCRTCONTROL_H

#include <cstdint>

namespace neverd::registration_crt {

inline constexpr uint32_t kControlFPExceptionMask = 0x8001f;
inline constexpr uint32_t kX86ControlFPPrecisionMask = 0x30000;

/// A complete PE32 precision selection is valid independently of the old
/// x87 state. The all-bits-set encoding is reserved.
constexpr bool isX86ControlFPPrecisionSetting(uint32_t Control, uint32_t Mask) {
  return Mask == kX86ControlFPPrecisionMask &&
         (Control & kX86ControlFPPrecisionMask) != kX86ControlFPPrecisionMask;
}

} // namespace neverd::registration_crt

#endif // NEVERD_IR_REGISTRATIONCRTCONTROL_H
