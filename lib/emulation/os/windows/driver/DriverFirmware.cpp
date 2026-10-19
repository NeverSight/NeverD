//===- DriverFirmware.cpp - Validated guest MCFG observations
//--------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validated guest MCFG observations.
///

#include "neverd/emulation/DriverFirmware.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <set>

namespace neverd::emulation {
llvm::Expected<uint32_t> validateDriverPciMcfg(const DriverPciMcfg &Facts) {
  auto Invalid = [](const char *Text) -> llvm::Error {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "pci_mcfg: %s", Text);
  };
  if (!Facts.Present) {
    if (!Facts.Table.empty())
      return Invalid("an absent table cannot carry bytes");
    return 0;
  }
  using namespace firmware;
  using namespace llvm::support::endian;
  const auto &B = Facts.Table;
  if (B.size() < McfgHeaderSize || B.size() > McfgMaxBytes ||
      (B.size() - McfgHeaderSize) % McfgEntrySize)
    return Invalid("invalid or oversized MCFG length");
  if (read32le(B.data()) != 0x4746434d || read32le(B.data() + 4) != B.size())
    return Invalid("MCFG signature or declared length mismatch");
  if (B[8] != 1)
    return Invalid("only ACPI MCFG revision 1 is supported");
  uint8_t Sum = 0;
  for (uint8_t C : B)
    Sum += C;
  if (Sum)
    return Invalid("invalid ACPI checksum");
  if (std::any_of(B.begin() + 36, B.begin() + McfgHeaderSize,
                  [](uint8_t C) { return C != 0; }))
    return Invalid("nonzero MCFG header reserved bytes");
  struct Allocation {
    uint64_t First, End;
    uint16_t Segment;
    uint8_t StartBus, EndBus;
  };
  std::vector<Allocation> Allocations;
  const uint32_t Count = (B.size() - McfgHeaderSize) / McfgEntrySize;
  for (uint32_t I = 0; I < Count; ++I) {
    const auto *E = B.data() + McfgHeaderSize + I * McfgEntrySize;
    const auto Base = read64le(E);
    const auto Segment = read16le(E + 8);
    const uint8_t Start = E[10], End = E[11];
    // ECAM base denotes bus zero even when StartBus is nonzero.
    const uint64_t EndOffset = (uint64_t(End) + 1) * EcamBusBytes;
    if (!Base || Base % EcamBusBytes || Start > End ||
        Base > UINT64_MAX - EndOffset || read32le(E + 12))
      return Invalid("invalid ECAM base, bus range or reserved bytes");
    Allocation A{Base + uint64_t(Start) * EcamBusBytes, Base + EndOffset,
                 Segment, Start, End};
    for (const auto &Other : Allocations)
      if ((Other.Segment == A.Segment && A.StartBus <= Other.EndBus &&
           Other.StartBus <= A.EndBus) ||
          (A.First < Other.End && Other.First < A.End))
        return Invalid("conflicting bus or physical ECAM ranges");
    Allocations.push_back(A);
  }
  return Count;
}
llvm::Error validateDriverSmbios(const DriverSmbios &Facts) {
  auto Invalid = [](const char *Text) -> llvm::Error {
    return llvm::createStringError(llvm::inconvertibleErrorCode(), "smbios: %s",
                                   Text);
  };
  const auto &B = Facts.Raw;
  if (B.size() < 14 || B.size() > firmware::SmbiosMaxBytes ||
      llvm::support::endian::read32le(B.data() + 4) != B.size() - 8)
    return Invalid("invalid RawSMBIOSData length");
  // Framing supported by DSP0134 3.8.0, sections 6.1 and 7.45. Envelope
  // metadata is retained verbatim; no type-specific hardware is synthesized.
  if (!((B[1] == 2 && B[2] >= 3 && B[2] <= 8) || (B[1] == 3 && B[2] <= 8)))
    return Invalid("supported table versions are SMBIOS 2.3-2.8 and 3.0-3.8");
  std::set<uint16_t> Handles;
  size_t At = 8;
  while (At < B.size()) {
    if (B.size() - At < 6 || B[At + 1] < 4 ||
        size_t(B[At + 1]) > B.size() - At - 2)
      return Invalid("truncated or invalid formatted structure");
    if (!Handles.insert(llvm::support::endian::read16le(B.data() + At + 2))
             .second)
      return Invalid("duplicate structure handle");
    size_t End = At + B[At + 1];
    while (End + 1 < B.size() && (B[End] || B[End + 1]))
      ++End;
    if (End + 1 >= B.size())
      return Invalid("unterminated structure string set");
    End += 2;
    if (B[At] == 127) {
      if (B[At + 1] != 4 || End != At + 6 || End != B.size())
        return Invalid("end marker must be the final six bytes");
      return llvm::Error::success();
    }
    At = End;
  }
  return Invalid("missing end-of-table marker");
}
llvm::Error validateDriverAcpiTables(const DriverAcpiTables &Facts) {
  auto Invalid = [](const char *Text) -> llvm::Error {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "acpi_tables: %s", Text);
  };
  if (Facts.Tables.size() > firmware::AcpiMaxTables)
    return Invalid("too many tables");
  size_t Total = 0;
  for (const auto &B : Facts.Tables) {
    if (B.size() < firmware::AcpiHeaderSize ||
        B.size() > firmware::AcpiMaxBytes - Total ||
        llvm::support::endian::read32le(B.data() + 4) != B.size())
      return Invalid("invalid table or aggregate length");
    Total += B.size();
    if (std::any_of(B.begin(), B.begin() + 4,
                    [](uint8_t C) { return C < 32 || C > 126; }) ||
        std::all_of(B.begin(), B.begin() + 4,
                    [](uint8_t C) { return C == 32; }))
      return Invalid("table signature must be printable ASCII");
    const auto Signature = llvm::support::endian::read32le(B.data());
    if (Signature == 0x53434146) // FACS does not have a DESCRIPTION_HEADER.
      return Invalid("FACS is outside the SDT provider inventory");
    uint8_t Sum = 0;
    for (uint8_t C : B)
      Sum += C;
    if (Sum)
      return Invalid("invalid table checksum");
    if (Signature == 0x4746434d) {
      auto Count = validateDriverPciMcfg({true, B});
      if (!Count)
        return Count.takeError();
    }
  }
  return llvm::Error::success();
}

llvm::Expected<std::optional<DriverPciMcfg>>
resolveDriverFirmwareMcfg(const std::optional<DriverPciMcfg> &Legacy,
                          const std::optional<DriverAcpiTables> &Inventory) {
  if (Legacy) {
    auto Count = validateDriverPciMcfg(*Legacy);
    if (!Count)
      return Count.takeError();
  }
  if (!Inventory)
    return Legacy;
  if (auto E = validateDriverAcpiTables(*Inventory))
    return std::move(E);
  DriverPciMcfg Derived;
  for (const auto &Table : Inventory->Tables)
    if (llvm::support::endian::read32le(Table.data()) == 0x4746434d) {
      Derived = {true, Table};
      break;
    }
  if (Legacy &&
      (Legacy->Present != Derived.Present || Legacy->Table != Derived.Table))
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "conflicting pci_mcfg and acpi_tables observations");
  return std::optional<DriverPciMcfg>(std::move(Derived));
}
} // namespace neverd::emulation
