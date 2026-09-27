//===- MobileDalvikAccess.h - Canonical encoded declaration flags ------===//
#pragma once

#include "MobileCommon.h"

#include <utility>

namespace neverd::mobile::dalvik::detail {
enum AccessBit : uint32_t {
  Public = 1,
  Private = 2,
  Protected = 4,
  Static = 8,
  Final = 0x10,
  Synchronized = 0x20,
  Volatile = 0x40,
  Transient = 0x80,
  Native = 0x100,
  Interface = 0x200,
  Abstract = 0x400,
  Strictfp = 0x800,
  Synthetic = 0x1000,
  Annotation = 0x2000,
  Enum = 0x4000,
  Constructor = 0x10000,
  DeclaredSynchronized = 0x20000
};
inline constexpr std::pair<uint32_t, const char *> DeclarationFlags[] = {
    {Public, "public"},
    {Private, "private"},
    {Protected, "protected"},
    {Static, "static"},
    {Final, "final"},
    {Synchronized, "synchronized"},
    {Volatile, "volatile"},
    {Transient, "transient"},
    {Native, "native"},
    {Interface, "interface"},
    {Abstract, "abstract"},
    {Strictfp, "strictfp"},
    {Synthetic, "synthetic"},
    {Annotation, "annotation"},
    {Enum, "enum"},
    {Constructor, "constructor"},
    {DeclaredSynchronized, "declared-synchronized"}};
inline void validateAccessBits(uint32_t value) {
  constexpr uint32_t known = [] {
    uint32_t bits = 0;
    for (auto [bit, name] : DeclarationFlags)
      bits |= bit;
    return bits;
  }();
  if (value & ~known)
    throw Error("unknown Dalvik declaration access flags");
}
} // namespace neverd::mobile::dalvik::detail
