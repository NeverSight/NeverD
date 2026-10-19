//===- McfgTestFixture.h - Independent synthetic ACPI fixture inputs
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent synthetic ACPI fixture inputs.
///

#ifndef NEVERD_TEST_MCFGTESTFIXTURE_H
#define NEVERD_TEST_MCFGTESTFIXTURE_H
#include "neverd/emulation/DriverSession.h"

namespace mcgffixture {
inline void put(std::vector<uint8_t> &B, size_t At, uint64_t Value,
                unsigned Width) {
  for (unsigned I = 0; I < Width; ++I)
    B[At + I] = uint8_t(Value >> (I * 8));
}
inline void checksum(std::vector<uint8_t> &B) {
  B[9] = 0;
  uint8_t Sum = 0;
  for (auto C : B)
    Sum += C;
  B[9] = uint8_t(0 - Sum);
}
inline neverd::emulation::DriverPciMcfg table(unsigned Count = 2) {
  neverd::emulation::DriverPciMcfg F;
  F.Present = true;
  // WDK 19041 acpitabl.h: 44-byte header and 16-byte MCFG_TABLE_ENTRY.
  F.Table.resize(44 + 16 * Count);
  put(F.Table, 0, 0x4746434d, 4);
  put(F.Table, 4, F.Table.size(), 4);
  F.Table[8] = 1;
  constexpr char Oem[] = "NEVERD", TableId[] = "NDMCFG  ";
  for (unsigned I = 0; I != 6; ++I)
    F.Table[10 + I] = Oem[I];
  for (unsigned I = 0; I != 8; ++I)
    F.Table[16 + I] = TableId[I];
  put(F.Table, 24, 1, 4);
  put(F.Table, 28, 0x2020444e, 4); // "ND  " creator ID.
  put(F.Table, 32, 1, 4);
  for (unsigned I = 0; I < Count; ++I) {
    put(F.Table, 44 + I * 16, 0x80000000ULL + uint64_t(I) * 0x10000000, 8);
    put(F.Table, 44 + I * 16 + 8, I, 2);
    F.Table[44 + I * 16 + 10] = 32;
    F.Table[44 + I * 16 + 11] = 63;
  }
  checksum(F.Table);
  return F;
}
inline neverd::emulation::DriverRegistryValue qword(const char *Name,
                                                    uint64_t Value) {
  neverd::emulation::DriverRegistryValue R{Name, 11, std::vector<uint8_t>(8)};
  put(R.Data, 0, Value, 8);
  return R;
}
inline neverd::emulation::DriverRegistryKey
parameters(uint64_t CountAddress, uint64_t PointerAddress, uint32_t Count,
           bool Present, unsigned Mode = 0) {
  return {
      "\\Registry\\Machine\\System\\CurrentControlSet\\Services\\NeverDDriver",
      {qword("McfgCountAddress", CountAddress),
       qword("McfgPointerAddress", PointerAddress),
       qword("McfgExpectedCount", Count), qword("McfgExpectedPresent", Present),
       qword("McfgTestMode", Mode)}};
}
} // namespace mcgffixture
#endif
