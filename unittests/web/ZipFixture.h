//===- ZipFixture.h - Inert ZIP32 test records ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// C++ ZIP record construction and independent stored-block deflate fixtures.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::web::test {
inline void zipPut(std::string &B, size_t At, uint64_t N, unsigned Width) {
  for (unsigned I = 0; I < Width; ++I)
    B.at(At + I) = char(N >> (8 * I));
}
inline uint32_t zipCRC(std::string_view Bytes) {
  uint32_t Result = 0xffffffff;
  for (const unsigned char C : Bytes) {
    Result ^= C;
    for (unsigned I = 0; I < 8; ++I)
      Result = (Result >> 1) ^ ((Result & 1) ? 0xedb88320 : 0);
  }
  return ~Result;
}
inline std::string zipDeflate(std::string_view Bytes) {
  std::string B;
  do {
    const auto N = std::min<size_t>(65535, Bytes.size());
    const auto At = B.size();
    B.resize(At + 5);
    B[At] = N == Bytes.size() ? 1 : 0;
    zipPut(B, At + 1, N, 2);
    zipPut(B, At + 3, (~N) & 0xffff, 2);
    B.append(Bytes.substr(0, N));
    Bytes.remove_prefix(N);
  } while (!Bytes.empty());
  return B;
}
struct ZipEntry {
  std::string Name, Bytes;
  uint16_t Method = 0, Flags = 0;
  uint32_t Mode = 0100644;
  bool SignedDescriptor = true;
  std::string Extra;
};
struct ZipFixture {
  std::string Bytes;
  std::vector<size_t> Local, Data, Central;
  size_t End = 0;
  explicit ZipFixture(const std::vector<ZipEntry> &Entries) {
    std::string Directory;
    for (const auto &E : Entries) {
      Local.push_back(Bytes.size());
      auto Payload = E.Method == 8 ? zipDeflate(E.Bytes) : E.Bytes;
      const auto CRC = zipCRC(E.Bytes);
      std::string H(30, '\0');
      zipPut(H, 0, 0x04034b50, 4);
      zipPut(H, 4, 20, 2);
      zipPut(H, 6, E.Flags, 2);
      zipPut(H, 8, E.Method, 2);
      if (!(E.Flags & 8)) {
        zipPut(H, 14, CRC, 4);
        zipPut(H, 18, Payload.size(), 4);
        zipPut(H, 22, E.Bytes.size(), 4);
      }
      zipPut(H, 26, E.Name.size(), 2);
      zipPut(H, 28, E.Extra.size(), 2);
      Bytes += H + E.Name + E.Extra;
      Data.push_back(Bytes.size());
      Bytes += Payload;
      if (E.Flags & 8) {
        std::string D(E.SignedDescriptor ? 16 : 12, '\0');
        const auto At = E.SignedDescriptor ? 4 : 0;
        if (E.SignedDescriptor)
          zipPut(D, 0, 0x08074b50, 4);
        zipPut(D, At, CRC, 4);
        zipPut(D, At + 4, Payload.size(), 4);
        zipPut(D, At + 8, E.Bytes.size(), 4);
        Bytes += D;
      }
      Central.push_back(Directory.size());
      std::string C(46, '\0');
      zipPut(C, 0, 0x02014b50, 4);
      zipPut(C, 4, (3 << 8) | 20, 2);
      zipPut(C, 6, 20, 2);
      zipPut(C, 8, E.Flags, 2);
      zipPut(C, 10, E.Method, 2);
      zipPut(C, 16, CRC, 4);
      zipPut(C, 20, Payload.size(), 4);
      zipPut(C, 24, E.Bytes.size(), 4);
      zipPut(C, 28, E.Name.size(), 2);
      zipPut(C, 30, E.Extra.size(), 2);
      zipPut(C, 38, uint64_t(E.Mode) << 16, 4);
      zipPut(C, 42, Local.back(), 4);
      Directory += C + E.Name + E.Extra;
    }
    for (auto &C : Central)
      C += Bytes.size();
    std::string E(22, '\0');
    zipPut(E, 0, 0x06054b50, 4);
    zipPut(E, 8, Entries.size(), 2);
    zipPut(E, 10, Entries.size(), 2);
    zipPut(E, 12, Directory.size(), 4);
    zipPut(E, 16, Bytes.size(), 4);
    Bytes += Directory;
    End = Bytes.size();
    Bytes += E;
  }
};
} // namespace neverd::web::test
