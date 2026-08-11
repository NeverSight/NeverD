//===- EVMIR.cpp - Staged Ethereum Virtual Machine IR -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/EVMIR.h"

#include "llvm/Support/ErrorHandling.h"

namespace neverd::evm {

llvm::StringRef storageKeyKindName(StorageKeyKind Kind) {
  switch (Kind) {
  case StorageKeyKind::Slot:
    return "slot";
  case StorageKeyKind::Hashed:
    return "hashed";
  case StorageKeyKind::HashedOffset:
    return "hashed-offset";
  case StorageKeyKind::Unknown:
    return "unknown";
  }
  llvm_unreachable("invalid storage key kind");
}

llvm::StringRef revertKindName(RevertKind Kind) {
  switch (Kind) {
  case RevertKind::Bare:
    return "bare";
  case RevertKind::Message:
    return "message";
  case RevertKind::Panic:
    return "panic";
  case RevertKind::Custom:
    return "custom";
  }
  llvm_unreachable("invalid revert kind");
}

} // namespace neverd::evm
