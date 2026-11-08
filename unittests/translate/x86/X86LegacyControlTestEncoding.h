//===- X86LegacyControlTestEncoding.h - MOV-CR bytes ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent original MOV-CR encodings, including ignored mod/prefix bits.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYCONTROLTESTENCODING_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYCONTROLTESTENCODING_H

#include <cstdint>
#include <vector>

namespace neverd::test {

inline std::vector<uint8_t> legacyControlBytes(unsigned Control, bool Write,
                                               unsigned General = 6,
                                               unsigned Mod = 3,
                                               bool Overrides = false,
                                               uint8_t Prefix = 0) {
  std::vector<uint8_t> Bytes;
  if (Prefix)
    Bytes.push_back(Prefix);
  if (Overrides)
    Bytes.insert(Bytes.end(), {0x66, 0x67, 0x64});
  Bytes.insert(Bytes.end(), {0x0f, uint8_t(Write ? 0x22 : 0x20),
                             uint8_t((Mod << 6) | (Control << 3) | General)});
  return Bytes;
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYCONTROLTESTENCODING_H
