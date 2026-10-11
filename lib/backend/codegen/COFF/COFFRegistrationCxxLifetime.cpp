//===- COFFRegistrationCxxLifetime.cpp - Active runtime object guards -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Follow validated catch ancestry when a nested handler borrows an outer
/// object.
//===----------------------------------------------------------------------===//
#include "COFFRegistrationFrameProof.h"

#include "neverd/Limits.h"

#include "llvm/IR/Instructions.h"

namespace neverd::coff_registration {
bool isActiveCxxCatch(const llvm::CatchPadInst *Owner,
                      const llvm::CatchPadInst *Current) {
  for (size_t Depth = 0; Current && Depth < limits::kMaxRegistrationEHRecords;
       ++Depth) {
    if (Current == Owner)
      return true;
    Current = llvm::dyn_cast<llvm::CatchPadInst>(
        Current->getCatchSwitch()->getParentPad());
  }
  return false;
}

const RegistrationCxxCatchFrameContract *
activeCxxReferenceHome(const RegistrationCxxFrameContract *Contract,
                       const RegistrationCxxCatchFrameContract *Current,
                       int64_t Offset, uint64_t Size) {
  if (!Contract || !Current ||
      Contract->Catches.size() > limits::kMaxRegistrationEHRecords)
    return nullptr;
  const RegistrationCxxCatchFrameContract *Found = nullptr;
  for (const auto &Catch : Contract->Catches) {
    const bool Matches = Size ? Offset < Catch.HomeOffset + 4 &&
                                    Catch.HomeOffset < Offset + int64_t(Size)
                              : Offset == Catch.HomeOffset;
    if (!Catch.Reference || !Matches ||
        !isActiveCxxCatch(Catch.Catch, Current->Catch))
      continue;
    if (Found && !Size)
      return nullptr;
    Found = &Catch;
  }
  return Found;
}
} // namespace neverd::coff_registration
