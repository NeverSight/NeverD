//===- COFFRegistrationFrameStores.h - Private PE32 spill definitions ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Index stores to allocations with no aliases or escaping uses. The frame
/// proof still checks widths, values and every reaching control-flow path.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_COFFREGISTRATIONFRAMESTORES_H
#define NEVERD_COFFREGISTRATIONFRAMESTORES_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace llvm {
class BasicBlock;
class Function;
class Instruction;
class StoreInst;
class Value;
} // namespace llvm

namespace neverd::coff_registration {
class RegistrationFrameStores {
public:
  bool build(llvm::ArrayRef<const llvm::Function *> Functions, size_t &Work);

  /// Return all potentially reaching stores in program order. For an
  /// unaliased allocation, only its direct stores can define it. Otherwise
  /// retain every store in this block prefix for the full alias check.
  /// An empty prefix still requires checking the block's predecessors.
  /// An exhausted shared budget has no result.
  std::optional<llvm::ArrayRef<const llvm::StoreInst *>>
  candidates(const llvm::Value *Root, const llvm::BasicBlock *Block,
             const llvm::Instruction *Through, size_t &Work) const;

private:
  using Blocks = llvm::DenseMap<const llvm::BasicBlock *,
                                std::vector<const llvm::StoreInst *>>;
  llvm::DenseMap<const llvm::Value *, Blocks> Stores;
  Blocks AllStores;
  llvm::DenseMap<const llvm::Instruction *, unsigned> Positions;
};
} // namespace neverd::coff_registration

#endif
