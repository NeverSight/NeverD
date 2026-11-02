//===- SymbolSpellingCxx.cpp - Itanium and Microsoft C++ names ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// C++ names read as LLVM's demanglers print them, made shorter without
/// naming anything else (CxxTemplateSimplifications.def): the standard
/// library's ABI namespaces, template arguments equal to their defaults and
/// the class keywords Microsoft's demangler writes in template arguments go,
/// and a specialization the standard names reads by that name.
/// `std::__cxx11::basic_string<char, std::char_traits<char>,
/// std::allocator<char>>::~basic_string()` reads `std::string::~string()`.
///
/// The simplification reads every qualified name in the demangled text,
/// parameter types included, as a tree of scopes and template arguments.  A
/// name it cannot read whole stays as the demangler printed it.
///
/// Their C identifier stems follow the C backend's own C++ rules
/// (CIdentifier.h), so this file gives none.
///
//===----------------------------------------------------------------------===//

#include "SymbolSpellingDetail.h"

#include "neverd/loader/SymbolSpelling.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Demangle/MicrosoftDemangle.h"

#include <optional>
#include <vector>

using namespace neverd;
using namespace neverd::symbol_spelling;

namespace {

/// One scope of a qualified name, `vector<int>` in `std::vector<int>::at`.
struct Scope {
  std::string Name;
  bool HasArguments = false;
  /// Each argument as it reads after simplification.
  std::vector<std::string> Arguments;
};

using QualifiedName = std::vector<Scope>;

std::string simplifyCxxText(llvm::StringRef Text);

bool isNameStart(char C) { return llvm::isAlpha(C) || C == '_' || C == '$'; }

bool isNameChar(char C) { return llvm::isAlnum(C) || C == '_' || C == '$'; }

/// The template arguments that start at \p Text[Pos] (a `<`), each
/// simplified, with \p Pos moved past the closing `>`; none when the
/// brackets do not balance.
std::optional<std::vector<std::string>> readArguments(llvm::StringRef Text,
                                                      size_t &Pos) {
  std::vector<std::string> Arguments;
  int Depth = 0;
  size_t Start = ++Pos;
  for (; Pos < Text.size(); ++Pos) {
    const char C = Text[Pos];
    if (C == '<' || C == '(' || C == '[' || C == '{') {
      ++Depth;
    } else if (C == ')' || C == ']' || C == '}') {
      if (--Depth < 0)
        return std::nullopt;
    } else if (C == '>' && Depth > 0) {
      // `->` in a function type closes nothing.
      if (Text[Pos - 1] != '-')
        --Depth;
    } else if ((C == '>' || C == ',') && Depth == 0) {
      llvm::StringRef Argument = Text.slice(Start, Pos).trim();
#define NEVERD_CXX_TAG_KEYWORD(Keyword) Argument.consume_front(Keyword);
#include "neverd/loader/CxxTemplateSimplifications.def"
      if (!Argument.empty() || C == ',')
        Arguments.push_back(simplifyCxxText(Argument));
      Start = Pos + 1;
      if (C == '>') {
        ++Pos;
        return Arguments;
      }
    }
  }
  return std::nullopt;
}

/// The qualified name that starts at \p Text[Pos], with \p Pos moved past
/// it; none when it does not read whole.
std::optional<QualifiedName> readQualifiedName(llvm::StringRef Text,
                                               size_t &Pos) {
  QualifiedName Name;
  while (true) {
    const size_t Start = Pos;
    if (Pos < Text.size() && Text[Pos] == '~' && !Name.empty())
      ++Pos;
    if (Pos >= Text.size() || !isNameStart(Text[Pos]))
      return std::nullopt;
    while (Pos < Text.size() && isNameChar(Text[Pos]))
      ++Pos;
    if (Text.slice(Start, Pos) == "operator") {
      // The operator a function name spells comes before its template
      // arguments: `operator<<` then `<std::char_traits<char>>`.
#define NEVERD_CXX_OPERATOR_TOKEN(Spelling)                                    \
  if (Pos == Start + 8 && Text.substr(Pos).starts_with(Spelling))              \
    Pos += llvm::StringRef(Spelling).size();
#include "neverd/loader/CxxTemplateSimplifications.def"
    }
    // An ABI tag is part of the name: `failure[abi:cxx11]`.
    if (Text.substr(Pos).starts_with("[abi:")) {
      const size_t Close = Text.find(']', Pos);
      if (Close == llvm::StringRef::npos)
        return std::nullopt;
      Pos = Close + 1;
    }
    Scope Part;
    Part.Name = Text.slice(Start, Pos).str();
    if (Pos < Text.size() && Text[Pos] == '<') {
      auto Arguments = readArguments(Text, Pos);
      if (!Arguments)
        return std::nullopt;
      Part.HasArguments = true;
      Part.Arguments = std::move(*Arguments);
    }
    Name.push_back(std::move(Part));
    if (!Text.substr(Pos).starts_with("::"))
      return Name;
    Pos += 2;
  }
}

std::string printScope(const Scope &Part) {
  std::string Out = Part.Name;
  if (!Part.HasArguments)
    return Out;
  // `operator< <int>`: the argument list must not extend the operator.
  if (!Out.empty() && Out.back() == '<')
    Out += ' ';
  Out += '<';
  Out += llvm::join(Part.Arguments, ", ");
  Out += '>';
  return Out;
}

std::string printPath(const QualifiedName &Name, size_t End) {
  std::string Out;
  for (size_t I = 0; I < End; ++I) {
    if (I)
      Out += "::";
    Out += Name[I].Name;
  }
  return Out;
}

std::string printQualifiedName(const QualifiedName &Name) {
  std::string Out;
  for (size_t I = 0; I < Name.size(); ++I) {
    if (I)
      Out += "::";
    Out += printScope(Name[I]);
  }
  return Out;
}

/// \p Pattern with each `$N` replaced by argument N.
std::string substituteArguments(llvm::StringRef Pattern,
                                llvm::ArrayRef<std::string> Arguments) {
  std::string Out;
  for (size_t I = 0; I < Pattern.size(); ++I) {
    if (Pattern[I] == '$' && I + 1 < Pattern.size() &&
        llvm::isDigit(Pattern[I + 1])) {
      const size_t Index = Pattern[++I] - '0';
      if (Index < Arguments.size())
        Out += Arguments[Index];
      continue;
    }
    Out += Pattern[I];
  }
  return Out;
}

void simplifyQualifiedName(QualifiedName &Name) {
  for (size_t I = 0; I + 1 < Name.size();) {
    bool Inline = false;
#define NEVERD_CXX_INLINE_NAMESPACE(Outer, Namespace)                          \
  Inline |= printPath(Name, I + 1) == Outer &&                                 \
            Name[I + 1].Name == Namespace && !Name[I + 1].HasArguments;
#include "neverd/loader/CxxTemplateSimplifications.def"
    if (Inline)
      Name.erase(Name.begin() + I + 1);
    else
      ++I;
  }
  for (size_t I = 0; I < Name.size(); ++I) {
    Scope &Part = Name[I];
    if (!Part.HasArguments)
      continue;
    const std::string Template = printPath(Name, I + 1);
    // Arguments equal to their defaults, from the last one down.
    bool Dropped = true;
    while (Dropped && Part.Arguments.size() > 1) {
      Dropped = false;
      const size_t Last = Part.Arguments.size() - 1;
#define NEVERD_CXX_DEFAULT_ARGUMENT(Owner, Index, Default)                     \
  if (!Dropped && Template == Owner && Last == Index &&                        \
      Part.Arguments[Last] == substituteArguments(Default, Part.Arguments))    \
    Dropped = true;
#include "neverd/loader/CxxTemplateSimplifications.def"
      if (Dropped)
        Part.Arguments.pop_back();
    }
    const std::string Specialization =
        Template + "<" + llvm::join(Part.Arguments, ", ") + ">";
    llvm::StringRef Alias;
#define NEVERD_CXX_ALIAS(Spelled, Standard)                                    \
  if (Alias.empty() && Specialization == Spelled)                              \
    Alias = Standard;
#include "neverd/loader/CxxTemplateSimplifications.def"
    if (Alias.empty())
      continue;
    size_t AliasEnd = 0;
    auto Replacement = readQualifiedName(Alias, AliasEnd);
    if (!Replacement || AliasEnd != Alias.size())
      continue;
    // Its constructors and destructor read by the alias too.
    const std::string TemplateName = Part.Name;
    const std::string AliasName = Replacement->back().Name;
    if (I + 1 < Name.size()) {
      std::string &Member = Name[I + 1].Name;
      if (Member == TemplateName)
        Member = AliasName;
      else if (Member == "~" + TemplateName)
        Member = "~" + AliasName;
    }
    const size_t Width = Replacement->size();
    Name.erase(Name.begin(), Name.begin() + I + 1);
    Name.insert(Name.begin(), Replacement->begin(), Replacement->end());
    I = Width - 1;
  }
}

/// \p Text with every qualified name it holds simplified.
std::string simplifyCxxText(llvm::StringRef Text) {
  std::string Out;
  size_t Pos = 0;
  while (Pos < Text.size()) {
    if (isNameStart(Text[Pos]) && (Pos == 0 || !isNameChar(Text[Pos - 1]))) {
      size_t End = Pos;
      if (auto Name = readQualifiedName(Text, End)) {
        simplifyQualifiedName(*Name);
        Out += printQualifiedName(*Name);
        Pos = End;
        continue;
      }
      // An identifier this cannot read whole stays as it is.
      const size_t Start = Pos;
      while (Pos < Text.size() && isNameChar(Text[Pos]))
        ++Pos;
      Out += Text.slice(Start, Pos);
      continue;
    }
    Out += Text[Pos++];
  }
  return Out;
}

} // namespace

std::string neverd::readableCxxTypeName(llvm::StringRef Name) {
  return simplifyCxxText(Name);
}

std::string neverd::cxxSourceName(llvm::StringRef Name) {
  switch (symbolScheme(Name)) {
  case SymbolScheme::Itanium: {
    if (Name.starts_with("__Z"))
      Name = Name.drop_front();
    const std::string Mangled = Name.str();
    llvm::ItaniumPartialDemangler D;
    if (D.partialDemangle(Mangled.c_str()) || D.isSpecialName())
      return {};
    if (D.isFunction())
      return simplifyCxxText(
          takeDemangled(D.getFunctionName(nullptr, nullptr)));
    if (D.isData())
      return simplifyCxxText(takeDemangled(D.finishDemangle(nullptr, nullptr)));
    return {};
  }
  case SymbolScheme::Microsoft: {
    if (Name.starts_with("_?"))
      Name = Name.drop_front();
    std::string_view Remaining = Name;
    llvm::ms_demangle::Demangler D;
    const auto *Node = D.parse(Remaining);
    using llvm::ms_demangle::NodeKind;
    if (D.Error || !Remaining.empty() || !Node || !Node->Name ||
        (Node->kind() != NodeKind::FunctionSymbol &&
         Node->kind() != NodeKind::VariableSymbol))
      return {};
    return simplifyCxxText(
        Node->Name->toString(llvm::ms_demangle::OF_NoTagSpecifier));
  }
  default:
    return {};
  }
}

bool neverd::isItaniumConstructorOrDestructor(llvm::StringRef Name) {
  if (symbolScheme(Name) != SymbolScheme::Itanium)
    return false;
  if (Name.starts_with("__Z"))
    Name = Name.drop_front();
  const std::string Mangled = Name.str();
  llvm::ItaniumPartialDemangler Demangler;
  return !Demangler.partialDemangle(Mangled.c_str()) &&
         Demangler.isFunction() && Demangler.isCtorOrDtor();
}

bool symbol_spelling::claimsItaniumName(llvm::StringRef) { return true; }

std::string symbol_spelling::readableItaniumName(llvm::StringRef Name) {
  return simplifyCxxText(takeDemangled(llvm::itaniumDemangle(Name)));
}

std::string symbol_spelling::stemOfItaniumName(llvm::StringRef) { return {}; }

bool symbol_spelling::claimsMicrosoftName(llvm::StringRef) { return true; }

std::string symbol_spelling::readableMicrosoftName(llvm::StringRef Name) {
  // `public:` says nothing a reader of decompiled code can use; the calling
  // convention and the return type do.
  return simplifyCxxText(takeDemangled(llvm::microsoftDemangle(
      Name, nullptr, nullptr, llvm::MSDF_NoAccessSpecifier)));
}

std::string symbol_spelling::stemOfMicrosoftName(llvm::StringRef) { return {}; }
