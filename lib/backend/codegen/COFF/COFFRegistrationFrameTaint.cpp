//===- COFFRegistrationFrameTaint.cpp - Indexed PE32 frame taint ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bound pointer-taint queries by their owning allocation and byte interval.
//===----------------------------------------------------------------------===//

#include "COFFRegistrationFrameTaint.h"

#include "neverd/Limits.h"

#include <algorithm>
#include <bit>
#include <iterator>

namespace neverd::coff_registration {
namespace {
bool charge(size_t &Work, size_t Count) {
  if (Work > limits::kMaxRegistrationEHStateWork ||
      Count > limits::kMaxRegistrationEHStateWork - Work)
    return false;
  Work += Count;
  return true;
}
bool validRange(const llvm::Value *Root, int64_t Offset, uint64_t Bytes) {
  return Root && Offset >= 0 && Bytes &&
         Bytes <= uint64_t(INT64_MAX) - uint64_t(Offset);
}
} // namespace

std::optional<bool> RegistrationFrameTaint::insert(const llvm::Value *Root,
                                                   int64_t Offset,
                                                   uint64_t Bytes,
                                                   size_t &Work) {
  if (!validRange(Root, Offset, Bytes) || !charge(Work, 1))
    return std::nullopt;
  auto &Space = Ranges[Root];
  if (!charge(Work, 4 * std::bit_width(Space.size()) + 1))
    return std::nullopt;
  uint64_t Begin = uint64_t(Offset), End = Begin + Bytes;
  auto It = Space.upper_bound(Begin);
  if (It != Space.begin() && std::prev(It)->second >= Begin)
    --It;
  if (It != Space.end() && It->first <= Begin && End <= It->second)
    return false;
  while (It != Space.end() && It->first <= End) {
    if (!charge(Work, 1))
      return std::nullopt;
    Begin = std::min(Begin, It->first);
    End = std::max(End, It->second);
    It = Space.erase(It);
  }
  Space.emplace_hint(It, Begin, End);
  return true;
}

std::optional<bool> RegistrationFrameTaint::overlaps(const llvm::Value *Root,
                                                     int64_t Offset,
                                                     uint64_t Bytes,
                                                     size_t &Work) const {
  if (!validRange(Root, Offset, Bytes) || !charge(Work, 1))
    return std::nullopt;
  const auto Found = Ranges.find(Root);
  if (Found == Ranges.end())
    return false;
  const auto &Space = Found->second;
  if (!charge(Work, 2 * std::bit_width(Space.size()) + 1))
    return std::nullopt;
  const auto It = Space.lower_bound(uint64_t(Offset) + Bytes);
  return It != Space.begin() && std::prev(It)->second > uint64_t(Offset);
}
} // namespace neverd::coff_registration
