//===- MIPS16IndirectCallTransport.h - o32 interworking calls -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_MIPS_MIPS16INDIRECTCALLTRANSPORT_H
#define NEVERD_IR_LOW_MIPS_MIPS16INDIRECTCALLTRANSPORT_H

#include "neverd/ir/low/LowIR.h"

#include <array>

namespace neverd {
struct BinaryImage;
class ExecutableCodeOwnerIndex;

/// The stub enters in classic mode with the tagged function pointer in v0.
/// Effects retain the actual indirect call, the t9 delay-slot copy and the
/// result moves after a returning call. A zero argument width denotes no
/// floating register transfer, as in an o32 variadic call.
struct MIPS16IndirectCallTransport {
  std::array<unsigned, 2> FloatingPrefix{};
  unsigned ResultBytes = 0;
  unsigned ResultElements = 0;
  std::vector<LowOp> Effects;
};

std::optional<MIPS16IndirectCallTransport> proveMIPS16IndirectCallTransport(
    const BinaryImage &Image, va_t Entry,
    const ExecutableCodeOwnerIndex *Owners = nullptr);
} // namespace neverd

#endif
