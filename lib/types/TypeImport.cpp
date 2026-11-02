//===- TypeImport.cpp - Independent prototype providers ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Import independently parsed prototypes with their original type identities.
///
//===----------------------------------------------------------------------===//

#include "neverd/types/TypeImport.h"

#include "neverd/types/Limits.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd::types {
namespace {

bool sameType(const TypeExprRef &A, const TypeExprRef &B, unsigned Depth);

bool sameEnum(const std::shared_ptr<const EnumDef> &A,
              const std::shared_ptr<const EnumDef> &B) {
  if (!A || !B)
    return A == B;
  if (A->Underlying != B->Underlying || A->Flags != B->Flags ||
      A->Values.size() != B->Values.size())
    return false;
  for (size_t I = 0; I < A->Values.size(); ++I)
    if (A->Values[I].Name != B->Values[I].Name ||
        A->Values[I].Value != B->Values[I].Value)
      return false;
  return true;
}

bool sameRecord(const std::shared_ptr<const RecordDef> &A,
                const std::shared_ptr<const RecordDef> &B, unsigned Depth) {
  if (!A || !B)
    return A == B;
  if (A->Union != B->Union || A->Size != B->Size ||
      A->Alignment != B->Alignment || A->Pack != B->Pack ||
      A->Defined != B->Defined || A->Members.size() != B->Members.size())
    return false;
  for (size_t I = 0; I < A->Members.size(); ++I) {
    const Member &X = A->Members[I], &Y = B->Members[I];
    if (X.Name != Y.Name || X.BitOffset != Y.BitOffset ||
        X.BitWidth != Y.BitWidth || X.StorageBitOffset != Y.StorageBitOffset ||
        !sameType(X.Type, Y.Type, Depth + 1))
      return false;
  }
  return true;
}

bool sameFunction(const std::shared_ptr<const FunctionDef> &A,
                  const std::shared_ptr<const FunctionDef> &B, unsigned Depth) {
  if (!A || !B)
    return A == B;
  if (A->Convention != B->Convention || A->Variadic != B->Variadic ||
      A->NoReturn != B->NoReturn || A->Params.size() != B->Params.size() ||
      !sameType(A->Return, B->Return, Depth + 1))
    return false;
  for (size_t I = 0; I < A->Params.size(); ++I)
    if (A->Params[I].Name != B->Params[I].Name ||
        !sameType(A->Params[I].Type, B->Params[I].Type, Depth + 1))
      return false;
  return true;
}

bool sameType(const TypeExprRef &A, const TypeExprRef &B, unsigned Depth) {
  if (Depth > limits::types::kPrototypeTypeImportMaxDepth)
    return false;
  if (!A || !B)
    return A == B;
  if (A->Kind != B->Kind || A->Const != B->Const || A->Volatile != B->Volatile)
    return false;
  switch (A->Kind) {
  case TypeExprKind::Builtin:
    return A->TheBuiltin == B->TheBuiltin;
  case TypeExprKind::Named:
    return A->Name == B->Name && A->NamedKind == B->NamedKind;
  case TypeExprKind::Pointer:
    return A->PointerBytes == B->PointerBytes && A->Reference == B->Reference &&
           sameType(A->Element, B->Element, Depth + 1);
  case TypeExprKind::Array:
    return A->Count == B->Count && sameType(A->Element, B->Element, Depth + 1);
  case TypeExprKind::Record:
    return sameRecord(A->Record, B->Record, Depth);
  case TypeExprKind::Enum:
    return sameEnum(A->Enum, B->Enum);
  case TypeExprKind::Function:
    return sameFunction(A->Function, B->Function, Depth);
  }
  return false;
}

bool sameDefinition(const LocalType &A, const LocalType &B) {
  return A.Kind == B.Kind && A.TypedefNamed == B.TypedefNamed &&
         sameRecord(A.Record, B.Record, 0) && sameEnum(A.Enum, B.Enum) &&
         sameType(A.Target, B.Target, 0) &&
         sameFunction(A.Function, B.Function, 0);
}

/// Change named references in every position, including anonymous records,
/// callback parameters and callback return types. Named cycles are not
/// followed.
class Rewriter {
public:
  explicit Rewriter(const std::map<std::string, std::string> &Names)
      : Names(Names) {}

  std::vector<std::string> dependencies(const LocalType &Type) {
    References.clear();
    Collect = true;
    type(Type.Target);
    record(Type.Record);
    function(Type.Function);
    Collect = false;
    llvm::sort(References);
    References.erase(std::unique(References.begin(), References.end()),
                     References.end());
    return References;
  }

  TypeExprRef type(const TypeExprRef &Original, unsigned Depth = 0) {
    if (!Original)
      return {};
    if (Depth > limits::types::kPrototypeTypeImportMaxDepth) {
      TooDeep = true;
      return {};
    }
    if (((Original->Kind == TypeExprKind::Pointer ||
          Original->Kind == TypeExprKind::Array) &&
         !Original->Element) ||
        (Original->Kind == TypeExprKind::Pointer && !Original->PointerBytes) ||
        (Original->Kind == TypeExprKind::Record && !Original->Record) ||
        (Original->Kind == TypeExprKind::Enum && !Original->Enum) ||
        (Original->Kind == TypeExprKind::Function && !Original->Function))
      Incomplete = true;
    const auto Element = type(Original->Element, Depth + 1);
    const auto Record = record(Original->Record, Depth);
    const auto Function = function(Original->Function, Depth);
    std::string Name = Original->Name;
    if (Original->Kind == TypeExprKind::Named) {
      if (Collect)
        References.push_back(Name);
      if (auto Found = Names.find(Original->Name); Found != Names.end())
        Name = Found->second;
    }
    if (Name == Original->Name && Element == Original->Element &&
        Record == Original->Record && Function == Original->Function)
      return Original;
    auto Copy = std::make_shared<TypeExpr>(*Original);
    Copy->Name = std::move(Name);
    Copy->Element = Element;
    Copy->Record = Record;
    Copy->Function = Function;
    return Copy;
  }

  std::shared_ptr<const RecordDef>
  record(const std::shared_ptr<const RecordDef> &Original, unsigned Depth = 0) {
    if (!Original)
      return {};
    std::shared_ptr<RecordDef> Copy;
    for (size_t I = 0; I < Original->Members.size(); ++I) {
      if (!Original->Members[I].Type)
        Incomplete = true;
      const auto Type = type(Original->Members[I].Type, Depth + 1);
      if (Type == Original->Members[I].Type)
        continue;
      if (!Copy)
        Copy = std::make_shared<RecordDef>(*Original);
      Copy->Members[I].Type = Type;
    }
    return Copy ? Copy : Original;
  }

  std::shared_ptr<const FunctionDef>
  function(const std::shared_ptr<const FunctionDef> &Original,
           unsigned Depth = 0) {
    if (!Original)
      return {};
    std::shared_ptr<FunctionDef> Copy;
    const auto Return = type(Original->Return, Depth + 1);
    if (Return != Original->Return) {
      Copy = std::make_shared<FunctionDef>(*Original);
      Copy->Return = Return;
    }
    for (size_t I = 0; I < Original->Params.size(); ++I) {
      if (!Original->Params[I].Type)
        Incomplete = true;
      const auto Type = type(Original->Params[I].Type, Depth + 1);
      if (Type == Original->Params[I].Type)
        continue;
      if (!Copy)
        Copy = std::make_shared<FunctionDef>(*Original);
      Copy->Params[I].Type = Type;
    }
    return Copy ? Copy : Original;
  }

  bool TooDeep = false;
  bool Incomplete = false;

private:
  const std::map<std::string, std::string> &Names;
  bool Collect = false;
  std::vector<std::string> References;
};

llvm::Error problem(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

} // namespace

llvm::Expected<PrototypeImport> importPrototypes(
    TypeLibrary &Library, const TypeLibrary &Provider, llvm::StringRef Prefix,
    std::optional<llvm::ArrayRef<llvm::StringRef>> RequestedNames) {
  if (Library.dataModel() != Provider.dataModel() ||
      Library.recordABI() != Provider.recordABI())
    return problem(
        "prototype providers must use the same data model and record ABI");
  if (Prefix.empty() ||
      (!llvm::isAlpha(Prefix.front()) && Prefix.front() != '_') ||
      !llvm::all_of(Prefix,
                    [](char C) { return llvm::isAlnum(C) || C == '_'; }))
    return problem("a prototype provider prefix must be a C identifier");

  llvm::StringSet<> Selected;
  if (RequestedNames)
    for (llvm::StringRef Name : *RequestedNames) {
      if (!Provider.findPrototype(Name))
        return problem("selected provider prototype is missing: " + Name.str());
      Selected.insert(Name);
    }

  PrototypeImport Result;
  const std::map<std::string, std::string> UnchangedNames;
  Rewriter Inspect(UnchangedNames);
  std::map<std::string, std::vector<std::string>> Dependencies;
  std::set<std::string> Needed;
  std::vector<std::string> Pending;
  const auto need = [&](const std::vector<std::string> &Names) {
    for (const auto &Name : Names)
      if (Needed.insert(Name).second)
        Pending.push_back(Name);
  };
  for (const auto &Entry : Provider.prototypes()) {
    if (RequestedNames && !Selected.contains(Entry.first()))
      continue;
    if (Library.findPrototype(Entry.first()))
      continue;
    if (!Entry.second)
      return problem(
          "prototype provider has an incomplete function declaration");
    Result.Prototypes.push_back(Entry.first().str());
    LocalType Routine;
    Routine.Function = Entry.second;
    need(Inspect.dependencies(Routine));
  }
  llvm::sort(Result.Prototypes);
  for (size_t I = 0; I < Pending.size(); ++I) {
    const auto *Type = Provider.find(Pending[I]);
    if (!Type)
      return problem("prototype provider lacks named type '" + Pending[I] +
                     "'");
    switch (Type->Kind) {
    case TypeKind::Struct:
    case TypeKind::Union:
      Inspect.Incomplete |= !Type->Record;
      break;
    case TypeKind::Enum:
      Inspect.Incomplete |= !Type->Enum;
      break;
    case TypeKind::Typedef:
      Inspect.Incomplete |= !Type->Target;
      break;
    case TypeKind::Function:
      Inspect.Incomplete |= !Type->Function;
      break;
    }
    const auto Names = Inspect.dependencies(*Type);
    Dependencies[Type->Name] = Names;
    need(Names);
  }
  if (Inspect.TooDeep)
    return problem("prototype provider exceeds the inline type nesting limit");
  if (Inspect.Incomplete)
    return problem("prototype provider has an incomplete type expression");
  Result.Dependencies.assign(Needed.begin(), Needed.end());

  // A matching outer definition is not reusable if a named dependency must
  // change identity. Propagate conflicts through cycles with one worklist.
  llvm::StringMap<std::vector<std::string>> Dependents;
  llvm::StringSet<> Conflicts;
  std::set<std::string> PrototypeNames(Result.Prototypes.begin(),
                                       Result.Prototypes.end());
  for (const auto &Entry : Library.prototypes())
    PrototypeNames.insert(Entry.first().str());
  Pending.clear();
  for (const auto &Name : Needed) {
    const auto *Source = Provider.find(Name);
    const auto *Existing = Library.find(Name);
    if (PrototypeNames.count(Name) ||
        (Existing && !sameDefinition(*Source, *Existing))) {
      Conflicts.insert(Name);
      Pending.push_back(Name);
    }
    if (!Existing)
      continue;
    for (const auto &Dependency : Dependencies.at(Name))
      Dependents[Dependency].push_back(Name);
  }
  for (size_t I = 0; I < Pending.size(); ++I) {
    const auto Found = Dependents.find(Pending[I]);
    if (Found == Dependents.end())
      continue;
    for (const auto &Name : Found->second)
      if (Conflicts.insert(Name).second)
        Pending.push_back(Name);
  }
  std::map<std::string, std::string> Names;
  std::set<std::string> Taken = Needed;
  Taken.insert(PrototypeNames.begin(), PrototypeNames.end());
  for (const auto *Type : Library.types())
    Taken.insert(Type->Name);
  for (const auto &Name : Needed) {
    if (!Conflicts.contains(Name))
      continue;
    const std::string Base = Prefix.str() + "_" + Name;
    std::string Imported = Base;
    for (size_t Suffix = 1; !Taken.insert(Imported).second; ++Suffix)
      Imported = Base + "_" + std::to_string(Suffix);
    Names[Name] = Imported;
    Result.Renamed.emplace_back(Name, Imported);
  }

  // Prepare everything before mutating the destination. Only the needed
  // closure is imported, so each provider need not duplicate the SDK prelude.
  Rewriter Rewrite(Names);
  std::vector<LocalType> Types;
  std::vector<std::shared_ptr<const FunctionDef>> Functions;
  for (const auto &Name : Needed) {
    if (Library.find(Name) && !Conflicts.contains(Name))
      continue;
    LocalType Copy = *Provider.find(Name);
    if (auto Found = Names.find(Name); Found != Names.end()) {
      Copy.Name = Found->second;
      Copy.Header.clear();
    }
    Copy.Target = Rewrite.type(Copy.Target);
    Copy.Record = Rewrite.record(Copy.Record);
    Copy.Function = Rewrite.function(Copy.Function);
    Result.Types.push_back(Copy.Name);
    Types.push_back(std::move(Copy));
  }
  for (const auto &Name : Result.Prototypes)
    Functions.push_back(Rewrite.function(Provider.prototypes().lookup(Name)));
  if (Rewrite.TooDeep)
    return problem("prototype provider exceeds the inline type nesting limit");
  for (auto &Type : Types)
    Library.define(std::move(Type));
  for (size_t I = 0; I < Result.Prototypes.size(); ++I)
    Library.definePrototype(Result.Prototypes[I], std::move(Functions[I]));
  return Result;
}

} // namespace neverd::types
