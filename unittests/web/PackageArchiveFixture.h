//===- PackageArchiveFixture.h - Inert archive fixtures ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent USTAR records and stored-block gzip fixtures in C++.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_PACKAGEARCHIVEFIXTURE_H
#define NEVERD_TEST_PACKAGEARCHIVEFIXTURE_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace neverd::web::test {
inline void tarNumber(std::string &B, size_t At, size_t Width, uint64_t N) {
  B[At + Width - 1] = '\0';
  for (size_t I = Width - 1; I; --I) {
    B[At + I - 1] = char('0' + (N & 7));
    N >>= 3;
  }
}
inline void tarChecksum(std::string &B) {
  B.replace(148, 8, 8, ' ');
  uint64_t Sum = 0;
  for (size_t I = 0; I < 512; ++I)
    Sum += uint8_t(B[I]);
  tarNumber(B, 148, 7, Sum);
}
inline std::string tarMember(std::string_view Path, std::string_view Bytes,
                             char Type = '0', std::string_view Link = {}) {
  std::string B(512, '\0');
  B.replace(0, std::min<size_t>(100, Path.size()), Path.substr(0, 100));
  tarNumber(B, 100, 8, 0755);
  tarNumber(B, 108, 8, 0);
  tarNumber(B, 116, 8, 0);
  tarNumber(B, 124, 12, Bytes.size());
  tarNumber(B, 136, 12, 0);
  B[156] = Type;
  B.replace(157, std::min<size_t>(100, Link.size()), Link.substr(0, 100));
  B.replace(257, 8,
            "ustar\0"
            "00",
            8);
  tarChecksum(B);
  B += Bytes;
  B.resize((B.size() + 511) / 512 * 512, '\0');
  return B;
}
inline std::string pax(std::string_view Key, std::string_view Value) {
  const auto Record = std::string(Key) + "=" + std::string(Value) + "\n";
  size_t Length = Record.size() + 2;
  while (Length != Record.size() + 1 + std::to_string(Length).size())
    Length = Record.size() + 1 + std::to_string(Length).size();
  return std::to_string(Length) + " " + Record;
}
inline std::string finishTar(std::string Members) {
  Members.append(1024, '\0');
  return Members;
}
inline std::string storedGzip(std::string_view Input) {
  std::string B("\x1f\x8b\x08\0\0\0\0\0\0\xff", 10);
  auto LE = [&](uint32_t N, unsigned Count) {
    while (Count--) {
      B += char(N);
      N >>= 8;
    }
  };
  const auto Original = Input;
  do {
    const auto N = uint32_t(std::min<size_t>(65535, Input.size()));
    B += char(N == Input.size() ? 1 : 0);
    LE(N, 2);
    LE((~N) & 0xffff, 2);
    B.append(Input.substr(0, N));
    Input.remove_prefix(N);
  } while (!Input.empty());
  uint32_t CRC = 0xffffffffU;
  for (const unsigned char C : Original) {
    CRC ^= C;
    for (unsigned I = 0; I < 8; ++I)
      CRC = (CRC >> 1) ^ ((CRC & 1) ? 0xedb88320U : 0);
  }
  LE(~CRC, 4);
  LE(uint32_t(Original.size()), 4);
  return B;
}
} // namespace neverd::web::test

#endif // NEVERD_TEST_PACKAGEARCHIVEFIXTURE_H
