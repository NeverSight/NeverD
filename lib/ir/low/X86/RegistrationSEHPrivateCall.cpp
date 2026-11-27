//===- RegistrationSEHPrivateCall.cpp - SEH private call effects ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationSEHPrivateCall.h"

#include "RegistrationABIPrivate.h"
#include "RegistrationOpaqueStack.h"
#include "RegistrationSourceCall.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include <algorithm>

namespace neverd::registration_state {

bool SEHPrivateCallProof::proveCallee(va_t Target, size_t &Work) {
  if (Image.IsRelocatable || !Function.ExceptionMetadata ||
      !Function.ExceptionMetadata->Registration ||
      Image.ExceptionMetadata.findFunction(Target))
    return false;
  const auto &EH = *Function.ExceptionMetadata;
  const bool EH4 = EH.Personality == ExceptionPersonality::ExceptHandler4;
  if ((!EH4 && EH.Personality != ExceptionPersonality::ExceptHandler3) ||
      (EH4 && (!Image.DynInfo.SecurityCookieRVA || Image.Base > UINT32_MAX ||
               Image.DynInfo.SecurityCookieRVA > UINT32_MAX - Image.Base)))
    return false;
  const va_t Cookie = EH4 ? Image.Base + Image.DynInfo.SecurityCookieRVA : 0;
  if (EH4 && Cookie > UINT32_MAX - 3)
    return false;
  const auto Table = coff_loader::getX86RegistrationSEHScopeTableRange(EH);
  if (!Table || EH.CodeRange.contains(Target))
    return false;
  const auto Pop = getCheckedX86CalleeStackPop(Image, Target, &Work);
  if (!Pop || *Pop)
    return false;
  const auto ABI =
      getCheckedX86RegistrationObjectCalleeABI(Image, Target, &Work);
  if (!ABI || ABI->StackPopBytes || !ABI->ECXReads.empty() ||
      !ABI->ECXWrites.empty())
    return false;
  // The common callee proof owns stack, nonvolatile and transitive memory
  // effects. Only an argument-free graph with no object borrow can retain an
  // arbitrary live jump buffer. Its result remains the ordinary call taint.
  for (const auto &Code : ABI->CodeRanges) {
    const auto *Owner = Image.getSegmentFor(Code.Begin);
    if (!registration_abi::chargeCalleeWork(Work, 1) ||
        EH.CodeRange.overlaps(Code) || !Owner || !Owner->isReadable() ||
        !Owner->isExecutable() ||
        (Owner->isWritable() && !Owner->ReadOnlyAfterRelocations) ||
        Code.End <= Code.Begin || Code.End - Owner->VA > Owner->Size)
      return false;
  }
  for (const auto &Write : ABI->ImageWrites) {
    if (Write.End <= Write.Begin || Table->overlaps(Write) ||
        (EH4 && Write.Begin < Cookie + 4 && Cookie < Write.End))
      return false;
    for (va_t Begin = Write.Begin; Begin != Write.End;) {
      const auto Width =
          uint16_t(std::min<va_t>(Write.End - Begin, UINT16_MAX));
      if (!registration_abi::hasSEHCallbackImageMemory(Image, Begin, Width,
                                                       true, Work))
        return false;
      Begin += Width;
    }
  }
  return true;
}

bool SEHPrivateCallProof::preservesFrame(const LowOp &Op, size_t &Work) {
  if (!registration_abi::chargeCalleeWork(Work, 1))
    return false;
  if (const auto It = Occurrences.find(&Op); It != Occurrences.end())
    return It->second;
  const auto *Source = checkedRegistrationDirectCall(Image, Function, Op, Work);
  if (!Source || isUnconditionalNoReturn(*Source))
    return false;
  const auto Target = Op.Inputs[0].Offset;
  auto It = Callees.find(Target);
  if (It == Callees.end()) {
    if (Callees.size() >= registration_abi::kMaxObjectCallbackNodes)
      return false;
    It = Callees.emplace(Target, false).first;
    It->second = proveCallee(Target, Work);
  }
  return Occurrences.emplace(&Op, It->second).first->second;
}

std::optional<uint32_t> SEHPrivateCallProof::opaqueStackPop(const LowOp &Op,
                                                            size_t &Work) {
  if (!registration_abi::chargeCalleeWork(Work, 1))
    return std::nullopt;
  if (const auto Known = StackOccurrences.find(&Op);
      Known != StackOccurrences.end())
    return Known->second;
  const auto *Source = checkedRegistrationDirectCall(Image, Function, Op, Work);
  if (!Source || isUnconditionalNoReturn(*Source))
    return std::nullopt;
  const va_t Target = Op.Inputs[0].Offset;
  auto Known = StackTargets.find(Target);
  if (Known == StackTargets.end()) {
    const auto Pop =
        registration_abi::projectOpaqueCallReturnStack(Image, Target, Work);
    if (Work >= limits::kMaxRegistrationEHStateWork)
      return std::nullopt;
    Known = StackTargets.emplace(Target, Pop).first;
  }
  return StackOccurrences.emplace(&Op, Known->second).first->second;
}

} // namespace neverd::registration_state
