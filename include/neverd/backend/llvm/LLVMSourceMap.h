//===- LLVMSourceMap.h - Non-owning LLVM source observations --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_LLVMSOURCEMAP_H
#define NEVERD_BACKEND_LLVM_LLVMSOURCEMAP_H

#include "neverd/sigs/LibraryRecognition.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/ValueHandle.h"

#include <functional>
#include <map>

namespace neverd {

/// A handle to the value an observation was made of.  Like WeakTrackingVH it
/// follows the value's replacement and becomes null when the value is
/// deleted, except for a call: replacing what a call returns leaves the call
/// in place, as when InstCombine reads a returned argument instead of the
/// call's result, so the handle stays with the call and takes the
/// replacement only if the call itself is then deleted.
class LLVMSourceHandle final : public llvm::CallbackVH {
public:
  LLVMSourceHandle() = default;
  LLVMSourceHandle(llvm::Value *V) : CallbackVH(V) {}
  LLVMSourceHandle(const LLVMSourceHandle &) = default;
  LLVMSourceHandle &operator=(const LLVMSourceHandle &) = default;
  ~LLVMSourceHandle() = default;
  LLVMSourceHandle &operator=(llvm::Value *V) {
    setValPtr(V);
    Replacement = nullptr;
    return *this;
  }

private:
  void deleted() override {
    llvm::Value *Next = Replacement;
    Replacement = nullptr;
    setValPtr(Next);
  }
  void allUsesReplacedWith(llvm::Value *New) override {
    if (llvm::isa<llvm::CallBase>(getValPtr()))
      Replacement = New;
    else
      setValPtr(New);
  }

  llvm::WeakTrackingVH Replacement;
};

} // namespace neverd

namespace llvm {
// Casts read the value a handle holds, as they read a WeakTrackingVH's.
template <> struct simplify_type<neverd::LLVMSourceHandle> {
  using SimpleType = Value *;
  static SimpleType getSimplifiedValue(neverd::LLVMSourceHandle &Handle) {
    return Handle;
  }
};
template <> struct simplify_type<const neverd::LLVMSourceHandle> {
  using SimpleType = Value *;
  static SimpleType getSimplifiedValue(const neverd::LLVMSourceHandle &Handle) {
    return Handle;
  }
};
} // namespace llvm

namespace neverd {

struct LLVMSourceObservation {
  va_t Function = 0;
  sigs::LibraryOccurrence Occurrence;
  /// Never retain a naked instruction pointer across optimization or module
  /// replacement.
  LLVMSourceHandle Value;
};

/// Only sidecar state: no metadata or naming changes in the LLVM module.
class LLVMSourceMap {
public:
  std::map<va_t, llvm::WeakTrackingVH> Functions;
  std::vector<LLVMSourceObservation> Observations;

  /// Before optimization, bind pure subexpressions to their observed parent
  /// within the same proven region. Loads, PHIs, calls and control flow are
  /// boundaries. A deleted parent still loses its mapping normally.
  void
  preserveExpressionOrigins(llvm::ArrayRef<sigs::LibraryRecognition> Matches);
  void refreshExpressionOrigins();

  /// Rebind a copied snapshot through an explicit clone map. Unmapped values
  /// become unknown; a caller publishes it only when publishing that clone.
  void remap(const std::function<llvm::Value *(llvm::Value *)> &Lookup) {
    for (auto &[Entry, Function] : Functions) {
      (void)Entry;
      Function = Function ? Lookup(Function) : nullptr;
    }
    for (auto &Observation : Observations)
      Observation.Value =
          Observation.Value ? Lookup(Observation.Value) : nullptr;
  }

private:
  std::map<va_t, std::vector<std::vector<sigs::LibraryOccurrence>>>
      ExpressionRegions;
};

} // namespace neverd

#endif
