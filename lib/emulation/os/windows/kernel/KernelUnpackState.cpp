//===- KernelUnpackState.cpp - Driver recovery ownership ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelAPIKind.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
void KernelModel::recordUnpackEnvironmentRead() {
  // Observed CPU identity and time cannot be rebound to a fresh kernel.
  // API and instruction reads share this dependency independently of storage.
  UnpackOpaqueEffects = true;
}

bool KernelModel::unpackImageMDLCall(KernelAPIKind Kind,
                                     llvm::ArrayRef<uint64_t> A) const {
  if (Kind == KernelAPIKind::IoAllocateMdl)
    return !A[4] && imageOwnerForRange(A[0], uint32_t(A[1])).has_value();
  const auto It =
      MDLs.find(A[Kind == KernelAPIKind::MmUnmapLockedPages ? 1 : 0]);
  if (It == MDLs.end() ||
      (It->second.Owner != LockedMdl::Ownership::Driver &&
       It->second.Owner != LockedMdl::Ownership::KernelLocked) ||
      !imageOwnerForRange(It->second.OriginalAddress, It->second.ByteCount))
    return false;
  if (Kind == KernelAPIKind::MmProbeAndLockPages)
    return uint8_t(A[1]) == windows::KernelMode;
  if (Kind == KernelAPIKind::MmMapLockedPagesSpecifyCache)
    return uint8_t(A[1]) == windows::KernelMode &&
           uint32_t(A[2]) == windows::MmCached && !A[3];
  return true;
}

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

llvm::Expected<std::vector<std::string>>
KernelModel::unpackDependencies() const {
  if (!UnpackBaseline)
    return llvm::createStringError("kernel recovery has no ownership baseline");
  std::vector<std::string> Reasons;
  if (UnpackOpaqueEffects)
    Reasons.emplace_back("unreconstructed kernel or environment effects");
  if (UnpackMDLIdentityRead)
    Reasons.emplace_back("observed kernel MDL physical identity");
  if (!MDLs.empty())
    Reasons.emplace_back("live kernel MDLs");
  if (!Allocations.empty())
    Reasons.emplace_back("live kernel pool allocations");
  if (EntryFinished)
    Reasons.emplace_back("DriverEntry has already returned");
  if (CurrentIRQL)
    Reasons.emplace_back("raised kernel IRQL");
  if (Unloading)
    Reasons.emplace_back("driver unload is in progress");
  for (const auto &[Address, Size] : ArenaAllocations)
    if (!UnpackBaseline->count(Address) && !FreedRanges.count(Address)) {
      Reasons.emplace_back("retained kernel object allocations");
      break;
    }
  bool Released = false, Changed = false;
  for (const auto &[Address, Before] : *UnpackBaseline) {
    if (FreedRanges.count(Address)) {
      Released = true;
      continue;
    }
    std::vector<uint8_t> Now(Before.size());
    if (auto E = Memory.snapshotBacking(Address, Now))
      return std::move(E);
    if (Now != Before)
      Changed = true;
  }
  if (Released)
    Reasons.emplace_back("released kernel loader objects");
  if (Changed)
    Reasons.emplace_back("changed kernel loader objects");
  return Reasons;
}
} // namespace neverd::emulation
