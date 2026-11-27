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
#include <vector>

namespace neverd {
struct BinaryImage;
struct Import;
struct LowBlock;
struct LowOp;
namespace registration_state {
struct FrameState;
enum class KernelStartupCall { Identifier, Clock, SetExceptionFilter };
std::optional<KernelStartupCall> kernelStartupCall(const Import &Imported);
/// Check a private clock output or the installed filter's code pointer.
/// The caller owns the live binding and private outgoing argument word.
bool hasBindingPreservingKernelArgument(const BinaryImage &Image,
                                        const LowBlock &Block, const LowOp &Op,
                                        va_t Slot, const FrameState &Frame,
                                        size_t &Work);
/// Authenticate the physical PE32 call and any immediate import veneer.
/// The caller owns the current slot binding and the provider's effects.
std::optional<va_t> checkedCRTCallSlot(const BinaryImage &Image,
                                       const LowOp &Op, size_t &Work);
/// A checked standard stream index or startup mode keeps import bindings live.
/// The caller supplies the current binding and the private outgoing stack word.
bool hasBindingPreservingCRTArgument(const BinaryImage &Image,
                                     const LowBlock &Block, const LowOp &Op,
                                     va_t Slot, size_t &Work);
/// Ordered possible no-argument callbacks of this original CRT invocation.
/// The caller supplies a current import binding and owns each callee's effects.
std::optional<std::vector<va_t>>
checkedCRTInitializerTargets(const BinaryImage &Image, const LowBlock &Block,
                             const LowOp &Op, va_t Slot, size_t &Work);
bool isCheckedControlFPSlot(const BinaryImage &Image, va_t Slot);
bool isCheckedControlFPCall(const BinaryImage &Image, const LowOp &Op,
                            size_t &Work);
} // namespace registration_state
} // namespace neverd

#endif
