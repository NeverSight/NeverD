//===- NeverDCmdTypes.cpp - The local types command -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `neverd types`: the program's local types as the workbench's Local Types
/// window shows them.  Without --type it lists them; --type lays a type out
/// (--layout) or renders it, with what it needs, as C, C++, Rust, Go,
/// Python, JavaScript or Lua; --all renders every type as one unit that
/// compiles.  Without a binary, --data-model converts the --declare files
/// from one language to another.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>

using namespace llvm;

namespace neverd::cli {
namespace {

/// The languages the engine renders, as --lang names them.
std::vector<std::string> languageIds() {
  std::vector<std::string> Ids;
  const char *Raw = neverd_types_languages_json();
  auto Parsed = json::parse(Raw ? Raw : "");
  neverd_free_string(Raw);
  if (!Parsed) {
    consumeError(Parsed.takeError());
    return Ids;
  }
  if (const json::Object *Root = Parsed->getAsObject())
    if (const json::Array *Languages = Root->getArray("languages"))
      for (const json::Value &Value : *Languages)
        if (const json::Object *Language = Value.getAsObject())
          if (std::optional<StringRef> Id = Language->getString("id"))
            Ids.push_back(Id->str());
  return Ids;
}

/// \p Ids as a message lists them: "c, cpp or rust".
std::string listed(ArrayRef<std::string> Ids) {
  std::string Text;
  for (size_t I = 0; I < Ids.size(); ++I) {
    if (I)
      Text += I + 1 == Ids.size() ? " or " : ", ";
    Text += Ids[I];
  }
  return Text;
}

/// What the engine says of the language --lang names.
std::optional<json::Object> languageTaken() {
  const char *Raw = neverd_types_languages_json();
  auto Parsed = json::parse(Raw ? Raw : "");
  neverd_free_string(Raw);
  if (!Parsed) {
    consumeError(Parsed.takeError());
    return std::nullopt;
  }
  if (const json::Object *Root = Parsed->getAsObject())
    if (const json::Array *Languages = Root->getArray("languages"))
      for (const json::Value &Value : *Languages)
        if (const json::Object *Language = Value.getAsObject();
            Language && Language->getString("id") == TypesLanguage.getValue())
          return *Language;
  return std::nullopt;
}

/// The spellings --lang takes, its own first.
std::vector<std::string> spellingIds() {
  std::vector<std::string> Ids;
  if (const std::optional<json::Object> Language = languageTaken())
    if (const json::Array *Spellings = Language->getArray("spellings"))
      for (const json::Value &Value : *Spellings)
        if (std::optional<StringRef> Id = Value.getAsString())
          Ids.push_back(Id->str());
  return Ids;
}

/// \p Options with the spelling --spelling asks, if it asks one.
json::Object withSpelling(json::Object Options) {
  if (!TypesSpelling.empty())
    Options["spelling"] = TypesSpelling.getValue();
  return Options;
}

/// The spelling the source --lang renders takes: the one asked, or the
/// language's own, as the engine says it.
std::string spellingTaken() {
  if (!TypesSpelling.empty())
    return TypesSpelling;
  if (const std::optional<json::Object> Language = languageTaken())
    return Language->getString("spelling").value_or("").str();
  return {};
}

std::string text(const json::Value &Value) {
  std::string Out;
  raw_string_ostream(Out) << Value;
  return Out;
}

/// The engine's JSON reply, or none with an error printed.
std::optional<json::Value> reply(neverd_session_t Sess, const char *Raw,
                                 StringRef What) {
  if (!Raw) {
    WithColor::error() << What << ": " << takeLastError(Sess) << "\n";
    return std::nullopt;
  }
  auto Parsed = json::parse(Raw);
  neverd_free_string(Raw);
  if (!Parsed) {
    consumeError(Parsed.takeError());
    WithColor::error() << What << ": the engine returned invalid JSON\n";
    return std::nullopt;
  }
  return std::move(*Parsed);
}

std::string hexText(int64_t Value) {
  std::string Out;
  raw_string_ostream(Out) << format_hex(static_cast<uint64_t>(Value), 0,
                                        /*Upper=*/true);
  if (Out.size() > 1)
    Out[1] = 'x';
  return Out;
}

int list(neverd_session_t Sess, raw_ostream &Out) {
  json::Object Options;
  if (!TypesFilter.empty())
    Options["filter"] = TypesFilter;
  if (!TypesOrigin.empty())
    Options["origin"] = TypesOrigin;
  if (TypesLanguage.getNumOccurrences())
    Options["language"] = TypesLanguage;
  std::optional<json::Value> List = reply(
      Sess, neverd_local_types_json(Sess, text(std::move(Options)).c_str()),
      "types");
  if (!List)
    return 1;
  if (JsonOutput) {
    Out << *List << "\n";
    return 0;
  }
  const json::Object &Result = *List->getAsObject();
  const json::Array *Types = Result.getArray("types");
  const json::Object *Stats = Result.getObject("stats");
  const StringRef Source = Result.getString("source").value_or("none");
  // How many each origin gave, of every type, whatever the list keeps.
  std::string From;
  if (const json::Object *Origins =
          Stats ? Stats->getObject("origins") : nullptr) {
    for (const auto &[Key, Label] :
         {std::pair<StringRef, StringRef>{"pdb", "from the PDB"},
          {"header", "from system headers"},
          {"user", "declared"}})
      if (const std::optional<int64_t> Count = Origins->getInteger(Key))
        From += (From.empty() ? "" : ", ") + std::to_string(*Count) + " " +
                Label.str();
  }
  if (From.empty())
    From = Source == "pdb" ? "from the PDB" : "none read";
  Out << "\nLocal types (" << (Types ? Types->size() : 0) << " listed; " << From
      << "; " << Result.getString("dataModel").value_or("") << "):\n";
  if (Stats && Source == "pdb")
    Out << format(
        "  %lld structures, %lld unions, %lld enumerations, %lld "
        "declared only\n",
        static_cast<long long>(Stats->getInteger("structs").value_or(0)),
        static_cast<long long>(Stats->getInteger("unions").value_or(0)),
        static_cast<long long>(Stats->getInteger("enums").value_or(0)),
        static_cast<long long>(Stats->getInteger("declaredOnly").value_or(0)));
  if (Types) {
    Out << format("  %6s  %-8s %8s %7s  %-6s  %s\n", "#", "Kind", "Size",
                  "Members", "Origin", "Name");
    for (const json::Value &Value : *Types) {
      const json::Object *Type = Value.getAsObject();
      if (!Type)
        continue;
      const std::optional<int64_t> Size = Type->getInteger("size");
      const std::optional<int64_t> Members = Type->getInteger("members");
      Out << format(
          "  %6lld  %-8s %8s %7s  %-6s  %s\n",
          static_cast<long long>(Type->getInteger("ordinal").value_or(0)),
          Type->getString("kind").value_or("").str().c_str(),
          Size ? hexText(*Size).c_str() : "-",
          Members ? std::to_string(*Members).c_str() : "-",
          Type->getString("origin").value_or("").str().c_str(),
          Type->getString("name").value_or("").str().c_str());
    }
  }
  if (Stats)
    if (const json::Array *Mismatches = Stats->getArray("nestingMismatches"))
      for (const json::Value &Name : *Mismatches)
        WithColor::warning()
            << "the members of " << Name.getAsString().value_or("")
            << " do not lay out as the PDB lists them\n";
  if (const json::Array *Renamed = Result.getArray("renamed"))
    for (const json::Value &Pair : *Renamed)
      if (const json::Array *Names = Pair.getAsArray();
          Names && Names->size() == 2)
        WithColor::note() << TypesLanguage << " spells "
                          << (*Names)[0].getAsString().value_or("") << " as "
                          << (*Names)[1].getAsString().value_or("") << "\n";
  return 0;
}

/// \p Members as rows of the layout table, a record in place followed by
/// its own members indented.
void layoutRows(const json::Array &Members, unsigned Depth, raw_ostream &Out) {
  for (const json::Value &Value : Members) {
    const json::Object *Member = Value.getAsObject();
    if (!Member)
      continue;
    const StringRef Kind = Member->getString("kind").value_or("");
    const int64_t Offset = Member->getInteger("offset").value_or(0);
    const int64_t Size = Member->getInteger("size").value_or(0);
    std::string Where = hexText(Offset);
    std::string Width = hexText(Size);
    if (Kind == "bitfield") {
      const int64_t Storage = Member->getInteger("storageOffset").value_or(0);
      const int64_t Bit =
          Member->getInteger("bitOffset").value_or(0) - Storage * 8;
      const int64_t Bits = Member->getInteger("bitWidth").value_or(0);
      Where += "." + std::to_string(Bit);
      Width = ":" + std::to_string(Bits);
    }
    std::string Name = std::string(Depth * 2, ' ');
    std::string Type = Member->getString("type").value_or("").str();
    if (Kind == "padding") {
      Name += "(padding)";
    } else {
      Name += Member->getString("name").value_or("").str();
      if (Kind == "struct" || Kind == "union")
        Type = Kind.str() + (Name.find_first_not_of(' ') == std::string::npos
                                 ? " (anonymous)"
                                 : "");
    }
    Out << format("  %-10s %-6s %-40s %s\n", Where.c_str(), Width.c_str(),
                  Name.c_str(), Type.c_str());
    if (const json::Array *Inner = Member->getArray("members"))
      layoutRows(*Inner, Depth + 1, Out);
  }
}

int layout(neverd_session_t Sess, raw_ostream &Out) {
  const std::string Options = text(withSpelling(json::Object{}));
  json::Array All;
  for (const std::string &Name : TypeNames) {
    std::optional<json::Value> Type =
        reply(Sess, neverd_local_type_json(Sess, Name.c_str(), Options.c_str()),
              "types");
    if (!Type)
      return 1;
    if (JsonOutput) {
      All.push_back(std::move(*Type));
      continue;
    }
    const json::Object &Object = *Type->getAsObject();
    Out << "\n" << Object.getString("kind").value_or("") << " " << Name;
    if (std::optional<int64_t> Size = Object.getInteger("size"))
      Out << ": " << hexText(*Size) << " (" << *Size << ") bytes";
    if (std::optional<int64_t> Align = Object.getInteger("alignment"))
      Out << ", aligned to " << *Align;
    if (std::optional<int64_t> Pack = Object.getInteger("pack"); Pack && *Pack)
      Out << ", packed to " << *Pack;
    Out << "\n";
    if (const json::Array *Members = Object.getArray("members")) {
      Out << format("  %-10s %-6s %-40s %s\n", "Offset", "Size", "Member",
                    "Type");
      layoutRows(*Members, 0, Out);
    }
    if (const json::Array *Values = Object.getArray("values"))
      for (const json::Value &Value : *Values)
        if (const json::Object *Entry = Value.getAsObject())
          Out << "  " << Entry->getString("name").value_or("") << " = "
              << *Entry->get("value") << "\n";
    if (std::optional<StringRef> Target = Object.getString("target"))
      Out << "  = " << *Target << "\n";
    if (std::optional<StringRef> Declaration = Object.getString("declaration"))
      Out << "  " << *Declaration << "\n";
  }
  if (JsonOutput)
    Out << json::Value(std::move(All)) << "\n";
  return 0;
}

int render(neverd_session_t Sess, raw_ostream &Out) {
  json::Object Options = withSpelling(
      json::Object{{"language", TypesLanguage},
                   {"offsets", !TypesNoOffsets},
                   {"hexOffsets", !TypesDecimalOffsets},
                   {"asserts", static_cast<bool>(TypesAsserts)},
                   {"prototypes", static_cast<bool>(TypesPrototypes)}});
  if (!TypesAll) {
    json::Array Names;
    for (const std::string &Name : TypeNames)
      Names.push_back(Name);
    Options["types"] = std::move(Names);
    Options["dependencies"] = !TypesAlone;
  }
  const char *Source =
      neverd_local_types_source(Sess, text(std::move(Options)).c_str());
  if (!Source) {
    WithColor::error() << "types: " << takeLastError(Sess) << "\n";
    return 1;
  }
  if (JsonOutput)
    Out << json::Value(json::Object{{"language", TypesLanguage},
                                    {"spelling", spellingTaken()},
                                    {"source", Source}})
        << "\n";
  else
    Out << Source;
  neverd_free_string(Source);
  return 0;
}

/// The language a file's extension says its declarations are in.
std::optional<StringRef> languageOfFile(StringRef Path) {
  const StringRef Extension = sys::path::extension(Path);
  if (Extension == ".h" || Extension == ".c")
    return StringRef("c");
  if (Extension == ".hpp" || Extension == ".hh" || Extension == ".hxx" ||
      Extension == ".cpp" || Extension == ".cc" || Extension == ".cxx")
    return StringRef("cpp");
  if (Extension == ".rs")
    return StringRef("rust");
  if (Extension == ".go")
    return StringRef("go");
  return std::nullopt;
}

/// Adds the types of each --declare file; false with each problem printed
/// as file:line:column.
bool declare(neverd_session_t Sess) {
  for (const std::string &Path : TypesDeclare) {
    const std::optional<StringRef> Language = languageOfFile(Path);
    if (!Language) {
      WithColor::error() << Path
                         << ": name C (.h), C++ (.hpp), Rust (.rs) or Go "
                            "(.go) declarations\n";
      return false;
    }
    auto Buffer = MemoryBuffer::getFile(Path);
    if (!Buffer) {
      WithColor::error() << "cannot read " << Path << ": "
                         << Buffer.getError().message() << "\n";
      return false;
    }
    const std::string Options =
        text(json::Object{{"language", Language->str()}});
    std::optional<json::Value> Reply =
        reply(Sess,
              neverd_local_types_declare(
                  Sess, (*Buffer)->getBuffer().str().c_str(), Options.c_str()),
              "types");
    if (!Reply)
      return false;
    const json::Array *Problems = Reply->getAsObject()->getArray("problems");
    if (Problems && !Problems->empty()) {
      for (const json::Value &Value : *Problems)
        if (const json::Object *Problem = Value.getAsObject())
          WithColor::error()
              << Path << ":" << Problem->getInteger("line").value_or(0) << ":"
              << Problem->getInteger("column").value_or(0) << ": "
              << Problem->getString("message").value_or("") << "\n";
      return false;
    }
  }
  return true;
}

/// The checks of options both ways of running share; false with an error
/// printed.
bool optionsValid() {
  if (const std::vector<std::string> Languages = languageIds();
      !is_contained(Languages, TypesLanguage.getValue())) {
    WithColor::error() << "--lang must be " << listed(Languages) << "\n";
    return false;
  }
  if (const std::vector<std::string> Spellings = spellingIds();
      !TypesSpelling.empty() &&
      !is_contained(Spellings, TypesSpelling.getValue())) {
    WithColor::error() << "--spelling must be " << listed(Spellings) << " in "
                       << TypesLanguage << "\n";
    return false;
  }
  if (TypesAll && !TypeNames.empty()) {
    WithColor::error() << "--all renders every type; leave out --type\n";
    return false;
  }
  if (TypesAlone && TypeNames.empty()) {
    WithColor::error() << "--alone needs a --type\n";
    return false;
  }
  if (TypesPrototypes && !TypesAll) {
    WithColor::error() << "--prototypes follows --all\n";
    return false;
  }
  return true;
}

} // namespace

int runTypesConvert() {
  if (TypesDataModel.empty()) {
    WithColor::error() << "types: name a binary, or --data-model LLP64, LP64 "
                          "or ILP32 to convert --declare files without one\n";
    return 1;
  }
  if (TypesDeclare.empty()) {
    WithColor::error() << "types: --data-model converts --declare files; "
                          "name at least one\n";
    return 1;
  }
  if (!optionsValid())
    return 1;
  if (TypesLayout || !TypesFilter.empty() || !TypesOrigin.empty()) {
    WithColor::error()
        << "--layout, --filter and --origin read a binary's types\n";
    return 1;
  }
  json::Array Declarations;
  for (const std::string &Path : TypesDeclare) {
    const std::optional<StringRef> Language = languageOfFile(Path);
    if (!Language) {
      WithColor::error() << Path
                         << ": name C (.h), C++ (.hpp), Rust (.rs) or Go "
                            "(.go) declarations\n";
      return 1;
    }
    auto Buffer = MemoryBuffer::getFile(Path);
    if (!Buffer) {
      WithColor::error() << "cannot read " << Path << ": "
                         << Buffer.getError().message() << "\n";
      return 1;
    }
    // A JSON string from a StringRef refers to the buffer, which goes.
    Declarations.push_back(
        json::Object{{"language", Language->str()},
                     {"source", (*Buffer)->getBuffer().str()}});
  }
  json::Object Request{{"dataModel", TypesDataModel.getValue()},
                       {"declarations", std::move(Declarations)},
                       {"keepGoing", TypesKeepGoing.getValue()}};
  if (!TypesRecordABI.empty())
    Request["recordABI"] = TypesRecordABI.getValue();
  const bool Renders = TypesAll || !TypeNames.empty();
  if (Renders) {
    json::Array Names;
    for (const std::string &Name : TypeNames)
      Names.push_back(Name);
    json::Object Render =
        withSpelling(json::Object{{"language", TypesLanguage.getValue()},
                                  {"offsets", !TypesNoOffsets},
                                  {"hexOffsets", !TypesDecimalOffsets},
                                  {"asserts", TypesAsserts.getValue()},
                                  {"prototypes", TypesPrototypes.getValue()},
                                  {"dependencies", !TypesAlone}});
    if (!Names.empty())
      Render["types"] = std::move(Names);
    Request["render"] = std::move(Render);
  }
  const char *Raw = neverd_types_convert(text(std::move(Request)).c_str());
  auto Parsed = json::parse(Raw ? Raw : "");
  neverd_free_string(Raw);
  if (!Parsed || !Parsed->getAsObject()) {
    if (!Parsed)
      consumeError(Parsed.takeError());
    WithColor::error() << "types: the engine returned invalid JSON\n";
    return 1;
  }
  const json::Object &Reply = *Parsed->getAsObject();
  if (std::optional<StringRef> Error = Reply.getString("error")) {
    WithColor::error() << "types: " << *Error << "\n";
    return 1;
  }
  // Problems by file and place; kept-going ones are warnings.  JSON says
  // them in its object instead.
  bool Problems = false;
  if (const json::Array *List = Reply.getArray("problems"))
    for (const json::Value &Value : *List)
      if (const json::Object *Problem = Value.getAsObject()) {
        Problems = true;
        if (JsonOutput)
          continue;
        const size_t Index =
            size_t(Problem->getInteger("declaration").value_or(0));
        (TypesKeepGoing ? WithColor::warning() : WithColor::error())
            << (Index < TypesDeclare.size() ? TypesDeclare[Index] : "?") << ":"
            << Problem->getInteger("line").value_or(0) << ":"
            << Problem->getInteger("column").value_or(0) << ": "
            << Problem->getString("message").value_or("") << "\n";
      }
  if (Problems && !TypesKeepGoing && !JsonOutput)
    return 1;
  std::unique_ptr<raw_fd_ostream> File;
  if (!OutputFile.empty()) {
    std::error_code EC;
    File = std::make_unique<raw_fd_ostream>(OutputFile, EC);
    if (EC) {
      WithColor::error() << "cannot write " << OutputFile << ": "
                         << EC.message() << "\n";
      return 1;
    }
  }
  raw_ostream &Out = File ? *File : outs();
  // One object for a program to read: what read, the problems, and the
  // source in the language and spelling it took.
  if (JsonOutput) {
    json::Object Answer = Reply;
    // A problem names its file, as the text says it.
    if (json::Array *List = Answer.getArray("problems"))
      for (json::Value &Value : *List)
        if (json::Object *Problem = Value.getAsObject()) {
          const size_t Index =
              size_t(Problem->getInteger("declaration").value_or(0));
          if (Index < TypesDeclare.size())
            (*Problem)["file"] = TypesDeclare[Index];
        }
    if (Renders) {
      Answer["language"] = TypesLanguage.getValue();
      Answer["spelling"] = spellingTaken();
    }
    Out << json::Value(std::move(Answer)) << "\n";
    return Problems && !TypesKeepGoing ? 1 : 0;
  }
  if (Renders) {
    Out << Reply.getString("source").value_or("");
    return 0;
  }
  // Without --type or --all, the names of the types and prototypes read.
  for (const char *Key : {"types", "prototypes"})
    if (const json::Array *Names = Reply.getArray(Key))
      for (const json::Value &Name : *Names)
        Out << (StringRef(Key) == "types" ? "type " : "prototype ")
            << Name.getAsString().value_or("") << "\n";
  return 0;
}

int runTypes(neverd_session_t Sess) {
  if (!optionsValid())
    return 1;
  if (TypesLayout && TypeNames.empty()) {
    WithColor::error() << "--layout needs a --type\n";
    return 1;
  }
  std::unique_ptr<raw_fd_ostream> File;
  if (!OutputFile.empty()) {
    std::error_code EC;
    File = std::make_unique<raw_fd_ostream>(OutputFile, EC);
    if (EC) {
      WithColor::error() << "cannot write " << OutputFile << ": "
                         << EC.message() << "\n";
      return 1;
    }
  }
  raw_ostream &Out = File ? *File : outs();
  if (!declare(Sess))
    return 1;
  if (TypesFit)
    return runTypesFit(Sess, Out);
  if (!TypesMember.empty())
    return runTypesMember(Sess, Out);
  if (TypeNames.empty() && !TypesAll)
    return list(Sess, Out);
  if (TypesLayout)
    return layout(Sess, Out);
  return render(Sess, Out);
}

} // namespace neverd::cli
