//===- X86HleTestCases.h - Independent HLE encoding corpus -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original RCX-source/RDI-address forms, shared by parser and export tests.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_X86_X86HLETESTCASES_H
#define NEVERD_UNITTESTS_LIFT_X86_X86HLETESTCASES_H

#include <cstdint>
#include <utility>
#include <vector>

namespace neverd::test {

struct X86HleTestCase {
  std::vector<uint8_t> Bytes;
  unsigned Operation;
  unsigned Width;
  bool Acquire;
};

inline std::vector<X86HleTestCase> x86HleTestCases() {
  std::vector<X86HleTestCase> Cases;
  // Independently transcribed opcode and /digit fields; no production encoder.
  constexpr unsigned Opcodes[] = {
      0x00, 0x10, 0x20, 0x1bb, 0x1b3, 0x1ab, 0x1b0, 0x1c7, 0xfe, 0xfe,
      0xf6, 0xf6, 0x08, 0x18,  0x28,  0x30,  0x1c0, 0x86,  0x88};
  for (unsigned Op = 0; Op != 19; ++Op)
    for (unsigned Width : {1u, 2u, 4u, 8u}) {
      if ((Op >= 3 && Op <= 5 && Width == 1) || (Op == 7 && Width != 8))
        continue;
      for (bool Acquire : {false, true}) {
        if (Op == 18 && Acquire)
          continue;
        std::vector<uint8_t> B{uint8_t(Acquire ? 0xf2 : 0xf3)};
        if (Op != 18)
          B.push_back(0xf0);
        if (Width == 2)
          B.push_back(0x66);
        if (Width == 8 && Op != 7)
          B.push_back(0x48);
        unsigned Code = Opcodes[Op];
        if ((Op < 3 || Op == 6 || Op >= 8) && Width != 1)
          ++Code;
        if (Code > 255)
          B.push_back(0x0f);
        B.push_back(Code & 255);
        const unsigned Ext = Op == 9 ? 0 : Op == 10 ? 3 : Op == 11 ? 2 : 1;
        B.push_back(7 | (Ext << 3));
        Cases.push_back({std::move(B), Op, Width, Acquire});
      }
    }
  return Cases;
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_LIFT_X86_X86HLETESTCASES_H
