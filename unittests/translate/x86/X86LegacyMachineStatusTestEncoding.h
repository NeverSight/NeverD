//===- X86LegacyMachineStatusTestEncoding.h - CR0 bytes --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent machine-status opcodes and protected operand/address widths.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYMACHINESTATUSTESTENCODING_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYMACHINESTATUSTESTENCODING_H

#include <cstdint>
#include <vector>

namespace neverd::test {

// Forms: SMSW, LMSW, CLTS, CLAC, STAC. Memory uses [EBX]/[BX].
inline std::vector<uint8_t>
legacyMachineStatusBytes(unsigned Form, bool Code32 = true,
                         bool Operand32 = true, bool Address32 = true,
                         int Register = 6, unsigned Prefix = 0) {
  std::vector<uint8_t> B;
  if (Prefix)
    B.push_back(Prefix);
  if (Code32 != Operand32)
    B.push_back(0x66);
  if (Code32 != Address32)
    B.push_back(0x67);
  B.insert(B.end(), {0x0f, uint8_t(Form == 2 ? 6 : 1)});
  if (Form >= 3)
    B.push_back(Form == 3 ? 0xca : 0xcb);
  else if (Form != 2)
    B.push_back((Form == 0 ? 0x20 : 0x30) |
                (Register < 0 ? Address32 ? 3 : 7 : 0xc0 | Register));
  return B;
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYMACHINESTATUSTESTENCODING_H
