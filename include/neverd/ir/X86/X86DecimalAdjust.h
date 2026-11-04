//===- X86DecimalAdjust.h - Legacy decimal contracts ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared opcode, undefined-flag and zero-base rules for legacy adjustments.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X86DECIMALADJUST_H
#define NEVERD_IR_X86_X86DECIMALADJUST_H

#include <cstdint>
#include <optional>

namespace neverd {

enum class X86DecimalAdjustOperation : uint8_t {
  DAA = 0x27,
  DAS = 0x2f,
  AAA = 0x37,
  AAS = 0x3f,
  AAM = 0xd4,
  AAD = 0xd5,
};

inline std::optional<X86DecimalAdjustOperation>
x86DecimalAdjustOperation(uint8_t Opcode) {
  switch (Opcode) {
  case 0x27:
  case 0x2f:
  case 0x37:
  case 0x3f:
  case 0xd4:
  case 0xd5:
    return static_cast<X86DecimalAdjustOperation>(Opcode);
  default:
    return std::nullopt;
  }
}

struct X86DecimalAdjustInstruction {
  X86DecimalAdjustOperation Operation;
  uint8_t Base = 10;

  bool hasBase() const {
    return Operation == X86DecimalAdjustOperation::AAM ||
           Operation == X86DecimalAdjustOperation::AAD;
  }

  bool raisesDivideError() const {
    return Operation == X86DecimalAdjustOperation::AAM && Base == 0;
  }

  /// Only these bits are supplied by an explicit CPU choice on completion.
  /// The other arithmetic flags are computed; faults preserve the input.
  uint32_t undefinedFlagsMask() const {
    using Op = X86DecimalAdjustOperation;
    switch (Operation) {
    case Op::AAA:
    case Op::AAS:
      return 0x8c4; // OF/SF/ZF/PF.
    case Op::DAA:
    case Op::DAS:
      return 0x800; // OF.
    case Op::AAM:
    case Op::AAD:
      return 0x811; // OF/AF/CF.
    }
    return 0;
  }
};

} // namespace neverd

#endif // NEVERD_IR_X86_X86DECIMALADJUST_H
