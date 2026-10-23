//===- RegistrationPrivateFrame.h - PE32 callee object domains -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared private-frame domains for preserved x86 callees.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONPRIVATEFRAME_H
#define NEVERD_IR_LOW_X86_REGISTRATIONPRIVATEFRAME_H

#include "RegistrationABIPrivate.h"
#include "RegistrationFrame.h"

#include "llvm/ADT/ArrayRef.h"

namespace neverd::registration_abi {

struct PrivateFrameState {
  registration_state::FrameState Frame;
  registration_state::FrameState Borrow;
  registration_state::FrameState BorrowSpills;
  registration_state::FrameState CopySource;
  registration_state::FrameState CopySourceSpills;
  std::set<int32_t> ObjectInitialized;
  std::set<int32_t> Initialized;

  bool operator==(const PrivateFrameState &) const = default;
  bool merge(const PrivateFrameState &Other);
};

struct PrivateInitializerResult {
  int32_t StackOffset;
  registration_state::FrameValue Object;
};

std::optional<PrivateInitializerResult>
applyPrivateInitializerCall(PrivateFrameState &State,
                            const RegistrationInitializerCalleeABI &Callee,
                            size_t &Work, ImageFrameEffects &Effects,
                            RegistrationThrowCalleeABI &Throw);

std::optional<PrivateInitializerResult>
applyBorrowedObjectCall(PrivateFrameState &State,
                        const RegistrationLeafCalleeABI &Callee,
                        llvm::ArrayRef<RegistrationObjectExtent> Initialized,
                        size_t &Work, ImageFrameEffects &Effects);

} // namespace neverd::registration_abi

#endif
