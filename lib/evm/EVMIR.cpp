//===- EVMIR.cpp - Staged Ethereum Virtual Machine IR -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/EVMIR.h"

#include <array>

namespace neverd::evm {

llvm::ArrayRef<StorageKeyKindInfo> storageKeyKindInfos() {
  static const std::array Table = {
#define EVM_STORAGE_KEY_KIND(ID, NAME, SUMMARY)                                \
  StorageKeyKindInfo{StorageKeyKind::ID, NAME, SUMMARY},
#include "neverd/evm/EVMRecoveredFacts.def"
  };
  return Table;
}

llvm::StringRef storageKeyKindName(StorageKeyKind Kind) {
  return storageKeyKindInfos()[static_cast<size_t>(Kind)].Name;
}

llvm::ArrayRef<RevertKindInfo> revertKindInfos() {
  static const std::array Table = {
#define EVM_REVERT_KIND(ID, NAME, SUMMARY)                                     \
  RevertKindInfo{RevertKind::ID, NAME, SUMMARY},
#include "neverd/evm/EVMRecoveredFacts.def"
  };
  return Table;
}

llvm::StringRef revertKindName(RevertKind Kind) {
  return revertKindInfos()[static_cast<size_t>(Kind)].Name;
}

} // namespace neverd::evm
