//===- CTypeFormat.cpp - Type to C string formatting ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Type-to-C-string formatting implementation.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/render/CTypeFormat.h"

#include "neverd/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/StringScan.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <stdexcept>
#include <string>

namespace neverd {

std::string escapeCString(llvm::StringRef Str) {
  std::string Result;
  Result.reserve(Str.size());
  for (unsigned char Ch : Str) {
    switch (Ch) {
    case '\n':
      Result += "\\n";
      break;
    case '\t':
      Result += "\\t";
      break;
    case '\r':
      Result += "\\r";
      break;
    case '\\':
      Result += "\\\\";
      break;
    case '"':
      Result += "\\\"";
      break;
    case '?':
      // Avoid trigraph translation in C11 consumers of arbitrary bytes.
      Result += "\\?";
      break;
    default:
      if (Ch >= 32 && Ch < 127) {
        Result += static_cast<char>(Ch);
      } else {
        // Hex escapes consume any following hex digits, and a short NUL
        // escape consumes following octal digits. Exactly three octal
        // digits encode one byte regardless of the next byte's spelling.
        Result += '\\';
        Result += static_cast<char>('0' + (Ch >> 6));
        Result += static_cast<char>('0' + ((Ch >> 3) & 7));
        Result += static_cast<char>('0' + (Ch & 7));
      }
      break;
    }
  }
  return Result;
}

bool isStringLiteralText(llvm::StringRef Text) {
  for (llvm::StringRef Prefix : {"\"", "L\"", "u8\"", "u\"", "U\""})
    if (Text.starts_with(Prefix))
      return true;
  return false;
}

namespace {
/// Whether the image's strings end with a NUL.  Go keeps a string as a
/// pointer and a length and its linker packs their bytes without
/// terminators, so a NUL-terminated read of a Go image's data is text by
/// coincidence, or the start of another object such as a type descriptor.
bool terminatesStrings(const BinaryImage &Img) {
  return Img.ExceptionMetadata.Runtime.Runtime != SourceLanguageRuntime::Go;
}
} // namespace

std::optional<std::string> imageStringLiteral(const BinaryImage *Img, va_t Addr,
                                              bool AllowEmpty) {
  if (!Img || Addr == 0 || Addr == InvalidVA)
    return std::nullopt;
  if (Img->findImportAt(Addr) || !terminatesStrings(*Img))
    return std::nullopt;
  const Segment *Seg = Img->getSegmentFor(Addr);
  if (!Seg || !Seg->isReadable() || Seg->isWritable() || Seg->isExecutable())
    return std::nullopt;
  // A file header or an alignment gap is mapped but holds no program data: a
  // small integer in an image loaded at zero lands there.
  if (!Img->hasObjectDataProvenance(Addr))
    return std::nullopt;
  constexpr unsigned kMaxLit = 64;
  auto Escape = [](uint32_t Ch, std::string &Out) {
    switch (Ch) {
    case '\\':
      Out += "\\\\";
      return true;
    case '"':
      Out += "\\\"";
      return true;
    case '\n':
      Out += "\\n";
      return true;
    case '\t':
      Out += "\\t";
      return true;
    case '\r':
      Out += "\\r";
      return true;
    default:
      if (Ch < 0x20 || Ch > 0x7E)
        return false;
      Out += static_cast<char>(Ch);
      return true;
    }
  };
  const uint64_t Off = Addr - Seg->VA;
  if (Off >= Seg->Data.size())
    return std::nullopt;
  const uint8_t *Base = Seg->Data.data() + Off;
  const size_t Remain = Seg->Data.size() - static_cast<size_t>(Off);
  // A sized data object that holds the bytes must be the string itself,
  // padded with zeros at most.  Bytes past the segment's data are zeros.
  auto IsOwnObject = [&](size_t Extent) {
    for (const Symbol &Sym : Img->Symbols) {
      if (Sym.IsFunc || !Sym.Size || Addr < Sym.Addr ||
          Addr - Sym.Addr >= Sym.Size)
        continue;
      if (Sym.Addr != Addr || Sym.Size < Extent)
        return false;
      const size_t End =
          static_cast<size_t>(std::min<uint64_t>(Sym.Size, Remain));
      if (End > Extent && !std::all_of(Base + Extent, Base + End,
                                       [](uint8_t Byte) { return !Byte; }))
        return false;
    }
    return true;
  };
  auto HasStableBytes = [&](size_t Extent) {
    if (Extent > InvalidVA - Addr || !IsOwnObject(Extent))
      return false;
    // Pointer tables can look like short ASCII/UTF-16 strings at one load
    // address. A fixup, including one beginning before this candidate, makes
    // those bytes unsuitable for replacement by a string literal. Use the
    // loader's shared storage-provenance query for every supported format.
    const va_t First = Addr >= 7 ? Addr - 7 : 0;
    const va_t End = Addr + Extent;
    for (va_t Byte = First; Byte < End; ++Byte)
      if (Img->hasRelocationProvenanceAt(Byte))
        return false;
    return true;
  };
  if (Remain >= 2 && Base[1] == 0) {
    std::string Body;
    unsigned N = 0;
    for (size_t I = 0; I + 1 < Remain && N < kMaxLit; I += 2, ++N) {
      const uint16_t Unit = readLE<uint16_t>(Base + I);
      if (Unit == 0) {
        if (!HasStableBytes(I + 2))
          return std::nullopt;
        if (N == 0)
          return AllowEmpty ? std::optional<std::string>("L\"\"")
                            : std::nullopt;
        return "L\"" + Body + "\"";
      }
      if (!Escape(Unit, Body))
        return std::nullopt;
    }
  }
  std::string Body;
  unsigned N = 0;
  for (size_t I = 0; I < Remain && N < kMaxLit; ++I, ++N) {
    const uint8_t Ch = Base[I];
    if (Ch == 0) {
      if (!HasStableBytes(I + 1))
        return std::nullopt;
      if (N == 0)
        return AllowEmpty ? std::optional<std::string>("\"\"") : std::nullopt;
      return "\"" + Body + "\"";
    }
    if (!Escape(Ch, Body))
      return std::nullopt;
  }
  return std::nullopt;
}

namespace {
/// How a comment beside code reads a string: its encoding's name unless that
/// is ASCII or UTF-8 (`GBK "你好"`), then its text in quotes with controls
/// escaped, `*` and `/` kept apart and long text cut.
std::string quotedStringText(strings::Encoding Kind, llvm::StringRef Text) {
  std::string Comment;
  if (Kind != strings::Encoding::UTF8)
    if (const llvm::StringRef Name = strings::encodingSpelling(Kind);
        !Name.empty())
      Comment += Name.str() + " ";
  Comment += '"';
  // Characters shown before the text is cut; UTF-8 continuation bytes
  // belong to the character before them.
  constexpr unsigned MaxCharacters = 64;
  unsigned Characters = 0;
  for (size_t I = 0; I < Text.size(); ++I) {
    const unsigned char Ch = Text[I];
    if ((Ch & 0xC0) != 0x80 && Characters++ == MaxCharacters) {
      Comment += "\xE2\x80\xA6"; // U+2026, the ellipsis, in UTF-8
      break;
    }
    switch (Ch) {
    case '\n':
      Comment += "\\n";
      break;
    case '\t':
      Comment += "\\t";
      break;
    case '\r':
      Comment += "\\r";
      break;
    case '"':
      Comment += "\\\"";
      break;
    case '\\':
      Comment += "\\\\";
      break;
    case '/':
      // `*/` would end the comment.
      if (!Comment.empty() && Comment.back() == '*')
        Comment += ' ';
      Comment += '/';
      break;
    default:
      if (Ch < 0x20 || Ch == 0x7F) {
        Comment += "\\x";
        Comment += llvm::hexdigit(Ch >> 4, /*LowerCase=*/true);
        Comment += llvm::hexdigit(Ch & 15, /*LowerCase=*/true);
      } else {
        Comment += static_cast<char>(Ch);
      }
      break;
    }
  }
  return Comment + '"';
}

/// The first string the scan finds in \p Bytes when it starts at their first
/// byte.
std::optional<strings::FoundString>
stringAtStart(llvm::ArrayRef<uint8_t> Bytes,
              const strings::ScanOptions &Options) {
  std::optional<strings::FoundString> First;
  strings::scan(Bytes, Options, [&](strings::FoundString &&Found) {
    if (!First)
      First = std::move(Found);
  });
  if (!First || First->Offset != 0)
    return std::nullopt;
  return First;
}

/// The readable, non-executable bytes from \p Addr, at most \p Window of
/// them; none when the image holds no such bytes there.
std::optional<llvm::ArrayRef<uint8_t>>
readableDataBytes(const BinaryImage *Img, va_t Addr, size_t Window) {
  if (!Img || Addr == 0 || Addr == InvalidVA)
    return std::nullopt;
  const Segment *Seg = Img->getSegmentFor(Addr);
  if (!Seg || !Seg->isReadable() || Seg->isExecutable() || Addr < Seg->VA)
    return std::nullopt;
  // A file header or an alignment gap is mapped but holds no program data: a
  // small integer in an image loaded at zero lands there.
  if (!Img->hasObjectDataProvenance(Addr))
    return std::nullopt;
  const uint64_t Off = Addr - Seg->VA;
  if (Off >= Seg->Data.size())
    return std::nullopt;
  return llvm::ArrayRef<uint8_t>(
      Seg->Data.data() + Off,
      std::min<size_t>(Window, Seg->Data.size() - static_cast<size_t>(Off)));
}

/// A string that does not end within this many bytes is not read.
constexpr size_t StringWindow = 4096;

/// Writes the characters of a C string literal in UTF-8 source text.
class CLiteralWriter {
public:
  explicit CLiteralWriter(std::string &Out) : Out(Out) {}

  /// The character \p Code: itself, a simple escape, or else the escapes of
  /// \p Units, its bytes or code units in the literal's element width.
  void character(uint32_t Code, llvm::ArrayRef<uint32_t> Units) {
    switch (Code) {
    case '\\':
      return text("\\\\");
    case '"':
      return text("\\\"");
    case '\n':
      return text("\\n");
    case '\t':
      return text("\\t");
    case '\r':
      return text("\\r");
    case '?':
      // `??` begins a trigraph for a consumer that still reads them.
      text(LastQuestion ? "\\?" : "?");
      LastQuestion = true;
      return;
    default:
      break;
    }
    if (Code >= 0x20 && Code < 0x7F)
      return text(std::string(1, static_cast<char>(Code)));
    if (Code >= 0xA0 && Code <= 0x10FFFF && (Code < 0xD800 || Code > 0xDFFF) &&
        strings::isShownCharacter(Code)) {
      char Encoded[UNI_MAX_UTF8_BYTES_PER_CODE_POINT];
      char *End = Encoded;
      llvm::ConvertCodePointToUTF8(Code, End);
      return text(llvm::StringRef(Encoded, End - Encoded));
    }
    for (uint32_t Unit : Units)
      escape(Unit);
  }

  /// A byte or code unit as a hexadecimal escape.
  void escape(uint32_t Unit) {
    Out += "\\x";
    Out += llvm::utohexstr(Unit);
    PendingHex = true;
    LastQuestion = false;
  }

private:
  void text(llvm::StringRef Text) {
    // A hexadecimal escape takes every hexadecimal digit after it.
    if (PendingHex && !Text.empty() && llvm::isHexDigit(Text.front()))
      Out += "\" \"";
    PendingHex = false;
    LastQuestion = false;
    Out += Text;
  }

  std::string &Out;
  bool PendingHex = false;
  bool LastQuestion = false;
};
} // namespace

std::optional<std::string> imageStringComment(const BinaryImage *Img,
                                              va_t Addr) {
  const auto Bytes = readableDataBytes(Img, Addr, StringWindow);
  if (!Bytes)
    return std::nullopt;
  const auto First = stringAtStart(*Bytes, strings::ScanOptions());
  if (!First)
    return std::nullopt;
  return quotedStringText(First->Kind, First->Text);
}

std::optional<ImageCString> imageCString(const BinaryImage *Img, va_t Addr) {
  if (Img && (Img->findImportAt(Addr) || !terminatesStrings(*Img)))
    return std::nullopt;
  const auto Bytes = readableDataBytes(Img, Addr, StringWindow);
  if (!Bytes)
    return std::nullopt;
  // The literal stands for the bytes, so a short string reads too.
  strings::ScanOptions Options;
  Options.MinLength = 1;
  const auto First = stringAtStart(*Bytes, Options);
  if (!First)
    return std::nullopt;
  const unsigned UnitBytes = strings::encodingUnitBytes(First->Kind);
  ImageCString Result;
  Result.UnitBytes = UnitBytes;
  Result.Bytes = First->Bytes + UnitBytes;
  if (!UnitBytes || Result.Bytes > Bytes->size() ||
      Result.Bytes > InvalidVA - Addr)
    return std::nullopt;
  // Bytes a relocation writes are an address, not text.
  for (va_t Byte = Addr >= 7 ? Addr - 7 : 0; Byte < Addr + Result.Bytes; ++Byte)
    if (Img->hasRelocationProvenanceAt(Byte))
      return std::nullopt;
  const llvm::ArrayRef<uint8_t> Text = Bytes->take_front(First->Bytes);
  std::string Body;
  CLiteralWriter Writer(Body);
  llvm::StringRef Prefix;
  auto Units = [&](const strings::DecodedCharacter &Character) {
    llvm::SmallVector<uint32_t, 4> Result;
    for (uint64_t Offset = Character.Offset;
         Offset + UnitBytes <= Character.Offset + Character.Bytes;
         Offset += UnitBytes)
      Result.push_back(UnitBytes == 1 ? Text[Offset]
                       : UnitBytes == 2
                           ? readLE<uint16_t>(Text.data() + Offset)
                           : readLE<uint32_t>(Text.data() + Offset));
    return Result;
  };
  switch (First->Kind) {
  case strings::Encoding::UTF16BE:
  case strings::Encoding::UTF32BE:
    return std::nullopt;
  case strings::Encoding::ASCII:
  case strings::Encoding::UTF8:
  case strings::Encoding::UTF16LE:
  case strings::Encoding::UTF32LE:
    Result.Element = UnitBytes == 1   ? "char"
                     : UnitBytes == 2 ? "char16_t"
                                      : "char32_t";
    Prefix = UnitBytes == 1 ? "" : UnitBytes == 2 ? "u" : "U";
    strings::decode(Text, First->Kind,
                    [&](const strings::DecodedCharacter &Character) {
                      const auto CodeUnits = Units(Character);
                      if (Character.Codes == 1)
                        Writer.character(Character.Code[0], CodeUnits);
                      else
                        for (uint32_t Unit : CodeUnits)
                          Writer.escape(Unit);
                    });
    break;
  default:
    // A legacy code page: its ASCII shows, its other bytes are escaped and
    // the comment reads them.
    Result.Element = "char";
    for (uint8_t Byte : Text) {
      const uint32_t Unit = Byte;
      if (Byte < 0x80)
        Writer.character(Byte, Unit);
      else
        Writer.escape(Byte);
    }
    Result.Note = quotedStringText(First->Kind, First->Text);
    break;
  }
  Result.Literal = (Prefix + "\"" + Body + "\"").str();
  return Result;
}

namespace {
std::string extendedIntegerType(unsigned Bytes, bool Signed) {
  if (Bytes == 32 || Bytes == 64)
    return std::string(Signed ? "int" : "uint") + std::to_string(Bytes * 8) +
           "_t";
  // C23 _BitInt covers every non-power-of-two slice up to the widest
  // register (a 512-bit ZMM value); e.g. a 224-bit YMM byte-shift window.
  if (Bytes == 0 || Bytes > 64)
    throw std::invalid_argument("C integer of " + std::to_string(Bytes) +
                                " bytes exceeds the supported bit width");
  return std::string(Signed ? "" : "unsigned ") + "_BitInt(" +
         std::to_string(Bytes * 8) + ")";
}

bool containsFunction(const TypeRef &Type) {
  auto Current = Type;
  for (unsigned Depth = 0; Current && Depth <= limits::kMaxCPointerNesting;
       ++Depth) {
    if (Current->Kind == NdTypeKind::Func)
      return true;
    if (Current->Kind != NdTypeKind::Ptr)
      return false;
    Current = Current->Pointee;
  }
  if (Current)
    throw std::invalid_argument("C type exceeds the pointer nesting limit");
  return false;
}
} // namespace

std::string declarationToC(const TypeRef &Ty, llvm::StringRef Declarator) {
  if (Ty && Ty->Kind == NdTypeKind::Array && Ty->ElemType) {
    std::string Inner = Declarator.str();
    Inner += Ty->ArrayCount ? "[" + std::to_string(Ty->ArrayCount) + "]" : "[]";
    return declarationToC(Ty->ElemType, Inner);
  }
  if (!containsFunction(Ty))
    return typeToC(Ty) + (Declarator.empty() ? "" : " " + Declarator.str());
  if (!equalSourceTypes(Ty, Ty))
    throw std::invalid_argument("C callback type has no supported signature");
  if (Ty->Kind == NdTypeKind::Ptr) {
    std::string Pointer = "*" + Declarator.str();
    if (Ty->Pointee->Kind == NdTypeKind::Func)
      Pointer = "(" + Pointer + ")";
    return declarationToC(Ty->Pointee, Pointer);
  }
  std::string Function = Declarator.str() + "(";
  for (size_t I = 0; I < Ty->ParamTypes.size(); ++I) {
    if (I)
      Function += ", ";
    Function += typeToC(Ty->ParamTypes[I]);
  }
  if (Ty->ParamTypes.empty())
    Function += "void";
  return declarationToC(Ty->RetType, Function + ")");
}

std::string typeToC(const TypeRef &Ty) {
  if (!Ty)
    return "uint32_t";
  if (containsFunction(Ty))
    return declarationToC(Ty, {});
  switch (Ty->Kind) {
  case NdTypeKind::Void:
    return "void";
  case NdTypeKind::Int:
    if (!Ty->SourceName.empty())
      return Ty->SourceName;
    if (Ty->IsSigned) {
      switch (Ty->Size) {
      case 1:
        return "int8_t";
      case 2:
        return "int16_t";
      case 4:
        return "int32_t";
      case 8:
        return "int64_t";
      case 16:
        return "__int128";
      default:
        return extendedIntegerType(Ty->Size, true);
      }
    } else {
      switch (Ty->Size) {
      case 1:
        return "uint8_t";
      case 2:
        return "uint16_t";
      case 4:
        return "uint32_t";
      case 8:
        return "uint64_t";
      case 16:
        return "unsigned __int128";
      default:
        return extendedIntegerType(Ty->Size, false);
      }
    }
  case NdTypeKind::Float:
    if (!Ty->SourceName.empty())
      return Ty->SourceName;
#define NEVERD_C_FLOAT_TYPE(Bytes, Spelling, FusedMultiplyAdd, ComputesWider)  \
  if (Ty->Size == Bytes)                                                       \
    return Spelling;
#include "neverd/backend/c/render/CFloatTypes.def"
    return "double";
  case NdTypeKind::Ptr:
    if (!Ty->Pointee || Ty->Pointee->Kind == NdTypeKind::Void)
      return "void*";
    return typeToC(Ty->Pointee) + "*";
  case NdTypeKind::Struct: {
    if (!Ty->SourceName.empty())
      return Ty->SourceName;
    if (sourceAggregateMembers(Ty).empty())
      throw std::invalid_argument("C record has no supported source layout");
    std::function<std::string(const TypeRef &)> Code = [&](const TypeRef &T) {
      if (isSourceBooleanType(T))
        return std::string("b1");
      if (T->Kind == NdTypeKind::Float)
        return std::string(T->Size == 4 ? "f" : "d");
      // Record declarations share a translation unit. Equal storage widths
      // do not make signed integers, unsigned integers and pointers the same
      // C field type; the tag must be independent of which function is first.
      if (T->Kind == NdTypeKind::Int)
        return std::string(T->IsSigned ? "i" : "u") +
               std::to_string(T->Size * 8);
      if (T->Kind == NdTypeKind::Ptr || !T->SourceName.empty()) {
        if (T->Kind == NdTypeKind::Ptr && T->Pointee->Kind == NdTypeKind::Void)
          return std::string("pv");
        // Include typed pointees, callback signatures and named nested
        // records without embedding their C declarator syntax in a tag.
        llvm::MD5 Hash;
        Hash.update(typeToC(T));
        return "t" + Hash.final().digest().str().str();
      }
      std::string Result = "r" + std::to_string(T->Fields.size());
      for (const auto &Field : T->Fields)
        Result += "_" + Code(Field);
      return Result + "_e";
    };
    return "struct nd_record_" + Code(Ty);
  }
  case NdTypeKind::Array:
    return typeToC(Ty->ElemType) + "*";
  case NdTypeKind::Unknown:
    if (Ty->Size > 0) {
      switch (Ty->Size) {
      case 1:
        return "uint8_t";
      case 2:
        return "uint16_t";
      case 4:
        return "uint32_t";
      case 8:
        return "uint64_t";
      case 16:
        return "unsigned __int128";
      default:
        return extendedIntegerType(Ty->Size, false);
      }
    }
    return "uint32_t";
  default:
    return "uint32_t";
  }
}

bool hasCSpelling(const TypeRef &Ty) {
  // typeToC is the authority on what C spells; ask it rather than restate
  // its rules.
  try {
    (void)typeToC(Ty);
    return true;
  } catch (const std::invalid_argument &) {
    return false;
  }
}

bool hasCValueLayout(const TypeRef &Ty) {
  if (Ty && Ty->Kind == NdTypeKind::Struct && !Ty->IsEnum &&
      sourceAggregateMembers(Ty).empty())
    return false;
  if (Ty && Ty->Kind == NdTypeKind::Array)
    return hasCValueLayout(Ty->ElemType);
  return hasCSpelling(Ty);
}

static std::string anonymousAggregateName(llvm::Type *Ty) {
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  Ty->print(Stream);
  llvm::MD5 Hash;
  Hash.update(Text);
  return "struct neverd_aggregate_" + Hash.final().digest().str().str();
}

std::string llvmStructName(llvm::StructType *ST) {
  if (ST->hasName()) {
    std::string Raw = ST->getName().str();
    std::string Clean;
    for (char Ch : Raw) {
      if (std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_')
        Clean += Ch;
      else
        Clean += '_';
    }
    if (Clean.empty() || std::isdigit(static_cast<unsigned char>(Clean[0])))
      Clean = "nd_" + Clean;
    return "struct " + Clean;
  }
  return anonymousAggregateName(ST);
}

bool isCIntegerVectorType(llvm::Type *Ty) {
  const auto *Vector = llvm::dyn_cast<llvm::FixedVectorType>(Ty);
  if (!Vector || !Vector->getElementType()->isIntegerTy() ||
      !llvm::isPowerOf2_32(Vector->getNumElements()))
    return false;
  const unsigned Bits = Vector->getElementType()->getIntegerBitWidth();
  return (Bits == 8 || Bits == 16 || Bits == 32 || Bits == 64) &&
         uint64_t(Vector->getNumElements()) * (Bits / 8) <=
             limits::kMaxCIntegerVectorBytes;
}

bool isCVectorType(llvm::Type *Ty) {
  if (isCIntegerVectorType(Ty))
    return true;
  const auto *Vector = llvm::dyn_cast<llvm::FixedVectorType>(Ty);
  if (!Vector || !llvm::isPowerOf2_32(Vector->getNumElements()))
    return false;
  auto *Element = Vector->getElementType();
  return (Element->isFloatTy() || Element->isDoubleTy() ||
          Element->isBFloatTy()) &&
         Vector->getPrimitiveSizeInBits() / 8 <=
             limits::kMaxCIntegerVectorBytes;
}

std::string typeToCLLVM(llvm::Type *Ty) {
  if (!Ty)
    return "void";

  if (Ty->isVoidTy())
    return "void";
  if (Ty->isIntegerTy(1))
    return "uint8_t";
  if (Ty->isIntegerTy(8))
    return "uint8_t";
  if (Ty->isIntegerTy(16))
    return "uint16_t";
  if (Ty->isIntegerTy(32))
    return "uint32_t";
  if (Ty->isIntegerTy(64))
    return "uint64_t";
  if (Ty->isIntegerTy()) {
    unsigned Bits = Ty->getIntegerBitWidth();
    if (Bits <= 8)
      return "uint8_t";
    if (Bits <= 16)
      return "uint16_t";
    if (Bits <= 32)
      return "uint32_t";
    if (Bits <= 64)
      return "uint64_t";
    if (Bits <= 128)
      return "__uint128_t";
    return "uint64_t";
  }
  if (Ty->isBFloatTy())
    return "__bf16";
  if (Ty->isFloatTy())
    return "float";
  if (Ty->isDoubleTy())
    return "double";
  if (Ty->isX86_FP80Ty())
    return "long double";
  if (Ty->isPointerTy())
    return "void*";

  if (auto *ST = llvm::dyn_cast<llvm::StructType>(Ty))
    return llvmStructName(ST);

  if (auto *VT = llvm::dyn_cast<llvm::FixedVectorType>(Ty)) {
    if (!isCVectorType(VT))
      throw std::invalid_argument(
          "C vector has no supported lane type or size");
    const unsigned Bytes = VT->getPrimitiveSizeInBits() / 8;
    return typeToCLLVM(VT->getElementType()) + " __attribute__((vector_size(" +
           std::to_string(Bytes) + ")))";
  }

  if (auto *AT = llvm::dyn_cast<llvm::ArrayType>(Ty)) {
    if (auto *IT = llvm::dyn_cast<llvm::IntegerType>(AT->getElementType()))
      return "struct neverd_array_" + std::to_string(AT->getNumElements()) +
             "_i" + std::to_string(IT->getBitWidth());
    return anonymousAggregateName(AT);
  }

  if (Ty->isFunctionTy())
    return "void*";

  return "void";
}

llvm::SmallVector<const char *, 3> getArchIntrinsicHeaders(Arch TheArch) {
  if (TheArch == Arch::X86 || TheArch == Arch::X64)
    return getX86IntrinsicHeaders();
  auto Headers = getARMIntrinsicHeaders();
  if (TheArch == Arch::AArch64)
    Headers.push_back("arm_sve.h");
  return Headers;
}

void emitCIndent(llvm::raw_ostream &OS, int Level) {
  for (int I = 0; I < Level; ++I)
    OS << "    ";
}

void writeCIndentedSnippet(llvm::raw_ostream &OS, llvm::StringRef Text,
                           int Level) {
  while (!Text.empty()) {
    const size_t Nl = Text.find('\n');
    const llvm::StringRef Line =
        Nl == llvm::StringRef::npos ? Text : Text.take_front(Nl);
    if (!Line.empty())
      emitCIndent(OS, Level);
    OS << Line;
    if (Nl == llvm::StringRef::npos)
      return;
    OS << '\n';
    Text = Text.drop_front(Nl + 1);
  }
}

} // namespace neverd
