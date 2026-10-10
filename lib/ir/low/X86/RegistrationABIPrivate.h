//===- RegistrationABIPrivate.h - PE32 call frame checks ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONABIPRIVATE_H
#define NEVERD_IR_LOW_X86_REGISTRATIONABIPRIVATE_H

#include "neverd/ir/low/RegistrationABI.h"

#include <algorithm>
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

template <typename Set, typename Vector>
void copyExtents(const Set &From, Vector &To) {
  for (const auto &[Begin, End] : From) {
    if (!To.empty() && Begin <= To.back().End)
      To.back().End = std::max(To.back().End, End);
    else
      To.push_back({Begin, End});
  }
}

bool chargeDecodedCallee(size_t &Work, const LowFunc &Function);
bool collectCalleeCodeRanges(const LowFunc &Function, const BinaryImage &Image,
                             size_t &Work,
                             std::vector<ExceptionAddressRange> &Ranges);

bool chargeCalleeWork(size_t &Work, size_t Amount);
bool callerPCIsNotReadBack(const ImageFrameEffects &Effects);
bool hasPrivateCallerFrame(const LowFunc &Function, const BinaryImage &Image,
                           size_t &Work, ImageFrameEffects &Effects,
                           bool BorrowECX = false,
                           RegistrationThrowCalleeABI *ThrowProof = nullptr,
                           bool *IndependentScalarReturn = nullptr);

} // namespace neverd::registration_abi

#endif // NEVERD_IR_LOW_X86_REGISTRATIONABIPRIVATE_H
