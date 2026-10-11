//===- RegistrationABIPrivate.h - PE32 call frame checks ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONABIPRIVATE_H
#define NEVERD_IR_LOW_X86_REGISTRATIONABIPRIVATE_H

#include "neverd/ir/low/RegistrationABI.h"

#include <cstddef>
#include <set>
#include <utility>

namespace neverd::registration_abi {

struct ImageFrameEffects {
  std::set<std::pair<va_t, va_t>> Reads;
  std::set<std::pair<va_t, va_t>> Writes;
  std::set<std::pair<va_t, va_t>> CallerPCWrites;
  std::set<std::pair<int32_t, int32_t>> ECXReads;
  std::set<std::pair<int32_t, int32_t>> ECXWrites;
};

bool chargeCalleeWork(size_t &Work, size_t Amount);
std::optional<uint32_t>
checkedRegistrationImportStackPop(const BinaryImage &Image, va_t Target);
bool callerPCIsNotReadBack(const ImageFrameEffects &Effects);
bool hasPrivateCallerFrame(const LowFunc &Function, const BinaryImage &Image,
                           size_t &Work, ImageFrameEffects &Effects,
                           bool BorrowECX = false,
                           RegistrationThrowCalleeABI *ThrowProof = nullptr,
                           bool *IndependentScalarReturn = nullptr);

} // namespace neverd::registration_abi

#endif // NEVERD_IR_LOW_X86_REGISTRATIONABIPRIVATE_H
