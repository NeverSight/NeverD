//===- X86IcedScan.h - Bounded iced scan adapter -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Private x86 classification adapter; declined inputs keep Capstone decoding.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_DECODE_X86ICEDSCAN_H
#define NEVERD_DECODE_X86ICEDSCAN_H
#include "capstone/capstone.h"

#include <cstddef>
#include <cstdint>
namespace neverd::x86iced {
class Scanner {
public:
  explicit Scanner(unsigned Bits);
  ~Scanner();
  Scanner(const Scanner &) = delete;
  Scanner &operator=(const Scanner &) = delete;
  static bool isCandidate(const uint8_t *Bytes, size_t Len) {
    return Bytes && Len && ((Bytes[0] & 0xfe) == 0xc4 || Bytes[0] == 0x62);
  }
  /// Bind only to a matching x86 handle. Detach before returning it to a pool.
  bool attach(csh Handle);
  static void detach(csh Handle);
  /// Return (Capstone ID << 8) | length, or zero for ordinary Capstone
  /// decoding.
  uint32_t scan(const uint8_t *Bytes, size_t Len);

private:
  static uint32_t scanCallback(void *Context, const uint8_t *Bytes, size_t Len);
  unsigned Bitness;
  void *State;
};
} // namespace neverd::x86iced
#endif // NEVERD_DECODE_X86ICEDSCAN_H
