#include "neverd/loader/ObjC/ObjCFormattedCalls.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCConstantStrings.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include <algorithm>
#include <map>

namespace neverd {
namespace {
std::optional<std::vector<uint16_t>>
readImmutableCFormat(const BinaryImage &Image, va_t Address) {
  constexpr uint32_t MaxUnits = 65536;
  uint32_t Units = 0;
  for (; Units <= MaxUnits; ++Units) {
    if (Address > InvalidVA - Units)
      return std::nullopt;
    const auto *Byte = Image.readVA(Address + Units, 1);
    if (!Byte)
      return std::nullopt;
    if (!*Byte)
      break;
  }
  if (Units > MaxUnits)
    return std::nullopt;
  auto Bytes = readImmutableImageBytes(Image, Address, Units + 1);
  if (!Bytes || Bytes->size() != Units + 1 || Bytes->back())
    return std::nullopt;
  std::vector<uint16_t> Result;
  Result.reserve(Units);
  for (uint32_t I = 0; I < Units; ++I)
    Result.push_back((*Bytes)[I]);
  return Result;
}

std::optional<SourceCallTypeHint>
bindFormatArguments(const BinaryImage &Image, SourceCallTypeHint Result,
                    unsigned FormatParameter, va_t FormatAddress,
                    SourceCallTypeHint::FormatSyntax Syntax,
                    llvm::ArrayRef<uint16_t> Format) {
  auto Arguments = objcFormatArgumentTypes(Format, Syntax);
  if (!Arguments || FormatParameter >= Result.Signature.Parameters.size() ||
      !Result.Signature.Parameters[FormatParameter].Type ||
      Result.Signature.Parameters[FormatParameter].Type->Kind !=
          NdTypeKind::Ptr ||
      Result.Signature.Parameters.size() + Arguments->size() > 64)
    return std::nullopt;
  const auto Fixed = unsigned(Result.Signature.Parameters.size());
  Result.Format = SourceCallTypeHint::FormatArguments{Fixed, FormatParameter,
                                                      FormatAddress, Syntax};
  for (const auto &Type : *Arguments)
    Result.Signature.Parameters.push_back({"format_arg", Type});
  std::string Diagnostic;
  if (!assignDarwinVariadicSourceABI(Result.Signature, Fixed, Image.Arch,
                                     Diagnostic))
    return std::nullopt;
  return Result;
}

// Predicate substitutions are tokens outside quoted literals. Extract only
// the proven conversion tokens, then share the promoted scalar type rules
// with NSString. The original string still goes to the framework parser.
std::optional<std::vector<uint16_t>>
predicateConversions(llvm::ArrayRef<uint16_t> Format) {
  std::vector<uint16_t> Result;
  uint16_t Quote = 0;
  for (size_t I = 0; I < Format.size(); ++I) {
    const auto C = Format[I];
    // Escape sequences can affect token boundaries; they need their own
    // lexical contract before their argument consumption can be inferred.
    if (C == '\\')
      return std::nullopt;
    if (Quote) {
      if (C == Quote)
        Quote = 0;
      continue;
    }
    if (C == '\'' || C == '"') {
      Quote = C;
      continue;
    }
    if (C != '%')
      continue;
    Result.push_back('%');
    if (++I == Format.size())
      return std::nullopt;
    if (Format[I] == 'K' || Format[I] == '@') {
      Result.push_back('@');
      continue;
    }
    // Width, precision, positional, and string-storage modifiers are not
    // established by this predicate contract. No guessed argument slots.
    if (Format[I] < 128 &&
        llvm::StringRef("hlqjzt").contains(char(Format[I]))) {
      const auto Length = Format[I];
      Result.push_back(Length);
      if (++I == Format.size())
        return std::nullopt;
      if ((Length == 'h' || Length == 'l') && Format[I] == Length) {
        Result.push_back(Length);
        if (++I == Format.size())
          return std::nullopt;
      }
    }
    if (Format[I] >= 128 ||
        !llvm::StringRef("diouxXaAeEfFgG").contains(char(Format[I])))
      return std::nullopt;
    Result.push_back(Format[I]);
  }
  return Quote ? std::nullopt : std::optional(std::move(Result));
}
} // namespace

std::optional<std::vector<TypeRef>>
objcFormatArgumentTypes(llvm::ArrayRef<uint16_t> Format,
                        SourceCallTypeHint::FormatSyntax Syntax) {
  if (Format.size() > 65536 || llvm::is_contained(Format, uint16_t(0)))
    return std::nullopt;
  if (Syntax == SourceCallTypeHint::FormatSyntax::Predicate) {
    auto Tokens = predicateConversions(Format);
    return Tokens ? objcFormatArgumentTypes(*Tokens) : std::nullopt;
  }
  const bool Printf = Syntax == SourceCallTypeHint::FormatSyntax::Printf;
  if (!Printf && Syntax != SourceCallTypeHint::FormatSyntax::NSString)
    return std::nullopt;
  size_t Cursor = 0;
  unsigned Sequential = 0;
  bool Positional = false, Unnumbered = false;
  std::map<unsigned, TypeRef> Types;
  auto Peek = [&]() { return Cursor < Format.size() ? Format[Cursor] : 0; };
  auto Digit = [&]() { return Peek() >= '0' && Peek() <= '9'; };
  auto Position = [&]() -> std::optional<unsigned> {
    const auto Start = Cursor;
    unsigned N = 0;
    while (Digit()) {
      N = std::min(65U, N * 10 + (Format[Cursor++] - '0'));
    }
    if (Peek() != '$') {
      Cursor = Start;
      return 0;
    }
    ++Cursor;
    return N && N <= 64 ? std::optional(N) : std::nullopt;
  };
  auto Argument = [&](unsigned Position, TypeRef Type) {
    Positional |= Position != 0;
    Unnumbered |= Position == 0;
    if (Positional && Unnumbered)
      return false;
    if (!Position)
      Position = ++Sequential;
    if (Position > 64)
      return false;
    auto [It, Added] = Types.emplace(Position, Type);
    return Added || equalSourceTypes(It->second, Type);
  };
  auto Width = [&]() {
    if (Peek() == '*') {
      ++Cursor;
      auto P = Position();
      return P && Argument(*P, NdType::makeInt(4, true));
    }
    while (Digit())
      ++Cursor;
    return true;
  };
  while (Cursor < Format.size()) {
    if (Format[Cursor++] != '%')
      continue;
    if (Peek() == '%') {
      ++Cursor;
      continue;
    }
    auto P = Position();
    if (!P)
      return std::nullopt;
    while (Peek() && Peek() < 128 &&
           llvm::StringRef("-+ #0'").contains(char(Peek())))
      ++Cursor;
    if (!Width())
      return std::nullopt;
    if (Peek() == '.') {
      ++Cursor;
      if (!Width())
        return std::nullopt;
    }
    std::string Length;
    if (Peek() && Peek() < 128 &&
        llvm::StringRef("hlqjztL").contains(char(Peek()))) {
      Length += char(Format[Cursor++]);
      if ((Length == "h" || Length == "l") && Peek() == Length.front())
        Length += char(Format[Cursor++]);
    }
    const auto Conversion = Peek();
    if (!Conversion || Conversion > 127)
      return std::nullopt;
    ++Cursor;
    TypeRef Type;
    if (llvm::StringRef("diouxX").contains(char(Conversion))) {
      if (Length == "L")
        return std::nullopt;
      const bool Narrow = Length.empty() || Length == "h" || Length == "hh";
      Type = NdType::makeInt(Narrow ? 4 : 8,
                             Conversion == 'd' || Conversion == 'i' ||
                                 Length == "h" || Length == "hh");
    } else if (llvm::StringRef("aAeEfFgG").contains(char(Conversion))) {
      if (!Length.empty() && Length != "l")
        return std::nullopt;
      Type = NdType::makeFloat(8);
    } else if (Conversion == 'c' || (!Printf && Conversion == 'C')) {
      if (!Length.empty())
        return std::nullopt;
      Type = NdType::makeInt(4, true);
    } else if (Conversion == 's' || (!Printf && Conversion == 'S')) {
      if (!Length.empty())
        return std::nullopt;
      Type = NdType::makePtr(
          NdType::makeInt(Conversion == 's' ? 1 : 2, Conversion == 's'));
    } else if ((!Printf && Conversion == '@') || Conversion == 'p') {
      if (!Length.empty())
        return std::nullopt;
      Type = NdType::makePtr(NdType::makeVoid());
    } else {
      // Count writes, locale extensions, long double, and wide C strings need
      // their own contracts; a plausible argument count is insufficient.
      return std::nullopt;
    }
    if (!Argument(*P, std::move(Type)))
      return std::nullopt;
  }
  std::vector<TypeRef> Result;
  for (const auto &[Position, Type] : Types) {
    if (Position != Result.size() + 1)
      return std::nullopt;
    Result.push_back(Type);
  }
  return Result;
}

std::optional<SourceCallTypeHint>
bindObjCFormatArguments(const BinaryImage &Image, SourceCallTypeHint Result,
                        unsigned FormatParameter, va_t FormatAddress,
                        SourceCallTypeHint::FormatSyntax Syntax) {
  auto Format = readObjCConstantString(Image, FormatAddress);
  if (!Format || Syntax == SourceCallTypeHint::FormatSyntax::Printf)
    return std::nullopt;
  return bindFormatArguments(Image, std::move(Result), FormatParameter,
                             FormatAddress, Syntax, Format->Units);
}

std::optional<SourceCallTypeHint>
bindCFormatArguments(const BinaryImage &Image, SourceCallTypeHint Result,
                     unsigned FormatParameter, va_t FormatAddress) {
  auto Format = readImmutableCFormat(Image, FormatAddress);
  if (!Format)
    return std::nullopt;
  return bindFormatArguments(Image, std::move(Result), FormatParameter,
                             FormatAddress,
                             SourceCallTypeHint::FormatSyntax::Printf, *Format);
}

std::optional<SourceCallTypeHint>
objcFormattedSourceCallHint(const BinaryImage &Image, llvm::StringRef Selector,
                            va_t FormatAddress) {
  auto Declaration = objcSelectorFormatDeclaration(Image, Selector);
  if (!Declaration)
    return std::nullopt;
  SourceCallTypeHint Call;
  Call.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Call.Selector = Selector.str();
  Call.TargetName = "objc_msgSend";
  Call.Signature = std::move(Declaration->Signature);
  return bindObjCFormatArguments(Image, std::move(Call),
                                 Declaration->FormatParameter, FormatAddress,
                                 Declaration->Syntax);
}

std::optional<SourceCallTypeHint>
objcFormattedSourceCallHint(const BinaryImage &Image, llvm::StringRef Selector,
                            llvm::ArrayRef<va_t> FormatAddresses) {
  if (FormatAddresses.empty() || FormatAddresses.size() > 64)
    return std::nullopt;
  std::vector<va_t> Addresses(FormatAddresses.begin(), FormatAddresses.end());
  llvm::sort(Addresses);
  if (!Addresses.front() ||
      std::adjacent_find(Addresses.begin(), Addresses.end()) != Addresses.end())
    return std::nullopt;
  auto Result = objcFormattedSourceCallHint(Image, Selector, Addresses.front());
  if (!Result || !Result->Format)
    return std::nullopt;
  for (size_t I = 1; I < Addresses.size(); ++I) {
    auto Candidate =
        objcFormattedSourceCallHint(Image, Selector, Addresses[I]);
    if (!Candidate || !Candidate->Format ||
        Candidate->CallKind != Result->CallKind ||
        Candidate->TargetName != Result->TargetName ||
        Candidate->Selector != Result->Selector ||
        Candidate->Format->FixedCount != Result->Format->FixedCount ||
        Candidate->Format->FormatParameter !=
            Result->Format->FormatParameter ||
        Candidate->Format->Syntax != Result->Format->Syntax ||
        !equalSourceABIs(Candidate->Signature, Result->Signature))
      return std::nullopt;
  }
  Result->Format->AlternativeFormatAddresses.assign(Addresses.begin() + 1,
                                                     Addresses.end());
  return Result;
}
} // namespace neverd
