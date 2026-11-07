//===- RegistrationFrameInputs.cpp - Bounded initialized caller words ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Share frame-value and initialization queries without image/CFG dependencies.
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"
#include "RegistrationFrame.h"

#include "neverd/Limits.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>

namespace neverd::registration_abi {
using namespace registration_state;

bool chargeCalleeWork(size_t &Work, size_t Amount) {
  if (Work > limits::kMaxRegistrationEHStateWork ||
      Amount > limits::kMaxRegistrationEHStateWork - Work) {
    Work = limits::kMaxRegistrationEHStateWork;
    return false;
  }
  Work += Amount;
  return true;
}

std::optional<FrameValue>
readCallerFrameWord(const FrameState &State, int32_t Offset,
                    const std::set<int32_t> *Initialized, size_t &Work) {
  if (Offset > INT32_MAX - 4 ||
      !chargeCalleeWork(Work,
                        State.memoryAccessWork(FrameValue::frame(Offset), 4)))
    return std::nullopt;
  if (Initialized) {
    if (!chargeCalleeWork(Work,
                          4 * (1 + 2 * std::bit_width(Initialized->size()))))
      return std::nullopt;
    for (int64_t Byte = Offset; Byte < int64_t(Offset) + 4; ++Byte)
      if (!Initialized->count(int32_t(Byte)))
        return std::nullopt;
  } else if (!State.Cells.count(Offset))
    return std::nullopt;
  // Sparse storage omits unknown scalars. Read through the memory owner so
  // a partial overlapping pointer store cannot become an untainted argument.
  return State.load(Offset, 4);
}

} // namespace neverd::registration_abi
