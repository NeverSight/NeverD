//===- X86LegacyDescriptorTestEncoding.h - Query bytes ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Literal descriptor opcodes with independent operand/address size prefixes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYDESCRIPTORTESTENCODING_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYDESCRIPTORTESTENCODING_H

#include <cstdint>
#include <vector>

namespace neverd::test {

inline std::vector<uint8_t>
legacyDescriptorBytes(unsigned Form, bool Code32 = true, bool Operand32 = true,
                      bool Address32 = true, unsigned Destination = 0,
                      int Source = 6, unsigned Prefix = 0) {
  std::vector<uint8_t> Bytes;
  if (Prefix)
    Bytes.push_back(Prefix);
  if (Code32 != Operand32)
    Bytes.push_back(0x66);
  if (Code32 != Address32)
    Bytes.push_back(0x67);
  Bytes.push_back(0x0f);
  Bytes.push_back(Form < 2 ? 2 + Form : 0);
  const unsigned Reg = Form < 2 ? Destination : Form + 2;
  // Memory uses [EBX] or [BX], avoiding a destination-dependent address.
  Bytes.push_back((Reg << 3) |
                  (Source < 0 ? Address32 ? 3 : 7 : 0xc0 | Source));
  return Bytes;
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYDESCRIPTORTESTENCODING_H
