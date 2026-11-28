//===- RISCVCycle.h - RISC-V cycle counter observations -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_RISCV_RISCVCYCLE_H
#define NEVERD_IR_INTRINSICS_RISCV_RISCVCYCLE_H

#include "neverd/ir/intrinsics/Intrinsics.h"

namespace neverd::riscv {
struct CycleRead {
  bool HighHalf;
  const char *Mnemonic;

  bool admits(Arch A) const {
    return isRISCV32(A) || (!HighHalf && isRISCV64(A));
  }
};

inline const CycleRead *cycleRead(Intrinsic Id) {
  switch (Id) {
#define NEVERD_RISCV_CYCLE(Name, Value, Word, High, Text)                      \
  case Intrinsic::Name: {                                                      \
    static constexpr CycleRead Read{High, Text};                               \
    return &Read;                                                              \
  }
#include "neverd/ir/intrinsics/RISCV/RISCVCycle.def"
  default:
    return nullptr;
  }
}
} // namespace neverd::riscv

#endif // NEVERD_IR_INTRINSICS_RISCV_RISCVCYCLE_H
