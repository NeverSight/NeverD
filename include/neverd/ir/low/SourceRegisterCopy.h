#ifndef NEVERD_IR_LOW_SOURCEREGISTERCOPY_H
#define NEVERD_IR_LOW_SOURCEREGISTERCOPY_H

#include "neverd/ir/low/SourceCallOccurrence.h"

#include <map>
#include <vector>

namespace neverd {
/// Source-only receipt for an exact local MOV64 leaf. Register offsets map
/// final destinations to values at leaf entry. Consumers must snapshot every
/// source before writing any destination. This is not a C ABI declaration.
/// A stored receipt has no authority without current image/LowIR validation.
struct SourceRegisterCopy {
  va_t Caller = 0;
  SourceCallOccurrenceKey Site;
  uint32_t CallWord = 0;
  std::vector<uint32_t> LeafWords;
  std::map<uint64_t, uint64_t> Registers;

  bool operator==(const SourceRegisterCopy &) const = default;
};
using SourceRegisterCopies =
    std::map<SourceCallOccurrenceKey, SourceRegisterCopy>;
} // namespace neverd
#endif
