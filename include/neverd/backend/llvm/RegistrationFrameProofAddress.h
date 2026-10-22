//===- RegistrationFrameProofAddress.h - PE32 frames ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared address and reaching-store candidates for registration-frame proofs.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_REGISTRATIONFRAMEPROOFADDRESS_H
#define NEVERD_BACKEND_LLVM_REGISTRATIONFRAMEPROOFADDRESS_H

#include "neverd/Common.h"
#include "neverd/backend/llvm/RegistrationFrameStores.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <functional>
#include <map>
#include <optional>

namespace llvm {
class DataLayout;
class IntrinsicInst;
class LoadInst;
} // namespace llvm

namespace neverd::registration_frame {

/// Candidate storage identity. Its use closure must independently prove bounds,
/// initialization, invocation lifetime, alias exclusion and callee effects.
struct Cell {
  const llvm::Value *Root;
  int64_t Offset;
  bool operator==(const Cell &) const = default;
  bool operator<(const Cell &Other) const {
    return Root != Other.Root
               ? std::less<const llvm::Value *>{}(Root, Other.Root)
               : Offset < Other.Offset;
  }
};

class AddressProof {
public:
  using Lookup = llvm::function_ref<std::optional<Cell>(const llvm::Value *)>;
  using LoadProjection =
      llvm::function_ref<std::optional<Cell>(const llvm::LoadInst &, Lookup)>;
  using EdgeProjection = llvm::function_ref<bool(
      const llvm::BasicBlock *, const llvm::BasicBlock *,
      llvm::SmallVectorImpl<const llvm::BasicBlock *> &)>;
  using ObjectBounds = llvm::function_ref<bool(const Cell &)>;

  /// Hooks project only separately authenticated runtime roots and resumption
  /// edges. They must outlive this query; empty hooks select ordinary LLVM CFG.
  AddressProof(const llvm::Function &Parent, const llvm::IntrinsicInst *Escape,
               va_t SecurityCookieVA,
               const coff_registration::RegistrationFrameStores &Stores,
               size_t &Work, bool &Exhausted, LoadProjection ProjectLoad = {},
               EdgeProjection Predecessors = {}, ObjectBounds Bounds = {});

  std::optional<Cell> locate(const llvm::Value *Value, unsigned Depth = 0);
  bool hasPE32Layout() const;

  /// Visit every potentially reaching full-width store before this exact load.
  /// Partial/atomic stores, unwritten paths or exhaustion fail. This is a value
  /// candidate only: the consumer must separately close aliases and call
  /// writes.
  bool
  visitReachingStores(const llvm::LoadInst &Load,
                      llvm::function_ref<bool(const llvm::StoreInst &)> Visit,
                      unsigned Depth = 0);

  bool
  visitReachingStoresAt(const Cell &Slot, uint64_t Bytes,
                        const llvm::Instruction &Before,
                        llvm::function_ref<bool(const llvm::StoreInst &)> Visit,
                        unsigned Depth = 0);

private:
  std::optional<Cell> locateImpl(const llvm::Value *Value, unsigned Depth);
  bool withinObject(const Cell &Where) const;
  const llvm::Function &Parent;
  const llvm::DataLayout &Layout;
  const llvm::IntrinsicInst *Escape;
  va_t SecurityCookieVA;
  const coff_registration::RegistrationFrameStores &Stores;
  size_t &Work;
  bool &Exhausted;
  LoadProjection ProjectLoad;
  EdgeProjection Predecessors;
  ObjectBounds Bounds;
  std::map<const llvm::Value *, std::optional<Cell>> Located;
};

} // namespace neverd::registration_frame
#endif // NEVERD_BACKEND_LLVM_REGISTRATIONFRAMEPROOFADDRESS_H
