#ifndef NEVERD_SDK_CAPI_BORROWEDBYTESOURCES_H
#define NEVERD_SDK_CAPI_BORROWEDBYTESOURCES_H

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/ADT/StringExtras.h"

#include <set>
#include <stdexcept>

namespace neverd::sdk {
using BorrowedByteRange = std::pair<va_t, uint32_t>;

inline std::string borrowedByteHelperName(BorrowedByteRange Range) {
  return "neverd_borrowed_bytes_" + llvm::utohexstr(Range.first, true) + "_" +
         std::to_string(Range.second) + "_address";
}

inline std::optional<SourceCallTypeHint>
borrowedByteSourceHint(const BinaryImage &Image, BorrowedByteRange Range) {
  if (!readImmutableImageBytes(Image, Range.first, Range.second))
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeBorrowedBytes;
  Hint.TargetAddress = Range.first;
  Hint.ByteCount = Range.second;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Error;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Error))
    return std::nullopt;
  return Hint;
}

inline std::string
renderBorrowedByteHelpers(const BinaryImage &Image,
                          const std::set<BorrowedByteRange> &Ranges,
                          std::set<std::string> &SharedFunctions) {
  std::string Source;
  for (auto Range : Ranges) {
    auto Bytes = readImmutableImageBytes(Image, Range.first, Range.second);
    if (!Bytes)
      throw std::runtime_error(
          "borrowed byte range no longer has immutable storage");
    const auto Name = borrowedByteHelperName(Range);
    SharedFunctions.insert(Name);
    Source +=
        "\nuintptr_t " + Name +
        "(void) {\n  _Alignas(16) static const unsigned char bytes[] = { ";
    for (auto Byte : *Bytes)
      Source += std::to_string(Byte) + ", ";
    Source += "0 };\n  return (uintptr_t)bytes;\n}\n";
  }
  return Source;
}
} // namespace neverd::sdk
#endif
