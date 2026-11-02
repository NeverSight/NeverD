//===- SBFSourcePartition.cpp - Bounded source regions --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Builds a linear-space source layout from existing LowIR ranges and HighIR
/// ownership. Regions bound native compiler work without reducing VM coverage.
///
//===----------------------------------------------------------------------===//

#include "neverd/sbf/emit/SBFSourcePartition.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <map>
#include <utility>

namespace neverd::sbf {

llvm::Expected<SourcePartition> partitionSource(const SBFProgram &Program,
                                                size_t MaximumSlots) {
  const size_t SlotCount = Program.Low.Instructions.size();
  if (MaximumSlots == 0 || SlotCount == 0 || SlotCount > kMaxInstructions)
    return llvm::make_error<llvm::StringError>(
        "sbf: invalid source partition size", llvm::inconvertibleErrorCode());
  const size_t Shared = Program.High.Functions.size();
  const size_t Unowned = Shared + 1;
  std::vector<size_t> OwnerBySlot(SlotCount, Unowned);
  llvm::BitVector Covered(SlotCount);
  for (const BasicBlock &Block : Program.Low.Blocks) {
    if (Block.StartSlot >= Block.EndSlot || Block.EndSlot > SlotCount)
      return llvm::make_error<llvm::StringError>(
          "sbf: invalid LowIR range in source partition",
          llvm::inconvertibleErrorCode());
    size_t Owner = Unowned;
    if (Block.ID < Program.High.BlockOwners.size()) {
      const size_t Known = Program.High.BlockOwners[Block.ID];
      if (Known < Program.High.Functions.size())
        Owner = Known;
      else if (Known == HighIR::AmbiguousFunction)
        Owner = Shared;
      else if (Known != HighIR::NoFunction)
        return llvm::make_error<llvm::StringError>(
            "sbf: invalid HighIR owner in source partition",
            llvm::inconvertibleErrorCode());
    }
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
      if (Covered.test(Slot))
        return llvm::make_error<llvm::StringError>(
            "sbf: overlapping LowIR ranges in source partition",
            llvm::inconvertibleErrorCode());
      Covered.set(Slot);
      OwnerBySlot[Slot] = Owner;
    }
  }
  std::map<size_t, std::vector<size_t>> Groups;
  for (size_t Slot = 0; Slot < SlotCount; ++Slot)
    Groups[OwnerBySlot[Slot]].push_back(Slot);
  SourcePartition Result;
  Result.RegionBySlot.resize(SlotCount);
  for (const auto &[Owner, Slots] : Groups) {
    for (size_t Start = 0; Start < Slots.size();) {
      const size_t Count = std::min(MaximumSlots, Slots.size() - Start);
      SourcePartitionRegion Region;
      if (Owner < Shared) {
        Region.Owner = SourceRegionOwner::Function;
        Region.FunctionIndex = Owner;
        Region.Name =
            "nd_function_" +
            llvm::utohexstr(Program.High.Functions[Owner].Address, true);
      } else {
        Region.Owner = Owner == Shared ? SourceRegionOwner::Shared
                                       : SourceRegionOwner::Unowned;
        Region.Name = Owner == Shared ? "nd_shared" : "nd_unowned";
      }
      Region.Name += "_pc_" + std::to_string(Slots[Start]);
      Region.Slots.assign(Slots.begin() + Start, Slots.begin() + Start + Count);
      for (size_t Slot : Region.Slots)
        Result.RegionBySlot[Slot] = Result.Regions.size();
      Result.Regions.push_back(std::move(Region));
      Start += Count;
    }
  }
  return Result;
}

} // namespace neverd::sbf
