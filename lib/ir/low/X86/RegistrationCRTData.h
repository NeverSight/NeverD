//===- RegistrationCRTData.h - Private CRT data receipts ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bind runtime data addresses without adding frame values or callable facts.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONCRTDATA_H
#define NEVERD_IR_LOW_X86_REGISTRATIONCRTDATA_H

#include "neverd/Common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace neverd {
struct BinaryImage;
struct LowOp;
struct NdVar;

namespace registration_abi {

bool isCheckedRegistrationCRTDataSlot(const BinaryImage &Image, va_t Slot);

/// One ordinary block's exact CRT int addresses. Receipts never feed frame
/// loads, scalar results, cached CALL targets or a native memory contract.
class CRTDataStoreProof {
  struct Pointer {
    va_t Slot;
    uint16_t Width;
  };
  std::array<std::optional<va_t>, 8> Registers;
  std::map<uint64_t, Pointer> Temps;
  va_t Instruction = InvalidVA;
  bool Active = false;

  void clear();
  std::optional<Pointer> read(const NdVar &Var) const;
  void write(const NdVar &Var, std::optional<Pointer> Value);

public:
  /// AuthenticatedLoad names only this exact current LOAD's typed import
  /// receipt. A current binding is required to introduce its runtime address.
  bool transfer(const LowOp &Op, std::optional<va_t> AuthenticatedLoad,
                bool BindingsLive, size_t &Work, bool &DisjointStore);
};

} // namespace registration_abi
} // namespace neverd

#endif // NEVERD_IR_LOW_X86_REGISTRATIONCRTDATA_H
