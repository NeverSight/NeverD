//===- IntegerMemory.h - RISC-V and PowerPC memory instruction forms --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Selectors retain the architectural operation, width and ordering through
/// every IR. Native code generation and single-hart evaluation consume the same
/// description.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_INTEGERMEMORY_H
#define NEVERD_IR_INTRINSICS_INTEGERMEMORY_H

#include "neverd/Common.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include <optional>

namespace neverd::integer_memory {
enum class Operation {
  Fence,
  Reserve,
  Conditional,
  Add,
  Swap,
  Xor,
  Or,
  And,
  SMin,
  SMax,
  UMin,
  UMax
};
struct Form {
  Operation Op;
  const char *Name;
  unsigned Bytes;
  unsigned Ordering = 0;
  unsigned inputs() const {
    return Op == Operation::Fence ? 0 : Op == Operation::Reserve ? 1 : 2;
  }
  bool matchesSignature(unsigned NumInputs, unsigned OutputBytes,
                        unsigned RegisterBytes) const {
    return NumInputs == 2 + inputs() &&
           OutputBytes == (inputs() ? RegisterBytes : 0);
  }
};
constexpr bool owns(Intrinsic Id) {
  switch (Id) {
#define NEVERD_INTEGER_MEMORY_INTRINSIC(Name, Value, CName)                    \
  case Intrinsic::Name:
#include "neverd/ir/intrinsics/IntegerMemory.def"
    return true;
  default:
    return false;
  }
}
inline std::optional<Form> form(Arch A, Intrinsic Id, uint64_t Key) {
  using O = Operation;
  if (Id == Intrinsic::RISCVFence && isRISCV(A) && Key <= 255)
    return Form{O::Fence, "fence", 0};
  if (Id == Intrinsic::PPCBarrier && isPowerPC(A)) {
    switch (Key) {
#define NEVERD_PPC_BARRIER(Word, Selector, Mnemonic)                           \
  case Selector:                                                               \
    return Form{O::Fence, Mnemonic, 0};
#include "neverd/ir/intrinsics/IntegerMemory.def"
    default:
      return std::nullopt;
    }
  }
  if (Id == Intrinsic::PPCReservation && isPowerPC(A)) {
    switch (Key) {
#define NEVERD_PPC_RESERVATION(XO, Selector, Op, Bytes, Mnemonic)              \
  case Selector:                                                               \
    if (Bytes == 8 && !isPowerPC64(A))                                         \
      return std::nullopt;                                                     \
    return Form{O::Op, Mnemonic, Bytes};
#include "neverd/ir/intrinsics/IntegerMemory.def"
    default:
      return std::nullopt;
    }
  }
  if (Id != Intrinsic::RISCVAtomic || !isRISCV(A) || Key > 255 ||
      ((Key & 1) && isRISCV32(A)))
    return std::nullopt;
  const unsigned Bytes = Key & 1 ? 8 : 4, Order = Key >> 1 & 3;
  switch (Key >> 3) {
#define NEVERD_RISCV_ATOMIC(Funct5, Op, Mnemonic)                              \
  case Funct5:                                                                 \
    return Form{O::Op, Mnemonic, Bytes, Order};
#include "neverd/ir/intrinsics/IntegerMemory.def"
  default:
    return std::nullopt;
  }
}
} // namespace neverd::integer_memory
#endif // NEVERD_IR_INTRINSICS_INTEGERMEMORY_H
