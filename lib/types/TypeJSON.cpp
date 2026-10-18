//===- TypeJSON.cpp - Local types as JSON for frontends -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/types/TypeJSON.h"

#include "TypeRenderShared.h"

namespace neverd::types {
namespace {

/// \p Text as JSON may hold it: names come from untrusted debug records.
std::string text(llvm::StringRef Text) {
  return llvm::json::isUTF8(Text) ? Text.str() : llvm::json::fixUTF8(Text);
}

/// The library type \p Type names through pointers and arrays, if any.
std::string refers(const TypeExpr &Type) {
  const TypeExpr *Inner = &Type;
  while ((Inner->Kind == TypeExprKind::Pointer ||
          Inner->Kind == TypeExprKind::Array) &&
         Inner->Element)
    Inner = Inner->Element.get();
  return Inner->Kind == TypeExprKind::Named ? Inner->Name : std::string();
}

/// Counts the named members a record shows, anonymous records' included.
size_t namedMembers(const RecordDef &Record) {
  size_t Count = 0;
  for (const Member &Field : Record.Members) {
    if (Field.isAnonymous())
      Count += namedMembers(*Field.Type->Record);
    else
      ++Count;
  }
  return Count;
}

class Builder {
public:
  Builder(const TypeLibrary &Library, BuiltinSpelling Spelling)
      : Library(Library), Speller(Library, options(Spelling)) {}

  llvm::json::Array members(const RecordDef &Record, uint64_t Base) const;
  std::string spell(const TypeExpr &Type,
                    const std::string &Declarator = std::string()) const {
    return Speller.declare(Type, Declarator);
  }

private:
  static RenderOptions options(BuiltinSpelling Spelling) {
    RenderOptions Options;
    Options.Spelling = Spelling;
    return Options;
  }
  static llvm::json::Object padding(uint64_t Byte, uint64_t Bytes) {
    return llvm::json::Object{{"kind", "padding"},
                              {"name", ""},
                              {"offset", Byte},
                              {"bitOffset", Byte * 8},
                              {"size", Bytes}};
  }

  const TypeLibrary &Library;
  render::CSpeller Speller;
};

llvm::json::Array Builder::members(const RecordDef &Record,
                                   uint64_t Base) const {
  llvm::json::Array Out;
  // Where the compiler would put the next member of a structure, in bytes
  // from the outermost record.
  uint64_t End = Base / 8;
  uint64_t Widest = 0;
  for (const Member &Field : Record.Members) {
    if (!Field.Type)
      continue;
    const uint64_t Bit = Base + Field.BitOffset;
    const uint64_t Size = Library.sizeOf(*Field.Type).value_or(0);
    const uint64_t Byte =
        Field.isBitfield() ? (Base + Field.StorageBitOffset) / 8 : Bit / 8;
    if (!Record.Union && Byte > End)
      Out.push_back(padding(End, Byte - End));
    llvm::json::Object Entry{{"name", text(Field.Name)},
                             {"offset", Byte},
                             {"bitOffset", Bit},
                             {"size", Size}};
    std::vector<uint64_t> Bounds;
    const TypeExpr &Element = render::arrayElement(*Field.Type, Bounds);
    if (Element.Kind == TypeExprKind::Record && Element.Record) {
      // A record in place, an array of one too.
      Entry["kind"] = Element.Record->Union ? "union" : "struct";
      std::string Bound;
      for (uint64_t Count : Bounds)
        Bound += "[" + std::to_string(Count) + "]";
      Entry["type"] =
          std::string(Element.Record->Union ? "union" : "struct") + Bound;
      Entry["members"] = members(*Element.Record, Bit);
    } else if (Field.isBitfield()) {
      Entry["kind"] = "bitfield";
      Entry["bitWidth"] = Field.BitWidth;
      Entry["storageOffset"] = Byte;
      Entry["type"] = spell(*Field.Type);
    } else {
      Entry["kind"] = "field";
      Entry["type"] = spell(*Field.Type);
    }
    const std::string Named = refers(*Field.Type);
    if (!Named.empty())
      Entry["refers"] = text(Named);
    Out.push_back(std::move(Entry));
    End = std::max(End, Byte + Size);
    Widest = std::max(Widest, Size);
  }
  // A structure's tail, or a union's beyond its widest alternative.
  const uint64_t Finish = Base / 8 + Record.Size;
  if (Record.Union && Record.Size > Widest)
    Out.push_back(padding(Base / 8 + Widest, Record.Size - Widest));
  else if (!Record.Union && Finish > End)
    Out.push_back(padding(End, Finish - End));
  return Out;
}

} // namespace

llvm::StringRef kindName(TypeKind Kind) {
  switch (Kind) {
  case TypeKind::Struct:
    return "struct";
  case TypeKind::Union:
    return "union";
  case TypeKind::Enum:
    return "enum";
  case TypeKind::Typedef:
    return "typedef";
  case TypeKind::Function:
    return "function";
  }
  return "struct";
}

llvm::StringRef originName(TypeOrigin Origin) {
  switch (Origin) {
  case TypeOrigin::PDB:
    return "pdb";
  case TypeOrigin::DWARF:
    return "dwarf";
  case TypeOrigin::Header:
    return "header";
  case TypeOrigin::User:
    return "user";
  }
  return "user";
}

llvm::StringRef dataModelName(DataModel Model) {
  switch (Model) {
  case DataModel::LLP64:
    return "LLP64";
  case DataModel::LP64:
    return "LP64";
  case DataModel::ILP32:
    return "ILP32";
  }
  return "LLP64";
}

std::optional<DataModel> dataModelNamed(llvm::StringRef Name) {
  for (DataModel Model : {DataModel::LLP64, DataModel::LP64, DataModel::ILP32})
    if (Name == dataModelName(Model))
      return Model;
  return std::nullopt;
}

std::optional<RecordABI> recordABINamed(llvm::StringRef Name) {
#define NEVERD_RECORD_ABI(Id, Spelling, Rules, ScalarAlignLimit)               \
  if (Name == Spelling)                                                        \
    return RecordABI::Id;
#include "neverd/types/RecordABIs.def"
  return std::nullopt;
}

llvm::json::Array typeListJSON(const TypeLibrary &Library) {
  llvm::json::Array Out;
  for (const LocalType *Type : Library.types()) {
    llvm::json::Object Entry{{"ordinal", Type->Ordinal},
                             {"name", text(Type->Name)},
                             {"kind", kindName(Type->Kind)},
                             {"origin", originName(Type->Origin)}};
    TypeExpr Self;
    Self.Kind = TypeExprKind::Named;
    Self.Name = Type->Name;
    if (std::optional<uint64_t> Size = Library.sizeOf(Self))
      Entry["size"] = *Size;
    if (Type->Record)
      Entry["members"] = namedMembers(*Type->Record);
    else if (Type->Enum)
      Entry["members"] = Type->Enum->Values.size();
    Out.push_back(std::move(Entry));
  }
  return Out;
}

llvm::json::Object typeJSON(const TypeLibrary &Library, const LocalType &Type,
                            BuiltinSpelling Spelling) {
  Builder Build(Library, Spelling);
  llvm::json::Object Out{{"ordinal", Type.Ordinal},
                         {"name", text(Type.Name)},
                         {"kind", kindName(Type.Kind)},
                         {"origin", originName(Type.Origin)}};
  if (!Type.Comment.empty())
    Out["comment"] = text(Type.Comment);
  if (Type.Record) {
    const RecordDef &Record = *Type.Record;
    Out["defined"] = Record.Defined;
    if (Record.Defined) {
      Out["size"] = Record.Size;
      Out["alignment"] = Library.alignOf(Record);
      Out["pack"] = Record.Pack;
      Out["declaredAlignment"] = Record.Alignment;
      Out["members"] = Build.members(Record, 0);
    }
  } else if (Type.Enum) {
    const EnumDef &Enum = *Type.Enum;
    const bool Signed = builtinInfo(Enum.Underlying).Signed;
    Out["size"] = builtinBytes(Enum.Underlying, Library.dataModel());
    Out["underlying"] = Build.spell(*makeBuiltin(Enum.Underlying));
    Out["flags"] = Enum.Flags;
    llvm::json::Array Values;
    for (const Enumerator &Value : Enum.Values) {
      llvm::json::Object Entry{{"name", text(Value.Name)}};
      if (Signed)
        Entry["value"] = static_cast<int64_t>(Value.Value);
      else
        Entry["value"] = Value.Value;
      if (!Value.Comment.empty())
        Entry["comment"] = text(Value.Comment);
      Values.push_back(std::move(Entry));
    }
    Out["values"] = std::move(Values);
  } else if (Type.Target) {
    Out["target"] = Build.spell(*Type.Target);
    const std::string Named = refers(*Type.Target);
    if (!Named.empty())
      Out["refers"] = text(Named);
    if (std::optional<uint64_t> Size = Library.sizeOf(*Type.Target))
      Out["size"] = *Size;
  } else if (Type.Function) {
    Out["declaration"] = Build.spell(*makeFunction(Type.Function), Type.Name);
  }
  return Out;
}

llvm::StringRef accessFitName(AccessFit Fit) {
  switch (Fit) {
  case AccessFit::Exact:
    return "exact";
  case AccessFit::Start:
    return "start";
  case AccessFit::Inside:
    return "inside";
  case AccessFit::Padding:
    return "padding";
  case AccessFit::Outside:
    return "outside";
  }
  return "outside";
}

namespace {

llvm::json::Object landing(const Builder &Build, const MemberAt &At) {
  llvm::json::Object Out{{"fit", accessFitName(At.Fit)},
                         {"path", text(memberPathText(At.Path))},
                         {"memberOffset", At.MemberOffset}};
  if (At.Fit == AccessFit::Padding || At.Fit == AccessFit::Outside)
    return Out;
  Out["memberSize"] = At.MemberBytes;
  if (At.Type)
    Out["type"] = Build.spell(*At.Type);
  if (At.Bitfield)
    Out["bitfield"] = true;
  return Out;
}

} // namespace

llvm::json::Object memberAtJSON(const TypeLibrary &Library, const MemberAt &At,
                                BuiltinSpelling Spelling) {
  return landing(Builder(Library, Spelling), At);
}

namespace {

/// typeFitJSON with the speller \p Build, which reads the whole library to
/// spell its names: made by the first fit with landings to spell, and kept
/// for the others of the same search.
llvm::json::Object fitJSON(const TypeLibrary &Library, const TypeFit &Fit,
                           BuiltinSpelling Spelling,
                           std::optional<Builder> &Build) {
  const LocalType &Type = *Fit.Type;
  llvm::json::Object Out{{"name", text(Type.Name)},
                         {"kind", kindName(Type.Kind)},
                         {"origin", originName(Type.Origin)}};
  TypeExpr Self;
  Self.Kind = TypeExprKind::Named;
  Self.Name = Type.Name;
  if (std::optional<uint64_t> Size = Library.sizeOf(Self))
    Out["size"] = *Size;
  if (Fit.Record && Fit.Record != Fit.Type) {
    Out["record"] = text(Fit.Record->Name);
    Out["recordSize"] = Fit.Record->Record->Size;
  }
  auto Names = [](llvm::ArrayRef<std::string> List) {
    llvm::json::Array Array;
    for (const std::string &Name : List)
      Array.push_back(text(Name));
    return Array;
  };
  auto Runs = [](llvm::ArrayRef<MatchRun> List) {
    llvm::json::Array Array;
    for (const MatchRun &Run : List)
      Array.push_back(llvm::json::Array{Run.Begin, Run.Length});
    return Array;
  };
  if (!Fit.Aliases.empty())
    Out["aliases"] = Names(Fit.Aliases);
  if (!Fit.PointerAliases.empty())
    Out["pointerAliases"] = Names(Fit.PointerAliases);
  if (Fit.Shift) {
    Out["shift"] = Fit.Shift;
    Out["shiftPath"] = text(memberPathText(Fit.ShiftPath));
  }
  Out["exact"] = Fit.Exact;
  Out["exactElements"] = Fit.ExactElements;
  Out["partial"] = Fit.Partial;
  Out["padding"] = Fit.Padding;
  Out["outside"] = Fit.Outside;
  if (!Fit.Fits.empty()) {
    if (!Build)
      Build.emplace(Library, Spelling);
    llvm::json::Array Fits;
    for (const MemberAt &At : Fit.Fits)
      Fits.push_back(landing(*Build, At));
    Out["fits"] = std::move(Fits);
  }
  if (!Fit.MatchedName.empty()) {
    Out["matchedName"] = text(Fit.MatchedName);
    Out["matched"] = Runs(Fit.MatchedRuns);
  }
  if (!Fit.MatchedMember.empty()) {
    Out["matchedMember"] = text(memberPathText(Fit.MatchedMember));
    Out["memberMatched"] = Runs(Fit.MatchedMemberRuns);
  }
  return Out;
}

} // namespace

llvm::json::Object typeFitJSON(const TypeLibrary &Library, const TypeFit &Fit,
                               BuiltinSpelling Spelling) {
  std::optional<Builder> Build;
  return fitJSON(Library, Fit, Spelling, Build);
}

llvm::json::Array typeFitsJSON(const TypeLibrary &Library,
                               llvm::ArrayRef<TypeFit> Fits,
                               BuiltinSpelling Spelling) {
  std::optional<Builder> Build;
  llvm::json::Array Out;
  for (const TypeFit &Fit : Fits)
    Out.push_back(fitJSON(Library, Fit, Spelling, Build));
  return Out;
}

} // namespace neverd::types
