//===- RISCVCounter.h - RISC-V base counter observations -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_RISCV_RISCVCOUNTER_H
#define NEVERD_IR_INTRINSICS_RISCV_RISCVCOUNTER_H

#include "neverd/ir/intrinsics/Intrinsics.h"

namespace neverd::riscv {
enum class Counter { Cycle, Time, Instret, Count };

struct CounterRead {
  Counter Kind;
  bool HighHalf;
  const char *Mnemonic;

  bool admits(Arch A) const {
    return isRISCV32(A) || (!HighHalf && isRISCV64(A));
  }
};

inline const CounterRead *counterRead(Intrinsic Id) {
  switch (Id) {
#define NEVERD_RISCV_COUNTER(Name, Value, Word, Kind, High, Text)              \
  case Intrinsic::Name: {                                                      \
    static constexpr CounterRead Read{Counter::Kind, High, Text};              \
    return &Read;                                                              \
  }
#include "neverd/ir/intrinsics/RISCV/RISCVCounter.def"
  default:
    return nullptr;
  }
}
} // namespace neverd::riscv

#endif // NEVERD_IR_INTRINSICS_RISCV_RISCVCOUNTER_H
