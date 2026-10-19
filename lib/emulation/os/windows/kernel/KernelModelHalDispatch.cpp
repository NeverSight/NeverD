///
/// \file
/// KernelModelHalDispatch.
///

#include "KernelHalDispatch.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
namespace neverd::emulation {
namespace {
llvm::Error invalid(const llvm::Twine &T) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "HAL dispatch: " + T);
}
bool within(uint64_t A, uint64_t N, uint64_t B, uint64_t M) {
  return A >= B && N <= M && A - B <= M - N;
}
bool overlap(uint64_t A, uint64_t N, uint64_t B, uint64_t M) {
  return N && M && A < B + M && B < A + N;
}
} // namespace
llvm::Error KernelModel::initializeHalDispatch() {
  uint64_t Table = hal_dispatch::Slot;
  if (Exports) {
    auto P = Exports->resolve(hal_dispatch::ExportName);
    if (!P)
      return P.takeError();
    if (!*P)
      return llvm::Error::success();
    const auto *E = Exports->lookup(*P);
    if (!E || E->Kind != KernelExportRegistry::ExportKind::HalDispatchData)
      return invalid("export must be typed table data");
    Table = *P;
  }
  HalDispatchSlot = Table;
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return llvm::Error::success();
  if (auto E = Memory.writeInteger(Table, hal_dispatch::Version, 4))
    return E;
  if (!AcpiTables || !Exports)
    return llvm::Error::success();
  auto Function = Exports->insertHalDispatchFunction(Table);
  if (!Function)
    return Function.takeError();
  if (auto E =
          Memory.writeInteger(Table + hal_dispatch::CacheOffset, *Function, 8))
    return E;
  HalCacheFunction = *Function;
  return llvm::Error::success();
}
llvm::Error KernelModel::initializeHalAcpiCache() {
  if (!HalCacheFunction)
    return llvm::Error::success();
  bool FirstMcfg = true;
  for (const auto &Table : AcpiTables->Tables) {
    uint64_t Pointer = 0;
    if (llvm::support::endian::read32le(Table.data()) == 0x4746434d &&
        FirstMcfg) {
      FirstMcfg = false;
      if (PciMcfgPublished && PciMcfg && PciMcfg->Present)
        Pointer = windows::FirmwareMcfgBase;
    }
    if (!Pointer) {
      auto P = allocatePhysicalBuffer(Table.size(), 16, 0, Table.size());
      if (!P)
        return P.takeError();
      Pointer = *P;
      if (auto E = Memory.write(Pointer, Table))
        return E;
    }
    HalAcpiCache.emplace_back(Pointer, Table.size());
  }
  return llvm::Error::success();
}
bool KernelModel::isHalDispatchRead(uint64_t A, uint32_t N) const {
  if (!HalDispatchSlot || !Version || Version->Major != 10 ||
      Version->Minor != 0 || Version->Build != 19041)
    return false;
  return within(A, N, *HalDispatchSlot, 4) ||
         (HalCacheFunction &&
          within(A, N, *HalDispatchSlot + hal_dispatch::CacheOffset, 8));
}
llvm::Error KernelModel::validateHalDispatchAccess(uint64_t A, uint32_t N,
                                                   bool Write) const {
  if (HalDispatchSlot && overlap(A, N, *HalDispatchSlot, hal_dispatch::Bytes)) {
    if (!Version || Version->Major != 10 || Version->Minor != 0 ||
        Version->Build != 19041)
      return invalid("table requires windows_version 10.0.19041");
    if (!HalCacheFunction &&
        overlap(A, N, *HalDispatchSlot + hal_dispatch::CacheOffset, 8))
      return invalid("cached table slot requires explicit acpi_tables and a "
                     "kernel export registry");
    if (Write || !isHalDispatchRead(A, N))
      return invalid("access to an opaque or read-only dispatch field");
  }
  for (const auto &[B, M] : HalAcpiCache)
    if (overlap(A, N, B, M) && (Write || !within(A, N, B, M)))
      return invalid("access outside an owned read-only ACPI cache table");
  return llvm::Error::success();
}
llvm::Expected<uint64_t>
KernelModel::callHalDispatch(const KernelExportRegistry::Export &E,
                             llvm::ArrayRef<uint64_t> A) {
  APICallDetail.clear();
  if (!HalDispatchSlot || !HalCacheFunction || !AcpiTables ||
      E.Name != hal_dispatch::CacheFunction ||
      E.Module != hal_dispatch::Provider || E.Binding != *HalDispatchSlot ||
      E.Address != *HalCacheFunction || A.size() != 3)
    return invalid(
        "call requires the live bound cached-table function and exact arity");
  if (CurrentIRQL > 1)
    return invalid("cached-table query requires IRQL <= APC_LEVEL");
  if (auto C = executionProcessContext();
      C && hasProcessAttachment(CurrentExecution))
    return invalid(
        "cached-table query during process attachment is unsupported");
  if (uint32_t(A[0]) == 0x53434146)
    return invalid("FACS is outside the explicit SDT cache provider");
  auto Match = [&](uint64_t Pointer,
                   llvm::ArrayRef<uint8_t> Field) -> llvm::Expected<bool> {
    if (!Pointer)
      return true;
    // The original cache uses bounded strncmp, including early equal NULs.
    for (size_t I = 0; I < Field.size(); ++I) {
      if (Pointer > UINT64_MAX - I)
        return invalid("OEM filter pointer overflows");
      if (auto E = objectBuffer(Pointer + I, 1, false))
        return E;
      auto B = Memory.readInteger(Pointer + I, 1);
      if (!B)
        return B.takeError();
      if (*B != Field[I])
        return false;
      if (!*B)
        return true;
    }
    return true;
  };
  for (size_t I = 0; I < AcpiTables->Tables.size(); ++I) {
    const auto &T = AcpiTables->Tables[I];
    if (llvm::support::endian::read32le(T.data()) != uint32_t(A[0]))
      continue;
    auto OEM = Match(A[1], llvm::ArrayRef(T).slice(10, 6));
    if (!OEM)
      return OEM.takeError();
    if (!*OEM)
      continue;
    auto ID = Match(A[2], llvm::ArrayRef(T).slice(16, 8));
    if (!ID)
      return ID.takeError();
    if (*ID)
      return HalAcpiCache.at(I).first;
  }
  return 0;
}
} // namespace neverd::emulation
