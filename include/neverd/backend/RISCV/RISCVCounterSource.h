//===- RISCVCounterSource.h - Native counter reads --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_RISCV_RISCVCOUNTERSOURCE_H
#define NEVERD_BACKEND_RISCV_RISCVCOUNTERSOURCE_H

#include "neverd/ir/intrinsics/RISCV/RISCVCounter.h"

#include "llvm/ADT/Twine.h"

#include <string>

namespace neverd::riscv {
inline std::string counterSource(const CounterRead &Read) {
  // Preserve the source spelling of the established cycle projection.
  const char *Local =
      Read.Kind == Counter::Cycle ? "__nd_cycle" : "__nd_counter";
  return (llvm::Twine("(__extension__ ({ __UINTPTR_TYPE__ ") + Local +
          "; __asm__ volatile (\"" + Read.Mnemonic + " %0\" : \"=r\" (" +
          Local + ")); " + Local + "; }))")
      .str();
}
} // namespace neverd::riscv

#endif // NEVERD_BACKEND_RISCV_RISCVCOUNTERSOURCE_H
