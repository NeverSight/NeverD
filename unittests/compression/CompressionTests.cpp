//===- CompressionTests.cpp - Static compression interoperability ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks valid and malformed frames across independently linked consumers.
///
//===----------------------------------------------------------------------===//

#include <array>
#include <cstring>
#include <iostream>
#include <vector>
#include <zlib.h>

extern "C" int decodeFrame(unsigned char *Output, unsigned long *Length,
                           const unsigned char *Input, unsigned long Size);

int main() {
  // RFC 1950 frame for "hello": independent bytes, not this encoder's output.
  constexpr unsigned char Frame[]{0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9,
                                  0x07, 0x00, 0x06, 0x2c, 0x02, 0x15};
  std::array<unsigned char, 4096> Output{};
  unsigned long Length = Output.size();
  if (std::strcmp(zlibVersion(), "1.3.2") ||
      decodeFrame(Output.data(), &Length, Frame, sizeof(Frame)) != Z_OK ||
      Length != 5 || std::memcmp(Output.data(), "hello", Length))
    return 1;

  std::array<unsigned char, 4096> Input{};
  for (size_t I = 0; I < Input.size(); ++I)
    Input[I] = static_cast<unsigned char>(I * 37);
  std::vector<unsigned char> Compressed(compressBound(Input.size()));
  unsigned long Size = Compressed.size();
  if (compress2(Compressed.data(), &Size, Input.data(), Input.size(), 6) !=
      Z_OK)
    return 2;
  Length = Output.size();
  if (decodeFrame(Output.data(), &Length, Compressed.data(), Size) != Z_OK ||
      Length != Input.size() || Output != Input)
    return 3;
  Compressed[Size - 1] ^= 1;
  Length = Output.size();
  if (decodeFrame(Output.data(), &Length, Compressed.data(), Size) !=
      Z_DATA_ERROR)
    return 4;
  std::cout << "static zlib: reference frame, cross-library roundtrip and "
               "checksum rejection passed\n";
}
