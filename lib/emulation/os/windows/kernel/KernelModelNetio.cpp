///
/// \file
/// KernelModelNetio.
///

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
namespace neverd::emulation {
namespace {
constexpr uint32_t MibTag = 0x70614c4e;
}
llvm::Expected<uint64_t> KernelModel::callNetio(llvm::StringRef Name,
                                                llvm::ArrayRef<uint64_t> A) {
  if (Name == "FreeMibTable") {
    if (!MibTables.contains(A[0]))
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "FreeMibTable requires an owned live MIB table base");
    return freePool({A[0], MibTag}, true);
  }
  const auto Level = uint32_t(A[0]);
  if (Level > 2)
    return uint64_t(windows::StatusInvalidParameter);
  if (!NetworkInterfaces || !NetworkInterfaces->Levels[Level])
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "GetIfTable2Ex requires an explicit network_interfaces observation for "
        "this level");
  if (auto E = objectBuffer(A[1], 8, true))
    return E;
  if (auto E = Memory.writeInteger(A[1], 0, 8))
    return E;
  const auto &Rows = *NetworkInterfaces->Levels[Level];
  const uint64_t Size = 12 + Rows.size() * DriverNetworkRowSize;
  auto Buffer = allocatePool({0x200, Size, MibTag}, false);
  if (!Buffer)
    return Buffer.takeError();
  if (!*Buffer)
    return uint64_t(0xc0000017); // Original wrapper STATUS_NO_MEMORY.
  std::vector<uint8_t> Bytes(Size, 0);
  llvm::support::endian::write32le(Bytes.data(), Rows.size());
  for (size_t I = 0; I < Rows.size(); ++I)
    std::copy(Rows[I].begin(), Rows[I].end(),
              Bytes.begin() + 8 + I * DriverNetworkRowSize);
  auto E = Memory.write(*Buffer, Bytes);
  if (!E)
    E = Memory.writeInteger(A[1], *Buffer, 8);
  if (E) {
    auto Released = freePool({*Buffer, MibTag}, true);
    if (!Released)
      E = llvm::joinErrors(std::move(E), Released.takeError());
    return E;
  }
  MibTables.insert(*Buffer);
  return uint64_t(0);
}
} // namespace neverd::emulation
