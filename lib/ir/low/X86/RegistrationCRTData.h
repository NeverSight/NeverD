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
struct Import;
struct NdVar;

namespace registration_abi {

enum class CRTDataAccess : uint8_t { ReadArgumentWord, WriteInt };

std::optional<CRTDataAccess>
registrationCRTAccessorAccess(const Import &Imported);
bool isCheckedRegistrationCRTDataSlot(const BinaryImage &Image, va_t Slot);

/// One ordinary block's authenticated CRT data addresses. Receipts never feed
/// frame loads, scalar results, cached CALL targets or a native memory
/// contract.
class CRTDataProof {
  struct Pointer {
    CRTDataAccess Access;
    uint16_t Width;
  };
  std::array<std::optional<CRTDataAccess>, 8> Registers;
  std::map<uint64_t, Pointer> Temps;
  va_t Instruction = InvalidVA;
  bool Active = false;

  void clear();
  std::optional<Pointer> read(const NdVar &Var) const;
  void write(const NdVar &Var, std::optional<Pointer> Value);

public:
  bool active() const { return Active; }
  bool eaxHoldsDataAddress() const;
  /// The checked no-argument accessor returned its CRT-owned cell in EAX.
  void recordAccessorReturn(CRTDataAccess Access, const NdVar &Output);

  /// AuthenticatedDataLoad identifies a direct read of an eager CRT int import.
  /// A live binding is required to introduce its runtime address.
  /// ClosedAccess permits a checked read or a write to allocated private
  /// stack bytes. Neither supplies a new CRT pointer.
  bool transfer(const LowOp &Op, bool AuthenticatedDataLoad, bool BindingsLive,
                bool ClosedAccess, size_t &Work,
                std::optional<CRTDataAccess> &CurrentAccess);
};

} // namespace registration_abi
} // namespace neverd

#endif // NEVERD_IR_LOW_X86_REGISTRATIONCRTDATA_H
