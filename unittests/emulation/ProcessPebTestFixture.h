//===- ProcessPebTestFixture.h - Owned user environment observations ------===//
///
/// \file
/// Owned user environment observations.
///

#ifndef NEVERD_TEST_PROCESS_PEB_FIXTURE_H
#define NEVERD_TEST_PROCESS_PEB_FIXTURE_H
#include "neverd/emulation/DriverSession.h"

namespace neverd::emulation::test {
inline DriverOptions processPebOptions() {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.ProcessInventory = DriverProcessInventory{
      {4, 0, "System"}, {100, 4, "Client.exe"}, {200, 4, "Other.exe"}};
  for (size_t I = 1; I < 3; ++I) {
    auto &P = O.ProcessInventory->at(I);
    P.PebAddress = 0x400000;
    DriverProcessMemoryRegion R;
    R.Base = *P.PebAddress;
    R.Size = 4096;
    R.Access = DriverUserPageAccess::ReadWrite;
    R.Bytes.resize(4096);
    auto Put = [&](size_t At, uint64_t Value) {
      for (unsigned B = 0; B < 8; ++B)
        R.Bytes[At + B] = uint8_t(Value >> (8 * B));
    };
    // Explicit finite provider bytes in the 19041 x64 PEB/Ldr layout.
    // These observations do not imply a user loader or module discovery.
    R.Bytes[2] = I == 1 ? 0xa1 : 0xb1;
    Put(0x10, I == 1 ? 0x140000000 : 0x180000000);
    Put(0x18, 0x400100);
    Put(0x110, 0x400200);
    Put(0x118, 0x400200);
    Put(0x200, 0x400110);
    Put(0x208, 0x400110);
    Put(0x230, I == 1 ? 0x140000000 : 0x180000000);
    P.Memory = std::vector<DriverProcessMemoryRegion>{std::move(R)};
  }
  return O;
}
} // namespace neverd::emulation::test
#endif
