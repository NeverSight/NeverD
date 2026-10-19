//===- SmbiosTestFixture.h - Explicit synthetic guest firmware bytes
//-------===//
///
/// \file
/// Explicit synthetic guest firmware bytes.
///

#ifndef NEVERD_TESTS_SMBIOSFIXTURE_H
#define NEVERD_TESTS_SMBIOSFIXTURE_H
#include "neverd/emulation/DriverFirmware.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::smbiosfixture {
// Identity records for a virtual lab. This is a parser/query fixture, not a
// claim of DMTF platform conformance or a copy of any host's hardware identity.
inline DriverSmbios table() {
  DriverSmbios F;
  F.Raw = {0, 3, 0, 0, 0, 0, 0, 0};
  auto Add = [&](std::vector<uint8_t> B, uint16_t Handle,
                 std::initializer_list<const char *> Strings) {
    B[1] = B.size();
    llvm::support::endian::write16le(B.data() + 2, Handle);
    F.Raw.insert(F.Raw.end(), B.begin(), B.end());
    for (const char *S : Strings) {
      while (*S)
        F.Raw.push_back(*S++);
      F.Raw.push_back(0);
    }
    if (Strings.size() == 0)
      F.Raw.push_back(0);
    F.Raw.push_back(0);
  };
  std::vector<uint8_t> Bios(24, 0);
  Bios[4] = 1;
  Bios[5] = 2;
  Bios[8] = 3;
  Bios[10] = 8; // Characteristics not supported; no inferred firmware features.
  Bios[19] = 16; // Explicit virtual machine.
  Bios[20] = 1;
  Bios[22] = Bios[23] = 255;
  Add(Bios, 0, {"NeverD", "Lab firmware 1.0", "10/16/2026"});
  std::vector<uint8_t> System(27, 0);
  System[0] = 1;
  System[4] = 1;
  System[5] = 2;
  System[6] = 3;
  System[7] = 4;
  for (unsigned I = 8; I != 24; ++I)
    System[I] = I + 1;
  System[24] = 6; // Power switch.
  Add(System, 0x100, {"NeverD", "Driver Lab", "1.0", "ND-LAB-0001"});
  std::vector<uint8_t> Board(15, 0);
  Board[0] = 2;
  Board[4] = 1;
  Board[5] = 2;
  Board[6] = 3;
  Board[7] = 4;
  Board[9] = 1;
  Board[12] = 3;  // Chassis handle 0x300.
  Board[13] = 10; // Motherboard.
  Add(Board, 0x200, {"NeverD", "Lab Board", "1.0", "ND-BOARD-0001"});
  std::vector<uint8_t> Chassis(22, 0);
  Chassis[0] = 3;
  Chassis[4] = 1;
  Chassis[5] = 3; // Desktop.
  Chassis[6] = 2;
  Chassis[7] = 3;
  Chassis[9] = Chassis[10] = Chassis[11] = 3; // Safe.
  Chassis[12] = 3;                            // No security lockout.
  Add(Chassis, 0x300, {"NeverD", "1.0", "ND-CHASSIS-0001"});
  Add({127, 4, 0, 0}, 0x7f00, {});
  llvm::support::endian::write32le(F.Raw.data() + 4, F.Raw.size() - 8);
  return F;
}
} // namespace neverd::emulation::smbiosfixture
#endif
