//===- AArch64AddressResult.h - Darwin address results ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_AARCH64ADDRESSRESULT_H
#define NEVERD_IR_LOW_AARCH64ADDRESSRESULT_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"

#include <capstone/aarch64.h>

namespace neverd {
class Decoder;
struct DecodedInsn;

/// Ordinary or authenticated return through LR in a decoded instruction
/// with operand detail.
bool isAArch64LinkRegisterReturn(const DecodedInsn &I);

/// Follow the value in Register at PC within this source function's bytes.
/// True when the function returns the address or stores it through an output.
bool darwinAddressValueEscapes(Decoder &Decode, llvm::ArrayRef<uint8_t> Bytes,
                               va_t Start, va_t PC, aarch64_reg Register);
} // namespace neverd

#endif // NEVERD_IR_LOW_AARCH64ADDRESSRESULT_H
