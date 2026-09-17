#ifndef NEVERD_LOADER_OBJC_OBJCCONSTANTSTRINGS_H
#define NEVERD_LOADER_OBJC_OBJCCONSTANTSTRINGS_H

#include "neverd/Common.h"

#include <optional>
#include <vector>

namespace neverd {
struct BinaryImage;

/// One proven Darwin constant-string object. Units exclude the terminator;
/// UTF-16 code units are retained without a lossy Unicode conversion.
struct ObjCConstantString {
  bool UTF16 = false;
  std::vector<uint16_t> Units;
  /// Verified backing bytes; ASCII strings can share a rebuilt literal pool.
  va_t ContentsAddress = 0;
};

std::optional<ObjCConstantString>
readObjCConstantString(const BinaryImage &Image, va_t Address);
} // namespace neverd

#endif
