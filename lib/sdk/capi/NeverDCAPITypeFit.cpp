//===- NeverDCAPITypeFit.cpp - C API: local types that fit a pointer ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Which local types a pointer may point at, judged by what the code does
/// through it (types::fitTypes over MedPointerAccesses), and where an access
/// lands in a record, as the listing names a structure offset.
///
//===----------------------------------------------------------------------===//

#include "FunctionDeclaration.h"
#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedPointerAccesses.h"
#include "neverd/sdk/NeverDCAPIDisasm.h"
#include "neverd/sdk/NeverDCAPILocalTypes.h"
#include "neverd/types/TypeFit.h"
#include "neverd/types/TypeJSON.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"

#include <optional>
#include <string>
#include <vector>

using namespace neverd;
using namespace neverd::sdk;

namespace {

/// How many accesses a request may judge, how long its text may be and how
/// many candidates it may ask for.
constexpr size_t MaxAccesses = 4096;
constexpr size_t MaxText = 256;
constexpr int64_t MaxLimit = 500;
/// The widest access a request may state, in bytes.
constexpr int64_t MaxAccessBytes = 1 << 16;

llvm::StringRef useName(types::AccessUse Use) {
  switch (Use) {
  case types::AccessUse::Read:
    return "read";
  case types::AccessUse::Write:
    return "write";
  case types::AccessUse::Address:
    return "address";
  }
  return "read";
}

std::optional<types::AccessUse> useNamed(llvm::StringRef Name) {
  for (types::AccessUse Use : {types::AccessUse::Read, types::AccessUse::Write,
                               types::AccessUse::Address})
    if (Name == useName(Use))
      return Use;
  return std::nullopt;
}

types::AccessUse useOf(MedPointerAccess::Use Use) {
  switch (Use) {
  case MedPointerAccess::Use::Read:
    return types::AccessUse::Read;
  case MedPointerAccess::Use::Write:
    return types::AccessUse::Write;
  case MedPointerAccess::Use::Address:
    return types::AccessUse::Address;
  }
  return types::AccessUse::Read;
}

/// An address as a request spells it: hexadecimal text with or without 0x,
/// or an integer.
std::optional<uint64_t> addressOf(const llvm::json::Value *Value) {
  if (!Value)
    return std::nullopt;
  if (std::optional<uint64_t> Integer = Value->getAsUINT64())
    return Integer;
  std::optional<llvm::StringRef> Text = Value->getAsString();
  if (!Text)
    return std::nullopt;
  llvm::StringRef Digits = *Text;
  if (!Digits.consume_front("0x"))
    Digits.consume_front("0X");
  uint64_t Address = 0;
  if (Digits.empty() || Digits.getAsInteger(16, Address))
    return std::nullopt;
  return Address;
}

/// The register a pointer is in, as the listing names it; empty for another
/// value.
std::string registerName(const MedVar &Var) {
  if (Var.Kind != MedVar::Reg)
    return {};
  const auto &Info = getTargetRegInfo(Var.TheArch);
  const char *Name =
      Info.GetRegName ? Info.GetRegName(Var.RegOff, Var.Size) : nullptr;
  return Name && Name[0] != '?' ? llvm::StringRef(Name).lower() : std::string();
}

/// The JSON object \p Json, or one without members for none; null with
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
  Error = "the request must be a JSON object";
  return std::nullopt;
}

/// Reads the request's own accesses into \p Accesses; false with \p Error
/// when one is malformed.
bool readAccesses(const llvm::json::Object &Request,
                  std::vector<types::PointerAccess> &Accesses,
                  std::string &Error) {
  const llvm::json::Value *Value = Request.get("accesses");
  if (!Value)
    return true;
  const llvm::json::Array *List = Value->getAsArray();
  if (!List || List->size() > MaxAccesses) {
    Error = "\"accesses\" must be an array of at most " +
            std::to_string(MaxAccesses) + " accesses";
    return false;
  }
  for (const llvm::json::Value &Entry : *List) {
    const llvm::json::Object *Access = Entry.getAsObject();
    std::optional<int64_t> Offset =
        Access ? Access->getInteger("offset") : std::nullopt;
    std::optional<int64_t> Size =
        Access ? Access->getInteger("size") : std::nullopt;
    if (!Offset || !Size || *Size < 0 || *Size > MaxAccessBytes) {
      Error = "an access needs an integer \"offset\" and a \"size\" of 0 to " +
              std::to_string(MaxAccessBytes) + " bytes";
      return false;
    }
    types::AccessUse Use =
        *Size ? types::AccessUse::Read : types::AccessUse::Address;
    if (const llvm::json::Value *Named = Access->get("use")) {
      std::optional<llvm::StringRef> Text = Named->getAsString();
      std::optional<types::AccessUse> Found =
          Text ? useNamed(*Text) : std::nullopt;
      if (!Found || (*Found == types::AccessUse::Address) != (*Size == 0)) {
        Error = "an access's \"use\" is read or write for bytes, address for "
                "none";
        return false;
      }
      Use = *Found;
    }
    Accesses.push_back({*Offset, static_cast<uint64_t>(*Size), Use});
  }
  return true;
}

/// Adds the accesses through the pointer the request's target names to
/// \p Accesses and describes them in \p Target; false with \p Error.
bool readTarget(neverd_session_t Sess, Session &S,
                const llvm::json::Object &Request,
                std::vector<types::PointerAccess> &Accesses,
                std::optional<llvm::json::Object> &Target, std::string &Error) {
  const llvm::json::Value *Value = Request.get("target");
  if (!Value)
    return true;
  const llvm::json::Object *Wanted = Value->getAsObject();
  const std::optional<uint64_t> Function = addressOf(Request.get("function"));
  const std::optional<uint64_t> Address =
      Wanted ? addressOf(Wanted->get("address")) : std::nullopt;
  const llvm::json::Value *Parameter =
      Wanted ? Wanted->get("parameter") : nullptr;
  const std::optional<int64_t> Index =
      Parameter ? Parameter->getAsInteger() : std::nullopt;
  if (!Function || (Address.has_value() == (Parameter != nullptr)) ||
      (Parameter && (!Index || *Index < 0))) {
    Error = "a \"target\" names an instruction's \"address\", or a "
            "\"parameter\" by its position from 0, of the \"function\"";
    return false;
  }
  // A parameter is where the function's declaration says the convention
  // passes it, read before the analysis the accesses come from.
  std::optional<SourceABIValueLocation> Location;
  if (Parameter) {
    Location = parameterLocation(Sess, S, *Function,
                                 static_cast<size_t>(*Index), Error);
    if (!Location)
      return false;
    if (Location->Kind != SourceABICarrierKind::IntegerRegister) {
      Error = "the parameter does not arrive in an integer register, where "
              "NeverD follows a pointer's accesses";
      return false;
    }
  }
  if (!neverd_prepare_function_view(Sess, *Function, "med")) {
    Error =
        S.LastError.empty() ? "the function's analysis failed" : S.LastError;
    return false;
  }
  const MedFunc *Func = S.findMedFunc(*Function);
  if (!Func) {
    Error = "no function starts at " + vaHex(*Function);
    return false;
  }
  const MedPointerAccesses Uses(*Func);
  if (Parameter) {
    // The register's value at entry: what the entry block's first copies
    // read of it, and what they make of it.
    std::vector<MedVar> Values;
    if (!Func->Blocks.empty())
      for (const MedOp &Op : Func->Blocks.front().Ops) {
        if (Op.Opcode != NdOp::COPY)
          break;
        if (Op.NumInputs == 1 && Op.Output.Kind == MedVar::Reg &&
            Op.Inputs[0].Kind == MedVar::Reg &&
            Op.Output.RegOff == Location->RegisterOffset &&
            Op.Inputs[0].RegOff == Location->RegisterOffset) {
          Values.push_back(Op.Inputs[0]);
          Values.push_back(Op.Output);
        }
      }
    std::vector<MedPointerAccess> Through;
    for (const MedVar &Base : Values)
      for (const MedPointerAccess &Access : Uses.through(Base))
        if (!llvm::is_contained(Through, Access))
          Through.push_back(Access);
    llvm::sort(Through,
               [](const MedPointerAccess &A, const MedPointerAccess &B) {
                 return std::tie(A.Addr, A.Offset, A.Bytes) <
                        std::tie(B.Addr, B.Offset, B.Bytes);
               });
    llvm::json::Object Described{{"parameter", *Index}};
    MedVar Register;
    Register.Kind = MedVar::Reg;
    Register.RegOff = Location->RegisterOffset;
    Register.Size = Location->ValueBytes ? Location->ValueBytes : 8;
    Register.TheArch = S.Img.Arch;
    if (std::string Name = registerName(Register); !Name.empty())
      Described["base"] = std::move(Name);
    llvm::json::Array List;
    for (const MedPointerAccess &Access : Through) {
      Accesses.push_back({Access.Offset, Access.Bytes, useOf(Access.Kind)});
      List.push_back(llvm::json::Object{{"address", vaHex(Access.Addr)},
                                        {"offset", Access.Offset},
                                        {"size", Access.Bytes},
                                        {"use", useName(useOf(Access.Kind))}});
    }
    Described["accesses"] = std::move(List);
    Target = std::move(Described);
    return true;
  }
  const auto At = Uses.at(*Address);
  if (!At) {
    Error = "the instruction at " + vaHex(*Address) +
            " reaches no memory through a pointer";
    return false;
  }
  llvm::json::Object Described{{"offset", At->second.Offset},
                               {"size", At->second.Bytes},
                               {"use", useName(useOf(At->second.Kind))}};
  if (std::string Name = registerName(At->first); !Name.empty())
    Described["base"] = std::move(Name);
  llvm::json::Array List;
  for (const MedPointerAccess &Access : Uses.through(At->first)) {
    Accesses.push_back({Access.Offset, Access.Bytes, useOf(Access.Kind)});
    List.push_back(llvm::json::Object{{"address", vaHex(Access.Addr)},
                                      {"offset", Access.Offset},
                                      {"size", Access.Bytes},
                                      {"use", useName(useOf(Access.Kind))}});
  }
  Described["accesses"] = std::move(List);
  Target = std::move(Described);
  return true;
}

/// The spelling the request asks member types in, C's own by default;
/// none with \p Error when it names none.
std::optional<types::BuiltinSpelling>
readSpelling(const llvm::json::Object &Request, std::string &Error) {
  const llvm::json::Value *Value = Request.get("spelling");
  if (!Value)
    return types::defaultSpelling(types::Language::C);
  std::optional<llvm::StringRef> Name = Value->getAsString();
  std::optional<types::BuiltinSpelling> Spelling =
      Name ? types::spellingNamed(*Name) : std::nullopt;
  if (!Spelling || !types::spellsBuiltins(types::Language::C, *Spelling)) {
    Error = "\"spelling\" must be " + types::spellingsText(types::Language::C);
    return std::nullopt;
  }
  return Spelling;
}

} // namespace

extern "C" {

const char *neverd_local_types_fit_json(neverd_session_t Sess,
                                        const char *RequestJson) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  std::string Error;
  std::optional<llvm::json::Object> Request = requestObject(RequestJson, Error);
  types::FitQuery Query;
  std::optional<types::BuiltinSpelling> Spelling;
  if (Request) {
    Spelling = readSpelling(*Request, Error);
    if (Spelling)
      readAccesses(*Request, Query.Accesses, Error);
  }
  if (!Error.empty()) {
    S->setError(Error);
    return nullptr;
  }
  if (const llvm::json::Value *Text = Request->get("text")) {
    std::optional<llvm::StringRef> Value = Text->getAsString();
    if (!Value || Value->size() > MaxText) {
      S->setError("\"text\" must be text of at most " +
                  std::to_string(MaxText) + " bytes");
      return nullptr;
    }
    Query.Text = Value->str();
  }
  for (auto [Key, Flag] :
       {std::pair<llvm::StringRef, bool *>{"records", &Query.RecordsOnly},
        {"shifts", &Query.Shifts}})
    if (const llvm::json::Value *Value = Request->get(Key)) {
      std::optional<bool> Set = Value->getAsBoolean();
      if (!Set) {
        S->setError(("\"" + Key + "\" must be true or false").str());
        return nullptr;
      }
      *Flag = *Set;
    }
  if (const llvm::json::Value *Value = Request->get("limit")) {
    std::optional<int64_t> Limit = Value->getAsInteger();
    if (!Limit || *Limit < 1 || *Limit > MaxLimit) {
      S->setError("\"limit\" must be 1 to " + std::to_string(MaxLimit));
      return nullptr;
    }
    Query.Limit = static_cast<size_t>(*Limit);
  }
  std::optional<llvm::json::Object> Target;
  if (!readTarget(Sess, *S, *Request, Query.Accesses, Target, Error)) {
    S->setError(Error);
    return nullptr;
  }
  if (Query.Accesses.size() > MaxAccesses)
    Query.Accesses.resize(MaxAccesses);
  const types::TypeLibrary *Library = S->readLocalTypes();
  if (!Library)
    return nullptr;
  size_t Total = 0;
  const std::vector<types::TypeFit> Found =
      types::fitTypes(*Library, Query, &Total);
  llvm::json::Array Accesses;
  for (const types::PointerAccess &Access : Query.Accesses)
    Accesses.push_back(llvm::json::Object{{"offset", Access.Offset},
                                          {"size", Access.Bytes},
                                          {"use", useName(Access.Use)}});
  llvm::json::Object Out{
      {"accesses", std::move(Accesses)},
      {"candidates", types::typeFitsJSON(*Library, Found, *Spelling)},
      {"total", Total}};
  if (Target)
    Out["target"] = std::move(*Target);
  return dupStr(jsonToString(llvm::json::Value(std::move(Out))));
}

const char *neverd_local_type_member_json(neverd_session_t Sess,
                                          const char *Name, long long Offset,
                                          unsigned long long Size,
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
  std::optional<types::BuiltinSpelling> Spelling;
  if (Options)
    Spelling = readSpelling(*Options, Error);
  if (!Spelling) {
    S->setError(Error);
    return nullptr;
  }
  if (!Name || Size > static_cast<unsigned long long>(MaxAccessBytes)) {
    S->setError(Name ? "an access is at most " +
                           std::to_string(MaxAccessBytes) + " bytes"
                     : std::string("no type named"));
    return nullptr;
  }
  const types::TypeLibrary *Library = S->readLocalTypes();
  if (!Library)
    return nullptr;
  const types::LocalType *Record = Library->resolve(Name);
  if (!Record || !Record->Record || !Record->Record->Defined ||
      (Record->Kind != types::TypeKind::Struct &&
       Record->Kind != types::TypeKind::Union)) {
    S->setError("no structure or union named " + jsonSafeText(Name));
    return nullptr;
  }
  const types::MemberAt At =
      types::memberAt(*Library, *Record->Record, Offset, Size);
  llvm::json::Object Out = types::memberAtJSON(*Library, At, *Spelling);
  Out["record"] = jsonSafeText(Record->Name);
  Out["size"] = Record->Record->Size;
  return dupStr(jsonToString(llvm::json::Value(std::move(Out))));
}

} // extern "C"
