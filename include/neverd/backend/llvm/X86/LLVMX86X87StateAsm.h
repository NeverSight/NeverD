//===- LLVMX86X87StateAsm.h - x87 state inline asm -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Inline-asm contracts of the x87 instructions that change register tags,
/// TOP or status when a native LLVM intrinsic is unavailable. The LLVM
/// emitter writes them and the LLVM-to-C writer recognizes the same table.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_LLVMX86X87STATEASM_H
#define NEVERD_BACKEND_LLVM_X86_LLVMX86X87STATEASM_H

#include "neverd/ir/X86/X87Stack.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>

namespace neverd {

enum class X87StateEffect : uint8_t {
#define X86_X87_STATE_ASM(Effect, Constraints, CClobbers) Effect,
#include "neverd/backend/llvm/X86/LLVMX86X87StateAsm.def"
};

/// LLVM constraint string of an x87 state instruction with \p Effect.
inline llvm::StringRef x87StateConstraints(X87StateEffect Effect) {
  switch (Effect) {
#define X86_X87_STATE_ASM(Name, Constraints, CClobbers)                        \
  case X87StateEffect::Name:                                                   \
    return Constraints;
#include "neverd/backend/llvm/X86/LLVMX86X87StateAsm.def"
  }
  return {};
}

/// GNU C clobber list with the same contract as x87StateConstraints.
inline llvm::StringRef x87StateCClobbers(X87StateEffect Effect) {
  switch (Effect) {
#define X86_X87_STATE_ASM(Name, Constraints, CClobbers)                        \
  case X87StateEffect::Name:                                                   \
    return CClobbers;
#include "neverd/backend/llvm/X86/LLVMX86X87StateAsm.def"
  }
  return {};
}

/// The effect of the x87 state instruction the intrinsic \p Id runs, if it
/// is one: FNINIT resets the unit, free/rotate instructions change the register
/// stack, and FWAIT and FNCLEX touch only the status.
inline std::optional<X87StateEffect> x87StateEffectOfIntrinsic(Intrinsic Id) {
  if (x87StackEffect(Id))
    return X87StateEffect::Stack;
  switch (Id) {
  case Intrinsic::X87Fninit:
    return X87StateEffect::Reset;
  case Intrinsic::X87Wait:
  case Intrinsic::X87Fnclex:
    return X87StateEffect::Status;
  default:
    return std::nullopt;
  }
}

/// The effect whose LLVM constraint string is \p Constraints.
inline std::optional<X87StateEffect>
x87StateEffectOf(llvm::StringRef Constraints) {
#define X86_X87_STATE_ASM(Name, ConstraintString, CClobbers)                   \
  if (Constraints == ConstraintString)                                         \
    return X87StateEffect::Name;
#include "neverd/backend/llvm/X86/LLVMX86X87StateAsm.def"
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_LLVMX86X87STATEASM_H
