//===- SourceLocation.cpp - Source coordinate conversion ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source coordinate conversion.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/SourceLocation.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"

#include <algorithm>

namespace neverd::web {
namespace {
// Called only after complete strict UTF-8 admission. Advance by one scalar.
uint32_t scalar(std::string_view Bytes, uint64_t &Offset) {
  const uint8_t Lead = Bytes[Offset++];
  if (Lead < 0x80)
    return Lead;
  unsigned Count = Lead < 0xE0 ? 1 : Lead < 0xF0 ? 2 : 3;
  uint32_t Value = Lead & ((1u << (6 - Count)) - 1);
  while (Count--)
    Value = (Value << 6) | (uint8_t(Bytes[Offset++]) & 0x3f);
  return Value;
}
} // namespace

bool sourceByteBoundary(std::string_view Bytes, uint64_t Offset) {
  return Offset <= Bytes.size() &&
         (Offset == Bytes.size() || (uint8_t(Bytes[Offset]) & 0xc0) != 0x80) &&
         !(Offset && Offset < Bytes.size() && Bytes[Offset - 1] == '\r' &&
           Bytes[Offset] == '\n');
}

TextCoordinates::TextCoordinates(std::string_view Text) : Bytes(Text) {
  if (Bytes.size() > 4 * 1024 * 1024)
    throw Error("coordinate_byte_budget_exceeded");
  if (!validUtf8(Bytes))
    throw Error("unsupported_encoding");
  Lines.push_back({0, 0, {{0, 0}}});
  uint64_t Cursor = 0, Column = 0;
  while (Cursor < Bytes.size()) {
    const auto Start = Cursor;
    const auto Code = scalar(Bytes, Cursor);
    if (Code == '\r' || Code == '\n' || Code == 0x2028 || Code == 0x2029) {
      Lines.back().End = Start;
      if (Code == '\r' && Cursor < Bytes.size() && Bytes[Cursor] == '\n')
        ++Cursor;
      if (Lines.size() == 200000)
        throw Error("coordinate_line_budget_exceeded");
      Lines.push_back({Cursor, Cursor, {{Cursor, 0}}});
      Column = 0;
    } else {
      Column += Code > 0xFFFF ? 2 : 1;
      auto &Checkpoints = Lines.back().Checkpoints;
      if (Cursor - Checkpoints.back().Byte >= 128)
        Checkpoints.push_back({Cursor, Column});
    }
  }
  Lines.back().End = Bytes.size();
}

TextPosition TextCoordinates::position(uint64_t Offset) const {
  if (Offset > Bytes.size())
    throw Error("invalid_source_position");
  const auto It = std::upper_bound(
      Lines.begin(), Lines.end(), Offset,
      [](uint64_t Byte, const Line &L) { return Byte < L.Start; });
  const auto &Line = *std::prev(It);
  if (Offset > Line.End)
    throw Error("invalid_source_position");
  const auto Check = std::upper_bound(
      Line.Checkpoints.begin(), Line.Checkpoints.end(), Offset,
      [](uint64_t Byte, const Checkpoint &C) { return Byte < C.Byte; });
  auto [Cursor, Column] = *std::prev(Check);
  while (Cursor < Offset)
    Column += scalar(Bytes, Cursor) > 0xFFFF ? 2 : 1;
  if (Cursor != Offset)
    throw Error("invalid_source_position");
  return {uint64_t(std::distance(Lines.begin(), std::prev(It))), Column};
}

uint64_t TextCoordinates::byteOffset(TextPosition Position) const {
  if (Position.Line >= Lines.size())
    throw Error("invalid_source_position");
  const auto &Line = Lines[Position.Line];
  const auto Check = std::upper_bound(
      Line.Checkpoints.begin(), Line.Checkpoints.end(), Position.UTF16Column,
      [](uint64_t Column, const Checkpoint &C) { return Column < C.Column; });
  auto [Cursor, Column] = *std::prev(Check);
  while (Column < Position.UTF16Column && Cursor < Line.End)
    Column += scalar(Bytes, Cursor) > 0xFFFF ? 2 : 1;
  if (Column != Position.UTF16Column)
    throw Error("invalid_source_position");
  return Cursor;
}
} // namespace neverd::web
