//===- RegistrationCallEffects.h - PE32 effects ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind retained callee effects to a fresh proof of the original image.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_REGISTRATIONCALLEFFECTS_H
#define NEVERD_IR_LOW_REGISTRATIONCALLEFFECTS_H

#include <cstddef>

namespace neverd {
struct RegistrationCalleeFrameContract;
struct RegistrationLeafCalleeABI;
struct RegistrationThrowCalleeABI;

/// Match all borrowed-object, image and executed-code extents, including
/// physical caller-PC writes. A fresh leaf proof also binds target and stack
/// cleanup. The caller still owns result usage, initializer must-writes and
/// the current object's bounds/lifetime. Cleanup contracts discard results.
bool hasSameX86RegistrationLeafEffects(
    const RegistrationCalleeFrameContract &Saved,
    const RegistrationLeafCalleeABI &Current, size_t &Work);

/// Match a private throw/rethrow's effects and exception identity. Direct CRT
/// imports need the caller's complete runtime-object/callback closure and are
/// deliberately excluded from this context-free comparison.
bool hasSameX86RegistrationThrowEffects(
    const RegistrationCalleeFrameContract &Saved,
    const RegistrationThrowCalleeABI &Current, size_t &Work);

/// Match a freshly proved cleanup-only parent, including its transitive
/// exception identities and eager import prerequisites.
bool hasSameX86RegistrationCxxEffects(
    const RegistrationCalleeFrameContract &Saved,
    const RegistrationCalleeFrameContract &Current, size_t &Work);
} // namespace neverd

#endif // NEVERD_IR_LOW_REGISTRATIONCALLEFFECTS_H
