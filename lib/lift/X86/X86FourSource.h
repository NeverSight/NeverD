//===- X86FourSource.h - Snapshot four architectural sources -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Pack independent register reads before committing an aliasing destination.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIFT_X86_X86FOURSOURCE_H
#define NEVERD_LIFT_X86_X86FOURSOURCE_H

#include "neverd/ir/intrinsics/X86/X86FourSource.h"
#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/lift/X86/X86Regs.h"

namespace neverd {
inline NdVar x86FourSourceSnapshot(X86Lifter::LiftState &S, unsigned Base,
                                   X86FourSourceControl C) {
  const auto Read = [&](unsigned I) {
    const auto Register = static_cast<x86_reg>(
        (C.scalar() ? X86_REG_XMM0 : X86_REG_ZMM0) + Base + I);
    return NdVar::reg(mapCapstoneReg(Register).Offset, C.sourceBytes());
  };
  NdVar Packed = Read(0);
  for (unsigned I = 1; I < X86FourSourceCount; ++I) {
    const auto Next = S.makeTemp((I + 1) * C.sourceBytes());
    S.emit(NdOp::CONCAT, Next, {Read(I), Packed});
    Packed = Next;
  }
  return Packed;
}
} // namespace neverd
#endif // NEVERD_LIFT_X86_X86FOURSOURCE_H
