//===- X64PortIOEncoding.h - Authenticated port operands -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Share exact long-mode port decoding between source admission and execution.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64PORTIOENCODING_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64PORTIOENCODING_H

#include "X64SegmentEncoding.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::analysis::detail {

struct X64PortIOEncoding {
  bool Input = false, String = false, Repeat = false, Stack = false;
  unsigned Bytes = 1, AddressBytes = 8, Segment = 0;
  std::optional<uint8_t> ImmediatePort;
};

inline std::optional<X64PortIOEncoding>
x64PortIOEncoding(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.empty() || Bytes.size() > 15)
    return std::nullopt;
  std::vector<uint8_t> Element;
  bool Repeat = false;
  size_t P = 0;
  for (; P != Bytes.size(); ++P) {
    const auto B = Bytes[P];
    if (B == 0xf3) {
      if (Repeat)
        return std::nullopt;
      Repeat = true;
    } else if (B == 0x66 || B == 0x67 || B == 0x26 || B == 0x2e || B == 0x36 ||
               B == 0x3e || B == 0x64 || B == 0x65) {
      Element.push_back(B);
    } else {
      break;
    }
  }
  Element.insert(Element.end(), Bytes.begin() + P, Bytes.end());
  P = 0;
  X64SegmentPrefixes Prefix;
  if (!x64SegmentPrefixes(Element, P, Prefix))
    return std::nullopt;
  const uint8_t Opcode = Element[P++];
  const bool String = (Opcode & 0xfc) == 0x6c;
  const bool Immediate = (Opcode & 0xfc) == 0xe4;
  if ((!String && !Immediate && (Opcode & 0xfc) != 0xec) ||
      (!String && Repeat) || P + unsigned(Immediate) != Element.size())
    return std::nullopt;
  X64PortIOEncoding F;
  F.Input = !(Opcode & 2);
  F.String = String;
  F.Repeat = Repeat;
  // REX.W overrides 66, but port transfers remain capped at a dword.
  F.Bytes = !(Opcode & 1) ? 1 : Prefix.Operand16 && !(Prefix.Rex & 8) ? 2 : 4;
  F.AddressBytes = Prefix.AddressBytes;
  F.Segment = Prefix.Segment;
  F.Stack = Prefix.Stack;
  if (Immediate)
    F.ImmediatePort = Element[P];
  return F;
}

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64PORTIOENCODING_H
