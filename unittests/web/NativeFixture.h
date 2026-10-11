//===- NativeFixture.h - Immutable native handoff test fixtures --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Immutable native handoff test fixtures.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace neverd::web::test {
// Synthetic inert images, constructed directly as little-endian bytes. Their
// only function returns 42; no compiler, linker or target execution is needed.
inline void nativePut(std::string &Bytes, uint64_t At, uint64_t Value,
                      unsigned Width) {
  if (At > Bytes.size() || Width > Bytes.size() - At)
    throw std::out_of_range("native fixture range");
  for (unsigned I = 0; I < Width; ++I)
    Bytes[At + I] = char(Value >> (I * 8));
}
inline std::string nativeCode(bool ARM = false) {
  return ARM ? std::string("\x40\x05\x80\x52\xc0\x03\x5f\xd6", 8)
             : std::string("\xb8\x2a\0\0\0\xc3", 6);
}
inline std::string nativeELF(bool ARM = false) {
  std::string B(512, '\0');
  B.replace(0, 7, "\177ELF\2\1\1", 7);
  nativePut(B, 16, 2, 2);
  nativePut(B, 18, ARM ? 183 : 62, 2);
  nativePut(B, 20, 1, 4);
  nativePut(B, 24, 0x400100, 8);
  nativePut(B, 32, 64, 8);
  nativePut(B, 52, 64, 2);
  nativePut(B, 54, 56, 2);
  nativePut(B, 56, 1, 2);
  nativePut(B, 58, 64, 2);
  nativePut(B, 64, 1, 4);
  nativePut(B, 68, 5, 4);
  nativePut(B, 80, 0x400000, 8);
  nativePut(B, 88, 0x400000, 8);
  nativePut(B, 96, B.size(), 8);
  nativePut(B, 104, B.size(), 8);
  nativePut(B, 112, 4096, 8);
  const auto Code = nativeCode(ARM);
  B.replace(256, Code.size(), Code);
  B.replace(320, 18, "NATIVE_TEXT_CANARY");
  return B;
}
inline std::string nativePE() {
  std::string B(1024, '\0');
  B.replace(0, 2, "MZ");
  nativePut(B, 0x3c, 0x80, 4);
  B.replace(0x80, 4, "PE\0\0", 4);
  nativePut(B, 0x84, 0x8664, 2);
  nativePut(B, 0x86, 1, 2);
  nativePut(B, 0x94, 240, 2);
  nativePut(B, 0x96, 0x22, 2);
  const uint64_t O = 0x98;
  nativePut(B, O, 0x20b, 2);
  nativePut(B, O + 4, 512, 4);
  nativePut(B, O + 16, 4096, 4);
  nativePut(B, O + 20, 4096, 4);
  nativePut(B, O + 24, 0x140000000, 8);
  nativePut(B, O + 32, 4096, 4);
  nativePut(B, O + 36, 512, 4);
  nativePut(B, O + 40, 6, 2);
  nativePut(B, O + 48, 6, 2);
  nativePut(B, O + 56, 8192, 4);
  nativePut(B, O + 60, 512, 4);
  nativePut(B, O + 68, 3, 2);
  nativePut(B, O + 72, 0x100000, 8);
  nativePut(B, O + 80, 4096, 8);
  nativePut(B, O + 88, 0x100000, 8);
  nativePut(B, O + 96, 4096, 8);
  nativePut(B, O + 108, 16, 4);
  const uint64_t S = O + 240;
  B.replace(S, 5, ".text");
  nativePut(B, S + 8, 6, 4);
  nativePut(B, S + 12, 4096, 4);
  nativePut(B, S + 16, 512, 4);
  nativePut(B, S + 20, 512, 4);
  nativePut(B, S + 36, 0x60000020, 4);
  B.replace(512, 6, nativeCode());
  B.replace(544, 18, "NATIVE_TEXT_CANARY");
  return B;
}
inline std::string nativePELongStub() {
  auto B = nativePE();
  B.insert(0x80, 8192, '\0');
  nativePut(B, 0x3c, 0x2080, 4);
  const uint64_t O = 0x2098;
  nativePut(B, O + 16, 0x3000, 4);
  nativePut(B, O + 20, 0x3000, 4);
  nativePut(B, O + 56, 0x4000, 4);
  nativePut(B, O + 60, 0x2200, 4);
  nativePut(B, O + 240 + 12, 0x3000, 4);
  nativePut(B, O + 240 + 20, 0x2200, 4);
  return B;
}
inline std::string nativeMachO() {
  std::string B(512, '\0');
  nativePut(B, 0, 0xfeedfacf, 4);
  nativePut(B, 4, 0x01000007, 4);
  nativePut(B, 8, 3, 4);
  nativePut(B, 12, 2, 4);
  nativePut(B, 16, 2, 4);
  nativePut(B, 20, 176, 4);
  nativePut(B, 32, 0x19, 4);
  nativePut(B, 36, 152, 4);
  B.replace(40, 6, "__TEXT");
  nativePut(B, 56, 0x100000000, 8);
  nativePut(B, 64, 4096, 8);
  nativePut(B, 80, 512, 8);
  nativePut(B, 88, 5, 4);
  nativePut(B, 92, 5, 4);
  nativePut(B, 96, 1, 4);
  B.replace(104, 6, "__text");
  B.replace(120, 6, "__TEXT");
  nativePut(B, 136, 0x100000100, 8);
  nativePut(B, 144, 6, 8);
  nativePut(B, 152, 256, 4);
  nativePut(B, 156, 4, 4);
  nativePut(B, 168, 0x80000400, 4);
  nativePut(B, 184, 0x80000028, 4);
  nativePut(B, 188, 24, 4);
  nativePut(B, 192, 256, 8);
  B.replace(256, 6, nativeCode());
  B.replace(320, 18, "NATIVE_TEXT_CANARY");
  return B;
}
} // namespace neverd::web::test
