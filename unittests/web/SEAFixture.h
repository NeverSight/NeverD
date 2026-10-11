//===- SEAFixture.h - Inert SEA test layouts ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Inert SEA test layouts.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neverd::web::sea_test {
inline void put(std::string &B, uint64_t At, uint64_t V, unsigned Width) {
  if (At > B.size() || Width > B.size() - At)
    throw std::out_of_range("fixture");
  for (unsigned I = 0; I < Width; ++I)
    B[At + I] = char(V >> (8 * I));
}
inline void append(std::string &B, uint64_t V, unsigned Width = 8) {
  const auto At = B.size();
  B.resize(At + Width);
  put(B, At, V, Width);
}
inline void string(std::string &B, std::string_view S) {
  append(B, S.size());
  B += S;
}
inline std::string blob(uint32_t Flags = 0,
                        std::string_view Main = "module.exports = 42;") {
  std::string B;
  append(B, 0x143da20, 4);
  append(B, Flags, 4);
  string(B, "/CANARY_PRIVATE/main.js");
  string(B, Main);
  if (Flags & 4)
    string(B, "CANARY_OPAQUE_V8");
  if (Flags & 8) {
    append(B, 2);
    string(B, "CANARY_KEY");
    string(B, "asset-one");
    string(B, "../../CANARY_OUTSIDE");
    string(B, "asset-two");
  }
  return B;
}
// Deliberately synthetic headers, not compiler/runtime qualification.
inline std::string elf(std::string_view Blob, bool ARM = false) {
  std::string B(4096 + 28 + ((Blob.size() + 3) & ~size_t(3)), '\0');
  B.replace(0, 7, "\177ELF\2\1\1", 7);
  put(B, 16, 2, 2);
  put(B, 18, ARM ? 183 : 62, 2);
  put(B, 20, 1, 4);
  put(B, 32, 64, 8);
  put(B, 52, 64, 2);
  put(B, 54, 56, 2);
  put(B, 56, 2, 2);
  put(B, 64, 1, 4);
  put(B, 68, 4, 4);
  put(B, 80, 0x400000, 8);
  put(B, 96, B.size(), 8);
  put(B, 104, B.size(), 8);
  put(B, 112, 4096, 8);
  put(B, 120, 4, 4);
  put(B, 128, 4096, 8);
  put(B, 136, 0x401000, 8);
  put(B, 152, B.size() - 4096, 8);
  put(B, 160, B.size() - 4096, 8);
  put(B, 168, 4, 8);
  put(B, 4096, 14, 4);
  put(B, 4100, Blob.size(), 4);
  B.replace(4108, 14, "NODE_SEA_BLOB\0", 14);
  B.replace(4124, Blob.size(), Blob);
  return B;
}
inline std::string macho(std::string_view Blob, bool ARM = false) {
  std::string B(4096 + Blob.size(), '\0');
  put(B, 0, 0xfeedfacf, 4);
  put(B, 4, ARM ? 0x100000c : 0x1000007, 4);
  put(B, 12, 2, 4);
  put(B, 16, 1, 4);
  put(B, 20, 152, 4);
  put(B, 32, 0x19, 4);
  put(B, 36, 152, 4);
  B.replace(40, 8, "NODE_SEA");
  put(B, 56, 0x100001000, 8);
  put(B, 64, Blob.size(), 8);
  put(B, 72, 4096, 8);
  put(B, 80, Blob.size(), 8);
  put(B, 96, 1, 4);
  B.replace(104, 15, "__NODE_SEA_BLOB");
  B.replace(120, 8, "NODE_SEA");
  put(B, 136, 0x100001000, 8);
  put(B, 144, Blob.size(), 8);
  put(B, 152, 4096, 4);
  B.replace(4096, Blob.size(), Blob);
  return B;
}
inline std::string pe(std::string_view Blob, bool ARM = false) {
  std::string B(1024 + Blob.size(), '\0');
  B.replace(0, 2, "MZ");
  put(B, 60, 128, 4);
  B.replace(128, 4, "PE\0\0", 4);
  put(B, 132, ARM ? 0xaa64 : 0x8664, 2);
  put(B, 134, 1, 2);
  put(B, 148, 240, 2);
  put(B, 152, 0x20b, 2);
  put(B, 212, 512, 4);
  put(B, 260, 16, 4);
  put(B, 280, 4096, 4);
  put(B, 284, B.size() - 512, 4);
  B.replace(392, 5, ".rsrc");
  put(B, 400, B.size() - 512, 4);
  put(B, 404, 4096, 4);
  put(B, 408, B.size() - 512, 4);
  put(B, 412, 512, 4);
  put(B, 526, 1, 2);
  put(B, 528, 10, 4);
  put(B, 532, 0x80000020, 4);
  put(B, 556, 1, 2);
  put(B, 560, 0x80000080, 4);
  put(B, 564, 0x80000040, 4);
  put(B, 590, 1, 2);
  put(B, 592, 0, 4);
  put(B, 596, 96, 4);
  put(B, 608, 4608, 4);
  put(B, 612, Blob.size(), 4);
  put(B, 640, 13, 2);
  for (size_t I = 0; I < 13; ++I)
    put(B, 642 + 2 * I, std::string_view("NODE_SEA_BLOB")[I], 2);
  B.replace(1024, Blob.size(), Blob);
  return B;
}
} // namespace neverd::web::sea_test
