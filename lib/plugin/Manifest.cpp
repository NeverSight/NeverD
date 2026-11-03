//===- Manifest.cpp - Shared process plugin declarations ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Owning validation for process manifests, configuration and result envelopes.
///
//===----------------------------------------------------------------------===//

#include "neverd/plugin/Manifest.h"

#include "Text.h"

#include "neverd/plugin/Config.h"
#include "neverd/sdk/Limits.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>

namespace neverd::plugin {
using namespace limits;
namespace {
const Json &get(const Json &Object, const char *Key) {
  static const Json Missing;
  auto I = Object.find(Key);
  return I == Object.end() ? Missing : *I;
}
const Json &array(const Json &Object, const char *Key) {
  static const Json Empty = Json::array();
  const auto &Value = get(Object, Key);
  return Value.is_array() ? Value : Empty;
}
std::string string(const Json &Value) {
  return Value.is_string() ? Value.get<std::string>() : std::string();
}
bool fields(const Json &Object, std::initializer_list<const char *> Allowed) {
  if (!Object.is_object())
    return false;
  for (auto I = Object.begin(); I != Object.end(); ++I)
    if (std::find(Allowed.begin(), Allowed.end(), I.key()) == Allowed.end())
      return false;
  return true;
}
bool matches(const Json &Value, const std::regex &Pattern) {
  return Value.is_string() &&
         std::regex_match(Value.get_ref<const std::string &>(), Pattern);
}
bool identifier(const Json &Value) {
  return Value.is_string() &&
         validIdentifier(Value.get_ref<const std::string &>());
}
bool text(const Json &Value, size_t Limit = kMaxLabelCharacters,
          bool Empty = false) {
  if (!Value.is_string())
    return false;
  const auto &Text = Value.get_ref<const std::string &>();
  return (Empty || !Text.empty()) && detail::textUnits(Text) <= Limit &&
         Text.find('\0') == std::string::npos;
}
bool integer(const Json &Value, double Min, double Max) {
  if (!Value.is_number())
    return false;
  const auto Number = Value.get<double>();
  return std::isfinite(Number) && std::floor(Number) == Number &&
         Number >= Min && Number <= Max;
}
bool parameter(const Json &Field, const Json &Value) {
  const auto Type = string(get(Field, "type"));
  if (Type == "boolean")
    return Value.is_boolean();
  if (Type == "integer")
    return integer(Value, Field.value("minimum", 0),
                   Field.value("maximum", kDefaultParameterMaximum));
  if (Type == "string")
    return text(Value, kMaxParameterCharacters, true);
  if (Type == "choice") {
    const auto &Choices = get(Field, "choices");
    return Value.is_string() &&
           std::find(Choices.begin(), Choices.end(), Value) != Choices.end();
  }
  return false;
}
bool parameterDeclaration(const Json &Field) {
  const auto Type = string(get(Field, "type"));
  if (Type == "integer") {
    if (!fields(Field,
                {"id", "title", "type", "default", "minimum", "maximum"}))
      return false;
    for (const auto *Key : {"minimum", "maximum"})
      if (Field.contains(Key) &&
          !integer(Field[Key], std::numeric_limits<int>::min(),
                   std::numeric_limits<int>::max()))
        return false;
  } else if (Type == "choice") {
    const auto &Choices = array(Field, "choices");
    if (!fields(Field, {"id", "title", "type", "default", "choices"}) ||
        Choices.empty() || Choices.size() > kMaxChoices)
      return false;
    std::set<std::string> Seen;
    for (const auto &Choice : Choices)
      if (!text(Choice) || !Seen.insert(string(Choice)).second)
        return false;
  } else if (!fields(Field, {"id", "title", "type", "default"}))
    return false;
  return identifier(get(Field, "id")) && text(get(Field, "title")) &&
         parameter(Field, get(Field, "default"));
}
bool address(const Json &Value) {
  static const std::regex Pattern("0x[0-9a-fA-F]{1,16}");
  return matches(Value, Pattern);
}
bool optionalAddress(const Json &Object) {
  return !Object.contains("address") || address(Object["address"]);
}
bool digest(const Json &Value) {
  static const std::regex Pattern("[a-f0-9]{64}");
  return matches(Value, Pattern);
}
std::filesystem::path programPath(const std::filesystem::path &Manifest,
                                  const std::string &Program) {
  namespace fs = std::filesystem;
  auto Path = fs::u8path(Program);
  if (Path.is_absolute())
    return Path;
#ifdef _WIN32
  // CreateProcess needs the image's extension, including for a bundled one.
  if (!Path.has_extension())
    Path += ".exe";
  // The narrow environment uses the ANSI code page, not UTF-8.
  const auto *Search = _wgetenv(L"PATH");
  using Text = std::wstring;
  constexpr wchar_t Separator = L';';
#else
  const auto *Search = std::getenv("PATH");
  using Text = std::string;
  constexpr char Separator = ':';
#endif
  const auto Local = Manifest.parent_path() / Path;
  if (fs::is_regular_file(Local))
    return Local;
  if (!Search)
    return {};
  std::basic_istringstream<Text::value_type> Stream(Search);
  Text Directory;
  while (std::getline(Stream, Directory, Separator)) {
#ifdef _WIN32
    auto Candidate = fs::path(Directory) / Path;
#else
    auto Candidate = fs::u8path(Directory) / Path;
#endif
    // An unusable search entry does not hide later ones.
    std::error_code EC;
    if (fs::is_regular_file(Candidate, EC))
      return fs::absolute(Candidate);
  }
  return {};
}
} // namespace

std::vector<std::string> queryServices() {
  return {
#define NEVERD_PLUGIN_QUERY(Name) Name,
#include "neverd/plugin/Services.def"
  };
}
bool validIdentifier(std::string_view Value) {
  static const std::regex Pattern("[a-z][a-z0-9_.-]{0,63}");
  return std::regex_match(Value.begin(), Value.end(), Pattern);
}

Json parseJson(std::string_view Text) {
  // nlohmann parses and destroys deep values iteratively, but copies them
  // recursively; refuse the nesting before any consumer copies a value.
  int Depth = 0;
  bool Quoted = false, Escaped = false;
  for (const char C : Text) {
    if (Quoted) {
      if (Escaped)
        Escaped = false;
      else if (C == '\\')
        Escaped = true;
      else if (C == '"')
        Quoted = false;
    } else if (C == '"')
      Quoted = true;
    else if (C == '[' || C == '{') {
      if (++Depth > kMaxJsonDepth)
        return Json(Json::value_t::discarded);
    } else if ((C == ']' || C == '}') && Depth)
      --Depth;
  }
  return Json::parse(Text.begin(), Text.end(), nullptr, false);
}

std::optional<Manifest> loadManifest(const std::filesystem::path &Path,
                                     std::string &Error) {
  const auto Fail = [&](std::string Message) -> std::optional<Manifest> {
    Error = std::move(Message);
    return std::nullopt;
  };
  std::error_code EC;
  const auto Size = std::filesystem::file_size(Path, EC);
  if (EC || Size > kMaxManifestBytes)
    return Fail("Cannot read plugin manifest (maximum 64 KiB)");
  std::ifstream File(Path, std::ios::binary);
  std::string Bytes(static_cast<size_t>(Size), '\0');
  if (!File.read(Bytes.data(), Bytes.size()))
    return Fail("Cannot read plugin manifest");
  const auto Root = parseJson(Bytes);
  if (Root.is_discarded() ||
      !fields(Root, {"schema", "id", "name", "version", "program", "arguments",
                     "actions", "queries", "icon", "services", "events"}) ||
      get(Root, "schema") != "neverd.gui-plugin.v1" ||
      !identifier(get(Root, "id")) || !text(get(Root, "name")) ||
      !text(get(Root, "version"), 64) ||
      !text(get(Root, "program"), kMaxPathCharacters) ||
      (Root.contains("icon") && !text(Root["icon"], kMaxPathCharacters)))
    return Fail("Invalid process plugin manifest");
  Manifest M;
  M.Id = Root["id"];
  M.Name = Root["name"];
  M.Version = Root["version"];
  M.Icon = string(get(Root, "icon"));
  M.Path = std::filesystem::absolute(Path);
  try {
    M.Program = programPath(M.Path, Root["program"]);
    if (!std::filesystem::is_regular_file(M.Program))
      return Fail("Plugin program is unavailable");
  } catch (const std::system_error &E) {
    // Includes filesystem_error and Windows path conversion failures.
    return Fail(E.what());
  }
  if (!get(Root, "arguments").is_array() ||
      Root["arguments"].size() > kMaxArguments)
    return Fail("Plugin arguments must be an array of at most 32 strings");
  for (const auto &Arg : Root["arguments"]) {
    if (!text(Arg, kMaxPathCharacters, true))
      return Fail("Invalid plugin argument");
    M.Arguments.push_back(Arg);
  }
  const auto Services = queryServices();
  if (Root.contains("queries") && !Root["queries"].is_array())
    return Fail("Plugin queries must be an array");
  for (const auto &Query : array(Root, "queries")) {
    if (!Query.is_string())
      return Fail("Plugin queries must contain strings");
    const auto Name = Query.get<std::string>();
    if (std::find(Services.begin(), Services.end(), Name) == Services.end()) {
      std::string Supported;
      for (const auto &Service : Services)
        Supported += (Supported.empty() ? "" : ", ") + Service;
      return Fail("Unsupported plugin query '" + Name +
                  "'. This host supports: " + Supported);
    }
    if (std::find(M.Queries.begin(), M.Queries.end(), Name) != M.Queries.end())
      return Fail("Repeated plugin query '" + Name + "'");
    M.Queries.push_back(Name);
  }
  for (auto [Key, Requirements] :
       {std::pair{"services", &M.Services}, std::pair{"events", &M.Events}}) {
    if (Root.contains(Key) && !Root[Key].is_array())
      return Fail(std::string("Plugin ") + Key + " must be an array");
    std::set<std::pair<std::string, uint32_t>> Seen;
    for (const auto &Item : array(Root, Key)) {
      if (!fields(Item, {"id", "version"}) || Item.size() != 2 ||
          !identifier(get(Item, "id")) || !Item.contains("version") ||
          !Item["version"].is_number_integer() || Item["version"] <= 0 ||
          Item["version"] > UINT32_MAX)
        return Fail(std::string("Invalid plugin ") + Key + " requirement");
      CapabilityRequirement Requirement{Item["id"], Item["version"]};
      if (!Seen.emplace(Requirement.Id, Requirement.Version).second)
        return Fail(std::string("Repeated plugin ") + Key + " requirement");
      Requirements->push_back(std::move(Requirement));
    }
  }
  M.Actions = array(Root, "actions");
  if (M.Actions.empty() || M.Actions.size() > kMaxActions)
    return Fail("A plugin must declare 1 to 32 actions");
  std::set<std::string> Ids;
  for (const auto &Action : M.Actions) {
    const auto Context = string(get(Action, "context"));
    if (!fields(Action, {"id", "title", "context", "context_menu", "parameters",
                         "timeout_ms", "icon"}) ||
        !identifier(get(Action, "id")) ||
        !Ids.insert(string(Action["id"])).second ||
        !text(get(Action, "title")) ||
        (Context != "none" && Context != "project" && Context != "address" &&
         Context != "function") ||
        (Action.contains("context_menu") &&
         !Action["context_menu"].is_boolean()) ||
        (Action.contains("icon") &&
         !text(Action["icon"], kMaxPathCharacters)) ||
        (Action.contains("parameters") && !Action["parameters"].is_array()))
      return Fail("Invalid plugin action");
    if (Action.contains("timeout_ms") &&
        !integer(Action["timeout_ms"], kMinActionTimeoutMs,
                 kMaxActionTimeoutMs))
      return Fail("Action timeout must be 100 to 3600000 ms");
    const auto &Parameters = array(Action, "parameters");
    if (Parameters.size() > kMaxParameters)
      return Fail("Too many plugin parameters");
    std::set<std::string> Keys;
    for (const auto &Field : Parameters)
      if (!parameterDeclaration(Field) ||
          !Keys.insert(string(Field["id"])).second)
        return Fail("Invalid plugin parameter or default");
  }
  Error.clear();
  return M;
}

bool validateParameters(const Json &Fields, const Json &Values,
                        std::string &Error) {
  if (!Values.is_object() || Values.size() != Fields.size()) {
    Error = "Parameter names do not match the action";
    return false;
  }
  for (const auto &Field : Fields) {
    const auto Id = Field["id"].get<std::string>();
    if (!parameter(Field, get(Values, Id.c_str()))) {
      Error = "Invalid parameter: " + Id;
      return false;
    }
  }
  Error.clear();
  return true;
}

bool validateParameterFields(const Json &Fields, std::string &Error) {
  if (!Fields.is_array() || Fields.size() > kMaxParameters) {
    Error = "Invalid action parameter fields";
    return false;
  }
  std::set<std::string> Keys;
  for (const auto &Field : Fields)
    if (!parameterDeclaration(Field) ||
        !Keys.insert(string(Field["id"])).second) {
      Error = "Invalid or repeated action parameter field";
      return false;
    }
  Error.clear();
  return true;
}

std::optional<Json> resolveParameters(const Json &Fields, const Json &User,
                                      const Json &Project, const Json &Explicit,
                                      std::string &Error) {
  Json Effective = Json::object();
  for (const auto &Field : Fields)
    Effective[Field["id"].get<std::string>()] = Field["default"];
  for (const auto *Layer : {&User, &Project, &Explicit}) {
    if (!Layer->is_object()) {
      Error = "Configuration layer must be an object";
      return std::nullopt;
    }
    for (auto I = Layer->begin(); I != Layer->end(); ++I) {
      if (!Effective.contains(I.key())) {
        Error = "Unknown parameter: " + I.key();
        return std::nullopt;
      }
      Effective[I.key()] = I.value();
    }
    // Validate each supplied layer. A later override must not hide bad stored
    // configuration or an unknown field from another frontend.
    if (!validateParameters(Fields, Effective, Error))
      return std::nullopt;
  }
  return Effective;
}

bool validateResult(const Json &Result, std::string &Error,
                    bool StructuredData) {
  const auto Fail = [&](const char *Message) {
    Error = Message;
    return false;
  };
  if (!fields(Result, {"views", "artifacts", "image_addition", "data"}) ||
      (!StructuredData && Result.contains("data")) ||
      !get(Result, "views").is_array() ||
      Result["views"].size() > kMaxResultViews ||
      (Result.contains("artifacts") && !Result["artifacts"].is_array()) ||
      array(Result, "artifacts").size() > kMaxArtifacts)
    return Fail("Invalid plugin result envelope");
  for (const auto &View : Result["views"]) {
    if (!View.is_object() || !text(get(View, "title")))
      return Fail("Result view needs a title");
    const auto Kind = string(get(View, "kind"));
    if (Kind == "text") {
      if (!fields(View, {"kind", "title", "text"}) ||
          !text(get(View, "text"), kMaxTextCharacters, true))
        return Fail("Invalid text view");
    } else if (Kind == "table") {
      const auto &Columns = array(View, "columns");
      if (!fields(View, {"kind", "title", "columns", "rows"}) ||
          Columns.empty() || Columns.size() > kMaxTableColumns ||
          !get(View, "rows").is_array() || View["rows"].size() > kMaxTableRows)
        return Fail("Invalid table view");
      for (const auto &Column : Columns)
        if (!text(Column, kMaxRequestIdCharacters))
          return Fail("Invalid table column");
      for (const auto &Row : View["rows"]) {
        if (!fields(Row, {"cells", "address"}) || !optionalAddress(Row) ||
            !get(Row, "cells").is_array() ||
            Row["cells"].size() != Columns.size())
          return Fail("Invalid table row");
        for (const auto &Cell : Row["cells"])
          if (!text(Cell, kMaxCellCharacters, true))
            return Fail("Invalid table cell");
      }
    } else if (Kind == "graph") {
      if (!fields(View, {"kind", "title", "nodes", "edges"}) ||
          !get(View, "nodes").is_array() || !get(View, "edges").is_array() ||
          View["nodes"].size() > kMaxGraphNodes ||
          View["edges"].size() > kMaxGraphEdges)
        return Fail("Invalid graph view");
      std::set<std::string> Ids;
      for (const auto &Node : View["nodes"]) {
        if (!fields(Node, {"id", "text", "address"}) ||
            !optionalAddress(Node) ||
            !text(get(Node, "id"), kMaxRequestIdCharacters) ||
            !text(get(Node, "text"), kMaxGraphNodeCharacters) ||
            !Ids.insert(string(Node["id"])).second)
          return Fail("Invalid graph node");
      }
      for (const auto &Edge : View["edges"])
        if (!fields(Edge, {"from", "to"}) ||
            !Ids.contains(string(get(Edge, "from"))) ||
            !Ids.contains(string(get(Edge, "to"))))
          return Fail("Invalid graph edge");
    } else
      return Fail("Unsupported result view kind");
  }
  const auto &Artifacts = array(Result, "artifacts");
  for (const auto &Artifact : Artifacts)
    if (!fields(Artifact, {"title", "path", "sha256"}) ||
        !text(get(Artifact, "title")) ||
        !text(get(Artifact, "path"), kMaxPathCharacters) ||
        !digest(get(Artifact, "sha256")))
      return Fail("Invalid artifact descriptor");
  if (Result.contains("image_addition")) {
    const auto &Addition = Result["image_addition"];
    const auto &Description = get(Addition, "description");
    if (!fields(Addition, {"artifact", "sample_sha256", "description"}) ||
        !integer(get(Addition, "artifact"), 0, double(Artifacts.size()) - 1) ||
        !digest(get(Addition, "sample_sha256")) ||
        !fields(Description,
                {"schema", "name", "address", "permissions", "functions"}) ||
        get(Description, "schema") != "neverd.analysis-section.v1" ||
        !text(get(Description, "name"), kMaxSectionNameCharacters) ||
        !text(get(Description, "address"), kMaxAddressCharacters) ||
        !text(get(Description, "permissions"), 3) ||
        !get(Description, "functions").is_array() ||
        Description["functions"].size() >
            ::neverd::sdk::kMaxAnalysisSectionFunctions)
      return Fail("Invalid analysis image addition");
  }
  Error.clear();
  return true;
}
} // namespace neverd::plugin
