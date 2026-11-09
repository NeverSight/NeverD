//===- NeverDCAPIFunctionType.cpp - A function's C declaration ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// neverd_func_type_json: the C declaration of a function as IDA's Y edits
/// it, the user's or the signature the decompiler prints, and where each of
/// its parameters arrives; a declaration checked before the user states it;
/// and the declaration with one parameter's type changed, as Convert to
/// struct * makes it.
///
//===----------------------------------------------------------------------===//

#include "FunctionDeclaration.h"
#include "JSONText.h"
#include "LocalTypesDebugContext.h"
#include "SessionImpl.h"

#include "neverd/ir/SourceParameterPlacement.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/sdk/Limits.h"
#include "neverd/sdk/NeverDCAPIDisasm.h"
#include "neverd/sdk/NeverDCAPIPersist.h"
#include "neverd/types/TypeParse.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/JSON.h"

#include <optional>
#include <string>
#include <vector>

using namespace neverd;
using namespace neverd::sdk;

namespace {

/// What a parameter's type is read in: the declaration of a function of
/// its own, whose one parameter it is.
constexpr llvm::StringLiteral ProbeHead = "void __neverd_parameter(";

/// The JSON object \p Json, or one without members for none; none with
/// \p Error when it is no object.
std::optional<llvm::json::Object> requestObject(const char *Json,
                                                std::string &Error) {
  if (!Json || !*Json)
    return llvm::json::Object();
  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Json);
  if (!Parsed) {
    Error = "malformed request: " + llvm::toString(Parsed.takeError());
    return std::nullopt;
  }
  if (llvm::json::Object *Object = Parsed->getAsObject())
    return std::move(*Object);
  Error = "the request is a JSON object";
  return std::nullopt;
}

/// The signature the decompiler printed for the function at \p Entry in
/// \p Source: the line of its definition that opens its body, without the
/// brace.  Its comments and the target features it is built for come
/// before it.
std::optional<std::string> printedSignature(llvm::StringRef Source,
                                            va_t Entry) {
  const std::string Marker = "/* neverd.entry: " + vaHex(Entry) + " */";
  const size_t At = Source.find(Marker);
  if (At == llvm::StringRef::npos)
    return std::nullopt;
  llvm::StringRef Rest = Source.substr(At + Marker.size());
  while (!Rest.empty()) {
    auto [Line, Next] = Rest.split('\n');
    Rest = Next;
    const llvm::StringRef Code = Line.trim();
    if (Code.empty() || Code.starts_with("/*") || Code.starts_with("*") ||
        Code.starts_with("__attribute__((target("))
      continue;
    if (Code.ends_with("{") && Code.contains('('))
      return Code.drop_back().rtrim().str();
    return std::nullopt;
  }
  return std::nullopt;
}

/// Where a calling convention passes a value: its register, as the listing
/// names it, or its slot on the entry stack.
std::string locationText(const Session &S, const SourceABIValueLocation &At) {
  if (At.Kind == SourceABICarrierKind::Stack)
    return (At.EntryStackOffset < 0 ? "stack-0x" : "stack+0x") +
           llvm::utohexstr(At.EntryStackOffset < 0
                               ? 0 - static_cast<uint64_t>(At.EntryStackOffset)
                               : static_cast<uint64_t>(At.EntryStackOffset));
  if (At.Kind != SourceABICarrierKind::IntegerRegister &&
      At.Kind != SourceABICarrierKind::FloatingRegister)
    return {};
  const auto &Info = getTargetRegInfo(S.Img.Arch);
  const char *Name = Info.GetRegName
                         ? Info.GetRegName(At.RegisterOffset, At.ValueBytes)
                         : nullptr;
  return Name && Name[0] != '?' ? llvm::StringRef(Name).lower() : std::string();
}

llvm::json::Array problemRows(llvm::ArrayRef<types::SourceProblem> Problems,
                              unsigned ColumnShift = 0) {
  llvm::json::Array Rows;
  for (const types::SourceProblem &Problem : Problems)
    Rows.push_back(llvm::json::Object{
        {"line", Problem.Line},
        {"column", Problem.Line == 1 && Problem.Column > ColumnShift
                       ? Problem.Column - ColumnShift
                       : Problem.Column},
        {"message", jsonSafeText(Problem.Message)}});
  return Rows;
}

std::string problemText(const types::SourceProblem &Problem) {
  return std::to_string(Problem.Line) + ":" + std::to_string(Problem.Column) +
         ": " + Problem.Message;
}

/// \p Function's declaration as \p Name, in the spelling the local types
/// view writes.
std::string declarationText(const types::TypeLibrary &Library,
                            const types::FunctionDef &Function,
                            llvm::StringRef Name) {
  types::RenderOptions Options;
  std::string Text = Function.NoReturn ? "_Noreturn " : "";
  return Text +
         types::spellType(Library,
                          *types::makeFunction(
                              std::make_shared<types::FunctionDef>(Function)),
                          Name.str(), Options);
}

/// Where the image's convention passes each of \p Function's parameters,
/// by the types the decompiler reads them as; none without rules.
std::optional<SourceParameterPlacement>
placementOf(const Session &S, const types::TypeLibrary &Library,
            const types::FunctionDef &Function, llvm::StringRef Name) {
  std::optional<FunctionSym> Signature = signatureOf(Library, Function, Name);
  if (!Signature)
    return std::nullopt;
  std::vector<TypeRef> Types;
  for (const auto &[Param, Type] : Signature->Params)
    Types.push_back(Type);
  return placeSourceParameters(S.Img.Arch, S.Img.abiFormat(),
                               Signature->ReturnType, Types);
}

/// The reply for the declaration \p Read of \p Text.
llvm::json::Object declarationReply(Session &S,
                                    const types::TypeLibrary &Library,
                                    const std::string &Text,
                                    const types::FunctionDeclaration &Read) {
  llvm::json::Object Reply{{"declaration", jsonSafeText(Text)}};
  if (!Read.ok()) {
    Reply["problems"] = problemRows(Read.Problems);
    return Reply;
  }
  const types::FunctionDef &Function = *Read.Function;
  const types::TypeSpeller Speller(Library, types::RenderOptions());
  Reply["name"] = jsonSafeText(Read.Name);
  Reply["return"] = jsonSafeText(
      Function.Return ? Speller.spell(*Function.Return) : std::string("void"));
  Reply["variadic"] = Function.Variadic;
  const std::optional<SourceParameterPlacement> Placement =
      placementOf(S, Library, Function, Read.Name);
  llvm::json::Array Parameters;
  for (size_t I = 0; I < Function.Params.size(); ++I) {
    const types::Parameter &Param = Function.Params[I];
    llvm::json::Object Row{
        {"name", jsonSafeText(Param.Name)},
        {"type", jsonSafeText(Param.Type ? Speller.spell(*Param.Type)
                                         : std::string())}};
    if (Placement && I < Placement->Parameters.size() &&
        !Placement->Parameters[I].empty())
      if (std::string Where =
              locationText(S, Placement->Parameters[I].front().Location);
          !Where.empty())
        Row["location"] = std::move(Where);
    Parameters.push_back(std::move(Row));
  }
  Reply["parameters"] = std::move(Parameters);
  return Reply;
}

} // namespace

namespace neverd::sdk {

std::optional<CurrentDeclaration>
currentDeclaration(neverd_session_t Sess, Session &S, va_t Entry,
                   const types::TypeLibrary &Library) {
  CurrentDeclaration Current;
  if (const auto Stated = S.FunctionAttributeEdits.find(Entry);
      Stated != S.FunctionAttributeEdits.end() && Stated->second.Type) {
    Current.Text = *Stated->second.Type;
    Current.Stated = true;
  } else {
    // The signature the user sees: the decompiler's own.
    const char *Source = neverd_decompile(Sess, Entry);
    const std::string Decompiled = Source ? Source : "";
    neverd_free_string(Source);
    if (Decompiled.empty())
      return std::nullopt;
    std::optional<std::string> Printed = printedSignature(Decompiled, Entry);
    if (!Printed) {
      S.setError("the decompiler printed no signature for the function at " +
                 vaHex(Entry));
      return std::nullopt;
    }
    Current.Text = std::move(*Printed);
  }
  Current.Read = types::parseFunctionDeclaration(Current.Text, Library);
  return Current;
}

std::optional<SourceABIValueLocation> parameterLocation(neverd_session_t Sess,
                                                        Session &S, va_t Entry,
                                                        size_t Index,
                                                        std::string &Error) {
  const types::TypeLibrary *Library = S.readLocalTypes();
  if (!Library) {
    Error = S.LastError;
    return std::nullopt;
  }
  std::optional<CurrentDeclaration> Current =
      currentDeclaration(Sess, S, Entry, *Library);
  if (!Current) {
    Error = S.LastError;
    return std::nullopt;
  }
  if (!Current->Read.ok()) {
    Error = "the function's declaration does not read: " +
            problemText(Current->Read.Problems.front());
    return std::nullopt;
  }
  if (Index >= Current->Read.Function->Params.size()) {
    Error = "the function's declaration has " +
            std::to_string(Current->Read.Function->Params.size()) +
            " parameters";
    return std::nullopt;
  }
  const std::optional<SourceParameterPlacement> Placement =
      placementOf(S, *Library, *Current->Read.Function, Current->Read.Name);
  if (!Placement || Index >= Placement->Parameters.size() ||
      Placement->Parameters[Index].empty()) {
    Error = "NeverD has no rules for where the image's convention passes "
            "the parameter";
    return std::nullopt;
  }
  return Placement->Parameters[Index].front().Location;
}

} // namespace neverd::sdk

extern "C" {

const char *neverd_func_type_json(neverd_session_t Sess, neverd_va_t Entry,
                                  const char *OptionsJson) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  std::string Error;
  std::optional<llvm::json::Object> Options = requestObject(OptionsJson, Error);
  if (!Options) {
    S->setError(Error);
    return nullptr;
  }
  (void)S->synchronizeFunctions();
  if (llvm::none_of(S->Functions,
                    [&](const FuncInfo &F) { return F.Entry == Entry; })) {
    S->setError("no function starts at " + vaHex(Entry));
    return nullptr;
  }
  const llvm::json::Value *Asked = Options->get("declaration");
  const llvm::json::Value *Index = Options->get("parameter");
  if (Asked && Index) {
    S->setError("a request checks a declaration or changes a parameter, not "
                "both");
    return nullptr;
  }
  const auto textOf = [&](const llvm::json::Value *Value,
                          llvm::StringRef Key) -> std::optional<std::string> {
    std::optional<llvm::StringRef> Text =
        Value ? Value->getAsString() : std::nullopt;
    if (!Text || Text->trim().empty() ||
        Text->size() > kMaxFunctionDeclarationBytes) {
      S->setError("\"" + Key.str() + "\" is C of at most " +
                  std::to_string(kMaxFunctionDeclarationBytes) + " bytes");
      return std::nullopt;
    }
    return Text->str();
  };
  types::TypeLibrary *Library = S->readLocalTypes();
  if (!Library)
    return nullptr;

  std::string Text;
  types::FunctionDeclaration Read;
  bool HasStated = false;
  if (Asked) {
    std::optional<std::string> Checked = textOf(Asked, "declaration");
    if (!Checked)
      return nullptr;
    Text = std::move(*Checked);
    Read = types::parseFunctionDeclaration(Text, *Library);
  } else {
    std::optional<CurrentDeclaration> Current =
        currentDeclaration(Sess, *S, Entry, *Library);
    if (!Current)
      return nullptr;
    Text = std::move(Current->Text);
    Read = std::move(Current->Read);
    HasStated = Current->Stated;
  }

  if (Index) {
    std::optional<int64_t> Position = Index->getAsInteger();
    std::optional<std::string> Type = textOf(Options->get("type"), "type");
    if (!Type)
      return nullptr;
    if (!Read.ok()) {
      S->setError("the function's declaration does not read: " +
                  problemText(Read.Problems.front()));
      return nullptr;
    }
    const size_t Count = Read.Function->Params.size();
    if (!Position || *Position < 0 || static_cast<size_t>(*Position) >= Count) {
      S->setError(Count ? "\"parameter\" is a position from 0 to " +
                              std::to_string(Count - 1)
                        : std::string("the function takes no parameters"));
      return nullptr;
    }
    // i386 declarations place every parameter on the stack, where a
    // function that takes some in registers (regparm, fastcall) finds none.
    if (S->Img.Arch == Arch::X86 && !HasStated) {
      if (const HighFunc *High = S->findHighFunc(Entry);
          High && llvm::any_of(High->Params, [](const HighParam &Param) {
            return Param.RegOff != kNoParamReg;
          })) {
        S->setError("the function takes parameters in registers, which "
                    "NeverD's i386 declarations do not place");
        return nullptr;
      }
    }
    // The type reads as the one parameter of a declaration of its own.
    const types::FunctionDeclaration Probe = types::parseFunctionDeclaration(
        ProbeHead.str() + *Type + ")", *Library);
    if (!Probe.ok() || Probe.Function->Params.size() != 1) {
      types::SourceProblem Problem =
          Probe.Problems.empty()
              ? types::SourceProblem{1, 1, "this is no parameter's type"}
              : Probe.Problems.front();
      if (Problem.Line == 1 && Problem.Column > ProbeHead.size())
        Problem.Column -= ProbeHead.size();
      S->setError("the type does not read: " + problemText(Problem));
      return nullptr;
    }
    types::FunctionDef Changed = *Read.Function;
    types::Parameter &Param = Changed.Params[*Position];
    Param.Type = Probe.Function->Params.front().Type;
    if (const std::string &Name = Probe.Function->Params.front().Name;
        !Name.empty())
      Param.Name = Name;
    llvm::StringSet<> Names;
    for (const types::Parameter &Other : Changed.Params)
      if (!Other.Name.empty() && !Names.insert(Other.Name).second) {
        S->setError("two parameters would be named " + Other.Name);
        return nullptr;
      }
    Text = declarationText(*Library, Changed, Read.Name);
    Read = types::parseFunctionDeclaration(Text, *Library);
    if (!Read.ok()) {
      S->setError("the changed declaration does not read: " +
                  problemText(Read.Problems.front()));
      return nullptr;
    }
  }

  llvm::json::Object Reply = declarationReply(*S, *Library, Text, Read);
  Reply["entry"] = vaHex(Entry);
  Reply["stated"] = !Asked && !Index && HasStated;
  // Whether a declaration stated for the function hides the prototype its
  // name binds, which analysis reads (neverd_user_facts_analysis_reads_json):
  // that reads the name, never the declaration.
  {
    const std::string Error = S->LastError;
    (void)S->decompileDebug();
    S->LastError = Error;
    const LocalTypesDebugContext *Local = S->LocalTypesDbg.get();
    Reply["hides_prototype"] = Local && Local->hidesPrototype(Entry);
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Reply))));
}

} // extern "C"
