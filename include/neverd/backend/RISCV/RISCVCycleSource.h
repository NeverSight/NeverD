//===- RISCVCycleSource.h - Native cycle counter reads --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_RISCV_RISCVCYCLESOURCE_H
#define NEVERD_BACKEND_RISCV_RISCVCYCLESOURCE_H

#include "neverd/ir/intrinsics/RISCV/RISCVCycle.h"

#include <string>

namespace neverd::riscv {
inline std::string cycleSource(const CycleRead &Read) {
  return std::string("(__extension__ ({ __UINTPTR_TYPE__ __nd_cycle; ") +
         "__asm__ volatile (\"" + Read.Mnemonic +
         " %0\" : \"=r\" (__nd_cycle)); __nd_cycle; }))";
}
} // namespace neverd::riscv

#endif // NEVERD_BACKEND_RISCV_RISCVCYCLESOURCE_H
