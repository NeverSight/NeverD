//===- RegistrationCRTFrame.h - Checked PE32 CRT frame effects --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Declares checked PE32 CRT frame effects.

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONCRTFRAME_H
#define NEVERD_IR_LOW_X86_REGISTRATIONCRTFRAME_H

#include "neverd/Common.h"

#include <optional>

namespace neverd {
struct BinaryImage;
struct LowOp;
namespace registration_state {
/// Authenticate the physical PE32 call and any immediate import veneer.
/// The caller owns the current slot binding and the provider's effects.
std::optional<va_t> checkedCRTCallSlot(const BinaryImage &Image,
                                       const LowOp &Op, size_t &Work);
bool isCheckedControlFPSlot(const BinaryImage &Image, va_t Slot);
bool isCheckedControlFPCall(const BinaryImage &Image, const LowOp &Op,
                            size_t &Work);
} // namespace registration_state
} // namespace neverd

#endif
