//===- KernelModelDMA.cpp - Adapter tables and coherent common buffers ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decode the selected WDK interface prefix and publish adapter-scoped method
/// identities. Common buffers use the same RAM authority as packet DMA.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <array>

namespace neverd::emulation {
namespace {
llvm::Error dmaAPIError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "DMA API: " + Text);
}
struct Operation {
  llvm::StringLiteral Name;
  unsigned Index, Arity;
  uint8_t MinimumIRQL, MaximumIRQL;
  bool Modeled;
};
constexpr Operation Operations[] = {
#define NEVERD_DMA_OPERATION(Name, Index, Arity, Minimum, Maximum, Modeled)    \
  {#Name, Index, Arity, Minimum, Maximum, Modeled},
#include "KernelDMAOperations.def"
#undef NEVERD_DMA_OPERATION
};
} // namespace

std::optional<unsigned> KernelModel::dmaArgumentCount(llvm::StringRef Name) {
  for (const auto &Op : Operations)
    if (Op.Name == Name)
      return Op.Arity;
  return std::nullopt;
}

llvm::Expected<uint64_t>
KernelModel::callDMAExport(const KernelExportRegistry::Export &Export,
                           llvm::ArrayRef<uint64_t> A) {
  const Operation *Selected = nullptr;
  for (const auto &Op : Operations)
    if (Op.Name == Export.Name)
      Selected = &Op;
  if (!Selected ||
      Export.Kind != KernelExportRegistry::ExportKind::DMAFunction ||
      A.size() != Selected->Arity || !DMA.adapter(Export.Binding) ||
      A[0] != Export.Binding)
    return dmaAPIError("method requires its exact live adapter binding");
  if (CurrentIRQL < Selected->MinimumIRQL ||
      CurrentIRQL > Selected->MaximumIRQL)
    return dmaAPIError(Export.Name + " called at an invalid IRQL");
  if (!Selected->Modeled)
    return dmaAPIError(Export.Name + " is outside the modeled DMA interface");
  if (Export.Name == "PutDmaAdapter") {
    if (auto E = DMA.putAdapter(A[0]))
      return E;
    return 0;
  }
  if (Export.Name == "AllocateCommonBuffer")
    return allocateCommonBuffer(A);
  if (Export.Name == "FreeCommonBuffer") {
    if (auto E = freeCommonBuffer(A))
      return E;
    return 0;
  }
  if (Export.Name == "GetDmaAlignment")
    return DMA.adapter(A[0])->Alignment;
  if (Export.Name == "GetScatterGatherList")
    return getScatterGatherList(A);
  if (Export.Name == "AllocateAdapterChannel")
    return allocateAdapterChannel(A);
  if (Export.Name == "MapTransfer")
    return mapTransfer(A);
  if (Export.Name == "FlushAdapterBuffers") {
    if (auto E = flushAdapterBuffers(A))
      return E;
    return 1;
  }
  if (Export.Name == "FreeMapRegisters") {
    if (auto E = freeMapRegisters(A))
      return E;
    return 0;
  }
  if (Export.Name == "PutScatterGatherList") {
    if (auto E = putScatterGatherList(A))
      return E;
    return 0;
  }
  return dmaAPIError("method has no implementation");
}

llvm::Expected<uint64_t>
KernelModel::getDMAAdapter(llvm::ArrayRef<uint64_t> A) {
  if (CurrentIRQL != scheduler::PassiveLevel || !Exports)
    return dmaAPIError(
        "IoGetDmaAdapter requires PASSIVE_LEVEL and export identity");
  const auto *Device = Resources.find(A[0]);
  if (!Device || !Device->Present || !isProviderDevice(A[0]))
    return dmaAPIError("IoGetDmaAdapter requires a present configured PDO");
  if (!A[1] || !A[2])
    return dmaAPIError(
        "IoGetDmaAdapter requires description and register output");
  auto ReadField = [&](uint64_t Offset,
                       unsigned Size) -> llvm::Expected<uint64_t> {
    if (Offset > UINT64_MAX - A[1])
      return dmaAPIError("description field address overflows");
    if (auto E = validateGuestAccess(A[1] + Offset, Size, false))
      return E;
    return Memory.readInteger(A[1] + Offset, Size);
  };
  auto Version = ReadField(dma::DescriptionVersion, 4);
  if (!Version)
    return Version.takeError();
  // Version probing is documented to return NULL for an unsupported version.
  // Never read a modern description tail or advertise a larger method table.
  if (*Version > 1)
    return 0;
  uint64_t Master = 0, Scatter = 0, Dma32 = 0, Dma64 = 0, Reserved = 0;
  uint64_t Interface = 0, Maximum = 0;
  struct Field {
    uint64_t Offset;
    unsigned Size;
    uint64_t *Value;
  };
  for (const auto &F :
       std::array<Field, 7>{{{dma::DescriptionMaster, 1, &Master},
                             {dma::DescriptionScatterGather, 1, &Scatter},
                             {dma::DescriptionDma32, 1, &Dma32},
                             {dma::DescriptionDma64, 1, &Dma64},
                             {dma::DescriptionReserved, 1, &Reserved},
                             {dma::DescriptionInterface, 4, &Interface},
                             {dma::DescriptionMaximumLength, 4, &Maximum}}}) {
    auto Value = ReadField(F.Offset, F.Size);
    if (!Value)
      return Value.takeError();
    *F.Value = *Value;
  }
  // Slave-channel fields are not selected for a bus-master description.
  if (!Master || Reserved || (!Dma32 && !Dma64) ||
      (Interface != UINT32_MAX && Interface != 0))
    return dmaAPIError(
        "only explicit Internal bus-master 32/64-bit DMA is modeled");
  if (!Maximum)
    return dmaAPIError("adapter description has zero MaximumLength");
  if (auto E = validateGuestAccess(A[2], 4, true))
    return E;
  if (!Device->Dma)
    return 0;
  const auto &Config = *Device->Dma;
  const uint64_t Mask = Dma64 ? UINT64_MAX : UINT32_MAX;
  if (Maximum > Config.MaximumLength || (Scatter && !Config.ScatterGather) ||
      Config.LogicalBase > Mask ||
      Config.LogicalLength - 1 > Mask - Config.LogicalBase)
    return 0;
  constexpr uint64_t Size = dma::AdapterSize + dma::OperationsSize;
  const uint64_t Base = (NextAllocation + 15) & ~uint64_t(15);
  if (Base > AllocationEnd || Size > AllocationEnd - Base ||
      Exports->availableThunkCount() < dma::OperationCount)
    return 0;
  KernelDMA::Adapter Adapter;
  Adapter.Object = Base;
  Adapter.Table = Base + dma::AdapterSize;
  Adapter.PDO = A[0];
  Adapter.AddressMask = Mask;
  Adapter.MaximumLength = uint32_t(Maximum);
  Adapter.MapRegisters = Config.MapRegisters;
  Adapter.Alignment = Config.Alignment;
  Adapter.ScatterGather = bool(Scatter);
  if (auto E = DMA.canCreateAdapter(Adapter))
    return E;
  auto Storage = allocate(Size);
  if (!Storage)
    return Storage.takeError();
  if (*Storage != Base)
    return dmaAPIError("adapter allocation changed after preflight");
  if (auto E = Memory.writeInteger(Base + dma::AdapterVersionOffset, 1, 2))
    return E;
  if (auto E = Memory.writeInteger(Base + dma::AdapterSizeOffset,
                                   dma::AdapterSize, 2))
    return E;
  if (auto E = Memory.writeInteger(Base + dma::AdapterOperationsOffset,
                                   Adapter.Table, 8))
    return E;
  if (auto E = Memory.writeInteger(Adapter.Table, dma::OperationsSize, 4))
    return E;
  for (const auto &Op : Operations) {
    auto Thunk = Exports->insertDMAFunction(Base, Op.Name);
    if (!Thunk)
      return Thunk.takeError();
    if (auto E = Memory.writeInteger(Adapter.Table + dma::OperationFirstOffset +
                                         Op.Index * profile::PointerSize,
                                     *Thunk, profile::PointerSize))
      return E;
  }
  if (auto E = Memory.writeInteger(A[2], Config.MapRegisters, 4))
    return E;
  if (auto E = DMA.createAdapter(Adapter))
    return E;
  return Base;
}

llvm::Expected<uint64_t>
KernelModel::allocateCommonBuffer(llvm::ArrayRef<uint64_t> A) {
  const uint32_t Length = uint32_t(A[1]);
  if (!Length || !A[2])
    return dmaAPIError(
        "AllocateCommonBuffer requires length and logical output");
  if (auto E = validateGuestAccess(A[2], 8, true))
    return E;
  auto Planned =
      DMA.planMapping(A[0], 0, 0, Length, true, DriverDmaDirection::ReadMemory);
  if (!Planned)
    return Planned.takeError();
  if (!*Planned)
    return 0;
  auto Plan = **Planned;
  const uint64_t StorageSize = Plan.Registers * DriverDmaPageSize;
  const uint64_t Base =
      (NextAllocation + DriverDmaPageSize - 1) & ~(DriverDmaPageSize - 1);
  if (Base > AllocationEnd || StorageSize > AllocationEnd - Base)
    return 0;
  // CacheEnabled is ignored by Windows on x64. Coherence does not imply that
  // this allocation has a particular initial byte value on actual Windows.
  auto Buffer =
      allocatePhysicalBuffer(StorageSize, DriverDmaPageSize, 0, Length);
  if (!Buffer) {
    auto Error = Buffer.takeError();
    if (Error.isA<PhysicalMemoryLimitError>()) {
      llvm::consumeError(std::move(Error));
      return 0;
    }
    return std::move(Error);
  }
  if (!*Buffer)
    return 0;
  Plan.Object = Plan.Owner = Plan.Backing = *Buffer;
  Plan.StorageSize = StorageSize;
  if (auto E = DMA.canPublishMapping(Plan))
    return E;
  if (auto E = Memory.writeInteger(A[2], Plan.Logical, 8))
    return E;
  if (auto E = DMA.publishMapping(Plan))
    return E;
  return *Buffer;
}

llvm::Error KernelModel::freeCommonBuffer(llvm::ArrayRef<uint64_t> A) {
  const auto *Found = DMA.mapping(A[3]);
  if (!Found || !Found->Common || Found->Adapter != A[0] ||
      Found->Length != uint32_t(A[1]) || Found->Logical != A[2])
    return dmaAPIError(
        "FreeCommonBuffer requires its exact original allocation");
  const auto Map = *Found;
  auto Release = DMA.planRelease(Map.Object);
  if (!Release)
    return Release.takeError();
  auto Ready = dmaPromotionIDs(Release->Ready);
  if (!Ready)
    return Ready.takeError();
  if (auto E = prepareReleaseRange(Map.Object, Map.StorageSize, Map.Pin))
    return E;
  if (auto E = releaseDMAMapping(*Release))
    return E;
  if (auto E = Physical.retire(Map.Owner))
    return E;
  FreedRanges.emplace(Map.Object, Map.StorageSize);
  return llvm::Error::success();
}
} // namespace neverd::emulation
