//===- CDeferredValue.h - Unknown scalar transport in C -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A PHI edge transports a value; it does not observe it. Carry availability
/// alongside mixed known/unknown PHIs and their scalar copies. Only a later
/// value read diagnoses missing machine state. Both C projections use this
/// finite worklist, including cycles and copies preceding their definitions.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_CDEFERREDVALUE_H
#define NEVERD_BACKEND_C_CDEFERREDVALUE_H

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd {

/// A known zero for AND, or all-one mask for OR, makes the other operand
/// unobservable. The caller must supply an already evaluated scalar for Known
/// and a missing-state expression with no native effects for Unknown.
inline std::string
shortCircuitUnknownBits(bool IsAnd, unsigned Bits,
                        const std::string &ResultType,
                        const std::string &Unknown, const std::string &Known,
                        std::optional<uint64_t> KnownConstant = std::nullopt) {
  const uint64_t WidthMask =
      Bits == 64 ? UINT64_MAX : (uint64_t(1) << Bits) - 1;
  const uint64_t Mask = IsAnd ? 0 : WidthMask;
  const std::string Constant =
      std::to_string(Mask) + (Bits == 64 ? "ULL" : "U");
  if (KnownConstant && (*KnownConstant & WidthMask) == Mask)
    return "((" + ResultType + ")" + Constant + ")";
  const std::string Carrier =
      "uint" + std::to_string(Bits == 1 ? 8 : Bits) + "_t";
  const std::string Value = "((" + Carrier + ")(" + Known + "))";
  return "((" + ResultType + ")(" + Value + " == " + Constant + " ? " +
         Constant + " : (((" + Carrier + ")(" + Unknown + "))" +
         (IsAnd ? " & " : " | ") + Value + ")))";
}

template <typename Key> struct CValueTransfer {
  Key Destination;
  std::optional<Key> Source;
  bool UnknownPhiInput = false;
};

template <typename Key>
std::set<Key>
deferredCValues(const std::vector<CValueTransfer<Key>> &Transfers) {
  std::set<Key> Deferred;
  std::vector<Key> Work;
  for (const auto &Transfer : Transfers) {
    if (Transfer.UnknownPhiInput &&
        Deferred.insert(Transfer.Destination).second)
      Work.push_back(Transfer.Destination);
  }
  if (Work.empty())
    return Deferred;
  std::map<Key, std::vector<Key>> Users;
  for (const auto &Transfer : Transfers)
    if (Transfer.Source)
      Users[*Transfer.Source].push_back(Transfer.Destination);
  while (!Work.empty()) {
    const Key Value = Work.back();
    Work.pop_back();
    const auto Found = Users.find(Value);
    if (Found == Users.end())
      continue;
    for (const Key &User : Found->second)
      if (Deferred.insert(User).second)
        Work.push_back(User);
  }
  return Deferred;
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_CDEFERREDVALUE_H
