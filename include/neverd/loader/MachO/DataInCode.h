//===- DataInCode.h - Mach-O embedded data ranges --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_MACHO_DATAINCODE_H
#define NEVERD_LOADER_MACHO_DATAINCODE_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/bit.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <vector>

namespace neverd {

struct MachODataInCodeRange {
  va_t Begin, End;
  uint16_t Kind;
  bool operator==(const MachODataInCodeRange &) const = default;
};

/// In a linked image the record offset is relative to the mapped Mach header.
/// In an MH_OBJECT it is the assembler's section address.
llvm::Expected<std::vector<MachODataInCodeRange>>
decodeMachODataInCode(llvm::ArrayRef<uint8_t> Bytes, va_t Base,
                      llvm::endianness Order);

/// Intersect a byte span with the loader's sorted, disjoint DICE records.
inline llvm::ArrayRef<MachODataInCodeRange>
machODataInCodeRanges(llvm::ArrayRef<MachODataInCodeRange> Ranges, va_t Start,
                      uint64_t Size) {
  if (Size == 0 || Ranges.empty())
    return {};
  const auto First =
      std::upper_bound(Ranges.begin(), Ranges.end(), Start,
                       [](va_t VA, const auto &R) { return VA < R.End; });
  const auto Last =
      std::partition_point(First, Ranges.end(), [&](const auto &R) {
        return R.Begin < Start || R.Begin - Start < Size;
      });
  return {First, Last};
}

} // namespace neverd

#endif // NEVERD_LOADER_MACHO_DATAINCODE_H
