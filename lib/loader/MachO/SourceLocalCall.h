#ifndef NEVERD_LOADER_MACHO_SOURCELOCALCALL_H
#define NEVERD_LOADER_MACHO_SOURCELOCALCALL_H

#include "neverd/ir/low/SourceCallOccurrence.h"

#include <map>

namespace neverd {
struct BinaryImage;
struct LowFunc;
using SourceLocalCalls = std::map<SourceCallOccurrenceKey, uint32_t>;
/// Exact original BL/LowIR pairs; the caller separately authenticates the leaf.
SourceLocalCalls sourceLocalCalls(const BinaryImage &Image,
                                  const LowFunc &Caller);
/// Immutable complete local leaf ownership, no interior entries or complex EH.
bool sourceLocalLeafRange(const BinaryImage &Image, va_t Entry, uint32_t Size);
} // namespace neverd
#endif
