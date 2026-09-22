#ifndef NEVERD_IR_LOW_SOURCECLASSGETTERCALL_H
#define NEVERD_IR_LOW_SOURCECLASSGETTERCALL_H

#include "neverd/ir/low/SourceCallOccurrence.h"

#include <array>
#include <map>
#include <string>

namespace neverd {
/// Exact ordinary-call facts. The CALL remains in every IR and still needs an
/// independently bound native declaration and source-closed body. This receipt
/// only proves that the zero-input getter cannot expose the caller's frame and
/// that x0 receives the current imported class identity.
struct SourceClassGetterCall {
  va_t Caller = 0;
  SourceCallOccurrenceKey Site;
  uint32_t CallWord = 0;
  std::array<uint32_t, 3> LeafWords{};
  va_t Slot = 0;
  std::string ClassName;
  std::string Module;
  bool operator==(const SourceClassGetterCall &) const = default;
};
using SourceClassGetterCalls =
    std::map<SourceCallOccurrenceKey, SourceClassGetterCall>;
} // namespace neverd
#endif
