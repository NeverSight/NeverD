#ifndef NEVERD_IR_LOW_SOURCEREGISTERCOPY_H
#define NEVERD_IR_LOW_SOURCEREGISTERCOPY_H

#include "neverd/ir/low/SourceCallOccurrence.h"

#include <map>
#include <variant>
#include <vector>

namespace neverd {
struct SourceEntryRegister {
  uint64_t Offset = 0;
  bool operator==(const SourceEntryRegister &) const = default;
};
/// Complete constant-string object identity and the freshly validated payload.
/// A numerical page fragment is never a publishable register value.
struct SourceConstantStringAddress {
  va_t Address = 0;
  bool UTF16 = false;
  std::vector<uint16_t> Units;
  va_t ContentsAddress = 0;
  bool operator==(const SourceConstantStringAddress &) const = default;
};
using SourceRegisterValue =
    std::variant<SourceEntryRegister, SourceConstantStringAddress>;
using SourceRegisterValues = std::map<uint64_t, SourceRegisterValue>;

/// Source-only receipt for an exact local MOV64/ADRP/ADD leaf. Register offsets
/// map final destinations to entry values or authenticated object addresses.
/// Consumers snapshot every entry source before writing any destination.
/// This is not a C ABI declaration.
/// A stored receipt has no authority without current image/LowIR validation.
struct SourceRegisterCopy {
  va_t Caller = 0;
  SourceCallOccurrenceKey Site;
  uint32_t CallWord = 0;
  std::vector<uint32_t> LeafWords;
  SourceRegisterValues Registers;

  bool operator==(const SourceRegisterCopy &) const = default;
};
using SourceRegisterCopies =
    std::map<SourceCallOccurrenceKey, SourceRegisterCopy>;
} // namespace neverd
#endif
