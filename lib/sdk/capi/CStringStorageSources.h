#ifndef NEVERD_SDK_CAPI_CSTRINGSTORAGESOURCES_H
#define NEVERD_SDK_CAPI_CSTRINGSTORAGESOURCES_H

#include "neverd/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/ADT/StringExtras.h"

#include <set>
#include <stdexcept>

namespace neverd::sdk {
// Preserve the complete literal pool, including embedded NULs and suffix
// sharing. A terminator alone does not establish a C array object boundary.
// Whole-section storage keeps every supported interior address in one shared,
// permanent object, including pointers retained by an external runtime.
inline std::optional<SourceCallTypeHint>
cstringStorageSourceHint(const BinaryImage &Image, va_t Address) {
  const auto *Section = Image.getSectionFor(Address);
  if (!Section || Section->VA != Address || !Section->Size ||
      Section->Size > limits::kMaxSourceCallBorrowedBytes ||
      (Section->Type & llvm::MachO::SECTION_TYPE) !=
          llvm::MachO::S_CSTRING_LITERALS ||
      !readImmutableImageBytes(Image, Address, Section->Size))
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeCStringStorage;
  Hint.TargetAddress = Address;
  Hint.ByteCount = Section->Size;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Error;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Error))
    return std::nullopt;
  return Hint;
}

inline std::string
renderCStringStorageHelpers(const BinaryImage &Image,
                            const std::set<va_t> &Sections,
                            std::set<std::string> &SharedFunctions) {
  std::string Source;
  for (const auto Address : Sections) {
    const auto Hint = cstringStorageSourceHint(Image, Address);
    if (!Hint)
      throw std::runtime_error("C string literal storage is no longer valid");
    const auto Bytes = readImmutableImageBytes(Image, Address, Hint->ByteCount);
    const auto Name =
        "neverd_cstring_storage_" + llvm::utohexstr(Address, true) + "_address";
    if (!SharedFunctions.insert(Name).second)
      continue;
    Source += "\nuintptr_t " + Name +
              "(void) {\n"
              "  static const unsigned char storage[" +
              std::to_string(Hint->ByteCount) + "] =\n    \"";
    unsigned Column = 0;
    for (uint8_t Byte : *Bytes) {
      if (Column++ == 64) {
        Source += "\"\n    \"";
        Column = 1;
      }
      if (Byte >= 32 && Byte <= 126 && Byte != '"' && Byte != '\\' &&
          Byte != '?') {
        Source += char(Byte);
      } else {
        // Fixed-width octal escapes cannot absorb a following digit. They
        // also keep NUL, non-ASCII bytes and trigraphs out of source syntax.
        Source += '\\';
        Source += char('0' + ((Byte >> 6) & 7));
        Source += char('0' + ((Byte >> 3) & 7));
        Source += char('0' + (Byte & 7));
      }
    }
    Source += "\";\n  return (uintptr_t)storage;\n}\n";
  }
  return Source;
}
} // namespace neverd::sdk
#endif
