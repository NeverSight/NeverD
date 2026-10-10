#pragma once

#include "BunFixture.h"

#include <algorithm>
#include <vector>

namespace neverd::web::test {
inline void appendLE(std::string &B, uint64_t V, unsigned Width) {
  const auto At = B.size();
  B.resize(At + Width);
  put(B, At, V, Width);
}

// Standard raw/RLE Zstd frames, authored as bytes in C++. No compressor or
// target runtime is needed for boundary and hostile-input fixtures.
inline std::string rawZstd(std::string_view Source) {
  std::string Frame;
  appendLE(Frame, 0xfd2fb528, 4);
  appendLE(Frame, 0xa0, 1); // Single segment, four-byte content size.
  appendLE(Frame, Source.size(), 4);
  do {
    const auto Size = std::min<size_t>(Source.size(), 128 * 1024);
    appendLE(Frame, (Size << 3) | (Size == Source.size()), 3);
    Frame += Source.substr(0, Size);
    Source.remove_prefix(Size);
  } while (!Source.empty());
  return Frame;
}

inline std::string repeatedZstd(uint32_t Size, char Byte = 'a') {
  std::string Frame;
  appendLE(Frame, 0xfd2fb528, 4);
  appendLE(Frame, 0xa0, 1);
  appendLE(Frame, Size, 4);
  do {
    const auto N = std::min<uint32_t>(Size, 128 * 1024);
    appendLE(Frame, (uint64_t(N) << 3) | 2 | (N == Size), 3);
    Frame += Byte;
    Size -= N;
  } while (Size);
  return Frame;
}

// Two known anchors: generated/original (0,0) and (0,7), both source 0.
inline std::string simpleMappings() {
  std::string B(90, '\0');
  put(B, 0, 90, 8);
  put(B, 8, 2, 8);
  put(B, 16, 1, 8);
  put(B, 24, 1, 4);
  put(B, 28, 56, 4);
  put(B, 56, 2, 1);
  put(B, 58, 1, 2);
  put(B, 72, 1, 8); // Original-line delta equals generated-line delta (0).
  put(B, 80, 1, 8); // Original-column delta equals generated-column delta (7).
  put(B, 88, 14, 1);
  return B;
}

inline std::string serializedBunMap(std::string_view Mappings,
                                    const std::vector<std::string> &Names,
                                    const std::vector<std::string> &Frames) {
  if (Names.size() != Frames.size())
    throw std::runtime_error("fixture source count");
  std::string B(8 + 16 * Names.size(), '\0');
  put(B, 0, Names.size(), 4);
  put(B, 4, Mappings.size(), 4);
  B += Mappings;
  uint64_t At = 8;
  for (const auto *Table : {&Names, &Frames})
    for (const auto &Text : *Table) {
      put(B, At, B.size(), 4);
      put(B, At + 4, Text.size(), 4);
      B += Text;
      At += 8;
    }
  return B;
}

inline std::string simpleBunMap() {
  return serializedBunMap(
      simpleMappings(), {"https://SECRET_MAP.invalid/?token=CANARY"},
      {rawZstd("export const secret = 'BUN_SOURCE_CANARY';")});
}
} // namespace neverd::web::test
