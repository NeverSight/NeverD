//===- MIPS16ReturnTransport.h - Proven FP result transfers -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_MIPS_MIPS16RETURNTRANSPORT_H
#define NEVERD_IR_LOW_MIPS_MIPS16RETURNTRANSPORT_H

#include "neverd/ir/low/LowIR.h"

namespace neverd {
struct BinaryImage;
class ExecutableCodeOwnerIndex;

/// A classic leaf copies one or two scalar results from o32 GPRs into FPRs,
/// then returns through its incoming link. The original operations include
/// the physical delay slot and any unpredictable FR=1 upper word. This is a
/// byte proof, independent of symbol spelling or a source declaration.
struct MIPS16ReturnTransport {
  uint16_t ElementBytes = 0;
  uint16_t Elements = 0;
  std::vector<LowOp> Effects;
};

std::optional<MIPS16ReturnTransport>
proveMIPS16ReturnTransport(const BinaryImage &Image, va_t Entry,
                           const ExecutableCodeOwnerIndex *Owners = nullptr);

} // namespace neverd

#endif
