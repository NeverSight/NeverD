//===- X86TransactionTestCases.h - Independent prefix cases ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original bytes and independently specified XBEGIN displacement widths.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_X86_X86TRANSACTIONTESTCASES_H
#define NEVERD_UNITTESTS_LIFT_X86_X86TRANSACTIONTESTCASES_H

#include <cstdint>
#include <utility>
#include <vector>

inline std::vector<std::pair<std::vector<uint8_t>, unsigned>>
x86TransactionTestPrefixes(bool Mode64) {
  std::vector<std::pair<std::vector<uint8_t>, unsigned>> Cases = {
      {{}, 4},
      {{0x66}, 2},
      {{0x67}, 4},
      {{0xf2}, 4},
      {{0xf3}, 4},
      {{0x26}, 4},
      {{0x2e}, 4},
      {{0x36}, 4},
      {{0x3e}, 4},
      {{0x64}, 4},
      {{0x65}, 4},
      {{0x66, 0x66}, 2},
      {{0x65, 0xf2, 0xf3, 0x67, 0x66}, 2}};
  if (Mode64) {
    for (unsigned Rex = 0x40; Rex <= 0x4f; ++Rex)
      Cases.push_back({{uint8_t(Rex)}, 4});
    Cases.insert(Cases.end(), {{{0x66, 0x48}, 4},
                               {{0x48, 0x66}, 2},
                               {{0x66, 0x40}, 2},
                               {{0x48, 0x66, 0x4f}, 4},
                               {{0x66, 0x4f, 0x40}, 2},
                               {{0x40, 0x65, 0x66, 0x4f, 0x67, 0x66}, 2},
                               {std::vector<uint8_t>(9, 0x40), 4}});
  }
  Cases.push_back({std::vector<uint8_t>(11, 0x66), 2});
  return Cases;
}

#endif // NEVERD_UNITTESTS_LIFT_X86_X86TRANSACTIONTESTCASES_H
