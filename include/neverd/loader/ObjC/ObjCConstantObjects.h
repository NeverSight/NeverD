#ifndef NEVERD_LOADER_OBJC_OBJCCONSTANTOBJECTS_H
#define NEVERD_LOADER_OBJC_OBJCCONSTANTOBJECTS_H

#include "neverd/loader/ObjC/ObjCConstantStrings.h"

#include <map>

namespace neverd {
struct ObjCConstantObject {
  enum class Kind { String, Integer, Array, Dictionary, ImportedBoolean };
  Kind TheKind = Kind::String;
  ObjCConstantString String;
  // ImportedBoolean leaves are keyed by their immutable pointer-table slot.
  // Their exact linker symbol supplies shared runtime identity, not the slot.
  std::string ImportName;
  char Encoding = 0;
  uint64_t Bits = 0;
  uint64_t Options = 0;
  std::vector<va_t> Elements;
  std::vector<va_t> Keys;
};

/// A bounded, acyclic graph of compiler-defined Darwin constant objects.
/// Native addresses identify local objects; imported Boolean leaves instead
/// identify exact immutable import slots. Edges retain their original order.
/// This does not authorize copying arbitrary runtime objects or mutable state.
using ObjCConstantObjectGraph = std::map<va_t, ObjCConstantObject>;

std::optional<ObjCConstantObjectGraph>
readObjCConstantObjectGraph(const BinaryImage &Image, va_t Root);
} // namespace neverd
#endif
