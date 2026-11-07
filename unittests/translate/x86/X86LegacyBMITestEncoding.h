//===- X86LegacyBMITestEncoding.h - Independent BMI fixtures --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Literal opcode/value cases shared by original CPU and exported-C tests.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYBMITESTENCODING_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYBMITESTENCODING_H

#include <array>
#include <cstdint>
#include <vector>

namespace neverd::test {

struct LegacyBMICase {
  const char *Name;
  uint8_t Map, PP, Opcode, Group;
  uint32_t Source, Control, Expected, DefinedFlags, UndefinedMask;
};

inline constexpr std::array<LegacyBMICase, 13> LegacyBMIForms{{
    {"andn", 2, 0, 0xf2, 0, 0x8000000f, 15, 0x80000000, 0x80, 0x14},
    {"bextr", 2, 0, 0xf7, 0, 0x8000000f, 0x402, 3, 0, 0x94},
    {"blsi", 2, 0, 0xf3, 3, 0x8000000c, 0, 4, 1, 0x14},
    {"blsmsk", 2, 0, 0xf3, 2, 0x8000000c, 0, 7, 0, 0x14},
    {"blsr", 2, 0, 0xf3, 1, 0x8000000c, 0, 0x80000008, 0x80, 0x14},
    {"bzhi", 2, 0, 0xf5, 0, 0x8000000f, 3, 7, 0, 0x14},
    {"mulx", 2, 3, 0xf6, 0, 0xffffffff, 0, 0xfffffffe, 0, 0},
    {"pdep", 2, 3, 0xf5, 0, 0xa5, 0xb, 0x85, 0, 0},
    {"pext", 2, 2, 0xf5, 0, 0xa5, 0x85, 0xb, 0, 0},
    {"rorx", 3, 3, 0xf0, 0, 0x80000001, 1, 0xc0000000, 0, 0},
    {"sarx", 2, 2, 0xf7, 0, 0x80000001, 33, 0xc0000000, 0, 0},
    {"shlx", 2, 1, 0xf7, 0, 0x80000001, 33, 2, 0, 0},
    {"shrx", 2, 3, 0xf7, 0, 0x80000001, 33, 0x40000000, 0, 0},
}};

inline std::vector<uint8_t> legacyBMIBytes(unsigned Form, unsigned Destination,
                                           unsigned VRegister, int Source,
                                           bool Address32 = true,
                                           uint8_t Immediate = 1) {
  const auto &F = LegacyBMIForms[Form];
  const unsigned V = F.Group ? Destination : Form == 9 ? 0 : VRegister;
  const unsigned Reg = F.Group ? F.Group : Destination;
  std::vector<uint8_t> Bytes{0xc4, uint8_t(0xe0 | F.Map),
                             uint8_t(((~V & 15) << 3) | F.PP), F.Opcode,
                             uint8_t((Reg << 3) | (Source >= 0 ? 0xc0 | Source
                                                   : Address32 ? 3
                                                               : 7))};
  if (Form == 9)
    Bytes.push_back(Immediate);
  return Bytes;
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYBMITESTENCODING_H
