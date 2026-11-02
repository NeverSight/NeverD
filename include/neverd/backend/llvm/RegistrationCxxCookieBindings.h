//===- RegistrationCxxCookieBindings.h - PE32 GS coverage ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind source cookie checks and dispatch occurrences to current LLVM calls.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_REGISTRATIONCXXCOOKIEBINDINGS_H
#define NEVERD_BACKEND_LLVM_REGISTRATIONCXXCOOKIEBINDINGS_H

#include "neverd/backend/llvm/RegistrationCookieProof.h"
#include "neverd/backend/llvm/X86/X86RegistrationCatch.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <map>
#include <vector>

namespace llvm {
class AllocaInst;
class CatchPadInst;
class CleanupPadInst;
} // namespace llvm

namespace neverd {
struct MedFunc;
struct X86RegistrationFrameLayout;
} // namespace neverd

namespace neverd::registration_frame {

/// The producer or independent source-segment replay supplies exact execution
/// ownership and order. Metadata alone does not establish those properties.
struct CxxSourceCallBinding {
  enum class Kind : uint8_t { Ordinary = 2, Cookie = 3 };
  const llvm::CallBase *Call = nullptr;
  va_t Owner = 0;
  int BlockId = -1;
  va_t Address = 0;
  va_t EndAddress = 0;
  int OpSeq = -1;
  Kind CallKind = Kind::Ordinary;
};

/// A generated transfer belongs to an ordered source cleanup relay. It has no
/// invented parent LowOp address or sequence number.
struct CxxCleanupCallBinding {
  const llvm::CallBase *Call = nullptr;
  const llvm::CleanupPadInst *Pad = nullptr;
  va_t Owner = 0;
  uint32_t ActionState = 0;
  va_t RelayTarget = 0;
  uint32_t Ordinal = 0;
};

struct CxxCookieCoverage {
  va_t SecurityCookieVA = 0;
  std::vector<CookieCall> Calls;
  std::map<const llvm::Instruction *, std::vector<CookieFrameGuard>> Dispatches;
};

/// Re-authenticate the handler and cleanup bytes, bind every reached source
/// call exactly once, and close the current external-call set. Every live
/// ordinary/catch call and every generated cleanup call gets the full guard
/// set, including state -1 caller-unwind calls. Source must come from current
/// source replay, with call execution segments and catch identities already
/// checked by the consumer. This does not prove the full CFG, ordinary call
/// ABIs, cookie values, frame privacy, initialized lifetimes or immutable image
/// effects, and grants no native dispatch or check-elision permission.
llvm::Expected<CxxCookieCoverage> bindCxxCookieCoverage(
    const BinaryImage &Image, const MedFunc &Source,
    const llvm::Function &Parent, const llvm::AllocaInst &Frame,
    const X86RegistrationFrameLayout &Layout,
    llvm::ArrayRef<CxxSourceCallBinding> Calls,
    llvm::ArrayRef<CxxCleanupCallBinding> Cleanups,
    const std::map<X86RegistrationCatchIdentity, const llvm::CatchPadInst *>
        &Catches,
    size_t &Work);

} // namespace neverd::registration_frame

#endif // NEVERD_BACKEND_LLVM_REGISTRATIONCXXCOOKIEBINDINGS_H
