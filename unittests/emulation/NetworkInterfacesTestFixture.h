///
/// \file
/// NetworkInterfacesTestFixture.
///

#ifndef NEVERD_TEST_NETWORK_INTERFACES_FIXTURE_H
#define NEVERD_TEST_NETWORK_INTERFACES_FIXTURE_H
#include "neverd/emulation/DriverSession.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/JSON.h"
namespace neverd::emulation::test {
inline std::vector<uint8_t> networkRow(unsigned Index, unsigned Level) {
  std::vector<uint8_t> R(DriverNetworkRowSize, 0);
  auto W = [&](unsigned At, uint32_t V) {
    llvm::support::endian::write32le(R.data() + At, V);
  };
  auto Q = [&](unsigned At, uint64_t V) {
    llvm::support::endian::write64le(R.data() + At, V);
  };
  Q(0, (uint64_t(6) << 48) | (uint64_t(Index) << 24));
  W(8, Index);
  R[12] = Index;
  R[0x490] = 0x55;
  for (auto At : {0x1c, 0x21e}) {
    const std::string Text = (At == 0x1c ? "Ethernet " : "Observed adapter ") +
                             std::to_string(Index);
    for (size_t I = 0; I < Text.size(); ++I)
      llvm::support::endian::write16le(R.data() + At + I * 2, Text[I]);
  }
  W(0x420, 6);
  R[0x424] = R[0x444] = 2;
  R[0x429] = R[0x449] = Index;
  W(0x464, 1500);
  W(0x468, 6);
  W(0x474, 14);
  W(0x478, 2);
  R[0x480] = 1;
  W(0x484, Level == 1 ? 1 : 2);
  W(0x488, 1);
  W(0x48c, Level == 1 ? 1 : 2);
  W(0x4a0, 1);
  Q(0x4a8, 1000000000);
  Q(0x4b0, 1000000000);
  if (Level != 2)
    for (unsigned At = 0x4b8; At <= 0x540; At += 8)
      Q(At, 1000 * (Level + 1) + At + Index);
  // Explicit padding observations must survive verbatim row copying.
  R[0x481] = 0xa5;
  R[0x4a4] = 0x5a;
  return R;
}
inline DriverOptions networkInterfacesOptions(unsigned Count = 2) {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.NetworkInterfaces.emplace();
  for (unsigned L = 0; L < 3; ++L) {
    O.NetworkInterfaces->Levels[L].emplace();
    for (unsigned I = 1; I <= Count; ++I)
      O.NetworkInterfaces->Levels[L]->push_back(networkRow(I, L));
  }
  O.Unload = true;
  return O;
}
inline llvm::json::Object networkInterfacesScenario(unsigned Count = 2) {
  const auto O = networkInterfacesOptions(Count);
  llvm::json::Object N;
  const char *Names[] = {"normal", "raw", "normal_without_statistics"};
  for (unsigned L = 0; L < 3; ++L) {
    llvm::json::Array Rows;
    for (const auto &R : *O.NetworkInterfaces->Levels[L])
      Rows.push_back(llvm::toHex(R, true));
    N[Names[L]] = std::move(Rows);
  }
  return llvm::json::Object{
      {"windows_version",
       llvm::json::Object{{"major", 10}, {"minor", 0}, {"build", 19041}}},
      {"network_interfaces", std::move(N)},
      {"unload", true}};
}
} // namespace neverd::emulation::test
#endif
