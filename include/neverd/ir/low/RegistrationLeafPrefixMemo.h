//===- RegistrationLeafPrefixMemo.h - Leaf prefix reuse -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reuse context-independent leaf prefix refusals within one unchanged-image
/// run.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_REGISTRATIONLEAFPREFIXMEMO_H
#define NEVERD_IR_LOW_REGISTRATIONLEAFPREFIXMEMO_H

#include "neverd/Common.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <tuple>

namespace neverd {
struct BinaryImage;

/// A completed prefix refusal and its original work charge. The image must
/// remain unchanged for this memo's lifetime. Only raw integer graphs without
/// EH and checks preceding the first child proof are retained; no callee ABI,
/// exhausted attempt or result from a child context is retained.
class RegistrationLeafPrefixMemo {
public:
  explicit RegistrationLeafPrefixMemo(const BinaryImage &Image)
      : Image(Image) {}
  RegistrationLeafPrefixMemo(const RegistrationLeafPrefixMemo &) = delete;
  RegistrationLeafPrefixMemo &
  operator=(const RegistrationLeafPrefixMemo &) = delete;

  std::optional<size_t> find(const BinaryImage &Other, va_t Target,
                             uint32_t Pop, bool ClosedCalls, unsigned Level);
  void record(const BinaryImage &Other, va_t Target, uint32_t Pop,
              bool ClosedCalls, unsigned Level, size_t Work);

private:
  const BinaryImage &Image;
  std::mutex Mutex;
  std::map<std::tuple<va_t, uint32_t, bool, unsigned>, size_t> Refusals;
};

/// The innermost scope serves this thread's ordinary/object leaf prefixes.
class RegistrationLeafPrefixMemoScope {
public:
  explicit RegistrationLeafPrefixMemoScope(RegistrationLeafPrefixMemo &Memo);
  ~RegistrationLeafPrefixMemoScope();
  RegistrationLeafPrefixMemoScope(const RegistrationLeafPrefixMemoScope &) =
      delete;
  RegistrationLeafPrefixMemoScope &
  operator=(const RegistrationLeafPrefixMemoScope &) = delete;

private:
  RegistrationLeafPrefixMemo *Previous;
};
} // namespace neverd

#endif // NEVERD_IR_LOW_REGISTRATIONLEAFPREFIXMEMO_H
