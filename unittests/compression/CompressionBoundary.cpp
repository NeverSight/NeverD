//===- CompressionBoundary.cpp - Shared zlib consumer ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decodes independently supplied zlib frames through a shared library.
///
//===----------------------------------------------------------------------===//

#include <zlib.h>

extern "C"
#ifdef _WIN32
    __declspec(dllexport)
#endif
    int decodeFrame(unsigned char *Output, unsigned long *Length,
                    const unsigned char *Input, unsigned long Size) {
  return uncompress(Output, Length, Input, Size);
}
