//===- MsvcAtlCallee.h - Shared ATL/STL C-display callees -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Table-driven MSVC ATL/STL display names shared by HighC, LLVMC, and
/// identifier stemming. The rows live in MsvcAtlCallees.def.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_MSVCATLCALLEE_H
#define NEVERD_BACKEND_C_MSVCATLCALLEE_H

#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/ir/NdTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace neverd {

enum class MsvcAtlCalleeKind {
#define MSVC_ATL_CALLEE(ID, MATCH, DECORATION, MATCH_KIND, ARITY_KIND,         \
                        MAX_ARGS, RETURN_KIND, CLASS_STEM, FASTCALL)           \
  ID,
#include "neverd/backend/c/MsvcAtlCallees.def"
};

enum class MsvcAtlMatchKind { Exact, Suffix, SuffixOrDtorAlias };
enum class MsvcAtlArityKind { Fixed, CtorDrop, Keep };
enum class MsvcAtlReturnKind { Void, Bool, Int32, VoidPtr, WCharPtr };
enum class MsvcAtlClassStem { FromMatch, CStringT };

struct MsvcAtlCallee {
  MsvcAtlCalleeKind Kind;
  llvm::StringRef Match;
  char Decoration;
  MsvcAtlMatchKind MatchKind;
  MsvcAtlArityKind ArityKind;
  unsigned MaxArgs;
  MsvcAtlReturnKind ReturnKind;
  MsvcAtlClassStem ClassStem;
  bool FastCall;
};

inline llvm::ArrayRef<MsvcAtlCallee> msvcAtlCallees() {
  static const MsvcAtlCallee Table[] = {
#define MSVC_ATL_CALLEE(ID, MATCH, DECORATION, MATCH_KIND, ARITY_KIND,         \
                        MAX_ARGS, RETURN_KIND, CLASS_STEM, FASTCALL)           \
    {MsvcAtlCalleeKind::ID,                                                    \
     MATCH,                                                                    \
     DECORATION,                                                               \
     MsvcAtlMatchKind::MATCH_KIND,                                             \
     MsvcAtlArityKind::ARITY_KIND,                                             \
     MAX_ARGS,                                                                 \
     MsvcAtlReturnKind::RETURN_KIND,                                           \
     MsvcAtlClassStem::CLASS_STEM,                                             \
     FASTCALL != 0},
#include "neverd/backend/c/MsvcAtlCallees.def"
  };
  return Table;
}

inline bool msvcAtlMatchIdentifier(const MsvcAtlCallee &Row,
                                   llvm::StringRef Identifier) {
  switch (Row.MatchKind) {
  case MsvcAtlMatchKind::Exact:
    return Identifier == Row.Match;
  case MsvcAtlMatchKind::Suffix:
  case MsvcAtlMatchKind::SuffixOrDtorAlias:
    if (Row.MatchKind == MsvcAtlMatchKind::SuffixOrDtorAlias &&
        Identifier.contains("::~"))
      return true;
    return Identifier.size() > Row.Match.size() &&
           Identifier.ends_with(Row.Match) &&
           Identifier[Identifier.size() - Row.Match.size() - 1] == '_';
  }
  return false;
}

inline const MsvcAtlCallee *msvcAtlCallee(llvm::StringRef Identifier) {
  for (const MsvcAtlCallee &Row : msvcAtlCallees())
    if (msvcAtlMatchIdentifier(Row, Identifier))
      return &Row;
  return nullptr;
}

inline const char *msvcAtlSpecialMemberStem(char Decoration) {
  if (!Decoration)
    return nullptr;
  for (const MsvcAtlCallee &Row : msvcAtlCallees())
    if (Row.Decoration == Decoration)
      return Row.Match.data();
  return nullptr;
}

inline TypeRef msvcAtlSyntheticReturn(MsvcAtlReturnKind Kind) {
  switch (Kind) {
  case MsvcAtlReturnKind::Void:
    return NdType::makeVoid();
  case MsvcAtlReturnKind::Bool: {
    auto Ty = NdType::makeInt(1, false);
    Ty->SourceName = "bool";
    return Ty;
  }
  case MsvcAtlReturnKind::Int32:
    return NdType::makeInt(4, true);
  case MsvcAtlReturnKind::VoidPtr:
    return NdType::makePtr();
  case MsvcAtlReturnKind::WCharPtr: {
    auto WChar = NdType::makeInt(2, false);
    WChar->SourceName = "wchar_t";
    return NdType::makePtr(WChar);
  }
  }
  return NdType::makeVoid();
}

inline TypeRef msvcAtlSyntheticThis(llvm::StringRef Identifier,
                                    const MsvcAtlCallee &Row) {
  llvm::StringRef Class;
  if (Row.ClassStem == MsvcAtlClassStem::FromMatch &&
      Identifier.size() > Row.Match.size() + 1 &&
      Identifier.ends_with(Row.Match) &&
      Identifier[Identifier.size() - Row.Match.size() - 1] == '_')
    Class = Identifier.drop_back(Row.Match.size() + 1);
  else
    Class = "CStringT";
  if (Class.empty())
    return NdType::makePtr(NdType::makeInt(1));
  return NdType::makePtr(NdType::makeNamedRecord(Class.str(), 8));
}

template <typename IsUnknown, typename KeepExtra>
size_t msvcAtlPrintedArgLimit(const MsvcAtlCallee &Row, size_t Have,
                              IsUnknown &&UnknownAt, KeepExtra &&Keep) {
  if (Row.ArityKind == MsvcAtlArityKind::Keep)
    return Have;
  if (Row.ArityKind == MsvcAtlArityKind::Fixed)
    return std::min(Have, static_cast<size_t>(Row.MaxArgs));
  size_t Limit = Have;
  while (Limit > Row.MaxArgs &&
         (UnknownAt(Limit - 1) || !Keep(Limit - 1)))
    --Limit;
  const size_t Cap = static_cast<size_t>(Row.MaxArgs) + 2;
  if (Limit > Cap)
    Limit = Cap;
  return Limit;
}

template <typename IsUnknown>
size_t msvcAtlPrintedArgLimit(const MsvcAtlCallee &Row, size_t Have,
                              IsUnknown &&UnknownAt) {
  return msvcAtlPrintedArgLimit(Row, Have, std::forward<IsUnknown>(UnknownAt),
                                [](size_t) { return true; });
}

inline bool msvcAtlTypesCallArgAsPointer(const MsvcAtlCallee &Row,
                                         size_t Index) {
  return Index == 0 ||
         (Index == 1 && Row.ArityKind == MsvcAtlArityKind::Fixed &&
          Row.MaxArgs >= 2);
}

/// Display-only operand type. Concatenate's 3rd/5th are `int32_t` lengths;
/// ABI zext/sext must not print as wrapping casts.
inline TypeRef msvcAtlExpectedCallArgType(const MsvcAtlCallee &Row,
                                          size_t Index) {
  if (Row.Kind == MsvcAtlCalleeKind::Concatenate) {
    if (Index == 0)
      return NdType::makePtr(NdType::makeNamedRecord("CStringT", 8));
    if (Index == 1 || Index == 3)
      return msvcAtlSyntheticReturn(MsvcAtlReturnKind::WCharPtr);
    if (Index == 2 || Index == 4)
      return NdType::makeInt(4, true);
    return {};
  }
  if (Row.Kind == MsvcAtlCalleeKind::Format) {
    if (Index == 0)
      return NdType::makePtr(NdType::makeNamedRecord("CStringT", 8));
    if (Index == 1)
      return msvcAtlSyntheticReturn(MsvcAtlReturnKind::WCharPtr);
    // Win64 varargs widen integers to 64 bits. Display them as `int`
    // after peeling ABI zext/cast. Pointer extras still print as the
    // identifier / `&slot` / call (`cstr`), not `(int32_t)p`.
    return NdType::makeInt(4, true);
  }
  if (msvcAtlTypesCallArgAsPointer(Row, Index))
    return NdType::makePtr(NdType::makeInt(1));
  return {};
}

inline std::string msvcAtlSyntheticPrototype(llvm::StringRef Identifier,
                                             const MsvcAtlCallee &Row,
                                             bool FastCall) {
  const TypeRef ReturnType = msvcAtlSyntheticReturn(Row.ReturnKind);
  const TypeRef This = msvcAtlSyntheticThis(Identifier, Row);
  const TypeRef WCharPtr = msvcAtlSyntheticReturn(MsvcAtlReturnKind::WCharPtr);
  std::string Declarator = Identifier.str() + "(";
  if (Row.Kind == MsvcAtlCalleeKind::Format) {
    Declarator += declarationToC(This, "this");
    Declarator += ", ";
    Declarator += declarationToC(WCharPtr, "fmt");
    Declarator += ", ...";
  } else if (Row.Kind == MsvcAtlCalleeKind::Concatenate) {
    Declarator += declarationToC(This, "dest");
    Declarator += ", ";
    Declarator += declarationToC(WCharPtr, "a");
    Declarator += ", ";
    Declarator += declarationToC(NdType::makeInt(4, true), "na");
    Declarator += ", ";
    Declarator += declarationToC(WCharPtr, "b");
    Declarator += ", ";
    Declarator += declarationToC(NdType::makeInt(4, true), "nb");
  } else if (Row.Kind == MsvcAtlCalleeKind::Ctor) {
    // Default / copy / wchar / manager overloads share one C stem.
    Declarator += declarationToC(This, "this");
    Declarator += ", ...";
  } else {
    Declarator += declarationToC(This, "this");
    if (msvcAtlTypesCallArgAsPointer(Row, 1)) {
      Declarator += ", ";
      Declarator += declarationToC(This, "src");
    }
  }
  Declarator += ")";
  std::string Prefix = "extern ";
  if (FastCall)
    Prefix += "__fastcall ";
  return Prefix + declarationToC(ReturnType, Declarator);
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_MSVCATLCALLEE_H
