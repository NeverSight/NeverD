//===- ImageStrings.cpp - The strings of a loaded image -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "ImageStrings.h"

#include "SessionImpl.h"

#include "neverd/loader/StringLiteralSymbols.h"
#include "neverd/sdk/Limits.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/JSON.h"

#include <algorithm>

namespace neverd::sdk {
namespace {

/// Where \p String ends, its terminator included.
va_t terminatedEnd(const ImageString &String) {
  return String.Address + String.Bytes +
         strings::encodingUnitBytes(String.Kind);
}

/// The string \p Literal holds at \p Addr, read whole: ASCII or UTF-8, else
/// the code page a scan reads it in, for bytes; UTF-16 or UTF-32 for wider
/// characters.  None where no encoding reads every character.  The symbol,
/// not a search, says the bytes are text, so the encodings \p Options search
/// do not choose it, and its characters may be controls.
std::optional<ImageString> literalString(const BinaryImage &Img, va_t Addr,
                                         const SymbolStringLiteral &Literal,
                                         const strings::ScanOptions &Options) {
  const Segment *Seg = Img.getSegmentFor(Addr);
  if (!Seg)
    return std::nullopt;
  const llvm::ArrayRef<uint8_t> Terminated(Seg->Data.data() + (Addr - Seg->VA),
                                           Literal.Bytes + Literal.UnitBytes);
  const llvm::ArrayRef<uint8_t> Text = Terminated.drop_back(Literal.UnitBytes);
  ImageString String;
  String.Address = Addr;
  String.Bytes = Literal.Bytes;
  String.Literal = true;
  const auto ReadsWhole = [&](strings::Encoding Kind) {
    std::string UTF8;
    unsigned Chars = 0;
    bool Whole = true;
    strings::decode(Text, Kind, [&](const strings::DecodedCharacter &Read) {
      Whole &= Read.Codes > 0;
      for (unsigned I = 0; I < Read.Codes; ++I) {
        char Buffer[UNI_MAX_UTF8_BYTES_PER_CODE_POINT];
        char *End = Buffer;
        llvm::ConvertCodePointToUTF8(Read.Code[I], End);
        UTF8.append(Buffer, End);
        ++Chars;
      }
    });
    if (!Whole)
      return false;
    String.Kind = Kind;
    String.Chars = Chars;
    String.Text = std::move(UTF8);
    return true;
  };
  switch (Literal.UnitBytes) {
  case 1: {
    if (llvm::all_of(Text, [](uint8_t Byte) { return Byte < 0x80; }))
      return ReadsWhole(strings::Encoding::ASCII) ? std::optional(String)
                                                  : std::nullopt;
    if (ReadsWhole(strings::Encoding::UTF8))
      return String;
    // A code page reads it only as a scan of its bytes would: the preferred
    // one first, then the common ones.
    strings::ScanOptions Whole = Options;
    Whole.Encodings |= strings::defaultEncodings();
    Whole.MinLength = 1;
    std::optional<strings::FoundString> Found =
        strings::scanAt(Terminated, Whole);
    if (!Found || Found->Bytes != Literal.Bytes ||
        !strings::isLegacyEncoding(Found->Kind))
      return std::nullopt;
    String.Kind = Found->Kind;
    String.Chars = Found->Chars;
    String.Text = std::move(Found->Text);
    return String;
  }
  case 2:
    return ReadsWhole(strings::Encoding::UTF16LE) ? std::optional(String)
                                                  : std::nullopt;
  case 4:
    return ReadsWhole(strings::Encoding::UTF32LE) ? std::optional(String)
                                                  : std::nullopt;
  default:
    return std::nullopt;
  }
}

/// The strings the image's symbols name as literals, in address order, none
/// overlapping another.
std::vector<ImageString> literalStrings(const BinaryImage &Img,
                                        const strings::ScanOptions &Options) {
  std::vector<ImageString> Literals;
  for (const Symbol &Sym : Img.Symbols) {
    if (Sym.IsFunc)
      continue;
    const auto Literal = symbolStringLiteralAt(Img, Sym.Addr, Sym.Name);
    // An empty literal holds no text for a list of strings to show.
    if (!Literal || !Literal->Bytes)
      continue;
    if (auto String = literalString(Img, Sym.Addr, *Literal, Options))
      Literals.push_back(std::move(*String));
  }
  std::stable_sort(Literals.begin(), Literals.end(),
                   [](const ImageString &A, const ImageString &B) {
                     return A.Address < B.Address;
                   });
  // The first literal to start keeps its bytes, as in a scan.
  std::vector<ImageString> Kept;
  for (auto &String : Literals)
    if (Kept.empty() || String.Address >= terminatedEnd(Kept.back()))
      Kept.push_back(std::move(String));
  return Kept;
}

} // namespace

std::string parseStringOptions(const char *Json,
                               strings::ScanOptions &Options) {
  if (!Json || !*Json)
    return {};
  auto Parsed = llvm::json::parse(Json);
  const llvm::json::Object *Object = Parsed ? Parsed->getAsObject() : nullptr;
  if (!Object) {
    if (!Parsed)
      llvm::consumeError(Parsed.takeError());
    return "string options must be a JSON object";
  }
  for (const auto &[Key, Value] : *Object) {
    if (Key == "min_length") {
      const auto Length = Value.getAsInteger();
      if (!Length || *Length < 1 || *Length > strings::MaxMinLength)
        return "min_length must be an integer from 1 to " +
               std::to_string(strings::MaxMinLength);
      Options.MinLength = static_cast<unsigned>(*Length);
    } else if (Key == "encodings") {
      const auto *Names = Value.getAsArray();
      if (!Names)
        return "encodings must be an array of encoding names";
      Options.Encodings = 0;
      for (const auto &Name : *Names) {
        const auto Text = Name.getAsString();
        const auto Encoding =
            Text ? strings::encodingNamed(*Text) : std::nullopt;
        if (!Encoding)
          return "unknown string encoding: " +
                 (Text ? Text->str() : std::string("non-string"));
        Options.Encodings |= strings::encodingBit(*Encoding);
      }
    } else if (Key == "preferred") {
      if (Value.kind() == llvm::json::Value::Null) {
        Options.Preferred.reset();
        continue;
      }
      const auto Text = Value.getAsString();
      const auto Encoding = Text ? strings::encodingNamed(*Text) : std::nullopt;
      if (!Encoding || !strings::isLegacyEncoding(*Encoding))
        return "preferred must name a legacy code page";
      Options.Preferred = *Encoding;
    } else {
      return "unknown string option: " + Key.str();
    }
  }
  return {};
}

const std::vector<ImageString> &
imageStrings(Session &S, const strings::ScanOptions &Options) {
  if (S.ImageStrings &&
      S.ImageStrings->Options.Encodings == Options.Encodings &&
      S.ImageStrings->Options.Preferred == Options.Preferred &&
      S.ImageStrings->Options.MinLength == Options.MinLength)
    return S.ImageStrings->Strings;
  ImageStringScan Scan;
  Scan.Options = Options;
  // A large range's spans scan on the workers; each is a large task.
  const auto Parallel = [](size_t Count,
                           llvm::function_ref<void(size_t)> Task) {
    parallelForEachWeighted(
        std::vector<uint64_t>(Count, 1),
        [&](auto Claim, size_t Total) {
          for (size_t I; (I = Claim()) < Total;)
            Task(I);
        },
        kMinParallelStringScanSpans);
  };
  const auto ScanRange = [&](va_t Start, const uint8_t *Bytes, size_t Size) {
    strings::scan(
        llvm::ArrayRef<uint8_t>(Bytes, Size), Options,
        [&](strings::FoundString &&Found) {
          Scan.Strings.push_back({Start + Found.Offset, Found.Bytes,
                                  Found.Chars, Found.Kind,
                                  std::move(Found.Text)});
        },
        Parallel);
  };
  // Data segments whole, and the data sections of code segments.
  for (const auto &Seg : S.Img.Segments) {
    if (!Seg.isExecutable()) {
      ScanRange(Seg.VA, Seg.Data.data(), Seg.Data.size());
      continue;
    }
    for (const auto &Sec : S.Img.Sections) {
      if (Sec.isExecutable() || !Sec.FileSz || !Seg.contains(Sec.VA) ||
          Sec.VA - Seg.VA >= Seg.Data.size())
        continue;
      const uint64_t Offset = Sec.VA - Seg.VA;
      const uint64_t Size =
          std::min<uint64_t>({Sec.Size, Sec.FileSz, Seg.Data.size() - Offset});
      ScanRange(Sec.VA, Seg.Data.data() + Offset, static_cast<size_t>(Size));
    }
  }
  // A literal a symbol names wins over text a scan reads in its bytes.
  const auto Literals = literalStrings(S.Img, Options);
  llvm::erase_if(Scan.Strings, [&](const ImageString &String) {
    // The first literal that ends past the string's start.
    const auto Literal =
        std::upper_bound(Literals.begin(), Literals.end(), String.Address,
                         [](va_t Address, const ImageString &Literal) {
                           return Address < terminatedEnd(Literal);
                         });
    return Literal != Literals.end() &&
           Literal->Address < terminatedEnd(String);
  });
  Scan.Strings.insert(Scan.Strings.end(), Literals.begin(), Literals.end());
  // Ranges are scanned in image order, which need not be address order.
  std::stable_sort(Scan.Strings.begin(), Scan.Strings.end(),
                   [](const ImageString &A, const ImageString &B) {
                     return A.Address < B.Address;
                   });
  S.ImageStrings = std::move(Scan);
  return S.ImageStrings->Strings;
}

const ImageString *stringHolding(const std::vector<ImageString> &Strings,
                                 va_t Address) {
  auto It = std::upper_bound(Strings.begin(), Strings.end(), Address,
                             [](va_t Value, const ImageString &String) {
                               return Value < String.Address;
                             });
  if (It == Strings.begin())
    return nullptr;
  --It;
  return Address - It->Address < It->Bytes ? &*It : nullptr;
}

} // namespace neverd::sdk
