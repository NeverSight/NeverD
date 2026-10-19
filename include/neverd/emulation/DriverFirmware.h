//===- DriverFirmware.h - Explicit guest firmware observations ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit guest firmware observations.
///

#ifndef NEVERD_EMULATION_DRIVERFIRMWARE_H
#define NEVERD_EMULATION_DRIVERFIRMWARE_H
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::emulation {
namespace firmware {
inline constexpr uint32_t McfgHeaderSize = 44;
inline constexpr uint32_t McfgEntrySize = 16;
inline constexpr uint32_t McfgMaxEntries = 256;
inline constexpr uint32_t McfgMaxBytes =
    McfgHeaderSize + McfgEntrySize * McfgMaxEntries;
inline constexpr uint64_t EcamBusBytes = 1 << 20;
inline constexpr uint32_t SmbiosMaxBytes = 1024 * 1024;
inline constexpr uint32_t AcpiHeaderSize = 36;
inline constexpr uint32_t AcpiMaxTables = 256;
inline constexpr uint32_t AcpiMaxBytes = 1024 * 1024;
} // namespace firmware

struct DriverPciMcfg {
  // An engaged observation with Present=false explicitly declares no table.
  // Present=true retains the exact complete ACPI MCFG bytes, including header.
  // Neither state alone implements an ECAM transaction or a PCI device.
  bool Present = false;
  std::vector<uint8_t> Table;
};

/// Validate the complete observation and return the authoritative entry count.
/// Revision 1, checksum, lengths, reserved fields, bus/physical range bounds
/// and conflicts are checked before any model publication.
llvm::Expected<uint32_t> validateDriverPciMcfg(const DriverPciMcfg &Facts);

struct DriverSmbios {
  // Complete RawSMBIOSData, including the 8-byte Windows envelope. These are
  // explicit guest bytes, never copied from the host firmware implicitly.
  std::vector<uint8_t> Raw;
};

/// Validate bounded RawSMBIOSData and SMBIOS structure framing, unique handles
/// and final end marker. Type-specific fields and platform conformance remain
/// the responsibility of the supplied observation; OEM records stay opaque.
llvm::Error validateDriverSmbios(const DriverSmbios &Facts);

struct DriverAcpiTables {
  // Complete ordered inventory of the ACPI provider's SDTs, including headers.
  // Duplicate signatures are retained; Get returns the first matching table.
  std::vector<std::vector<uint8_t>> Tables;
};
llvm::Error validateDriverAcpiTables(const DriverAcpiTables &Facts);
/// The first inventory MCFG is authoritative. Reject contradictory legacy
/// observations; omission of the whole inventory never means an empty list.
llvm::Expected<std::optional<DriverPciMcfg>>
resolveDriverFirmwareMcfg(const std::optional<DriverPciMcfg> &Legacy,
                          const std::optional<DriverAcpiTables> &Inventory);
} // namespace neverd::emulation
#endif
