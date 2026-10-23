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
#include "neverd/libc/LibCFloatingArguments.h"
#include "neverd/libc/LibCNames.h"

#include <map>

using namespace neverd;

void HighCWriter::prepareRecordDefinitions(const std::vector<HighFunc> &Funcs) {
  if (!Opts.RecordDefinitions || !Opts.EmitIncludes)
    return;
  // Every named record and enumeration the unit's types reach, and the
  // typedefs its declarations name them by, which a signature spells.
  std::vector<TypeRef> Records;
  std::set<const NdType *> Seen;
  std::set<std::string> Named;
  std::function<void(const TypeRef &)> Visit = [&](const TypeRef &Type) {
    if (!Type || !Seen.insert(Type.get()).second)
      return;
    if (!Type->TypedefName.empty() && Named.insert(Type->TypedefName).second)
      Records.push_back(NdType::makeNamedRecord(Type->TypedefName));
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
  // Import pointers need the library's tag kind, but do not require its
  // layout. In particular, a union must not acquire a struct forward tag.
  std::set<std::string> ImportRecords;
  const auto VisitImportType = [&](TypeRef Type) {
    VisitEnumeration(Type);
    while (Type && Type->Kind == NdTypeKind::Ptr) {
      if (Type->Pointee && Type->Pointee->Kind == NdTypeKind::Struct &&
          !Type->Pointee->IsEnum &&
          ImportRecords.insert(Type->Pointee->SourceName).second)
        Records.push_back(Type);
      Type = Type->Pointee;
    }
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
        VisitImportType(Callee->ReturnType);
        for (const auto &[Name, Type] : Callee->Params)
          VisitImportType(Type);
        // Definitions are collected before headers are written. Known
        // library headers supply their own value records.
        if (const auto Floating =
                libc::floatingCallArguments(*Callee, Opts.TheArch, Opts.Format);
            Floating && !libc::headerFor(Callee->Name) &&
            (!Floating->ParameterElements.empty() ||
             Floating->ReturnElements)) {
          Visit(Callee->ReturnType);
          for (const auto &[Name, Type] : Callee->Params)
            Visit(Type);
        }
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
  // Those an earlier rendering's code read members of through others.
  if (Opts.VariableTypes) {
    for (const auto &[Key, Type] : Opts.VariableTypes->Locals)
      Visit(Type);
    for (const auto &[Address, Type] : Opts.VariableTypes->Globals)
      Visit(Type);
  }
  for (const TypeRef &Record : ExtraRecords)
    Visit(Record);
  if (Records.empty())
    return;
  // Their members as the code names them, which the definitions must agree
  // with.
  if (Dbg)
    for (const TypeRef &Record : Records)
      if (Record->Kind != NdTypeKind::Ptr)
        Dbg->completeType(Record);
  RecordDefinitions = Opts.RecordDefinitions(Records);
}

std::string HighCWriter::declaredAs(const TypeRef &Type,
                                    llvm::StringRef Declarator) const {
  if (const std::optional<std::string> Typedef = declaredTypedef(Type))
    return *Typedef + (Declarator.empty() ? "" : " " + Declarator.str());
  return declarationToC(Type, Declarator);
}

std::optional<std::string>
HighCWriter::declaredTypedef(const TypeRef &Type) const {
  if (Type && !Type->TypedefName.empty() &&
      llvm::is_contained(RecordDefinitions.Types, Type->TypedefName))
    return cNamedTypeSpelling(Type->TypedefName);
  return std::nullopt;
}

bool HighCWriter::memberOfDefinedRecord(const HighExpr &Addr) const {
  if (RecordDefinitions.Records.empty() &&
      RecordDefinitions.HeaderRecords.empty())
    return false;
  const auto Typed = typedPointerOffset(Addr);
  if (!Typed || !Typed->first)
    return false;
  const TypeRef Record = declaredRecordPointee(*Typed->first);
  return Record && !Record->SourceName.empty() && holdsRecord(Record);
}

void HighCWriter::writeSourceRecordDeclarations(
    const std::vector<HighFunc> &Funcs,
    const std::set<std::string> &CallTargets) {
  std::set<std::string> Defined;
  for (const HighFunc &Function : Funcs)
    Defined.insert(Function.Name);
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
        // So does a C library routine's name, which a header the unit
        // includes may declare (`struct stat` beside <sys/stat.h>'s stat).
        const bool Ordinary =
            CallTargets.count(Name) || AddressTakenFunctions.count(Name) ||
            Defined.count(Name) || GlobalIdentifierAllocator.holds(Name) ||
            libc::headerOfDataObject(Name) || libc::isKnownFunction(Name);
        if (Ordinary)
          TagRecords.emplace(Name, Keyword == llvm::StringRef("union"));
        else if (Declared.insert(Name).second) {
          // A header the unit includes names some records by a typedef
          // alone, which the unit spells them by.
          if (!llvm::is_contained(RecordDefinitions.HeaderTypedefs, Name))
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
  if (Opts.VariableTypes) {
    for (const auto &[Key, Type] : Opts.VariableTypes->Locals)
      Visit(Type);
    for (const auto &[Address, Type] : Opts.VariableTypes->Globals)
      Visit(Type);
  }
  for (const HighFunc &Function : Funcs) {
    Visit(Function.ReturnType);
    for (const HighParam &Param : Function.Params)
      Visit(Param.Type);
    if (Dbg)
      if (auto Found = Dbg->resolveFunction(Function.Entry))
        Signature(*Found);
  }
  for (const auto &[Name, Function] : DebugExternSigs)
    if (!CallTargets.count(Name) || isOwnFunctionName(Name, Funcs) ||
        calleeHeaders(Name).empty())
      Signature(Function);
  // And those its objects point to.
  for (const auto &[Address, Object] : ImageObjects)
    Visit(Object.Type);
  if (!Declared.empty())
    OS << "\n";
}

const char *HighCWriter::recordTagKeyword(llvm::StringRef Name) const {
  const auto Tag = TagRecords.find(Name.str());
  if (Tag == TagRecords.end())
    return nullptr;
  return Tag->second ? "union" : "struct";
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

void HighCWriter::wantRecord(const TypeRef &Record) const {
  if (WantedRecords && Record && !Record->SourceName.empty())
    WantedRecords->push_back(Record);
}

bool HighCWriter::holdsRecord(const TypeRef &Record) const {
  const std::string Name = cNamedTypeSpelling(Record->SourceName);
  // A record of a system header the unit includes is complete there, with
  // the program's members by their names (renderEmbeddedC).
  return RecordDefinitions.Records.count(Name) ||
         RecordDefinitions.HeaderRecords.count(Name);
}

bool HighCWriter::definesRecord(const TypeRef &Record) const {
  return !Opts.RecordDefinitions || !Opts.EmitIncludes ||
         (Record && !Record->SourceName.empty() && holdsRecord(Record));
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
  for (const auto &[Address, Object] : ImageObjects) {
    Add(CSourceName::Kind::Object, Object.Name, Object.Symbol, Address);
    if (Opts.SourceMap && Object.Type)
      for (const auto &Function : Funcs) {
        SourceVariable Variable;
        Variable.Function = Function.Entry;
        Variable.Address = Address;
        Variable.Id = "global:" + std::to_string(Address);
        Variable.Name = Object.Name;
        Variable.Bytes = Object.Type->Size;
        Variable.Type = Object.Type;
        Variable.Ambiguous = !dataNameWidth(Address, Object).has_value();
        recordSourceVariable(Opts.SourceMap->Variables, std::move(Variable));
      }
  }
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
