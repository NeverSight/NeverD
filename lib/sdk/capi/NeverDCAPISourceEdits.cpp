//===- NeverDCAPISourceEdits.cpp - Checked source variable types ----------===//
#include "LocalTypesDebugContext.h"
#include "SessionImpl.h"
#include "SourceEdits.h"

#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/sdk/Limits.h"
#include "neverd/sdk/NeverDCAPIPersist.h"
#include "neverd/types/TypeParse.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"

using namespace neverd;
using namespace neverd::sdk;

void neverd::sdk::noteUnboundVariableTypes(CSourceMap &Map,
                                           const SourceVariableTypes &Types,
                                           va_t Function) {
  for (const auto &[Key, Type] : Types.Locals) {
    if (Key.first != Function)
      continue;
    const auto Found = Map.Variables.find(Key);
    if (Found == Map.Variables.end() || Found->second.Ambiguous ||
        !SourceVariableTypes::compatible(Type, Found->second.Bytes))
      Map.StaleVariableTypes.push_back(
          Key.second + ": the original value cannot be bound unambiguously");
  }
}

std::string neverd::sdk::sourceVariableSnapshot(llvm::StringRef IR) {
  const auto Hash = llvm::SHA256::hash(llvm::arrayRefFromStringRef(IR));
  return "source-variable-v1:" + llvm::toHex(Hash, true);
}

std::string neverd::sdk::SnapshotStream::snapshot() {
  flush();
  return "source-variable-v1:" + llvm::toHex(Hash.final(), true);
}

std::string
neverd::sdk::editedVariableSnapshot(llvm::StringRef Before,
                                    const SourceVariableEdit &Edit) {
  std::string Key = Before.str();
  for (const std::string *Part : {&Edit.Kind, &Edit.From, &Edit.To})
    Key += ":" + std::to_string(Part->size()) + ":" + *Part;
  return sourceVariableSnapshot(Key);
}

std::string neverd::sdk::highVariableSnapshot(const HighFunc &Function,
                                              bool Symbolic) {
  // Serialize the unedited value graph, once per node. Printed HighIR can
  // duplicate shared expressions exponentially and hides coalesced SSA keys.
  SnapshotStream OS;
  OS << Function.Entry << ':' << Function.OriginalSize << ':' << Symbolic;
  std::map<const HighExpr *, unsigned> Seen;
  bool Complete = true;
  auto Variable = [&](const MedVar &V) {
    OS << ':' << unsigned(V.Kind) << ':' << V.Id << ':' << V.SSAVer << ':'
       << V.RenameTag << ':' << V.Size << ':'
       << (V.Kind == MedVar::Stack ? V.StackOff : V.RegOff);
  };
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E || !Complete) {
      OS << "null;";
      return;
    }
    auto [It, Inserted] = Seen.emplace(E.get(), Seen.size());
    OS << '#' << It->second;
    if (!Inserted)
      return;
    if (Seen.size() > kMaxSourceExpressionSnapshotNodes ||
        OS.tell() > kMaxSourceExpressionSnapshotBytes) {
      Complete = false;
      return;
    }
    OS << ':' << unsigned(E->Kind) << ':' << unsigned(E->Op) << ':'
       << unsigned(E->MemoryOrdering) << ':' << unsigned(E->MemoryAddressSpace)
       << ':' << E->ConstVal << ':' << unsigned(E->ConstProvenance) << ':'
       << E->AddressOwnerVA << ':' << E->CallAddr << ':' << E->CallSiteAddr
       << ':' << unsigned(E->IntrinsicId) << ':' << E->DoesNotReturn;
    if (E->Type)
      OS << ':' << unsigned(E->Type->Kind) << ':' << E->Type->Size;
    Variable(E->Var);
    for (const auto &Output : E->IntrinsicOutputs)
      Variable(Output);
    OS << '[';
    for (const auto &Child : E->Operands)
      Visit(Child);
    OS << "]target:";
    Visit(E->IndirectTarget);
  };
  size_t Statements = 0;
  walkStmts(Function.Body, [&](const HighStmt &Stmt) {
    if (!Complete ||
        ++Statements >
            ::neverd::sdk::
                kNeverDCAPISourceEditsNeverdSdkHighVariableSnapshotStatementsLimit) {
      Complete = false;
      return;
    }
    OS << "stmt:" << unsigned(Stmt.Kind) << ':' << Stmt.Addr << ':'
       << Stmt.GotoTarget << ':' << Stmt.LoopHeaderAddr << ':'
       << Stmt.Body.size() << ':' << Stmt.ElseBody.size() << ':'
       << Stmt.DefaultBody.size() << ':' << Stmt.IsPhiCopy;
    for (const auto &Case : Stmt.Cases)
      OS << "case:" << Case.Value << ':' << Case.Body.size();
    forEachExpr(Stmt, Visit);
  });
  return Complete ? OS.snapshot() : std::string();
}

TypeRef neverd::sdk::parseSourceVariableType(Session &S, llvm::StringRef Text,
                                             uint16_t Bytes,
                                             std::string &Error) {
  if (Text.empty() || Text.size() > kMaxFunctionDeclarationBytes ||
      Text.contains('\0')) {
    Error = "A variable type must contain between 1 and 4096 bytes";
    return nullptr;
  }
  const auto *Library = S.readLocalTypes();
  if (!Library) {
    Error = S.LastError;
    return nullptr;
  }
  auto Read = types::parseFunctionDeclaration(
      ("void __neverd_variable(" + Text + ")").str(), *Library);
  if (!Read.ok() || Read.Function->Variadic ||
      Read.Function->Params.size() != 1) {
    Error = Read.Problems.empty() ? "Enter exactly one scalar or pointer type"
                                  : Read.Problems.front().Message;
    return nullptr;
  }
  const auto &Parameter = Read.Function->Params.front();
  if (!Parameter.Name.empty()) {
    Error =
        "Enter a type without a variable name; use N to rename the variable";
    return nullptr;
  }
  TypeRef Type =
      Parameter.Type ? decompilerTypeOf(*Library, *Parameter.Type) : nullptr;
  if (!SourceVariableTypes::compatible(Type, Bytes)) {
    Error = "The type must be a scalar or pointer of exactly " +
            std::to_string(Bytes) + " bytes";
    return nullptr;
  }
  return Type;
}

SourceVariableTypes
neverd::sdk::sourceVariableTypes(Session &S, va_t Entry,
                                 llvm::StringRef Snapshot,
                                 std::vector<std::string> *Stale) {
  SourceVariableTypes Result;
  for (const auto &Edit : S.SourceOptions.VariableTypes) {
    if (!Edit.Address && Edit.Function != Entry)
      continue;
    if (!Edit.Address && (Snapshot.empty() || Edit.Snapshot != Snapshot)) {
      if (Stale)
        Stale->push_back(
            Edit.Id +
            ": the source analysis changed; reset or reapply the type");
      continue;
    }
    // The parser checks its width again at the actual binding below. Never
    // give a saved assertion a native debug signature or an image extent.
    std::string Error;
    TypeRef Type = parseSourceVariableType(S, Edit.Type, Edit.Bytes, Error);
    if (Type && Edit.Floating != (Type->Kind == NdTypeKind::Float)) {
      Type.reset();
      Error = "the asserted type changes floating-point bit interpretation";
    }
    if (!Type) {
      if (Stale)
        Stale->push_back(Edit.Id + ": " + Error);
      continue;
    }
    if (Edit.Address)
      Result.Globals[Edit.Address] = Type;
    else
      Result.Locals[{Entry, Edit.Id}] = Type;
  }
  return Result;
}

const char *neverd_source_type_json(neverd_session_t Sess,
                                    const char *Request) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  const llvm::StringRef RequestText = Request ? Request : "{}";
  if (RequestText.size() >
      ::neverd::sdk::
          kNeverDCAPISourceEditsNeverdSourceTypeJsonRequestTextSizeLimit) {
    S->setError("source type request exceeds 32 KiB");
    return nullptr;
  }
  auto Parsed = llvm::json::parse(RequestText);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    S->setError("A source type request must be a JSON object");
    return nullptr;
  }
  const auto *Object = Parsed->getAsObject();
  if (Object && Object->get("object_address")) {
    const auto Text = Object->getString("object_address");
    va_t Address = 0;
    if (!Text || !Text->starts_with("0x") ||
        Text->drop_front(2).getAsInteger(16, Address)) {
      S->setError("object_address must be a hexadecimal address");
      return nullptr;
    }
    const Segment *Segment = S->Img.getSegmentFor(Address);
    TypeRef Type;
    std::string Name;
    uint64_t Bytes = 0;
    if (S->Dbg)
      if (const auto Data = S->Dbg->resolveDataObject(Address);
          Data && Data->Addr == Address) {
        Name = Data->Name;
        Type = Data->Type;
        Bytes = Data->Size;
      }
    if (const auto Item = S->DataItems.find(Address);
        Item != S->DataItems.end()) {
      // Only an explicitly sized scalar item supplies a missing extent.
#define NEVERD_DATA_SIZE_NAME(Keyword, Width, Prefix)                          \
  if (Item->second.Kind == Keyword)                                            \
    Bytes = Width;
#include "neverd/DataNames.def"
    }
    if (!Bytes || Name.empty())
      if (const auto *Symbol = S->Img.findSymbolAt(Address);
          Symbol && !Symbol->IsFunc && !Symbol->IsSection) {
        if (!Bytes)
          Bytes = Symbol->Size;
        Name = Symbol->Name;
      }
    if (!Segment || !Bytes || Bytes > Segment->Size - (Address - Segment->VA)) {
      S->setError("No sized global object starts at this address; define a "
                  "scalar data item first");
      return nullptr;
    }
    if (!Type && Bytes <= 16)
      Type = NdType::makeInt(Bytes, false);
    if (!Type || Type->Size != Bytes || !hasCSpelling(Type)) {
      S->setError(
          "This object has no unambiguous source type of its storage width");
      return nullptr;
    }
    if (const auto Rename = S->Renames.find(Address);
        Rename != S->Renames.end())
      Name = Rename->second;
    if (Name.empty())
      Name = "data_" + llvm::utohexstr(Address);
    std::string Out;
    llvm::raw_string_ostream OS(Out);
    OS << llvm::json::Value(
        llvm::json::Object{{"address", vaHex(Address)},
                           {"name", Name},
                           {"type", declarationToC(Type, "", true)},
                           {"bytes", Bytes},
                           {"floating", Type->Kind == NdTypeKind::Float}});
    return dupStr(Out);
  }
  const auto Text = Object ? Object->getString("type") : std::nullopt;
  const auto Bytes = Object ? Object->getInteger("bytes") : std::nullopt;
  if (!Text || !Bytes || *Bytes < 1 || *Bytes > 16 ||
      (Object->get("floating") && !Object->getBoolean("floating"))) {
    S->setError(
        "A source type request needs a type and its machine byte width");
    return nullptr;
  }
  std::string Error;
  TypeRef Type = parseSourceVariableType(*S, *Text, *Bytes, Error);
  if (Type && Object->getBoolean("floating").value_or(false) !=
                  (Type->Kind == NdTypeKind::Float)) {
    Error = "A source type cannot change floating-point bit interpretation";
    Type.reset();
  }
  if (!Type) {
    S->setError(Error);
    return nullptr;
  }
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  OS << llvm::json::Value(
      llvm::json::Object{{"type", *Text}, {"bytes", *Bytes}, {"valid", true}});
  return dupStr(Out);
}
