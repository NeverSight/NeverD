//===- RegistrationFrameStores.h - Private PE32 spill definitions ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Index stores, intervening effects and static allocation lifetimes. The frame
/// proof still checks widths, values and every reaching control-flow path.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_REGISTRATIONFRAMESTORES_H
#define NEVERD_BACKEND_LLVM_REGISTRATIONFRAMESTORES_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace llvm {
class AllocaInst;
class BasicBlock;
class Function;
class Instruction;
class IntrinsicInst;
class StoreInst;
class Value;
} // namespace llvm

namespace neverd::coff_registration {
class RegistrationFrameStores {
public:
  /// Lifetime markers must name a direct static allocation or poison. An
  /// unsupported marker or exhausted shared budget rejects the whole index.
  bool build(llvm::ArrayRef<const llvm::Function *> Functions, size_t &Work);

  bool contains(const llvm::Instruction &I) const {
    return Positions.count(&I);
  }

  /// Return all potentially reaching stores in program order. For an
  /// unaliased allocation, only its direct stores can define it. Otherwise
  /// retain every store in this block prefix for the full alias check.
  /// An empty prefix still requires checking the block's predecessors.
  /// An exhausted shared budget has no result.
  std::optional<llvm::ArrayRef<const llvm::StoreInst *>>
  candidates(const llvm::Value *Root, const llvm::BasicBlock *Block,
             const llvm::Instruction *Through, size_t &Work) const;

  /// Non-STORE memory/lifetime effects after After (exclusive) and through
  /// Through (inclusive). A null After starts at block entry. Only an alloca
  /// whose complete use list consists of direct loads/stores can skip these
  /// effects. The caller must prove preservation across every returned event.
  std::optional<llvm::ArrayRef<const llvm::Instruction *>>
  interveningEffects(const llvm::Value *Root, const llvm::BasicBlock *Block,
                     const llvm::Instruction *After,
                     const llvm::Instruction *Through, size_t &Work) const;

  /// Every write to an unaliased allocation, including later and loop writes.
  /// Unknown ownership or an exhausted budget returns no result.
  std::optional<llvm::ArrayRef<const llvm::StoreInst *>>
  directDefinitions(const llvm::Value *Root, size_t &Work) const;

  struct LifetimeInfo {
    bool HasMarkers = false;
    bool StartsDead = false;
  };
  std::optional<LifetimeInfo> lifetimeInfo(const llvm::AllocaInst &Root,
                                           size_t &Work) const;
  /// Last lifetime marker through this block prefix. A null marker means no
  /// event; an empty optional means the index/budget cannot answer the query.
  std::optional<const llvm::IntrinsicInst *>
  latestLifetime(const llvm::AllocaInst &Root, const llvm::BasicBlock &Block,
                 const llvm::Instruction *Through, size_t &Work) const;

private:
  using Blocks = llvm::DenseMap<const llvm::BasicBlock *,
                                std::vector<const llvm::StoreInst *>>;
  llvm::DenseMap<const llvm::Value *, Blocks> Stores;
  llvm::DenseMap<const llvm::Value *, std::vector<const llvm::StoreInst *>>
      DirectStores;
  Blocks AllStores;
  llvm::DenseMap<const llvm::BasicBlock *,
                 std::vector<const llvm::Instruction *>>
      Effects;
  llvm::DenseMap<const llvm::Instruction *, unsigned> Positions;
  struct LifetimeEvents {
    bool StartsDead = false;
    llvm::DenseMap<const llvm::BasicBlock *,
                   std::vector<const llvm::IntrinsicInst *>>
        Markers;
  };
  llvm::DenseMap<const llvm::AllocaInst *, LifetimeEvents> Lifetimes;
};
} // namespace neverd::coff_registration

#endif // NEVERD_BACKEND_LLVM_REGISTRATIONFRAMESTORES_H
