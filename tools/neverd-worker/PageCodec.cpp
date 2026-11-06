//===- PageCodec.cpp - Compact form of kept engine pages ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "PageCodec.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace neverd::worker {
namespace {
constexpr unsigned HashBits = 15;
constexpr std::size_t MinMatch = 4;
constexpr std::size_t MaxDistance = 1u << 24;

void putVarint(std::string &out, std::uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<char>(value | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<char>(value));
}

bool getVarint(std::string_view in, std::size_t &at, std::uint64_t &value) {
  value = 0;
  for (unsigned shift = 0; shift < 64 && at < in.size(); shift += 7) {
    const auto byte = static_cast<unsigned char>(in[at++]);
    value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
    if (!(byte & 0x80))
      return true;
  }
  return false;
}

std::uint32_t read32(const char *at) {
  std::uint32_t value;
  std::memcpy(&value, at, sizeof value);
  return value;
}
} // namespace

std::string compressPage(std::string_view raw) {
  std::string out;
  out.reserve(raw.size() / 3 + 16);
  putVarint(out, raw.size());
  // Positions are one past their byte, so zero means empty.
  thread_local std::vector<std::uint32_t> table;
  table.assign(std::size_t{1} << HashBits, 0);
  const char *const data = raw.data();
  std::size_t literal = 0, at = 0;
  const auto flush = [&](std::size_t matchLength, std::size_t distance) {
    putVarint(out, at - literal);
    out.append(data + literal, at - literal);
    putVarint(out, matchLength);
    if (matchLength)
      putVarint(out, distance);
  };
  while (at + MinMatch <= raw.size()) {
    const auto word = read32(data + at);
    auto &slot = table[(word * 2654435761u) >> (32 - HashBits)];
    const std::size_t candidate = slot;
    slot = static_cast<std::uint32_t>(at + 1);
    if (!candidate || at + 1 - candidate > MaxDistance ||
        read32(data + candidate - 1) != word) {
      ++at;
      continue;
    }
    std::size_t length = MinMatch;
    const std::size_t from = candidate - 1;
    while (at + length < raw.size() && data[from + length] == data[at + length])
      ++length;
    flush(length, at - from);
    at += length;
    literal = at;
  }
  at = raw.size();
  flush(0, 0);
  out.shrink_to_fit();
  return out;
}

std::optional<std::string> decompressPage(std::string_view compact) {
  std::size_t at = 0;
  std::uint64_t size = 0;
  // A kept page is far smaller than this; anything larger is not one.
  if (!getVarint(compact, at, size) || size > (std::uint64_t{1} << 32))
    return std::nullopt;
  std::string out(static_cast<std::size_t>(size), '\0');
  std::size_t written = 0;
  while (true) {
    std::uint64_t literal = 0, match = 0, distance = 0;
    if (!getVarint(compact, at, literal) || literal > compact.size() - at ||
        literal > out.size() - written)
      return std::nullopt;
    std::memcpy(out.data() + written, compact.data() + at, literal);
    at += literal;
    written += literal;
    if (!getVarint(compact, at, match))
      return std::nullopt;
    if (!match)
      break;
    if (!getVarint(compact, at, distance) || !distance || distance > written ||
        match > out.size() - written)
      return std::nullopt;
    const char *from = out.data() + written - distance;
    char *to = out.data() + written;
    if (distance >= match)
      std::memcpy(to, from, match);
    else
      for (std::uint64_t i = 0; i != match; ++i)
        to[i] = from[i];
    written += match;
  }
  if (written != out.size() || at != compact.size())
    return std::nullopt;
  return out;
}

} // namespace neverd::worker
