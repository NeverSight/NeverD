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

#include <iterator>
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
  return Id == Intrinsic::RISCVAtomic || Id == Intrinsic::RISCVFence ||
         Id == Intrinsic::PPCReservation || Id == Intrinsic::PPCBarrier;
}
inline std::optional<Form> form(Arch A, Intrinsic Id, uint64_t Key) {
  using O = Operation;
  if (Id == Intrinsic::RISCVFence && isRISCV(A) && Key <= 255)
    return Form{O::Fence, "fence", 0};
  if (Id == Intrinsic::PPCBarrier && isPowerPC(A)) {
    constexpr const char *Names[] = {"sync", "lwsync", "isync", "eieio"};
    if (Key < std::size(Names))
      return Form{O::Fence, Names[Key], 0};
  }
  if (Id == Intrinsic::PPCReservation && isPowerPC(A) && Key < 4) {
    const bool Wide = Key & 1, Store = Key & 2;
    if (!Wide || isPowerPC64(A))
      return Form{Store ? O::Conditional : O::Reserve,
                  Store ? (Wide ? "stdcx." : "stwcx.")
                        : (Wide ? "ldarx" : "lwarx"),
                  Wide ? 8u : 4u};
  }
  if (Id != Intrinsic::RISCVAtomic || !isRISCV(A) || Key > 255 ||
      ((Key & 1) && A == Arch::RISCV32))
    return std::nullopt;
  const unsigned Bytes = Key & 1 ? 8 : 4, Order = Key >> 1 & 3;
  switch (Key >> 3) {
  case 0:
    return Form{O::Add, "amoadd", Bytes, Order};
  case 1:
    return Form{O::Swap, "amoswap", Bytes, Order};
  case 2:
    return Form{O::Reserve, "lr", Bytes, Order};
  case 3:
    return Form{O::Conditional, "sc", Bytes, Order};
  case 4:
    return Form{O::Xor, "amoxor", Bytes, Order};
  case 8:
    return Form{O::Or, "amoor", Bytes, Order};
  case 12:
    return Form{O::And, "amoand", Bytes, Order};
  case 16:
    return Form{O::SMin, "amomin", Bytes, Order};
  case 20:
    return Form{O::SMax, "amomax", Bytes, Order};
  case 24:
    return Form{O::UMin, "amominu", Bytes, Order};
  case 28:
    return Form{O::UMax, "amomaxu", Bytes, Order};
  default:
    return std::nullopt;
  }
}
} // namespace neverd::integer_memory
#endif // NEVERD_IR_INTRINSICS_INTEGERMEMORY_H
