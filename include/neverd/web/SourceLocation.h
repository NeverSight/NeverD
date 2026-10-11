//===- SourceLocation.h - Source coordinate conversion -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source coordinate conversion.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <compare>
#include <cstdint>
#include <string_view>
#include <vector>

namespace neverd::web {
/// Requires strictly valid UTF-8 input; recognizes scalar and CRLF boundaries.
bool sourceByteBoundary(std::string_view Bytes, uint64_t Offset);

struct TextPosition {
  uint64_t Line = 0;
  uint64_t UTF16Column = 0;
  auto operator<=>(const TextPosition &) const = default;
};

/// Borrowed immutable UTF-8 text. The caller must keep it alive. Columns are
/// zero-based UTF-16 units. CRLF is one line break; CR, LF, U+2028 and U+2029
/// also terminate lines. A position inside a UTF-8 scalar, surrogate pair or
/// CRLF is rejected rather than rounded to a different source location.
class TextCoordinates {
public:
  explicit TextCoordinates(std::string_view Bytes);
  TextPosition position(uint64_t ByteOffset) const;
  uint64_t byteOffset(TextPosition Position) const;
  uint64_t lineCount() const { return Lines.size(); }

private:
  struct Checkpoint {
    uint64_t Byte;
    uint64_t Column;
  };
  struct Line {
    uint64_t Start;
    uint64_t End;
    std::vector<Checkpoint> Checkpoints;
  };
  std::string_view Bytes;
  std::vector<Line> Lines;
};
} // namespace neverd::web
