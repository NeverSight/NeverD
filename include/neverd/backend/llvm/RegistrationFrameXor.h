//===- RegistrationFrameXor.h - PE32 cookie value candidates ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact low-word XOR recipes over frame addresses and one image cookie.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_REGISTRATIONFRAMEXOR_H
#define NEVERD_BACKEND_LLVM_REGISTRATIONFRAMEXOR_H

#include "neverd/backend/llvm/RegistrationFrameProofAddress.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>

namespace neverd::registration_frame {

struct XorValue {
  uint32_t Constant = 0;
  bool Cookie = false;
  std::set<Cell> Addresses;
  bool operator==(const XorValue &) const = default;
};

/// Candidate value equality through current SSA and every reaching store.
/// Intervening call/lifetime effects use AddressProof's checked projections;
/// an unknown effect cannot reuse an earlier spill. The consumer must still
/// close aliases and the full frame lifetime, authenticate those projections,
/// and prove that the original image cookie remains unchanged.
/// Neither a candidate nor cancellation alone grants native EH permission.
class XorProof {
public:
  XorProof(AddressProof &Addresses, va_t CookieVA, size_t &Work,
           bool &Exhausted)
      : Addresses(Addresses), CookieVA(CookieVA), Work(Work),
        Exhausted(Exhausted) {}

  std::optional<XorValue> evaluate(const llvm::Value *Value,
                                   unsigned Depth = 0);
  std::optional<XorValue> readWordAt(const Cell &Slot,
                                     const llvm::Instruction &Before);

private:
  std::optional<XorValue> evaluateImpl(const llvm::Value *Value,
                                       unsigned Depth);
  AddressProof &Addresses;
  va_t CookieVA;
  size_t &Work;
  bool &Exhausted;
  std::map<const llvm::Value *, std::optional<XorValue>> Values;
  std::set<const llvm::Value *> Active;
};

} // namespace neverd::registration_frame
#endif // NEVERD_BACKEND_LLVM_REGISTRATIONFRAMEXOR_H
