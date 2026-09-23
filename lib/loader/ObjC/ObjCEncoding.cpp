#include "neverd/loader/ObjC/ObjCEncoding.h"

#include <algorithm>

namespace neverd {
namespace {
bool digits(llvm::StringRef Encoding, size_t &I, bool Required) {
  const auto Begin = I;
  while (I < Encoding.size() && Encoding[I] >= '0' && Encoding[I] <= '9')
    ++I;
  return I - Begin <= 10 && (!Required || I != Begin);
}

bool quoted(llvm::StringRef Encoding, size_t &I) {
  if (I >= Encoding.size() || Encoding[I++] != '"')
    return false;
  while (I < Encoding.size()) {
    const auto C = Encoding[I++];
    if (C == '"')
      return true;
    if (C < 0x20 || C > 0x7e || C == '\\')
      return false;
  }
  return false;
}

// Validate the syntax inside an opaque aggregate pointer. No field widths,
// offsets or ownership are inferred from this traversal.
bool skipPointeeType(llvm::StringRef Encoding, size_t &I, unsigned Depth) {
  if (Depth > 16)
    return false;
  while (I < Encoding.size() &&
         llvm::StringRef("rnNoORV").contains(Encoding[I]))
    ++I;
  if (I >= Encoding.size())
    return false;
  const char C = Encoding[I++];
  if (llvm::StringRef("vcCsSiIlLqQBfdD#:*?").contains(C))
    return true;
  if (C == '^')
    return skipPointeeType(Encoding, I, Depth + 1);
  if (C == 'b')
    return digits(Encoding, I, true);
  if (C == 'j')
    return I < Encoding.size() &&
           llvm::StringRef("fdD").contains(Encoding[I++]);
  if (C == '@') {
    if (I < Encoding.size() && Encoding[I] == '?')
      ++I;
    else if (I < Encoding.size() && Encoding[I] == '"')
      return quoted(Encoding, I);
    return true;
  }
  if (C == '[') {
    if (!digits(Encoding, I, true) || !skipPointeeType(Encoding, I, Depth + 1))
      return false;
    return I < Encoding.size() && Encoding[I++] == ']';
  }
  if (C != '{' && C != '(')
    return false;
  const char Close = C == '{' ? '}' : ')';
  const auto Name = I;
  while (I < Encoding.size() && Encoding[I] != '=' && Encoding[I] != Close) {
    const char Byte = Encoding[I++];
    if (Byte < 0x21 || Byte > 0x7e ||
        llvm::StringRef("{}()[]\"\\").contains(Byte))
      return false;
  }
  if (I == Name || I >= Encoding.size())
    return false;
  if (Encoding[I++] == Close)
    return true; // An opaque named declaration has no field list.
  while (I < Encoding.size() && Encoding[I] != Close) {
    if (Encoding[I] == '"' && !quoted(Encoding, I))
      return false;
    if (!skipPointeeType(Encoding, I, Depth + 1))
      return false;
  }
  return I < Encoding.size() && Encoding[I++] == Close;
}
} // namespace

std::optional<std::string> objcEncodedObjectClass(llvm::StringRef Encoding) {
  if (Encoding.size() > 4096)
    return std::nullopt;
  while (!Encoding.empty() &&
         llvm::StringRef("rnNoORV").contains(Encoding.front()))
    Encoding = Encoding.drop_front();
  if (!Encoding.consume_front("@\"") || !Encoding.consume_back("\"") ||
      Encoding.empty())
    return std::nullopt;
  auto Letter = [](char C) {
    return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_';
  };
  if (!Letter(Encoding.front()) ||
      !std::all_of(Encoding.begin(), Encoding.end(),
                   [&](char C) { return Letter(C) || (C >= '0' && C <= '9'); }))
    return std::nullopt;
  return Encoding.str();
}

std::optional<std::string>
objcEncodedObjectProtocol(llvm::StringRef Encoding) {
  if (Encoding.size() > 4096)
    return std::nullopt;
  while (!Encoding.empty() &&
         llvm::StringRef("rnNoORV").contains(Encoding.front()))
    Encoding = Encoding.drop_front();
  if (!Encoding.consume_front("@\"<") || !Encoding.consume_back(">\"") ||
      Encoding.empty())
    return std::nullopt;
  auto Letter = [](char C) {
    return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_';
  };
  if (!Letter(Encoding.front()) ||
      !std::all_of(Encoding.begin(), Encoding.end(), [&](char C) {
        return Letter(C) || (C >= '0' && C <= '9');
      }))
    return std::nullopt;
  return Encoding.str();
}

// Declaration syntax and natural record layout are independent of physical
// calling conventions. SourceABI must separately accept each value carrier.
TypeRef parseObjCSourceType(llvm::StringRef Encoding, size_t &I,
                            unsigned Depth) {
  if (Depth > 16 || Encoding.size() > 4096)
    return nullptr;
  while (I < Encoding.size() &&
         llvm::StringRef("rnNoORV").contains(Encoding[I]))
    ++I;
  if (I >= Encoding.size())
    return nullptr;
  const char C = Encoding[I++];
  switch (C) {
  case 'v':
    return NdType::makeVoid();
  case 'c':
    return NdType::makeInt(1, true);
  case 'C':
  case 'B':
    return NdType::makeInt(1, false);
  case 's':
    return NdType::makeInt(2, true);
  case 'S':
    return NdType::makeInt(2, false);
  case 'i':
    return NdType::makeInt(4, true);
  case 'I':
    return NdType::makeInt(4, false);
  // Darwin uses q/Q for 64-bit long; legacy l/L encodings remain 32 bits.
  case 'l':
    return NdType::makeInt(4, true);
  case 'L':
    return NdType::makeInt(4, false);
  case 'q':
    return NdType::makeInt(8, true);
  case 'Q':
    return NdType::makeInt(8, false);
  case 'f':
    return NdType::makeFloat(4);
  case 'd':
    return NdType::makeFloat(8);
  case '#':
  case ':':
    return NdType::makePtr(NdType::makeVoid());
  case '*':
    return NdType::makePtr(NdType::makeInt(1));
  case '@':
    if (I < Encoding.size() && Encoding[I] == '?') {
      ++I; // This pointer does not describe the block's invoke ABI.
      return NdType::makePtr(NdType::makeVoid());
    }
    if (I < Encoding.size() && Encoding[I] == '"') {
      if (!quoted(Encoding, I))
        return nullptr;
    }
    return NdType::makePtr(NdType::makeVoid());
  case '{': {
    const auto Name = I;
    while (I < Encoding.size() && Encoding[I] != '=') {
      const char Byte = Encoding[I++];
      if (Byte < 0x21 || Byte > 0x7e ||
          llvm::StringRef("{}()[]\"\\").contains(Byte))
        return nullptr;
    }
    if (I == Name || I >= Encoding.size())
      return nullptr;
    ++I;
    std::vector<TypeRef> Fields;
    while (I < Encoding.size() && Encoding[I] != '}') {
      if (Fields.size() >= 64 || (Encoding[I] == '"' && !quoted(Encoding, I)))
        return nullptr;
      auto Field = parseObjCSourceType(Encoding, I, Depth + 1);
      if (!Field)
        return nullptr;
      Fields.push_back(std::move(Field));
    }
    if (I >= Encoding.size() || Encoding[I++] != '}')
      return nullptr;
    return NdType::makeStruct(std::move(Fields));
  }
  case '^': {
    auto Start = I;
    while (Start < Encoding.size() &&
           llvm::StringRef("rnNoORV").contains(Encoding[Start]))
      ++Start;
    if (Start < Encoding.size() &&
        llvm::StringRef("{([").contains(Encoding[Start])) {
      if (!skipPointeeType(Encoding, I, Depth + 1))
        return nullptr;
      return NdType::makePtr(NdType::makeVoid());
    }
    auto Pointee = parseObjCSourceType(Encoding, I, Depth + 1);
    return Pointee ? NdType::makePtr(Pointee) : nullptr;
  }
  default:
    return nullptr;
  }
}

TypeRef parseObjCScalarType(llvm::StringRef Encoding, size_t &Offset,
                            unsigned Depth) {
  auto Type = parseObjCSourceType(Encoding, Offset, Depth);
  return Type && Type->Kind != NdTypeKind::Struct ? Type : nullptr;
}

std::optional<SourceFunctionTypeHint>
parseObjCMethodEncoding(llvm::StringRef Selector, llvm::StringRef Encoding) {
  if (Selector.empty() || Selector.size() > 4096 || Encoding.size() > 4096)
    return std::nullopt;
  size_t I = 0;
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = parseObjCSourceType(Encoding, I);
  if (!Hint.ReturnType || !digits(Encoding, I, false))
    return std::nullopt;
  std::vector<char> Codes;
  while (I < Encoding.size() && Hint.Parameters.size() < 64) {
    auto Start = I;
    while (Start < Encoding.size() &&
           llvm::StringRef("rnNoORV").contains(Encoding[Start]))
      ++Start;
    Codes.push_back(Encoding.substr(Start).starts_with("@?") ? '?'
                    : Start < Encoding.size()                ? Encoding[Start]
                                                             : '\0');
    auto Type = parseObjCSourceType(Encoding, I);
    if (!Type || Type->Kind == NdTypeKind::Void || !digits(Encoding, I, false))
      return std::nullopt;
    const auto Index = Hint.Parameters.size();
    Hint.Parameters.push_back({Index == 0   ? "objc_self"
                               : Index == 1 ? "objc_cmd"
                                            : "arg" + std::to_string(Index - 2),
                               std::move(Type)});
  }
  const auto Arity = std::count(Selector.begin(), Selector.end(), ':');
  if (I != Encoding.size() || Hint.Parameters.size() < 2 ||
      (Codes[0] != '@' && Codes[0] != '#') || Codes[1] != ':' ||
      Hint.Parameters.size() != static_cast<size_t>(Arity) + 2)
    return std::nullopt;
  return Hint;
}

std::optional<SourceFunctionTypeHint>
parseObjCFunctionEncoding(llvm::StringRef Encoding) {
  if (Encoding.empty() || Encoding.size() > 4096)
    return std::nullopt;
  size_t I = 0;
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = parseObjCSourceType(Encoding, I);
  if (!Hint.ReturnType || !digits(Encoding, I, true))
    return std::nullopt;
  while (I < Encoding.size() && Hint.Parameters.size() < 64) {
    auto Type = parseObjCSourceType(Encoding, I);
    if (!Type || Type->Kind == NdTypeKind::Void || !digits(Encoding, I, true))
      return std::nullopt;
    Hint.Parameters.push_back(
        {"arg" + std::to_string(Hint.Parameters.size()), std::move(Type)});
  }
  return I == Encoding.size() ? std::optional(std::move(Hint)) : std::nullopt;
}

} // namespace neverd
