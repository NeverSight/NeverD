//===- KernelObjectTypes.h - Opaque guest object type identities
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Opaque guest object type identities.
///

#ifndef NEVERD_EMULATION_KERNELOBJECTTYPES_H
#define NEVERD_EMULATION_KERNELOBJECTTYPES_H
#include "neverd/emulation/DriverProfile.h"

#include "llvm/ADT/StringRef.h"

#include <array>
#include <optional>

namespace neverd::emulation::object_type {
enum class Kind : unsigned { Thread, Process, File, Driver };
struct Entry {
  Kind Type;
  llvm::StringLiteral Name;
};
inline constexpr std::array Entries = {
    Entry{Kind::Thread, "PsThreadType"}, Entry{Kind::Process, "PsProcessType"},
    Entry{Kind::File, "IoFileObjectType"},
    Entry{Kind::Driver, "IoDriverObjectType"}};
inline std::optional<Kind> find(llvm::StringRef Name) {
  for (const auto &Entry : Entries)
    if (Entry.Name == Name)
      return Entry.Type;
  return std::nullopt;
}
constexpr uint64_t slot(Kind Type) {
  return profile::ObjectTypeDataBase + unsigned(Type) * profile::PointerSize;
}
// These tokens identify model-owned opaque types, never native OBJECT_TYPEs.
constexpr uint64_t identity(Kind Type) {
  return profile::ObjectTypeDataBase + 0x100 +
         unsigned(Type) * profile::PointerSize;
}
} // namespace neverd::emulation::object_type
#endif
