//===- KernelUnpackState.cpp - Driver recovery ownership ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelModel.h"

namespace neverd::emulation {
llvm::Error KernelModel::captureUnpackBaseline() {
  if (!DriverObject || UnpackBaseline)
    return llvm::createStringError("kernel recovery baseline is not fresh");
  std::map<uint64_t, std::vector<uint8_t>> Baseline;
  for (const auto &[Address, Size] : ArenaAllocations) {
    auto &Bytes = Baseline[Address];
    Bytes.resize(Size);
    if (auto E = Memory.snapshotBacking(Address, Bytes))
      return E;
  }
  UnpackBaseline = std::move(Baseline);
  return llvm::Error::success();
}

std::map<uint64_t, uint64_t> KernelModel::unpackAllocations() const {
  // Borrowed loader objects are external dependencies too. Retaining their
  // guest addresses in an image cannot bind a fresh DriverEntry's objects.
  std::map<uint64_t, uint64_t> Out;
  for (const auto &[Address, Size] : ArenaAllocations)
    if (!FreedRanges.count(Address))
      Out.emplace(Address, Size);
  for (const auto &[Address, Allocation] : Allocations)
    Out[Address] = Allocation.Size;
  return Out;
}

llvm::Expected<bool> KernelModel::hasUnpackDependencies() const {
  if (!UnpackBaseline)
    return llvm::createStringError("kernel recovery has no ownership baseline");
  if (UnpackOpaqueEffects || !Allocations.empty() || EntryFinished ||
      CurrentIRQL || Unloading)
    return true;
  for (const auto &[Address, Size] : ArenaAllocations)
    if (!UnpackBaseline->count(Address) && !FreedRanges.count(Address))
      return true;
  for (const auto &[Address, Before] : *UnpackBaseline) {
    if (FreedRanges.count(Address))
      return true;
    std::vector<uint8_t> Now(Before.size());
    if (auto E = Memory.snapshotBacking(Address, Now))
      return std::move(E);
    if (Now != Before)
      return true;
  }
  return false;
}
} // namespace neverd::emulation
