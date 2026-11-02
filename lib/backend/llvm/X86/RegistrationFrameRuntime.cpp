//===- RegistrationFrameRuntime.cpp - Native PE32 runtime domains ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check runtime exception fields and source jump-buffer ranges.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/RegistrationFrameRuntime.h"

#include "llvm/IR/Instructions.h"

namespace neverd::coff_registration {

const llvm::Value *RegistrationSEHExceptionContract::pointerResult(
    const llvm::LoadInst &Load) const {
  const auto Bridge = Bridges.find(Load.getFunction());
  if (Bridge != Bridges.end() && Bridge->second->getValueOperand() == &Load)
    return Bridge->second;
  const auto Found = Reads.find(&Load);
  if (Found == Reads.end())
    return nullptr;
  const auto &Access = Found->second;
  using Kind = RegistrationSEHExceptionAccess::Kind;
  if (Access.Field == Kind::PointersSlot)
    return Access.Bridge;
  if (Access.Field == Kind::RecordPointer)
    return Access.Bridge->getValueOperand();
  return nullptr;
}

bool RegistrationSEHExceptionContract::validRead(const llvm::LoadInst &Load,
                                                 const llvm::Value *Root,
                                                 int64_t Offset) const {
  const auto Found = Reads.find(&Load);
  if (Found == Reads.end() || !Load.getType()->isIntegerTy(32) ||
      Load.isAtomic() || !Load.isVolatile() ||
      Load.getAlign() != llvm::Align(1))
    return false;
  const auto &Access = Found->second;
  if (Load.getFunction() != Access.Bridge->getFunction())
    return false;
  using Kind = RegistrationSEHExceptionAccess::Kind;
  switch (Access.Field) {
  case Kind::PointersSlot:
    return Root == Frame && Offset == PointersOffset;
  case Kind::RecordPointer:
    return Root == Access.Bridge && Offset == 0;
  case Kind::ExceptionCode:
    return Root == Access.Bridge->getValueOperand() && Offset == 0;
  case Kind::ExceptionFlags:
    return Root == Access.Bridge->getValueOperand() && Offset == 4;
  }
  return false;
}

bool RegistrationSEHExceptionContract::clobbersPointers(
    const llvm::StoreInst &Store, const llvm::Value *Root, int64_t Offset,
    uint64_t Size) const {
  const auto Bridge = Bridges.find(Store.getFunction());
  if (Root != Frame || Bridge == Bridges.end() || Bridge->second == &Store)
    return false;
  return Offset < PointersOffset + 4 &&
         (Offset >= PointersOffset || Size > uint64_t(PointersOffset - Offset));
}

bool RegistrationSEHJumpContract::readsSourceBuffer(int64_t Offset,
                                                    uint64_t Bytes) const {
  for (const auto &[Begin, End] : SourceBuffers)
    if (Offset < End && (Offset >= Begin || Bytes > uint64_t(Begin - Offset)))
      return true;
  return false;
}

} // namespace neverd::coff_registration
