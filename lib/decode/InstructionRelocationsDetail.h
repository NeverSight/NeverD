//===- InstructionRelocationsDetail.h - Per-ISA relocation fields -*- C++ -*-//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The instruction-set specific part of instructionRelocations: operand
/// fields whose address or scalar role depends on the encoding of one ISA.
/// Each ISA lives in its own <ISA>/<ISA>InstructionRelocations.cpp.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_DECODE_INSTRUCTIONRELOCATIONSDETAIL_H
#define NEVERD_LIB_DECODE_INSTRUCTIONRELOCATIONSDETAIL_H

#include "neverd/decode/InstructionRelocations.h"

namespace neverd {
namespace instruction_relocations_detail {

/// i386 and x86-64 fields of \p DI (X86/X86InstructionRelocations.cpp).
void addX86Fields(const BinaryImage &Img, const DecodedInsn &DI,
                  InstructionRelocations &Result);

/// maxAdjacentRelocatedFields for i386 and x86-64
/// (X86/X86InstructionRelocations.cpp).
std::optional<unsigned> maxAdjacentX86RelocatedFields(Arch A, unsigned Width);

/// Whether this x86 relocation addresses memory narrower than a pointer.
bool isX86NarrowMemoryDisplacement(const BinaryImage &Img,
                                   const DecodedInsn &DI, va_t FieldVA);

/// AArch64 fields of \p DI (AArch64/AArch64InstructionRelocations.cpp).
void addAArch64Fields(const BinaryImage &Img, const DecodedInsn &DI,
                      InstructionRelocations &Result);

/// ARM fields of \p DI (ARM/ARMInstructionRelocations.cpp).
void addARMFields(const BinaryImage &Img, const DecodedInsn &DI,
                  InstructionRelocations &Result);

} // namespace instruction_relocations_detail
} // namespace neverd

#endif // NEVERD_LIB_DECODE_INSTRUCTIONRELOCATIONSDETAIL_H
