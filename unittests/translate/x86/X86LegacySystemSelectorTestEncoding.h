//===- X86LegacySystemSelectorTestEncoding.h - LDTR/TR bytes ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent literal opcodes, operand width and address width.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYSYSTEMSELECTORTESTENCODING_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYSYSTEMSELECTORTESTENCODING_H

#include <cstdint>
#include <vector>

namespace neverd::test {

inline std::vector<uint8_t>
legacySystemSelectorBytes(unsigned Form, bool Code32 = true,
                          bool Operand32 = true, bool Address32 = true,
                          int Register = 6, unsigned Prefix = 0) {
  std::vector<uint8_t> B;
  if (Prefix)
    B.push_back(Prefix);
  if (Code32 != Operand32)
    B.push_back(0x66);
  if (Code32 != Address32)
    B.push_back(0x67);
  B.insert(B.end(), {0x0f, 0x00,
                     uint8_t((Form << 3) | (Register < 0 ? Address32 ? 3 : 7
                                                         : 0xc0 | Register))});
  return B;
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYSYSTEMSELECTORTESTENCODING_H
