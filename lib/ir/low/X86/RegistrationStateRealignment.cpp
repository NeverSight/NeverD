//===- RegistrationStateRealignment.cpp - x86 EH frame coordinates --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"

#include "llvm/ADT/STLExtras.h"

#include <utility>

namespace neverd::registration_state {

bool RegistrationStateSolver::validateRealignedLayout() {
  const auto &Layout = *Chain.RealignedFrame;
  const bool StackBased = Layout.BaseRegister == 4;
  if (!KnownCxx || (!StackBased && Layout.BaseRegister != 6) ||
      Layout.Alignment < limits::kMinRegistrationFrameAlignment ||
      Layout.Alignment > limits::kMaxRegistrationFrameAlignment ||
      (Layout.Alignment & (Layout.Alignment - 1)) ||
      Layout.AllocationBytes > limits::kMaxSourceCallBorrowedBytes ||
      Chain.RegistrationOffset != -12 || Chain.TryLevelOffset != -4 ||
      Chain.SeededTryLevel != -1)
    return false;
  if (StackBased) {
    // A catch continuation would also have to restore the independent entry
    // EBP. This layout has no saved-parent cell in the aligned frame, so its
    // coordinate proof currently admits cleanup-only functions.
    if (!EH.Cxx->TryBlocks.empty() || Function.Entry > UINT32_MAX - 8 ||
        (Layout.DefinitionVA != Function.Entry + 3 &&
         Layout.DefinitionVA != Function.Entry + 5) ||
        Layout.AllocationBytes < 12 ||
        int64_t(Layout.BaseOffset) != -int64_t(Layout.AllocationBytes) ||
        Layout.SavedParentFrameOffset != 0)
      return false;
    const auto Entry = llvm::find_if(Function.Blocks, [&](const LowBlock &B) {
      return B.StartAddr == Function.Entry;
    });
    if (Entry == Function.Blocks.end() ||
        Entry->EndAddr < Layout.DefinitionVA + 3)
      return false;
    const va_t Prefix = Layout.DefinitionVA - 3;
    for (const auto &[Address, Size] :
         {std::pair{Prefix, 1u}, std::pair{Prefix + 1, 2u},
          std::pair{Layout.DefinitionVA, 0u}}) {
      unsigned Matches = 0;
      for (const auto &Boundary : Entry->InstructionBoundaries) {
        if (!charge(1))
          return false;
        const bool ExpectedSize =
            Size ? Boundary.Size == Size
                 : Boundary.Size == 3 || Boundary.Size == 6;
        if (Boundary.Address == Address && ExpectedSize &&
            Boundary.Size <= Entry->EndAddr - Address &&
            Boundary.Control == LowInstructionControl::None)
          ++Matches;
      }
      if (Matches != 1)
        return false;
    }
    return true;
  }
  if (Function.Entry > UINT32_MAX - 17 ||
      Layout.DefinitionVA != Function.Entry + 15 ||
      Layout.AllocationBytes < 20 ||
      Layout.AllocationBytes > limits::kMaxSourceCallBorrowedBytes ||
      Layout.BaseOffset > -20 ||
      int64_t(Layout.BaseOffset) + Layout.AllocationBytes < 0 ||
      Layout.SavedParentFrameOffset != -20 || Chain.RegistrationOffset != -12 ||
      Chain.TryLevelOffset != -4 || Chain.SeededTryLevel != -1)
    return false;
  // The coordinate change must occur on the ordinary entry path. Metadata
  // alone cannot seed ESI or skip the LowIR prologue that constructs it.
  const auto Entry = llvm::find_if(Function.Blocks, [&](const LowBlock &Block) {
    return Block.StartAddr == Function.Entry;
  });
  if (Entry == Function.Blocks.end() || Entry->EndAddr < Function.Entry + 17)
    return false;
  static constexpr std::pair<unsigned, unsigned> Prologue[] = {
      {0, 1}, {1, 2}, {3, 1}, {4, 1}, {5, 1}, {6, 3}, {9, 6}, {15, 2}};
  for (const auto &[Offset, Size] : Prologue) {
    unsigned Matches = 0;
    for (const auto &Boundary : Entry->InstructionBoundaries) {
      if (!charge(1))
        return false;
      if (Boundary.Address == Function.Entry + Offset &&
          Boundary.Size == Size &&
          Boundary.Control == LowInstructionControl::None)
        ++Matches;
    }
    if (Matches != 1)
      return false;
  }
  return true;
}

bool RegistrationStateSolver::realignedMemoryIsDisjoint(
    const FrameValue &Address, uint16_t Width) const {
  if (!Chain.RealignedFrame)
    return true;
  if (Address.EntryOffset)
    return *Address.EntryOffset >=
           (Chain.RealignedFrame->BaseRegister == 4 ? 0 : -12);
  if (Address.Offset) {
    const auto &Layout = *Chain.RealignedFrame;
    const int64_t AlignedTop =
        int64_t(Layout.BaseOffset) + Layout.AllocationBytes;
    return int64_t(*Address.Offset) + Width <= AlignedTop;
  }
  return true;
}

} // namespace neverd::registration_state
