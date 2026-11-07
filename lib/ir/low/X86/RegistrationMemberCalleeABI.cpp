//===- RegistrationMemberCalleeABI.cpp - PE32 scalar member arguments -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove object and scalar-argument member invocations in one bounded graph.
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"

#include "neverd/Limits.h"

namespace neverd {
std::optional<RegistrationLeafCalleeABI>
getCheckedX86RegistrationMemberCalleeABI(const BinaryImage &Image, va_t Target,
                                         size_t *CumulativeWork,
                                         RegistrationLeafResult ResultKind) {
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  // Prove each leaf or graph once. Its root's stack cleanup determines which
  // invocation form the common result admits.
  if (auto Leaf = registration_abi::checkedLeafCalleeABI(Image, Target, Work,
                                                         ResultKind))
    return Leaf;
  registration_abi::ObjectCallbackContext Context;
  auto Member = registration_abi::checkedObjectCallbackCalleeABI(Image, Target,
                                                                 Work, Context);
  if (!Member ||
      (Member->StackPopBytes &&
       (Member->StackPopBytes / 4 >= limits::kMaxCallArgs ||
        (Member->ECXReads.empty() && Member->ECXWrites.empty()))) ||
      (ResultKind == RegistrationLeafResult::Observed &&
       !Member->HasIndependentScalarReturn))
    return std::nullopt;
  return Member;
}
} // namespace neverd
