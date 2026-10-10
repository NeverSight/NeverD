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
  // PE metadata is readable even without a module-list call. A pointer into a
  // modeled provider remains borrowed state; never treat the input image as
  // such a dependency, since that is the image being recovered.
  for (size_t I = 0; I + 1 < LoadedModules.size(); ++I) {
    const auto &Module = LoadedModules[I];
    uint64_t Begin = Module.Base;
    const uint64_t End = Module.Base + Module.Size;
    // Exact named exports have a separate import-rebinding contract. Excluding
    // only those values preserves ordinary import cells while still exposing
    // retained module bases, metadata and interior service-body pointers.
    for (auto It = Exports->entries().lower_bound(Begin);
         It != Exports->entries().end() && It->first < End; ++It) {
      if (It->second.Kind != KernelExportRegistry::ExportKind::ModuleExport)
        continue;
      if (It->first > Begin)
        Out.emplace(Begin, It->first - Begin);
      Begin = It->first + 1;
    }
    if (Begin < End)
      Out.emplace(Begin, End - Begin);
  }
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
