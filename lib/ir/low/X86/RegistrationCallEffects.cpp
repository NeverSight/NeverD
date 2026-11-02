//===- RegistrationCallEffects.cpp - Current PE32 effects ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare source effects with independently reproved original callees.
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/RegistrationCallEffects.h"

#include "RegistrationABIPrivate.h"

#include "neverd/ir/RegistrationCall.h"
#include "neverd/ir/low/RegistrationABI.h"

#include <vector>

namespace neverd {
namespace {
using registration_abi::chargeCalleeWork;

bool sameExtent(const ExceptionAddressRange &A,
                const ExceptionAddressRange &B) {
  return A.Begin == B.Begin && A.End == B.End;
}
bool sameExtent(const RegistrationObjectExtent &A,
                const RegistrationObjectExtent &B) {
  return A == B;
}

template <typename T>
bool sameExtents(const std::vector<T> &Saved, const std::vector<T> &Current,
                 size_t &Work) {
  if (!chargeCalleeWork(Work, 1) || Saved.size() != Current.size() ||
      !chargeCalleeWork(Work, Saved.size()))
    return false;
  for (size_t I = 0; I != Saved.size(); ++I)
    if (!sameExtent(Saved[I], Current[I]))
      return false;
  return true;
}

template <typename T>
bool sameImageEffects(const RegistrationCalleeFrameContract &Saved,
                      const T &Current, size_t &Work) {
  return sameExtents(Saved.ImageReads, Current.ImageReads, Work) &&
         sameExtents(Saved.ImageWrites, Current.ImageWrites, Work) &&
         sameExtents(Saved.CallerPCWrites, Current.CallerPCWrites, Work) &&
         sameExtents(Saved.CodeRanges, Current.CodeRanges, Work);
}
} // namespace

bool hasSameX86RegistrationLeafEffects(
    const RegistrationCalleeFrameContract &Saved,
    const RegistrationLeafCalleeABI &Current, size_t &Work) {
  return chargeCalleeWork(Work, 1) && !Saved.isThrow() &&
         !Saved.isCxxParent() && !Saved.DoesNotReturn &&
         Saved.Target == Current.Target &&
         Saved.StackPopBytes == Current.StackPopBytes && !Saved.ThrownTypeVA &&
         !Saved.ThrownObjectSize && Saved.RuntimeThrowInfos.empty() &&
         sameExtents(Saved.ECXReads, Current.ECXReads, Work) &&
         sameExtents(Saved.ECXWrites, Current.ECXWrites, Work) &&
         sameImageEffects(Saved, Current, Work);
}

bool hasSameX86RegistrationThrowEffects(
    const RegistrationCalleeFrameContract &Saved,
    const RegistrationThrowCalleeABI &Current, size_t &Work) {
  return chargeCalleeWork(Work, 1) && Saved.isThrow() &&
         !Saved.isRuntimeThrow() && Saved.DoesNotReturn &&
         Saved.isRethrow() == Current.IsRethrow &&
         Saved.Target == Current.Target && !Saved.StackPopBytes &&
         !Saved.HasIndependentScalarReturn && Saved.ECXReads.empty() &&
         Saved.ECXWrites.empty() && Saved.ECXInitialized.empty() &&
         Saved.RuntimeThrowInfos.empty() &&
         Saved.ThrownTypeVA ==
             (Current.IsRethrow ? 0 : Current.ThrowInfo.TypeDescriptorVA) &&
         Saved.ThrownObjectSize ==
             (Current.IsRethrow ? 0 : Current.ThrowInfo.ObjectSize) &&
         sameImageEffects(Saved, Current, Work);
}

bool hasSameX86RegistrationCxxEffects(
    const RegistrationCalleeFrameContract &Saved,
    const RegistrationCalleeFrameContract &Current, size_t &Work) {
  return chargeCalleeWork(Work, Saved.RequiredImports.size() +
                                    Saved.RuntimeThrowInfos.size() + 1) &&
         Saved.isCxxParent() && Current.isCxxParent() &&
         Saved.Target == Current.Target && !Saved.StackPopBytes &&
         !Current.StackPopBytes &&
         Saved.DoesNotReturn == Current.DoesNotReturn &&
         (!Saved.DoesNotReturn || !Saved.HasIndependentScalarReturn) &&
         !Saved.ThrownTypeVA && !Saved.ThrownObjectSize &&
         Saved.ECXReads.empty() && Saved.ECXWrites.empty() &&
         Saved.ECXInitialized.empty() &&
         Saved.HasIndependentScalarReturn ==
             Current.HasIndependentScalarReturn &&
         Saved.RequiredImports == Current.RequiredImports &&
         Saved.RuntimeThrowInfos == Current.RuntimeThrowInfos &&
         sameImageEffects(Saved, Current, Work);
}
} // namespace neverd
