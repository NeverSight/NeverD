//===- KernelModelFirmwareQueries.cpp - Guest RSMB query protocol
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest RSMB query protocol.
///

#include "KernelModel.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation {
llvm::Expected<uint64_t>
KernelModel::queryFirmwareInformation(llvm::ArrayRef<uint64_t> A, bool ZwCall) {
  auto Unsupported = [](const char *Text) -> llvm::Error {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "firmware query: %s", Text);
  };
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return Unsupported("class 76 requires Windows 10.0.19041");
  auto Context = executionProcessContext();
  if (!ZwCall && Context && Context->PreviousMode != 0)
    return Unsupported("Nt class 76 UserMode capture is not modeled");
  auto Writable = [&](uint64_t Address, uint32_t Length) -> llvm::Error {
    if (auto E = objectBuffer(Address, Length, true))
      return E;
    // Preflight ordinary RAM only. A stopped caller cannot observe intermediate
    // native stores; MMIO and failed multi-span writes must not gain effects.
    auto Backing = Memory.pinBacking(Address, Length);
    if (!Backing)
      return Backing.takeError();
    return llvm::Error::success();
  };
  const uint32_t Length = uint32_t(A[2]);
  if (Length < 16) {
    if (A[3]) {
      if (auto E = Writable(A[3], 4))
        return E;
      if (auto E = Memory.writeInteger(A[3], 16, 4))
        return E;
    }
    return uint64_t(0xc0000004); // STATUS_INFO_LENGTH_MISMATCH
  }
  if (auto E = objectBuffer(A[1], 16, false))
    return E;
  auto Captured = Memory.pinBacking(A[1], 16);
  if (!Captured)
    return Captured.takeError();
  std::array<uint8_t, 16> Header;
  if (auto E = Memory.read(A[1], Header))
    return E;
  using namespace llvm::support::endian;
  const uint32_t Provider = read32le(Header.data());
  const bool RawSmbios = Provider == 0x52534d42;
  if (!RawSmbios && Provider != 0x41435049)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "firmware query: unmodeled provider 0x%08x (RSMB/ACPI only)", Provider);
  if (RawSmbios && !Smbios)
    return Unsupported("RSMB requires explicit smbios input");
  if (!RawSmbios && !AcpiTables)
    return Unsupported("ACPI requires explicit acpi_tables input");
  const uint32_t Action = read32le(Header.data() + 4);
  const uint32_t TableID = read32le(Header.data() + 8);
  APICallDetail = std::string(RawSmbios ? "RSMB" : "ACPI") +
                  " action=" + std::to_string(Action) + " table_id=0x" +
                  llvm::utohexstr(TableID);
  const uint32_t Capacity = Length - 16;
  // Native 19041 ExpGetSystemFirmwareTableInformation replaces the incoming
  // TableBufferLength with outer length minus 16 before calling the provider.
  // WmipRawSMBiosTableHandler ignores TableID, enumerates ID 0 and supports
  // actions 0/1. ReturnLength includes the 16-byte query header on all of these
  // provider paths, including BUFFER_TOO_SMALL and INVALID_PARAMETER_1.
  std::array<uint8_t, 4> ID{};
  std::vector<uint8_t> IDs;
  llvm::ArrayRef<uint8_t> Payload;
  uint32_t Status = 0;
  if (RawSmbios) {
    Payload = Action == 0 ? llvm::ArrayRef<uint8_t>(ID)
                          : llvm::ArrayRef<uint8_t>(Smbios->Raw);
    if (Action > 1)
      Status = 0xc00000ef;
  } else if (Action == 0) {
    for (const auto &Table : AcpiTables->Tables)
      IDs.insert(IDs.end(), Table.begin(), Table.begin() + 4);
    Payload = IDs;
    if (IDs.empty())
      Status = 0xc0140019; // STATUS_ACPI_INVALID_TABLE, native empty-list path.
  } else if (Action == 1) {
    for (const auto &Table : AcpiTables->Tables)
      if (read32le(Table.data()) == TableID) {
        Payload = Table;
        break; // Native Get selects the first duplicate signature.
      }
    if (Payload.empty())
      Status = 0xc0000225; // STATUS_NOT_FOUND
  } else
    Status = 0xc0000002; // STATUS_NOT_IMPLEMENTED
  const uint32_t Required = Status ? Capacity : Payload.size();
  const bool Copy = !Status && Capacity >= Required;
  if (!Status && !Copy)
    Status = 0xc0000023;
  if (auto E = Writable(A[1] + 12, 4))
    return E;
  if (Copy)
    if (auto E = Writable(A[1] + 16, Required))
      return E;
  if (A[3])
    if (auto E = Writable(A[3], 4))
      return E;
  if (auto E = Memory.writeInteger(A[1] + 12, Required, 4))
    return E;
  if (Copy)
    if (auto E = Memory.write(A[1] + 16, Payload))
      return E;
  if (A[3])
    if (auto E = Memory.writeInteger(A[3], uint64_t(Required) + 16, 4))
      return E;
  return uint64_t(Status);
}
} // namespace neverd::emulation
