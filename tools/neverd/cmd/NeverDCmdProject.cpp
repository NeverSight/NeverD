//===- NeverDCmdProject.cpp - Projects under version control --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `neverd merge-project` and `neverd setup-git`: a project's folder beside a
/// binary (`<binary>.neverd/`) shared through git.  The folder brings its own
/// .gitignore and .gitattributes; a clone names the merge driver once.
///
/// git merges text by lines, and two people who each add a row to the same
/// table add it after the same last row, which git calls a conflict however
/// far apart the addresses are.  merge-project is git's merge driver for the
/// files: it merges a table by its rows' addresses, an object by its keys and
/// a row by its fields, as IDA's team server merges databases, and writes the
/// result as the workbench and the command line write the file.  Only a row
/// both sides changed differently is a conflict, marked as git marks one.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;

namespace neverd::cli {
namespace {

/// The address a table row names, "addr" in every table of a project.
std::optional<uint64_t> rowAddress(const json::Value &Row) {
  const json::Object *Object = Row.getAsObject();
  if (!Object)
    return std::nullopt;
  std::optional<StringRef> Text = Object->getString("addr");
  if (!Text)
    return std::nullopt;
  StringRef Digits = *Text;
  uint64_t Address = 0;
  if (!Digits.consume_front_insensitive("0x") || Digits.empty() ||
      Digits.getAsInteger(16, Address))
    return std::nullopt;
  return Address;
}

/// The rows of a table by their addresses; none when \p Value is no table
/// or names an address twice.  An absent file is an empty table.
std::optional<std::map<uint64_t, const json::Value *>>
tableRows(const json::Value *Value) {
  std::map<uint64_t, const json::Value *> Rows;
  if (!Value)
    return Rows;
  const json::Array *Array = Value->getAsArray();
  if (!Array)
    return std::nullopt;
  for (const json::Value &Row : *Array) {
    std::optional<uint64_t> Address = rowAddress(Row);
    if (!Address || !Rows.emplace(*Address, &Row).second)
      return std::nullopt;
  }
  return Rows;
}

bool same(const json::Value *A, const json::Value *B) {
  return A == B || (A && B && *A == *B);
}

/// The three-way merge of a value, absent where null: one side's change
/// over the base, both sides' equal changes, and an object both changed key
/// by key.  Sets \p Conflict where both sides changed a value differently,
/// and then keeps ours.
std::optional<json::Value> mergeValue(const json::Value *Base,
                                      const json::Value *Ours,
                                      const json::Value *Theirs,
                                      bool &Conflict) {
  auto copy = [](const json::Value *Value) -> std::optional<json::Value> {
    return Value ? std::optional<json::Value>(*Value) : std::nullopt;
  };
  if (same(Ours, Theirs) || same(Base, Theirs))
    return copy(Ours);
  if (same(Base, Ours))
    return copy(Theirs);
  const json::Object *OursObject = Ours ? Ours->getAsObject() : nullptr;
  const json::Object *TheirsObject = Theirs ? Theirs->getAsObject() : nullptr;
  const json::Object *BaseObject = Base ? Base->getAsObject() : nullptr;
  if (OursObject && TheirsObject && (!Base || BaseObject)) {
    std::vector<std::string> Keys;
    for (const json::Object *Object : {BaseObject, OursObject, TheirsObject})
      if (Object)
        for (const auto &Entry : *Object)
          Keys.push_back(Entry.first.str());
    llvm::sort(Keys);
    Keys.erase(std::unique(Keys.begin(), Keys.end()), Keys.end());
    json::Object Merged;
    for (const std::string &Key : Keys)
      if (std::optional<json::Value> Field = mergeValue(
              BaseObject ? BaseObject->get(Key) : nullptr, OursObject->get(Key),
              TheirsObject->get(Key), Conflict))
        Merged[Key] = std::move(*Field);
    return json::Value(std::move(Merged));
  }
  Conflict = true;
  return copy(Ours);
}

/// A list without addresses, as the local types' declarations: ours in its
/// order without what theirs removed, then what theirs added.
json::Value mergeList(const json::Array *Base, const json::Array *Ours,
                      const json::Array *Theirs) {
  auto contains = [](const json::Array *Array, const json::Value &Value) {
    return Array && llvm::is_contained(*Array, Value);
  };
  json::Array Merged;
  for (const json::Value &Row : *Ours)
    if (!contains(Base, Row) || contains(Theirs, Row))
      Merged.push_back(Row);
  for (const json::Value &Row : *Theirs)
    if (!contains(Base, Row) && !contains(Ours, Row))
      Merged.push_back(Row);
  return json::Value(std::move(Merged));
}

/// \p Value as the project's files are written: two-space indentation,
/// sorted keys and a final newline.
std::string fileText(const json::Value &Value) {
  return formatv("{0:2}", Value).str() + "\n";
}

/// \p Row as an element of a table: its lines indented one level.
std::string rowText(const json::Value &Row, bool Last) {
  const std::string Lines = formatv("{0:2}", Row).str();
  std::string Text;
  for (StringRef Line : split(Lines, '\n'))
    Text += "  " + Line.str() + "\n";
  if (!Last)
    Text.insert(Text.size() - 1, ",");
  return Text;
}

std::optional<std::string> readText(StringRef Path) {
  auto Buffer = MemoryBuffer::getFile(Path, /*IsText=*/false,
                                      /*RequiresNullTerminator=*/false);
  if (!Buffer)
    return std::nullopt;
  return (*Buffer)->getBuffer().str();
}

/// A file's JSON, null for an empty one (no common ancestor), or an error.
Expected<std::optional<json::Value>> parseFile(StringRef Text) {
  if (Text.trim().empty())
    return std::optional<json::Value>();
  Expected<json::Value> Value = json::parse(Text);
  if (!Value)
    return Value.takeError();
  return std::optional<json::Value>(std::move(*Value));
}

std::string conflictText(StringRef Ours, StringRef Theirs) {
  auto block = [](StringRef Text) {
    std::string Block = Text.str();
    if (!Block.empty() && Block.back() != '\n')
      Block += '\n';
    return Block;
  };
  return "<<<<<<< ours\n" + block(Ours) + "=======\n" + block(Theirs) +
         ">>>>>>> theirs\n";
}

} // namespace

int runMergeProject() {
  if (MergeProjectFiles.size() != 3) {
    WithColor::error() << "merge-project takes <base> <ours> <theirs>\n";
    return 2;
  }
  const std::string &BasePath = MergeProjectFiles[0];
  const std::string &OursPath = MergeProjectFiles[1];
  const std::string &TheirsPath = MergeProjectFiles[2];
  const std::string Name =
      MergeProjectPath.empty() ? OursPath : MergeProjectPath.getValue();
  std::optional<std::string> BaseText = readText(BasePath),
                             OursText = readText(OursPath),
                             TheirsText = readText(TheirsPath);
  if (!BaseText || !OursText || !TheirsText) {
    WithColor::error() << Name << ": cannot read the three versions\n";
    return 2;
  }
  auto write = [&](const std::string &Text) {
    std::error_code Error;
    raw_fd_ostream Out(OursPath, Error);
    if (Error) {
      WithColor::error() << Name << ": " << Error.message() << "\n";
      return false;
    }
    Out << Text;
    return true;
  };
  // Versions that are no JSON, as a file that still holds conflict
  // markers, merge as git merges text: the whole file is the conflict.
  auto Base = parseFile(*BaseText);
  auto Ours = parseFile(*OursText);
  auto Theirs = parseFile(*TheirsText);
  if (!Base || !Ours || !Theirs) {
    consumeError(Base.takeError());
    consumeError(Ours.takeError());
    consumeError(Theirs.takeError());
    errs() << Name << ": a version is not JSON; both are kept as a conflict\n";
    return write(conflictText(*OursText, *TheirsText)) ? 1 : 2;
  }
  const json::Value *BaseValue = *Base ? &**Base : nullptr;
  const json::Value *OursValue = *Ours ? &**Ours : nullptr;
  const json::Value *TheirsValue = *Theirs ? &**Theirs : nullptr;

  auto BaseRows = tableRows(BaseValue), OursRows = tableRows(OursValue),
       TheirsRows = tableRows(TheirsValue);
  if (BaseRows && OursRows && TheirsRows) {
    // A table: row by row, by address.
    struct Merged {
      std::optional<json::Value> Row;
      const json::Value *Ours = nullptr, *Theirs = nullptr;
      bool Conflict = false;
    };
    std::map<uint64_t, Merged> Rows;
    std::vector<uint64_t> Addresses;
    for (const auto *Table : {&*BaseRows, &*OursRows, &*TheirsRows})
      for (const auto &Entry : *Table)
        Addresses.push_back(Entry.first);
    llvm::sort(Addresses);
    Addresses.erase(std::unique(Addresses.begin(), Addresses.end()),
                    Addresses.end());
    std::vector<uint64_t> Conflicts;
    for (uint64_t Address : Addresses) {
      auto rowOf = [&](const std::map<uint64_t, const json::Value *> &Table) {
        auto Found = Table.find(Address);
        return Found == Table.end() ? nullptr : Found->second;
      };
      Merged Entry;
      Entry.Ours = rowOf(*OursRows);
      Entry.Theirs = rowOf(*TheirsRows);
      Entry.Row = mergeValue(rowOf(*BaseRows), Entry.Ours, Entry.Theirs,
                             Entry.Conflict);
      if (Entry.Conflict)
        Conflicts.push_back(Address);
      if (Entry.Row || Entry.Conflict)
        Rows.emplace(Address, std::move(Entry));
    }
    if (Conflicts.empty()) {
      json::Array Table;
      for (auto &[Address, Entry] : Rows)
        Table.push_back(std::move(*Entry.Row));
      return write(fileText(json::Value(std::move(Table)))) ? 0 : 2;
    }
    // Each row both sides changed differently is marked as git marks a
    // conflict, both versions in full; the rest is merged.
    std::string Text = "[\n";
    size_t Index = 0;
    for (auto &[Address, Entry] : Rows) {
      const bool Last = ++Index == Rows.size();
      if (!Entry.Conflict) {
        Text += rowText(*Entry.Row, Last);
        continue;
      }
      Text += "<<<<<<< ours\n";
      if (Entry.Ours)
        Text += rowText(*Entry.Ours, Last);
      Text += "=======\n";
      if (Entry.Theirs)
        Text += rowText(*Entry.Theirs, Last);
      Text += ">>>>>>> theirs\n";
    }
    Text += "]\n";
    errs() << Name << ": " << Conflicts.size()
           << " row(s) both sides changed differently:";
    for (uint64_t Address : Conflicts)
      errs() << " 0x" << utohexstr(Address, /*LowerCase=*/true);
    errs() << "\n";
    return write(Text) ? 1 : 2;
  }

  bool Conflict = false;
  std::optional<json::Value> Merged;
  const json::Array *BaseList = BaseValue ? BaseValue->getAsArray() : nullptr;
  const json::Array *OursList = OursValue ? OursValue->getAsArray() : nullptr;
  const json::Array *TheirsList =
      TheirsValue ? TheirsValue->getAsArray() : nullptr;
  if (OursList && TheirsList && (!BaseValue || BaseList))
    Merged = same(BaseValue, OursValue) ? *TheirsValue
             : same(BaseValue, TheirsValue)
                 ? *OursValue
                 : mergeList(BaseList, OursList, TheirsList);
  else
    Merged = mergeValue(BaseValue, OursValue, TheirsValue, Conflict);
  if (Conflict) {
    errs() << Name << ": both sides changed a value differently\n";
    return write(conflictText(*OursText, *TheirsText)) ? 1 : 2;
  }
  return write(Merged ? fileText(*Merged) : std::string()) ? 0 : 2;
}

int runSetupGit(const char *Argv0) {
  const std::string Folder =
      SetupGitFolder.empty() ? std::string(".") : SetupGitFolder.getValue();
  ErrorOr<std::string> Git = sys::findProgramByName("git");
  if (!Git) {
    WithColor::error() << "git is not on the PATH\n";
    return 1;
  }
  auto git = [&](ArrayRef<StringRef> Arguments, bool Quiet = false) {
    std::vector<StringRef> Command{*Git, "-C", Folder};
    llvm::append_range(Command, Arguments);
    const std::optional<StringRef> Silent[] = {std::nullopt, StringRef(""),
                                               StringRef("")};
    return sys::ExecuteAndWait(*Git, Command, std::nullopt,
                               Quiet
                                   ? ArrayRef(Silent)
                                   : ArrayRef<std::optional<StringRef>>{}) == 0;
  };
  if (!git({"rev-parse", "--git-dir"}, /*Quiet=*/true)) {
    WithColor::error() << Folder << " is not in a git repository\n";
    return 1;
  }
  // Each project folder's .gitattributes asks for the driver; the command is
  // this clone's own setting, as git keeps no command in the files it
  // shares.
  static int Anchor;
  const std::string Self = sys::fs::getMainExecutable(Argv0, &Anchor);
  const std::string Driver =
      "\"" + Self + "\" merge-project %O %A %B --path %P";
  if (!git({"config", "merge.neverd.name", "NeverD project files"}) ||
      !git({"config", "merge.neverd.driver", Driver})) {
    WithColor::error() << "cannot set the merge driver in git's config\n";
    return 1;
  }
  outs() << "git merges NeverD project files with: " << Driver << "\n";
  return 0;
}

} // namespace neverd::cli
