//===- HighCSourceNames.cpp - The names the emitted C spells --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Records which symbol each function and object identifier of the emitted C
/// stands for (CSourceMap::Names), so that a reader of the text can name it
/// as its source language does: `rust_eh_probe_main` is
/// `_RNvCs8vGhbR5OvgK_13rust_eh_probe4main`, `rust_eh_probe::main` in Rust.
///
//===----------------------------------------------------------------------===//

#include "HighCWriter.h"

#include "neverd/backend/c/CSourceMap.h"

#include <map>

using namespace neverd;

void HighCWriter::prepareRecordDefinitions(const std::vector<HighFunc> &Funcs) {
  if (!Opts.RecordDefinitions || !Opts.EmitIncludes)
    return;
  // Every named record and enumeration the unit's types reach.
  std::vector<TypeRef> Records;
  std::set<const NdType *> Seen;
  std::set<std::string> Named;
  std::function<void(const TypeRef &)> Visit = [&](const TypeRef &Type) {
    if (!Type || !Seen.insert(Type.get()).second)
      return;
    switch (Type->Kind) {
    case NdTypeKind::Ptr:
      Visit(Type->Pointee);
      return;
    case NdTypeKind::Array:
      Visit(Type->ElemType);
      return;
    case NdTypeKind::Func:
      Visit(Type->RetType);
      for (const TypeRef &Param : Type->ParamTypes)
        Visit(Param);
      return;
    case NdTypeKind::Struct:
      if (!Type->RegisterReturnRecord && !Type->SourceName.empty() &&
          Named.insert(Type->SourceName).second)
        Records.push_back(Type);
      return;
    default:
      return;
    }
  };
  // The enumerations a callee's declaration takes, whose enumerators its
  // calls pass: C cannot declare an enumeration before defining it.
  const auto VisitEnumeration = [&](const TypeRef &Type) {
    if (Type && Type->Kind == NdTypeKind::Struct && Type->IsEnum &&
        !Type->SourceName.empty() && Named.insert(Type->SourceName).second)
      Records.push_back(Type);
  };
  std::function<void(const HighExpr &)> VisitExpr = [&](const HighExpr &E) {
    Visit(E.Type);
    if (E.SourceCallHint) {
      Visit(E.SourceCallHint->Signature.ReturnType);
      for (const auto &Parameter : E.SourceCallHint->Signature.Parameters)
        Visit(Parameter.Type);
    }
    if (E.Kind == ExprKind::Call && E.IntrinsicId == Intrinsic::None)
      if (const std::optional<FunctionSym> Callee = debugCallee(E)) {
        VisitEnumeration(Callee->ReturnType);
        for (const auto &[Name, Type] : Callee->Params)
          VisitEnumeration(Type);
      }
    E.forEachChildExpr([&](const ExprPtr &Child) { VisitExpr(*Child); });
  };
  for (const HighFunc &Function : Funcs) {
    Visit(Function.ReturnType);
    for (const HighParam &Param : Function.Params)
      Visit(Param.Type);
    for (const HighLocal &Local : Function.Locals)
      Visit(Local.Type);
    walkStmts(Function.Body, [&](const HighStmt &Statement) {
      forEachExpr(Statement, [&](const ExprPtr &E) { VisitExpr(*E); });
    });
    if (Dbg)
      if (auto Found = Dbg->resolveFunction(Function.Entry)) {
        Visit(Found->ReturnType);
        for (const auto &[Name, Type] : Found->Params)
          Visit(Type);
      }
  }
  if (Records.empty())
    return;
  // Their members as the code names them, which the definitions must agree
  // with.
  if (Dbg)
    for (const TypeRef &Record : Records)
      Dbg->completeType(Record);
  RecordDefinitions = Opts.RecordDefinitions(Records);
  // The typedefs and enumerators they declare are the program's names: the
  // unit's own step around them.
  for (const std::string &Name : RecordDefinitions.Identifiers)
    GlobalIdentifierAllocator.allocateVerbatim(Name);
  for (const std::string &Name : RecordDefinitions.Enumerations)
    GlobalIdentifierAllocator.allocateVerbatim(Name);
}

bool HighCWriter::memberOfDefinedRecord(const HighExpr &Addr) const {
  if (RecordDefinitions.Records.empty())
    return false;
  const auto Typed = typedPointerOffset(Addr);
  if (!Typed || !Typed->first)
    return false;
  const TypeRef Record = declaredRecordPointee(*Typed->first);
  return Record && !Record->SourceName.empty() &&
         RecordDefinitions.Records.count(
             cNamedTypeSpelling(Record->SourceName));
}

void HighCWriter::writeSourceRecordDeclarations(
    const std::vector<HighFunc> &Funcs) {
  std::set<std::string> Declared;
  std::set<const NdType *> Seen;
  std::function<void(const TypeRef &)> Visit = [&](const TypeRef &Type) {
    if (!Type || !Seen.insert(Type.get()).second)
      return;
    if (Type->Kind == NdTypeKind::Ptr) {
      const TypeRef &Record = Type->Pointee;
      if (Record && Record->Kind == NdTypeKind::Struct && !Record->IsEnum &&
          !Record->SourceName.empty()) {
        const std::string Name = cNamedTypeSpelling(Record->SourceName);
        // A record the program's types know takes the keyword they give it.
        const auto Tag = RecordDefinitions.Tags.find(Name);
        const char *Keyword = Tag != RecordDefinitions.Tags.end() && Tag->second
                                  ? "union"
                                  : "struct";
        if (Declared.insert(Name).second) {
          OS << "typedef " << Keyword << " " << Name << " " << Name << ";\n";
          RecordTypedefs.insert(Name);
        }
      }
      Visit(Record);
    }
  };
  auto Signature = [&](const FunctionSym &Function) {
    Visit(Function.ReturnType);
    for (const auto &[Name, Type] : Function.Params)
      Visit(Type);
  };
  for (const HighFunc &Function : Funcs) {
    Visit(Function.ReturnType);
    for (const HighParam &Param : Function.Params)
      Visit(Param.Type);
    if (Dbg)
      if (auto Found = Dbg->resolveFunction(Function.Entry))
        Signature(*Found);
  }
  for (const auto &[Name, Function] : DebugExternSigs)
    Signature(Function);
  if (!Declared.empty())
    OS << "\n";
}

bool HighCWriter::spellsRecordPointer(const TypeRef &Type) const {
  if (!Type || Type->Kind != NdTypeKind::Ptr || !Type->Pointee ||
      Type->Pointee->Kind != NdTypeKind::Struct ||
      Type->Pointee->SourceName.empty())
    return false;
  // Only the program's local types leave a record the unit names without a
  // typedef; a unit without includes leaves its declarations to whoever
  // builds it.
  return !Opts.RecordDefinitions || !Opts.EmitIncludes ||
         Type->Pointee->IsEnum ||
         RecordTypedefs.count(cNamedTypeSpelling(Type->Pointee->SourceName));
}

bool HighCWriter::definesRecord(const TypeRef &Record) const {
  return !Opts.RecordDefinitions || !Opts.EmitIncludes ||
         (Record && !Record->SourceName.empty() &&
          RecordDefinitions.Records.count(
              cNamedTypeSpelling(Record->SourceName)));
}

void HighCWriter::recordSourceNames(const std::vector<HighFunc> &Funcs) {
  if (!Opts.SourceMap && !Opts.SourceNames)
    return;
  std::map<std::string, CSourceName> ByIdentifier;
  auto Add = [&](CSourceName::Kind Kind, const std::string &Identifier,
                 llvm::StringRef Symbol, std::optional<va_t> Address) {
    if (Identifier.empty())
      return;
    auto [It, Inserted] = ByIdentifier.try_emplace(Identifier);
    CSourceName &Name = It->second;
    if (Inserted) {
      Name.TheKind = Kind;
      Name.Identifier = Identifier;
      Name.Symbol = Symbol.str();
      Name.Address = Address;
      return;
    }
    // One identifier for several symbols, such as an MSVC stem the MSVC rules
    // share between overloads, names none of them.
    if (Name.Symbol != Symbol)
      Name.Symbol.clear();
    if (Address && Name.Address != Address)
      Name.Address.reset();
  };
  for (const HighFunc &Func : Funcs)
    if (auto It = FunctionIdentifiers.find(&Func);
        It != FunctionIdentifiers.end())
      Add(CSourceName::Kind::Function, It->second, Func.Name, Func.Entry);
  for (const auto &[Source, Identifier] : ExternalSourceIdentifiers)
    Add(CSourceName::Kind::Function, Identifier, Source, std::nullopt);
  for (const auto &[Source, Identifier] : ExternalFunctionIdentifiers)
    if (!ByIdentifier.count(Identifier))
      Add(CSourceName::Kind::Function, Identifier, Source, std::nullopt);
  for (const auto &[Identifier, Source] : ReferencedFunctionSymbols)
    if (!ByIdentifier.count(Identifier))
      Add(CSourceName::Kind::Function, Identifier, Source, std::nullopt);
  for (const auto &[Address, Object] : ImageObjects)
    Add(CSourceName::Kind::Object, Object.Name, Object.Symbol, Address);
  // Keep source type identity beside the C projection. A tag shared by two
  // template instantiations is deliberately left in C, as for colliding
  // function names; replacing either by the other would invent a layout.
  std::set<const NdType *> Seen;
  std::function<void(const TypeRef &)> AddType = [&](const TypeRef &Type) {
    if (!Type || !Seen.insert(Type.get()).second)
      return;
    const llvm::StringRef Source = Type->SourceName;
    if (Type->Kind == NdTypeKind::Struct &&
        (Source.contains("::") || Source.contains('<')))
      Add(CSourceName::Kind::Type, cNamedTypeSpelling(Type->SourceName),
          Type->SourceName, std::nullopt);
    AddType(Type->Pointee);
    for (const TypeRef &Field : Type->FieldDisplayTypes)
      AddType(Field);
  };
  auto AddSignature = [&](const FunctionSym &Function) {
    AddType(Function.ReturnType);
    for (const auto &[Name, Type] : Function.Params)
      AddType(Type);
  };
  for (const HighFunc &Func : Funcs) {
    AddType(Func.ReturnType);
    for (const HighParam &Param : Func.Params)
      AddType(Param.Type);
    for (const HighLocal &Local : Func.Locals)
      AddType(Local.Type);
    if (Dbg)
      if (auto Function = Dbg->resolveFunction(Func.Entry))
        AddSignature(*Function);
  }
  for (const auto &[Identifier, Function] : DebugExternSigs)
    AddSignature(Function);
  std::vector<CSourceName> Names;
  Names.reserve(ByIdentifier.size());
  for (auto &[Identifier, Name] : ByIdentifier)
    Names.push_back(std::move(Name));
  if (Opts.SourceNames)
    *Opts.SourceNames = Names;
  if (Opts.SourceMap)
    Opts.SourceMap->Names = std::move(Names);
}
