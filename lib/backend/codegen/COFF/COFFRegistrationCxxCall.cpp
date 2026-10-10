//===- COFFRegistrationCxxCall.cpp - PE32 preserved call attributes ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reject optimizer promises that contradict a preserved callee's effects.
//===----------------------------------------------------------------------===//

#include "COFFRegistrationCxxIRProof.h"

#include "llvm/IR/Instructions.h"

namespace neverd::coff_registration {
bool hasExactCxxCallAttributes(const llvm::CallBase &Call,
                               const CxxIRCall &Receipt) {
  const auto *Callee = Call.getCalledFunction();
  if (!Callee)
    return false;
  const auto Valid = [&](const llvm::AttributeList &Attributes, bool AtCall) {
    if (Attributes.getRetAttrs().hasAttributes())
      return false;
    for (unsigned I = 0; I < Call.arg_size(); ++I)
      if (Attributes.getParamAttrs(I).hasAttributes())
        return false;
    return llvm::all_of(Attributes.getFnAttrs(), [&](llvm::Attribute A) {
      return A.isEnumAttribute() &&
             ((A.getKindAsEnum() == llvm::Attribute::NoReturn &&
               Receipt.Contract.DoesNotReturn) ||
              (A.getKindAsEnum() == llvm::Attribute::NoUnwind && AtCall &&
               Receipt.Cleanup));
    });
  };
  return Valid(Call.getAttributes(), true) &&
         Valid(Callee->getAttributes(), false);
}
} // namespace neverd::coff_registration
