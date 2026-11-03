//===- X86LegacyEncoding.h - Protected operand grammar --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared strict prefixes, ModRM/SIB and 16/32-bit effective addresses.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X86LEGACYENCODING_H
#define NEVERD_LIB_TRANSLATE_X86_X86LEGACYENCODING_H

#include "neverd/ir/intrinsics/X86/X86SegmentRegisters.h"

#include "llvm/ADT/ArrayRef.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd::translate::detail {

struct X86LegacyAddress {
  X86SegmentRegister MemorySegment = X86SegmentRegister::DS;
  std::optional<unsigned> Register, Base, Index;
  uint32_t Displacement = 0;
  unsigned AddressBytes = 4, Scale = 1;

  uint32_t offset(const std::array<uint32_t, 8> &GPR) const {
    const uint32_t Value = Displacement + (Base ? GPR[*Base] : 0) +
                           (Index ? GPR[*Index] * Scale : 0);
    return AddressBytes == 4 ? Value : uint16_t(Value);
  }
};

class X86LegacyEncoding {
public:
  unsigned OperandBytes, AddressBytes;
  std::optional<X86SegmentRegister> Segment;
  uint8_t Group1Prefix = 0;

  X86LegacyEncoding(llvm::ArrayRef<uint8_t> Bytes, bool Code32)
      : OperandBytes(Code32 ? 4 : 2), AddressBytes(OperandBytes), Rest(Bytes),
        Valid(!Bytes.empty() && Bytes.size() <= 15) {
    bool OperandOverride = false, AddressOverride = false;
    while (!Rest.empty()) {
      const uint8_t B = Rest.front();
      if ((B == 0xf0 || B == 0xf2 || B == 0xf3) && !Group1Prefix) {
        Group1Prefix = B;
      } else if (B == 0x66 && !OperandOverride) {
        OperandOverride = true;
        OperandBytes = 6 - OperandBytes;
      } else if (B == 0x67 && !AddressOverride) {
        AddressOverride = true;
        AddressBytes = 6 - AddressBytes;
      } else if ((B == 0x26 || B == 0x2e || B == 0x36 || B == 0x3e ||
                  B == 0x64 || B == 0x65) &&
                 !Segment) {
        Segment = static_cast<X86SegmentRegister>(B >= 0x64 ? B - 0x60
                                                            : (B - 0x26) / 8);
      } else {
        break;
      }
      Rest = Rest.drop_front();
    }
  }

  uint32_t value(unsigned Bytes) {
    if (Rest.size() < Bytes) {
      Valid = false;
      return 0;
    }
    uint32_t V = 0;
    for (unsigned I = 0; I != Bytes; ++I)
      V |= uint32_t(Rest[I]) << (I * 8);
    Rest = Rest.drop_front(Bytes);
    return V;
  }

  uint8_t byte() { return value(1); }
  bool done() const { return Valid && Rest.empty(); }

  unsigned modRM(X86LegacyAddress &A) {
    A.AddressBytes = AddressBytes;
    const uint8_t B = byte();
    const unsigned Mod = B >> 6, RM = B & 7;
    if (Mod == 3) {
      A.Register = RM;
      return (B >> 3) & 7;
    }
    unsigned DispBytes = Mod == 1 ? 1 : Mod == 2 ? AddressBytes : 0;
    if (AddressBytes == 2) {
      if (Mod == 0 && RM == 6) {
        DispBytes = 2;
      } else {
        constexpr unsigned Bases[]{3, 3, 5, 5, 6, 7, 5, 3};
        A.Base = Bases[RM];
        if (RM < 4)
          A.Index = RM & 1 ? 7 : 6;
      }
    } else if (RM == 4) {
      const uint8_t SIB = byte();
      const unsigned Index = (SIB >> 3) & 7, Base = SIB & 7;
      A.Scale = 1u << (SIB >> 6);
      if (Index != 4)
        A.Index = Index;
      if (Mod == 0 && Base == 5)
        DispBytes = 4;
      else
        A.Base = Base;
    } else if (Mod == 0 && RM == 5) {
      DispBytes = 4;
    } else {
      A.Base = RM;
    }
    A.Displacement = value(DispBytes);
    if (DispBytes == 1)
      A.Displacement = int8_t(A.Displacement);
    A.MemorySegment =
        Segment.value_or(A.Base == 4 || A.Base == 5 ? X86SegmentRegister::SS
                                                    : X86SegmentRegister::DS);
    return (B >> 3) & 7;
  }

private:
  llvm::ArrayRef<uint8_t> Rest;
  bool Valid;
};

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYENCODING_H
