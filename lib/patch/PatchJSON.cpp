//===- PatchJSON.cpp - Byte patch rows ------------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded, lossless byte patch persistence shared by every adapter.
//===----------------------------------------------------------------------===//

#include "neverd/patch/Limits.h"
#include "neverd/patch/PatchIO.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/raw_ostream.h"

#if NEVERD_USE_SIMDJSON
#include "simdjson.h"
#endif

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace neverd::patch;
namespace patchLimits = neverd::limits::patch;

static llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::make_error<llvm::StringError>(Message,
                                             llvm::inconvertibleErrorCode());
}

llvm::Expected<std::vector<uint8_t>>
neverd::patch::parseHex(llvm::StringRef Text, uint64_t Limit) {
  if (Text.size() % 2 || Text.size() / 2 > Limit)
    return invalid("hex bytes need pairs of digits within the byte limit");
  std::vector<uint8_t> Bytes;
  Bytes.reserve(Text.size() / 2);
  for (size_t I = 0; I < Text.size(); I += 2) {
    const unsigned H = llvm::hexDigitValue(Text[I]);
    const unsigned L = llvm::hexDigitValue(Text[I + 1]);
    if (H > 15 || L > 15)
      return invalid("invalid hexadecimal byte");
    Bytes.push_back((H << 4) | L);
  }
  return Bytes;
}

namespace {
// This root-array writer accepts only the fixed patch inventory schema keys.
// Pretty selects the canonical two-space sidecar format. General string
// values still use LLVM's JSON escaping. Keep the small attribute sink chain
// inline so schema keys reach raw_ostream as constants.
template <bool Pretty> class PatchOutput {
  llvm::raw_ostream &Stream;
  bool FirstObject = true;
  bool FirstAttribute = true;

  LLVM_ATTRIBUTE_ALWAYS_INLINE void attributeBegin(llvm::StringLiteral Key) {
    if (!FirstAttribute)
      Stream << ',';
    FirstAttribute = false;
    if constexpr (Pretty)
      Stream << "\n    ";
    Stream << '"' << Key << (Pretty ? "\": " : "\":");
  }

public:
  explicit PatchOutput(llvm::raw_ostream &Stream) : Stream(Stream) {}

  template <typename Callback> void array(Callback Emit) {
    Stream << '[';
    Emit();
    if constexpr (Pretty)
      if (!FirstObject)
        Stream << '\n';
    Stream << ']';
  }

  template <typename Callback> void object(Callback Emit) {
    if (!FirstObject)
      Stream << ',';
    if constexpr (Pretty)
      Stream << "\n  ";
    FirstObject = false;
    FirstAttribute = true;
    Stream << '{';
    Emit();
    if constexpr (Pretty)
      Stream << "\n  ";
    Stream << '}';
  }

  LLVM_ATTRIBUTE_ALWAYS_INLINE void attribute(llvm::StringLiteral Key,
                                              llvm::StringRef Text) {
    attributeBegin(Key);
    Stream << llvm::json::Value(Text);
  }

  LLVM_ATTRIBUTE_ALWAYS_INLINE void attribute(llvm::StringLiteral Key,
                                              uint64_t Value) {
    attributeBegin(Key);
    Stream << Value;
  }

  LLVM_ATTRIBUTE_ALWAYS_INLINE void hexAttribute(llvm::StringLiteral Key,
                                                 llvm::StringRef Text) {
    attributeBegin(Key);
    Stream << '"' << Text << '"';
  }
};

class HexValue {
public:
  static constexpr size_t MaxSize =
      2 + std::numeric_limits<uint64_t>::digits / 4;

private:
  char Buffer[MaxSize] = {'0', 'x'};

public:
  llvm::StringRef format(uint64_t Value) {
    const auto End =
        std::to_chars(Buffer + 2, Buffer + sizeof Buffer, Value, 16).ptr;
    return llvm::StringRef(Buffer, End - Buffer);
  }
};

// Estimate compact output capacity from metadata without a second row scan.
// Skip small output that fits in the remaining stream buffer. This is only
// an allocation hint; the stream remains growable as usual.
template <bool FullRows>
static void reserveCompactOutput(llvm::raw_ostream &Stream,
                                 const PatchOverlay &Overlay) {
  if (Overlay.empty())
    return;
  // Remove the representative decimal zero and the string terminator.
  constexpr size_t RowSyntaxBytes =
      (FullRows
           ? sizeof(
                 R"({"addr":"","current":"","file_off":"","kind":"","original":"","size":0,"va":""})")
           : sizeof(R"({"address":"","size":0})")) -
      sizeof("0");
  constexpr size_t MaxKindSize = std::max({
#define NEVERD_PATCH_KIND(Id, Spelling) sizeof(Spelling) - 1,
#include "neverd/patch/PatchKinds.def"
  });
  constexpr size_t MaxSizeDigits = std::numeric_limits<uint64_t>::digits10 + 1;
  const uint64_t Count = Overlay.rows().size();
  const uint64_t HexBytes = FullRows ? 4 * Overlay.bytes() : 0;
  constexpr uint64_t MaxRowOverhead =
      RowSyntaxBytes + (FullRows ? 3 : 1) * HexValue::MaxSize +
      (FullRows ? MaxKindSize : 0) + MaxSizeDigits;
  const auto BufferRemaining =
      Stream.GetBufferSize() - Stream.GetNumBytesInBuffer();
  if (2 + (Count - 1) + Count * MaxRowOverhead + HexBytes <= BufferRemaining)
    return;
  char SizeBuffer[MaxSizeDigits];
  const auto SizeEnd =
      std::to_chars(SizeBuffer, SizeBuffer + sizeof SizeBuffer, Overlay.bytes())
          .ptr;
  uint64_t RowOverhead =
      RowSyntaxBytes + HexValue::MaxSize + (SizeEnd - SizeBuffer);
  if constexpr (FullRows) {
    HexValue Hex;
    const auto FileOffsetSize =
        Hex.format(Overlay.rows().rbegin()->first).size();
    RowOverhead += 2 * FileOffsetSize + MaxKindSize;
  }
  Stream.reserveExtraSpace(2 + (Count - 1) + Count * RowOverhead + HexBytes);
}

// Only formatter-produced hexadecimal text reaches these helpers. It is ASCII
// without quotes or escapes, so generic string validation/escaping is
// redundant.
static void writeHexAttribute(llvm::json::OStream &Output, llvm::StringRef Key,
                              llvm::StringRef Text) {
  Output.attributeBegin(Key);
  Output.rawValue(
      [&](llvm::raw_ostream &Stream) { Stream << '"' << Text << '"'; });
  Output.attributeEnd();
}

template <bool Pretty>
LLVM_ATTRIBUTE_ALWAYS_INLINE void writeHexAttribute(PatchOutput<Pretty> &Output,
                                                    llvm::StringLiteral Key,
                                                    llvm::StringRef Text) {
  Output.hexAttribute(Key, Text);
}

template <typename OutputT>
void writeRows(OutputT &Output, const PatchOverlay &Overlay) {
  llvm::SmallString<32> Bytes;
  HexValue Hex;
  // Each view is consumed before the next conversion reuses its storage.
  Output.array([&] {
    for (const auto &[Offset, Row] : Overlay.rows())
      Output.object([&] {
        const auto FileOffset = Hex.format(Offset);
        writeHexAttribute(Output, "addr", FileOffset);
        llvm::toHex(Row.Current, true, Bytes);
        writeHexAttribute(Output, "current", Bytes);
        writeHexAttribute(Output, "file_off", FileOffset);
        Output.attribute("kind", patchKindName(Row.Kind));
        llvm::toHex(Row.Original, true, Bytes);
        writeHexAttribute(Output, "original", Bytes);
        Output.attribute("size", Row.size());
        writeHexAttribute(Output, "va", Hex.format(Row.Address));
      });
  });
}
} // namespace

void neverd::patch::writePatchRows(llvm::json::OStream &Output,
                                   const PatchOverlay &Overlay) {
  writeRows(Output, Overlay);
}

void neverd::patch::writePatchRows(llvm::raw_ostream &Stream,
                                   const PatchOverlay &Overlay) {
  reserveCompactOutput<true>(Stream, Overlay);
  PatchOutput<false> Output(Stream);
  writeRows(Output, Overlay);
}

void neverd::patch::writePatchSidecarRows(llvm::raw_ostream &Stream,
                                          const PatchOverlay &Overlay) {
  PatchOutput<true> Output(Stream);
  writeRows(Output, Overlay);
}

void neverd::patch::writePatchRanges(llvm::raw_ostream &Stream,
                                     const PatchOverlay &Overlay) {
  reserveCompactOutput<false>(Stream, Overlay);
  PatchOutput<false> Output(Stream);
  HexValue Hex;
  Output.array([&] {
    for (const auto &[Offset, Row] : Overlay.rows())
      Output.object([&] {
        writeHexAttribute(Output, "address", Hex.format(Row.Address));
        Output.attribute("size", Row.size());
      });
  });
}

namespace {
struct RowFields {
  std::optional<llvm::StringRef> FileOffset, Address, Key, Original, Current,
      Kind;
  std::optional<uint64_t> Size;
  bool HasKey = false;

  void setString(llvm::StringRef Name, std::optional<llvm::StringRef> Value) {
    auto *Field = llvm::StringSwitch<std::optional<llvm::StringRef> *>(Name)
                      .Case("file_off", &FileOffset)
                      .Case("va", &Address)
                      .Case("addr", &Key)
                      .Case("original", &Original)
                      .Case("current", &Current)
                      .Case("kind", &Kind)
                      .Default(nullptr);
    if (Field)
      *Field = Value;
    if (Name == "addr")
      HasKey = true;
  }
};

llvm::Expected<RowFields> readFields(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid("patch row must be an object");
  RowFields Fields;
  for (const auto &[Key, Field] : *Object)
    if (Key == "size")
      Fields.Size = Field.getAsUINT64();
    else
      Fields.setString(Key, Field.getAsString());
  return Fields;
}

#if NEVERD_USE_SIMDJSON
llvm::Expected<RowFields> readFields(simdjson::dom::element Value) {
  simdjson::dom::object Object;
  if (Value.get_object().get(Object))
    return invalid("patch row must be an object");
  RowFields Fields;
  // Iterate in source order: LLVM's reader retains the last duplicate key,
  // including a wrong-typed replacement of an earlier valid field.
  for (const auto Field : Object) {
    const llvm::StringRef Key(Field.key.data(), Field.key.size());
    if (Key == "size") {
      uint64_t Size;
      if (Field.value.get_uint64().get(Size))
        Fields.Size.reset();
      else
        Fields.Size = Size;
    } else {
      std::string_view Text;
      if (Field.value.get_string().get(Text))
        Fields.setString(Key, std::nullopt);
      else
        Fields.setString(Key, llvm::StringRef(Text.data(), Text.size()));
    }
  }
  return Fields;
}
#endif

llvm::Expected<PatchRow> readRow(const RowFields &Fields, uint64_t &Total) {
  PatchRow Row;
  const auto ReadAddress = [](std::optional<llvm::StringRef> Text,
                              uint64_t &Out) {
    if (!Text || !Text->consume_front("0x") || Text->empty())
      return false;
    // Preserve leading zeroes without converting excess significant digits.
    constexpr size_t MaxDigits = std::numeric_limits<uint64_t>::digits / 4;
    if (Text->size() > MaxDigits) {
      *Text = Text->ltrim('0');
      if (Text->empty()) {
        Out = 0;
        return true;
      }
      if (Text->size() > MaxDigits)
        return false;
    }
    const auto Parsed = std::from_chars(Text->begin(), Text->end(), Out, 16);
    return Parsed.ec == std::errc{} && Parsed.ptr == Text->end();
  };
  const auto Kind = Fields.Kind ? parsePatchKind(*Fields.Kind) : std::nullopt;
  if (!ReadAddress(Fields.FileOffset, Row.FileOffset) ||
      !ReadAddress(Fields.Address, Row.Address) || !Fields.Size ||
      !*Fields.Size || *Fields.Size > patchLimits::kMaxPatchedBytes - Total ||
      !Fields.Original || !Fields.Current || !Kind)
    return invalid("invalid patch row fields");
  if (Fields.HasKey) {
    uint64_t Key = 0;
    if (!ReadAddress(Fields.Key, Key) || Key != Row.FileOffset)
      return invalid("patch row key differs from file offset");
  }
  auto Original = parseHex(*Fields.Original, patchLimits::kMaxPatchedBytes);
  if (!Original)
    return Original.takeError();
  auto Current = parseHex(*Fields.Current, patchLimits::kMaxPatchedBytes);
  if (!Current)
    return Current.takeError();
  if (Original->size() != *Fields.Size || Current->size() != *Fields.Size)
    return invalid("patch row size does not match its bytes");
  Total += *Fields.Size;
  Row.Original = std::move(*Original);
  Row.Current = std::move(*Current);
  Row.Kind = *Kind;
  return Row;
}

template <typename Array>
llvm::Expected<std::vector<PatchRow>> readRows(const Array &Values) {
  if (Values.size() > patchLimits::kMaxPatchRows)
    return invalid("patches.json must be a bounded array");
  std::vector<PatchRow> Rows;
  Rows.reserve(Values.size());
  uint64_t Total = 0;
  for (const auto &Value : Values) {
    auto Fields = readFields(Value);
    if (!Fields)
      return Fields.takeError();
    auto Row = readRow(*Fields, Total);
    if (!Row)
      return Row.takeError();
    Rows.push_back(std::move(*Row));
  }
  return Rows;
}
} // namespace

llvm::Expected<std::vector<PatchRow>>
neverd::patch::parsePatchRows(llvm::StringRef Text) {
  if (Text.size() > patchLimits::kMaxPatchesFileBytes)
    return invalid("patches.json exceeds its size limit");
#if NEVERD_USE_SIMDJSON
  // An empty StringRef may have null storage. Keep its legacy syntax error
  // without passing that pointer to the SIMD parser's input copy.
  if (!Text.empty()) {
    // Extract typed rows from the validated tape; no per-row JSON object tree
    // or parser storage survives this call. Borrowed strings are decoded into
    // owning byte vectors before the temporary parser is destroyed.
    simdjson::dom::parser Parser;
    simdjson::dom::element Root;
    if (!Parser.parse(Text.data(), Text.size()).get(Root)) {
      simdjson::dom::array Values;
      if (Root.get_array().get(Values))
        return invalid("patches.json must be a bounded array");
      return readRows(Values);
    }
  }
  // LLVM accepts some legacy number spellings and provides existing syntax
  // diagnostics. Preserve that contract for inputs outside the SIMD parser.
#endif
  auto Parsed = llvm::json::parse(Text);
  if (!Parsed)
    return Parsed.takeError();
  const auto *Values = Parsed->getAsArray();
  if (!Values)
    return invalid("patches.json must be a bounded array");
  return readRows(*Values);
}
