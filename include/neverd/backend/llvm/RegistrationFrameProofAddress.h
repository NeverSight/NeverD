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
#include <utility>

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
  /// True only if this separately authenticated current event preserves the
  /// complete queried interval. Unknown and partial writes must return false.
  /// This hook does not replace the consumer's complete alias/use closure.
  using MemoryProjection = llvm::function_ref<bool(const llvm::Instruction &,
                                                   const Cell &, uint64_t)>;

  /// Hooks project only separately authenticated runtime roots, resumption
  /// edges and effects. They must outlive this query; the queried IR must stay
  /// unchanged. Empty hooks select the ordinary CFG and reject unknown effects.
  AddressProof(const llvm::Function &Parent, const llvm::IntrinsicInst *Escape,
               va_t SecurityCookieVA,
               const coff_registration::RegistrationFrameStores &Stores,
               size_t &Work, bool &Exhausted, LoadProjection ProjectLoad = {},
               EdgeProjection Predecessors = {}, ObjectBounds Bounds = {});
  AddressProof(const llvm::Function &Parent, const llvm::IntrinsicInst *Escape,
               va_t SecurityCookieVA,
               const coff_registration::RegistrationFrameStores &Stores,
               size_t &Work, bool &Exhausted, LoadProjection ProjectLoad,
               EdgeProjection Predecessors, ObjectBounds Bounds,
               MemoryProjection PreservesMemory);

  std::optional<Cell> locate(const llvm::Value *Value, unsigned Depth = 0);
  bool hasPE32Layout() const;
  /// Whether the immutable store index covers this instruction in our module.
  bool isIndexed(const llvm::Instruction &I) const;

  /// Whether this static allocation is alive immediately before the access
  /// on every predecessor. Pointer arithmetic itself does not require this.
  /// Cross-function uses of an unmarked parent allocation still require the
  /// consumer's authenticated callback invocation; marked parents need a
  /// call-site lifetime projection and are not guessed alive in callbacks.
  bool isAliveAt(const llvm::AllocaInst &Root, const llvm::Instruction &Before);

  /// Visit every potentially reaching full-width store before this exact load.
  /// Partial/atomic stores, unwritten paths, unproved intervening effects or
  /// exhaustion fail. The consumer must still close aliases and the complete
  /// frame lifetime; a reaching-store candidate alone is not that proof.
  bool
  visitReachingStores(const llvm::LoadInst &Load,
                      llvm::function_ref<bool(const llvm::StoreInst &)> Visit,
                      unsigned Depth = 0);

  bool
  visitReachingStoresAt(const Cell &Slot, uint64_t Bytes,
                        const llvm::Instruction &Before,
                        llvm::function_ref<bool(const llvm::StoreInst &)> Visit,
                        unsigned Depth = 0);

  /// Check one intervening effect using intrinsic-owned memory semantics and
  /// the consumer's current runtime contract. This does not close aliases or
  /// validate the rest of a callback's writes.
  bool preservesMemory(const llvm::Instruction &I, const Cell &Slot,
                       uint64_t Bytes, unsigned Depth = 0);

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
  MemoryProjection PreservesMemory;
  std::map<const llvm::Value *, std::optional<Cell>> Located;
  std::map<std::pair<const llvm::AllocaInst *, const llvm::Instruction *>, bool>
      Alive;
};

} // namespace neverd::registration_frame
#endif // NEVERD_BACKEND_LLVM_REGISTRATIONFRAMEPROOFADDRESS_H
