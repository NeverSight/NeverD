//===- RegistrationCookieProof.h - Current PE32 GS protocol ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Certify retained cookie calls before admitting their conditional effects.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_REGISTRATIONCOOKIEPROOF_H
#define NEVERD_BACKEND_LLVM_REGISTRATIONCOOKIEPROOF_H

#include "neverd/backend/llvm/RegistrationImageProof.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>

namespace llvm {
class CallBase;
} // namespace llvm

namespace neverd {
struct BinaryImage;
} // namespace neverd

namespace neverd::registration_frame {

struct CookieCall {
  const llvm::CallBase *Call = nullptr;
  va_t CheckVA = 0;
};

struct CookieFrameGuard {
  Cell Slot;
  Cell XORBase;
  va_t CheckVA = 0;
};

struct CookieDispatch {
  /// The synchronous event whose exceptional dispatch observes these cells.
  /// Its separately authenticated effects must preserve both cells and image.
  const llvm::Instruction *Before = nullptr;
  llvm::ArrayRef<CookieFrameGuard> Guards;
};

struct CookieProofContext {
  const llvm::Function &Parent;
  const llvm::IntrinsicInst *Escape;
  const coff_registration::RegistrationFrameStores &Stores;
  llvm::ArrayRef<const llvm::Function *> Functions;
  va_t SecurityCookieVA;
  AddressProof::LoadProjection ProjectLoad = {};
  AddressProof::EdgeProjection Predecessors = {};
  AddressProof::ObjectBounds Bounds = {};
  AddressProof::MemoryProjection PreservesMemory = {};
  ImageProof::WriteProjection PreservesImage = {};
};

/// Bind the current declaration, sole physical ECX argument and strict call
/// attributes. This does not authenticate its source occurrence or values.
bool hasExactCookieCallABI(const llvm::CallBase &Call, va_t CheckVA);

/// Prove each check using only already proved prior checks, then close every
/// dispatch and the whole image. Fresh query caches are used for each round;
/// unresolved cyclic check dependencies reject. The IR/index/hooks must stay
/// immutable throughout this query. Source occurrence and dispatch coverage,
/// complete frame privacy, callback ownership and mapped image permissions are
/// independent consumer obligations. In particular, hooks must not assume a
/// check succeeds or that the image/guard invariant being proved already holds.
bool proveCookieProtocol(const BinaryImage &Image,
                         const CookieProofContext &Context,
                         llvm::ArrayRef<CookieCall> Calls,
                         llvm::ArrayRef<CookieDispatch> Dispatches,
                         size_t &Work, bool &Exhausted);

} // namespace neverd::registration_frame

#endif // NEVERD_BACKEND_LLVM_REGISTRATIONCOOKIEPROOF_H
