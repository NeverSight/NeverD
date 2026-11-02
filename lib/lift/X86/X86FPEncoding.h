//===- X86FPEncoding.h - Shared legacy/VEX FP encoding contract -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate FP prefixes once, distinguishing GPR, vector and address
/// extension fields in legacy, REX, REX2, VEX2 and VEX3 encodings.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86FPENCODING_H
#define NEVERD_LIB_LIFT_X86_X86FPENCODING_H

#include "X86LiftDetail.h"

#include <cstddef>
#include <cstdint>

namespace neverd {

struct FPEncodingInfo {
  size_t OpcodeOffset = 0;
  uint8_t Opcode = 0;
  uint8_t ModRM = 0;
  uint8_t Segment = 0;
  uint16_t AddressSize = 0;
  bool Mode64 = false;
  bool W = false;
  bool L = false;
  uint8_t RegExtension = 0;
  uint8_t BaseExtension = 0;
  uint8_t IndexExtension = 0;
  uint8_t RawVvvv = 15;

  // REX2 extends GPRs to 32, but R4/B4 do not extend XMM registers.
  unsigned vectorRegIndex() const {
    return ((ModRM >> 3) & 7) | (RegExtension & 8);
  }
  unsigned vectorRMIndex() const { return (ModRM & 7) | (BaseExtension & 8); }
  unsigned gprRegIndex() const { return ((ModRM >> 3) & 7) | RegExtension; }
  unsigned gprRMIndex() const { return (ModRM & 7) | BaseExtension; }
  unsigned vvvvIndex() const { return (~RawVvvv) & (Mode64 ? 15 : 7); }
  bool unusedVvvv() const { return RawVvvv == 15; }
};

/// Map and PP describe the instruction family. W/L, used/unused vvvv and
/// operand roles remain family-owned. No unsupported prefix is discarded.
bool parseFPEncoding(const cs_insn *Insn, const cs_x86 &X86, Arch Target,
                     bool Vex, unsigned Map, unsigned PP,
                     FPEncodingInfo &Encoding);

bool matchesFPGPROperand(const cs_x86_op &Operand, unsigned Index,
                         unsigned Width);

inline bool validateFPEncodingTail(const cs_insn *Insn, const cs_x86 &X86,
                                   const FPEncodingInfo &Encoding,
                                   const cs_x86_op &Operand,
                                   size_t TrailingBytes = 0) {
  return validateCanonicalScalarConversionTail(
      Insn, X86, Encoding.OpcodeOffset + 2, Encoding.Segment, Encoding.Mode64,
      Encoding.AddressSize, Encoding.BaseExtension, Encoding.IndexExtension,
      Operand, TrailingBytes);
}

} // namespace neverd

#endif // NEVERD_LIB_LIFT_X86_X86FPENCODING_H
