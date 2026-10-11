//===- DarwinEntropyTestData.h - Explicit replay inputs --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DARWINENTROPYTESTDATA_H
#define NEVERD_TESTS_DARWINENTROPYTESTDATA_H
#include "neverd/emulation/DarwinSystemOptions.h"
namespace neverd::emulation::darwin_test {
// Independently specified exact replay data; not a native RNG oracle.
inline DarwinSystemOptions entropyReplayOptions() {
  DarwinSystemOptions Options;
  std::vector<uint8_t> Maximum(256);
  for (unsigned I = 0; I != Maximum.size(); ++I)
    Maximum[I] = uint8_t(I);
  Options.EntropyReads = std::vector<std::vector<uint8_t>>{
      {0xde, 0xad, 0xbe, 0xef}, {0, 0xff, 0x80, 0xa5}, {0x7f}, Maximum};
  return Options;
}
inline std::string entropyReplayJSON() {
  std::string Hex;
  constexpr char Digits[] = "0123456789abcdef";
  for (unsigned I = 0; I != 256; ++I) {
    Hex += Digits[I >> 4];
    Hex += Digits[I & 15];
  }
  return "{\"entropy_reads\":[\"deadbeef\",\"00ff80a5\",\"7f\",\"" + Hex +
         "\"]}";
}
} // namespace neverd::emulation::darwin_test
#endif
