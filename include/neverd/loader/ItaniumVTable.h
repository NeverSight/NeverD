//===- ItaniumVTable.h - Primary virtual tables -------------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Index primary Itanium virtual-table slots and their declared targets.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ITANIUMVTABLE_H
#define NEVERD_LOADER_ITANIUMVTABLE_H

#include "neverd/Common.h"

#include <map>
#include <optional>
#include <string>
#include <unordered_map>

namespace neverd {
struct BinaryImage;
struct Symbol;

/// An overridable declaration, never proof of the runtime dispatch target.
struct ItaniumVirtualSlot {
  va_t Table, Slot, Target;
};

/// Borrows symbols from one unchanged image. Ambiguous class definitions
/// remain ambiguous; primary address points require relocation-backed slots.
class ItaniumVTableIndex {
  std::map<std::string, const Symbol *> Tables;
  std::unordered_map<va_t, const Symbol *> TypeInfos;

public:
  ItaniumVTableIndex() = default;
  explicit ItaniumVTableIndex(const BinaryImage &Image);
  void add(const BinaryImage &Image, const Symbol &Symbol);
  std::optional<ItaniumVirtualSlot> resolve(const BinaryImage &Image,
                                            const std::string &Class,
                                            uint64_t ByteOffset) const;
};
} // namespace neverd

#endif // NEVERD_LOADER_ITANIUMVTABLE_H
