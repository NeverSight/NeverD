//===- ImageStrings.h - The strings of a loaded image -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The strings of a loaded image as the C API publishes them: the JSON
/// options that choose encodings and a minimum length, and one scan of the
/// image's data under them, kept on the Session until the options or the
/// image change.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SDK_CAPI_IMAGESTRINGS_H
#define NEVERD_LIB_SDK_CAPI_IMAGESTRINGS_H

#include "neverd/Common.h"
#include "neverd/support/StringScan.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd {
struct BinaryImage;
}

namespace neverd::sdk {

struct Session;

/// A string of the image, without its terminator.
struct ImageString {
  va_t Address = 0;
  uint64_t Bytes = 0;
  unsigned Chars = 0;
  strings::Encoding Kind = strings::Encoding::ASCII;
  /// The characters as UTF-8.
  std::string Text;
  /// Whether a symbol names the bytes as a string literal (MSVC's `??_C@`),
  /// rather than the scan finding text there.
  bool Literal = false;
};

/// One scan of the image and the options it was made with.
struct ImageStringScan {
  strings::ScanOptions Options;
  std::vector<ImageString> Strings;
};

/// Reads the JSON options of neverd_strings_ex_json (`encodings`,
/// `preferred`, `min_length`) into \p Options.  Returns why they are invalid,
/// or an empty string.  Null or empty text keeps the defaults.
std::string parseStringOptions(const char *Json, strings::ScanOptions &Options);

/// The strings of the loaded image under \p Options, in address order: data
/// segments whole, the data sections of code segments, and the literals the
/// image's symbols name wherever they lie and however short they are.
const std::vector<ImageString> &
imageStrings(Session &S, const strings::ScanOptions &Options);

/// The string of \p Strings holding \p Address, if one does.
const ImageString *stringHolding(const std::vector<ImageString> &Strings,
                                 va_t Address);

struct ReferredImageText {
  const ImageString *String = nullptr;
  uint64_t TextOffset = 0;
};

/// Match a referenced byte to displayed text. A nonempty sequence of non-NUL
/// ASCII control bytes immediately preceding the text is retained as a prefix;
/// callers must keep the original target, rather than rewriting it to the text.
std::optional<ReferredImageText>
referredImageText(const BinaryImage &Img,
                  const std::vector<ImageString> &Strings, unsigned MinLength,
                  va_t Address);

} // namespace neverd::sdk

#endif // NEVERD_LIB_SDK_CAPI_IMAGESTRINGS_H
