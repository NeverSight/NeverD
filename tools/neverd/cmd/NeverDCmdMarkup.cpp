//===- NeverDCmdMarkup.cpp - User annotation commands --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Handlers for user-authored markup: `bookmarks`, `annotate`, and `rename`.
/// Bookmarks persist to a JSON sidecar next to the binary; annotations and
/// renames persist through the session's C-API store.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/support/ProjectFiles.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

using namespace llvm;

namespace neverd::cli {

int runBookmarks() {
  // The project folder beside the input keeps the bookmarks, as the
  // workbench keeps them; an older NeverD's file beside the input moves in.
  const std::filesystem::path Input = InputFile.getValue();
  project_files::migrate(Input);
  const std::string BmPath =
      project_files::path(Input, project_files::File::Bookmarks).string();

  using Bookmark = std::tuple<uint64_t, std::string, std::string>;

  // By address, in lowercase hexadecimal, as the workbench keeps the same
  // file: one order and one spelling under version control.
  auto toJson = [](ArrayRef<Bookmark> Bms) {
    std::vector<Bookmark> Sorted(Bms.begin(), Bms.end());
    llvm::stable_sort(Sorted, [](const Bookmark &A, const Bookmark &B) {
      return std::get<0>(A) < std::get<0>(B);
    });
    json::Array Arr;
    for (const auto &[A, N, T] : Sorted)
      Arr.push_back(json::Object{
          {"addr", "0x" + utohexstr(A, true)}, {"name", N}, {"note", T}});
    return json::Value(std::move(Arr));
  };

  auto loadBm = [&]() -> std::vector<Bookmark> {
    std::vector<Bookmark> Bms;
    auto BufOrErr = MemoryBuffer::getFile(BmPath);
    if (!BufOrErr)
      return Bms;
    auto Parsed = json::parse((*BufOrErr)->getBuffer());
    if (!Parsed) {
      consumeError(Parsed.takeError());
      return Bms;
    }
    const json::Array *Arr = Parsed->getAsArray();
    if (!Arr)
      return Bms;
    for (const json::Value &V : *Arr) {
      const json::Object *Obj = V.getAsObject();
      if (!Obj)
        continue;
      uint64_t Addr = 0;
      const json::Value *A = Obj->get("addr");
      if (!A)
        continue;
      // Accept both the current "0x..." string form and the legacy
      // numeric form written by older builds.
      if (std::optional<StringRef> S = A->getAsString()) {
        std::optional<uint64_t> ParsedAddr = parseAddrArg(*S);
        if (!ParsedAddr)
          continue;
        Addr = *ParsedAddr;
      } else if (std::optional<double> N = A->getAsNumber()) {
        if (!std::isfinite(*N) || *N < 0.0 || *N >= 18446744073709551616.0 ||
            std::trunc(*N) != *N)
          continue;
        Addr = static_cast<uint64_t>(*N);
      } else {
        continue;
      }
      Bms.emplace_back(Addr, Obj->getString("name").value_or("").str(),
                       Obj->getString("note").value_or("").str());
    }
    return Bms;
  };

  auto saveBm = [&](ArrayRef<Bookmark> Bms) -> bool {
    std::error_code EC = project_files::prepare(Input);
    if (EC) {
      WithColor::error() << "cannot create the project folder: " << EC.message()
                         << "\n";
      return false;
    }
    raw_fd_ostream Out(BmPath, EC);
    if (EC) {
      WithColor::error() << "cannot write: " << EC.message() << "\n";
      return false;
    }
    Out << formatv("{0:2}", toJson(Bms)) << "\n";
    return true;
  };

  if (BookmarkList || (BookmarkAdd.empty() && BookmarkRemove.empty())) {
    auto Bms = loadBm();
    if (JsonOutput) {
      outs() << formatv("{0:2}", toJson(Bms)) << "\n";
    } else {
      outs() << "\nBookmarks (" << Bms.size() << "):\n";
      outs() << format("  %-18s %-30s %s\n", "Address", "Name", "Note");
      outs() << "  " << std::string(60, '-') << "\n";
      for (auto &[A, N, T] : Bms)
        outs() << format("  0x%-16s %-30s %s\n", utohexstr(A).c_str(),
                         N.c_str(), T.c_str());
    }
    return 0;
  }

  if (!BookmarkAdd.empty()) {
    std::optional<uint64_t> Addr = parseAddrArg(BookmarkAdd);
    if (!Addr) {
      WithColor::error() << "invalid bookmark address\n";
      return 1;
    }
    auto Bms = loadBm();
    for (auto &[A, N, T] : Bms) {
      if (A == *Addr) {
        outs() << "Bookmark already exists at 0x" << utohexstr(*Addr) << "\n";
        return 0;
      }
    }

    std::string Name = BookmarkName.empty() ? ("0x" + utohexstr(*Addr))
                                            : BookmarkName.getValue();
    Bms.emplace_back(*Addr, Name, "");
    if (!saveBm(Bms))
      return 1;
    outs() << "Added bookmark: " << Name << " @ 0x" << utohexstr(*Addr) << "\n";
    return 0;
  }

  if (!BookmarkRemove.empty()) {
    std::optional<uint64_t> Addr = parseAddrArg(BookmarkRemove);
    if (!Addr) {
      WithColor::error() << "invalid bookmark address\n";
      return 1;
    }
    auto Bms = loadBm();
    auto It = std::remove_if(Bms.begin(), Bms.end(), [Addr](auto &B) {
      return std::get<0>(B) == *Addr;
    });
    if (It == Bms.end()) {
      outs() << "No bookmark at 0x" << utohexstr(*Addr) << "\n";
      return 0;
    }
    Bms.erase(It, Bms.end());
    if (!saveBm(Bms))
      return 1;
    outs() << "Removed bookmark at 0x" << utohexstr(*Addr) << "\n";
    return 0;
  }

  return 0;
}

int runAnnotate(neverd_session_t Sess) {
  if (AnnotateList || (AnnotateAdd.empty() && AnnotateRemove.empty())) {
    const char *Json = neverd_annotations_json(Sess);
    if (JsonOutput) {
      outs() << (Json ? Json : "[]") << "\n";
    } else {
      outs() << "\nAnnotations:\n";
      outs() << format("  %-18s %s\n", "Address", "Comment");
      outs() << "  " << std::string(60, '-') << "\n";
      size_t Count = 0;
      auto Parsed = json::parse(Json ? Json : "[]");
      if (!Parsed) {
        consumeError(Parsed.takeError());
      } else if (const json::Array *Arr = Parsed->getAsArray()) {
        for (const json::Value &V : *Arr) {
          const json::Object *Obj = V.getAsObject();
          if (!Obj)
            continue;
          outs() << format("  %-18s %s\n",
                           Obj->getString("addr").value_or("").str().c_str(),
                           Obj->getString("text").value_or("").str().c_str());
          ++Count;
        }
      }
      if (Count == 0)
        outs() << "  (none)\n";
    }
    neverd_free_string(Json);
    return 0;
  }

  if (!AnnotateAdd.empty()) {
    std::optional<uint64_t> Addr = parseAddrArg(AnnotateAdd);
    if (!Addr) {
      WithColor::error() << "invalid annotation address\n";
      return 1;
    }
    if (AnnotateText.empty()) {
      WithColor::error() << "--text is required with --add\n";
      return 1;
    }

    neverd_annotation_set(Sess, *Addr, AnnotateText.getValue().c_str());
    if (neverd_annotations_save(Sess) != 0) {
      WithColor::error() << "annotation save failed: " << takeLastError(Sess)
                         << "\n";
      return 1;
    }
    if (!JsonOutput)
      outs() << "Added annotation at 0x" << utohexstr(*Addr) << ": "
             << AnnotateText.getValue() << "\n";
    else {
      json::Object Result;
      Result["addr"] = "0x" + utohexstr(*Addr);
      Result["text"] = AnnotateText.getValue();
      outs() << json::Value(std::move(Result)) << "\n";
    }
    return 0;
  }

  if (!AnnotateRemove.empty()) {
    std::optional<uint64_t> Addr = parseAddrArg(AnnotateRemove);
    if (!Addr) {
      WithColor::error() << "invalid annotation address\n";
      return 1;
    }
    neverd_annotation_remove(Sess, *Addr);
    if (neverd_annotations_save(Sess) != 0) {
      WithColor::error() << "annotation save failed: " << takeLastError(Sess)
                         << "\n";
      return 1;
    }
    if (!JsonOutput)
      outs() << "Removed annotation at 0x" << utohexstr(*Addr) << "\n";
    return 0;
  }

  return 0;
}

int runRename(neverd_session_t Sess) {
  if (RenameList) {
    const char *Json = neverd_renames_json(Sess);
    if (JsonOutput) {
      outs() << (Json ? Json : "[]") << "\n";
    } else {
      auto Parsed = json::parse(Json ? Json : "[]");
      if (Parsed) {
        auto *Arr = Parsed->getAsArray();
        if (Arr && !Arr->empty()) {
          outs() << "\nRenames:\n";
          outs() << format("  %-18s %-30s %s\n", "Address", "Original",
                           "Renamed");
          outs() << "  " << std::string(60, '-') << "\n";
          for (const auto &V : *Arr) {
            auto *Obj = V.getAsObject();
            if (!Obj)
              continue;
            auto Addr = Obj->getString("addr").value_or("");
            auto Orig = Obj->getString("original").value_or("");
            auto Ren = Obj->getString("renamed").value_or("");
            outs() << format("  %-18s %-30s %s\n", std::string(Addr).c_str(),
                             std::string(Orig).c_str(),
                             std::string(Ren).c_str());
          }
        } else {
          outs() << "No renames.\n";
        }
      }
    }
    if (Json)
      neverd_free_string(Json);
  } else if (!RenameAddr.empty()) {
    const std::optional<uint64_t> Addr = parseAddrArg(RenameAddr);
    if (!Addr) {
      WithColor::error() << "invalid rename address\n";
      return 1;
    }
    if (RenameClear == !RenameTo.empty()) {
      WithColor::error() << "rename --addr takes --to <name> or --clear\n";
      return 1;
    }
    if (neverd_rename_addr(Sess, *Addr,
                           RenameClear ? nullptr
                                       : RenameTo.getValue().c_str()) != 0) {
      WithColor::error() << "rename failed: " << takeLastError(Sess) << "\n";
      return 1;
    }
    if (JsonOutput) {
      json::Object Result;
      Result["addr"] = "0x" + utohexstr(*Addr);
      Result["name"] =
          RenameClear ? json::Value(nullptr) : json::Value(RenameTo.getValue());
      outs() << json::Value(std::move(Result)) << "\n";
    } else if (RenameClear) {
      outs() << "Cleared the name of 0x" << utohexstr(*Addr) << "\n";
    } else {
      outs() << "Named 0x" << utohexstr(*Addr) << " " << RenameTo.getValue()
             << "\n";
    }
  } else {
    if (RenameFrom.empty() || RenameTo.empty()) {
      WithColor::error()
          << "rename requires --func <old> --to <new> or --addr <address>\n";
      return 1;
    }
    int Ret = neverd_rename_func(Sess, RenameFrom.getValue().c_str(),
                                 RenameTo.getValue().c_str());
    if (Ret != 0) {
      WithColor::error() << "rename failed: " << takeLastError(Sess) << "\n";
      return 1;
    }
    if (JsonOutput) {
      json::Object Result;
      Result["old"] = RenameFrom.getValue();
      Result["new"] = RenameTo.getValue();
      outs() << json::Value(std::move(Result)) << "\n";
    } else {
      outs() << "Renamed: " << RenameFrom.getValue() << " -> "
             << RenameTo.getValue() << "\n";
    }
  }

  return 0;
}

namespace {
/// The attributes a function edit row states, as "key=value" in
/// FunctionAttributes.def's order.
std::string functionAttributeText(const json::Object &Row) {
  std::string Text;
  const auto Note = [&](StringRef Key) {
    const json::Value *Value = Row.get(Key);
    if (!Value)
      return;
    if (!Text.empty())
      Text += ' ';
    Text += Key;
    Text += '=';
    if (const auto Flag = Value->getAsBoolean())
      Text += *Flag ? "true" : "false";
    else if (const auto Spelled = Value->getAsString())
      Text += *Spelled;
    else
      Text += "default";
  };
#define NEVERD_FUNCTION_FLAG(Member, Key) Note(Key);
#define NEVERD_FUNCTION_COLOR(Key) Note(Key);
#define NEVERD_FUNCTION_TYPE(Member, Key) Note(Key);
#include "neverd/sdk/FunctionAttributes.def"
  return Text;
}

/// State what the function at --edit is by --set's key=value pairs.
int editFunctionAttributes(neverd_session_t Sess, uint64_t Addr) {
  json::Object Row;
  for (StringRef Item : FunctionSet) {
    const auto [Key, Value] = Item.split('=');
    if (Key.empty() || !Item.contains('=')) {
      WithColor::error() << "--set takes key=value, not '" << Item << "'\n";
      return 1;
    }
    if (Value == "true" || Value == "false")
      Row[Key] = Value == "true";
    else if (Value == "default")
      Row[Key] = nullptr;
    else
      Row[Key] = Value;
  }
  const std::string Text = formatv("{0}", json::Value(json::Object(Row)));
  if (neverd_func_edit(Sess, Addr, Text.c_str()) != 0) {
    WithColor::error() << "edit failed: " << takeLastError(Sess) << "\n";
    return 1;
  }
  if (neverd_functions_save(Sess) != 0) {
    WithColor::error() << "function edits save failed: " << takeLastError(Sess)
                       << "\n";
    return 1;
  }
  if (JsonOutput) {
    json::Object Result{{"addr", "0x" + utohexstr(Addr)},
                        {"edited", std::move(Row)}};
    outs() << json::Value(std::move(Result)) << "\n";
  } else {
    outs() << "Edited function at 0x" << utohexstr(Addr) << ": "
           << functionAttributeText(Row) << "\n";
  }
  return 0;
}

/// Print what the Edit function dialog shows of the function at --show.
int showFunctionAttributes(neverd_session_t Sess, uint64_t Addr) {
  const char *Json = neverd_func_attributes_json(Sess, Addr);
  if (!Json) {
    WithColor::error() << takeLastError(Sess) << "\n";
    return 1;
  }
  const std::string Text = Json;
  neverd_free_string(Json);
  if (JsonOutput) {
    outs() << Text << "\n";
    return 0;
  }
  auto Parsed = json::parse(Text);
  const json::Object *Row = Parsed ? Parsed->getAsObject() : nullptr;
  if (!Row) {
    consumeError(Parsed.takeError());
    WithColor::error() << "the engine described the function as invalid JSON\n";
    return 1;
  }
  const json::Object Empty;
  const json::Object *Stated = Row->getObject("stated");
  if (!Stated)
    Stated = &Empty;
  // Each attribute as its value, and whether the user or analysis says so.
  const auto Line = [&](StringRef Label, StringRef Key) {
    const json::Value *Value = Row->get(Key);
    std::string Shown = "unknown";
    if (const auto Flag = Value ? Value->getAsBoolean() : std::nullopt)
      Shown = *Flag ? "yes" : "no";
    else if (const auto Spelled = Value ? Value->getAsString() : std::nullopt)
      Shown = Spelled->str();
    else if (const auto Count = Value ? Value->getAsInteger() : std::nullopt)
      Shown = std::to_string(*Count);
    else if (Key == "color" || Key == "purged_bytes" || Key == "end" ||
             Key == "type")
      Shown = "-";
    if (Stated->get(Key))
      Shown += " (stated)";
    outs() << format("  %-18s %s\n", (Label + ":").str().c_str(),
                     Shown.c_str());
  };
  outs() << "\nFunction " << Row->getString("name").value_or("") << ":\n";
  Line("Start address", "start");
  Line("End address", "end");
  Line("Does not return", "noreturn");
  Line("Library func", "library");
  Line("Static func", "static");
  Line("Color", "color");
  Line("Purged bytes", "purged_bytes");
  Line("Type", "type");
  return 0;
}
/// Show the declaration of the function at --declaration as IDA's Y edits
/// it: its own, the one --check reads, or the one --retype makes, which
/// --apply states as the function's type.
int functionDeclaration(neverd_session_t Sess, uint64_t Addr) {
  json::Object Options;
  if (!FunctionCheck.empty() && !FunctionRetype.empty()) {
    WithColor::error() << "--check and --retype work one at a time\n";
    return 1;
  }
  if (!FunctionCheck.empty())
    Options["declaration"] = FunctionCheck.getValue();
  if (!FunctionRetype.empty()) {
    const auto [Index, Type] = StringRef(FunctionRetype).split('=');
    unsigned Position = 0;
    if (Index.trim().getAsInteger(10, Position) || Type.trim().empty()) {
      WithColor::error() << "--retype takes index=type, such as "
                            "0='struct _KAPC *'\n";
      return 1;
    }
    Options["parameter"] = static_cast<int64_t>(Position);
    Options["type"] = Type.trim().str();
  }
  if (FunctionApply && Options.empty()) {
    WithColor::error() << "--apply states the declaration --check or "
                          "--retype gives\n";
    return 1;
  }
  const std::string Request =
      Options.empty() ? std::string()
                      : formatv("{0}", json::Value(json::Object(Options)));
  const char *Json = neverd_func_type_json(
      Sess, Addr, Request.empty() ? nullptr : Request.c_str());
  if (!Json) {
    WithColor::error() << takeLastError(Sess) << "\n";
    return 1;
  }
  auto Parsed = json::parse(Json);
  neverd_free_string(Json);
  json::Object *Row = Parsed ? Parsed->getAsObject() : nullptr;
  if (!Row) {
    consumeError(Parsed.takeError());
    WithColor::error() << "the engine described the declaration as invalid "
                          "JSON\n";
    return 1;
  }
  const json::Array *Problems = Row->getArray("problems");
  const std::string Declaration =
      Row->getString("declaration").value_or("").str();
  if (FunctionApply) {
    if (Problems && !Problems->empty()) {
      const json::Object *First = (*Problems)[0].getAsObject();
      WithColor::error() << "the declaration does not read: "
                         << First->getString("message").value_or("") << "\n";
      return 1;
    }
    const std::string Edit =
        formatv("{0}", json::Value(json::Object{{"type", Declaration}}));
    if (neverd_func_edit(Sess, Addr, Edit.c_str()) != 0) {
      WithColor::error() << "edit failed: " << takeLastError(Sess) << "\n";
      return 1;
    }
    if (neverd_functions_save(Sess) != 0) {
      WithColor::error() << "function edits save failed: "
                         << takeLastError(Sess) << "\n";
      return 1;
    }
    (*Row)["applied"] = true;
  }
  if (JsonOutput) {
    outs() << json::Value(std::move(*Row)) << "\n";
    return 0;
  }
  const char *Origin = FunctionApply             ? "stated"
                       : !FunctionCheck.empty()  ? "checked"
                       : !FunctionRetype.empty() ? "proposed"
                       : Row->getBoolean("stated").value_or(false)
                           ? "stated"
                           : "as the decompiler prints it";
  outs() << "\nFunction 0x" << utohexstr(Addr) << " (" << Origin << "):\n  "
         << Declaration << "\n";
  if (Problems && !Problems->empty()) {
    for (const json::Value &Value : *Problems)
      if (const json::Object *Problem = Value.getAsObject())
        outs() << "  problem at " << Problem->getInteger("line").value_or(0)
               << ":" << Problem->getInteger("column").value_or(0) << ": "
               << Problem->getString("message").value_or("") << "\n";
    return FunctionCheck.empty() ? 0 : 1;
  }
  if (const json::Array *Parameters = Row->getArray("parameters")) {
    outs() << format("  %-5s %-20s %-28s %s\n", "#", "Name", "Type",
                     "Arrives in");
    size_t Index = 0;
    for (const json::Value &Value : *Parameters)
      if (const json::Object *Param = Value.getAsObject())
        outs() << format(
            "  %-5zu %-20s %-28s %s\n", Index++,
            Param->getString("name").value_or("").str().c_str(),
            Param->getString("type").value_or("").str().c_str(),
            Param->getString("location").value_or("-").str().c_str());
  }
  return 0;
}
} // namespace

int runFunctionEdits(neverd_session_t Sess) {
  const bool Create = !FunctionCreate.empty();
  const bool Delete = !FunctionDelete.empty();
  const bool Edit = !FunctionEdit.empty();
  const bool Show = !FunctionShow.empty();
  const bool Declaration = !FunctionDeclaration.empty();
  if (Create + Delete + Edit + Show + Declaration > 1) {
    WithColor::error() << "--create, --delete, --edit, --show and "
                          "--declaration work one at a time\n";
    return 1;
  }
  if (!Declaration &&
      (!FunctionCheck.empty() || !FunctionRetype.empty() || FunctionApply)) {
    WithColor::error() << "--check, --retype and --apply work with "
                          "--declaration\n";
    return 1;
  }
  if (Edit == FunctionSet.empty()) {
    WithColor::error() << (Edit ? "--edit needs the attributes --set states\n"
                                : "--set needs the function --edit names\n");
    return 1;
  }
  if (Create || Delete || Edit || Show || Declaration) {
    std::optional<uint64_t> Addr =
        parseAddrArg(Create        ? FunctionCreate
                     : Delete      ? FunctionDelete
                     : Edit        ? FunctionEdit
                     : Declaration ? FunctionDeclaration
                                   : FunctionShow);
    if (!Addr) {
      WithColor::error() << "invalid function address\n";
      return 1;
    }
    // The symbol view of a stripped image lists no detected function; look
    // further before refusing to edit one, as the workbench lists them.
    if (!Create && neverd_func_find_by_addr(Sess, *Addr) < 0 &&
        neverd_session_discover_functions(Sess) < 0) {
      WithColor::error() << "function discovery failed: " << takeLastError(Sess)
                         << "\n";
      return 1;
    }
    if (Edit)
      return editFunctionAttributes(Sess, *Addr);
    if (Show)
      return showFunctionAttributes(Sess, *Addr);
    if (Declaration)
      return functionDeclaration(Sess, *Addr);
    if ((Create ? neverd_func_create : neverd_func_delete)(Sess, *Addr) != 0) {
      WithColor::error() << (Create ? "create" : "delete")
                         << " failed: " << takeLastError(Sess) << "\n";
      return 1;
    }
    if (neverd_functions_save(Sess) != 0) {
      WithColor::error() << "function edits save failed: "
                         << takeLastError(Sess) << "\n";
      return 1;
    }
    if (JsonOutput) {
      json::Object Result;
      Result["addr"] = "0x" + utohexstr(*Addr);
      Result["created"] = Create;
      outs() << json::Value(std::move(Result)) << "\n";
    } else {
      outs() << (Create ? "Created function at 0x" : "Deleted function at 0x")
             << utohexstr(*Addr) << "\n";
    }
    return 0;
  }

  const char *Json = neverd_functions_json(Sess);
  if (JsonOutput) {
    outs() << (Json ? Json : "[]") << "\n";
  } else {
    outs() << "\nFunction edits:\n";
    outs() << format("  %-18s %s\n", "Address", "Edit");
    outs() << "  " << std::string(30, '-') << "\n";
    size_t Count = 0;
    auto Parsed = json::parse(Json ? Json : "[]");
    if (!Parsed) {
      consumeError(Parsed.takeError());
    } else if (const json::Array *Arr = Parsed->getAsArray()) {
      for (const json::Value &V : *Arr) {
        const json::Object *Obj = V.getAsObject();
        if (!Obj)
          continue;
        std::string Edit = Obj->getString("state").value_or("").str();
        if (const std::string Attributes = functionAttributeText(*Obj);
            !Attributes.empty())
          Edit += (Edit.empty() ? "" : " ") + Attributes;
        outs() << format("  %-18s %s\n",
                         Obj->getString("addr").value_or("").str().c_str(),
                         Edit.c_str());
        ++Count;
      }
    }
    if (Count == 0)
      outs() << "  (none)\n";
  }
  neverd_free_string(Json);
  return 0;
}

namespace {
#define NEVERD_DATA_ITEM_KIND(Id, Spelling)                                    \
  constexpr StringLiteral Id(Spelling);
#include "neverd/DataNames.def"

/// The kind of a value item \p Bytes wide: its size keyword, for the widths
/// a value item has (1, 2, 4 or 8); empty otherwise.
StringRef valueItemKind(unsigned Bytes) {
  if (Bytes > 8 || (Bytes & (Bytes - 1)) != 0)
    return {};
#define NEVERD_DATA_SIZE_NAME(SizeKeyword, Size, Prefix)                       \
  if (Bytes == Size)                                                           \
    return SizeKeyword;
#include "neverd/DataNames.def"
  return {};
}

/// The row that makes the bytes at \p Addr the item the options ask for, or
/// an error message.
Expected<json::Object> itemRow(neverd_session_t Sess, uint64_t Addr) {
  if (!ItemData.empty()) {
    const StringRef Kind = valueItemKind(ItemSize);
    if (Kind.empty())
      return createStringError(inconvertibleErrorCode(),
                               "a value is 1, 2, 4 or 8 bytes");
    return json::Object{{"kind", Kind}};
  }
  if (!ItemUndefine.empty()) {
    if (!ItemSize)
      return createStringError(inconvertibleErrorCode(),
                               "undefined bytes are at least one byte");
    return json::Object{{"kind", UndefinedItemKind},
                        {"size", static_cast<int64_t>(ItemSize)}};
  }
  // The string the scan reads there, from its first character.
  std::string Options;
  if (!ItemEncoding.empty())
    Options = formatv("{0}", json::Value(json::Object{
                                 {"encodings", json::Array{ItemEncoding}}}));
  const char *Found =
      neverd_string_at(Sess, Addr, Options.empty() ? nullptr : Options.c_str());
  if (!Found)
    return createStringError(inconvertibleErrorCode(),
                             "no string starts at 0x" + utohexstr(Addr) + ": " +
                                 takeLastError(Sess));
  auto Parsed = json::parse(Found);
  neverd_free_string(Found);
  const json::Object *String = Parsed ? Parsed->getAsObject() : nullptr;
  if (!String) {
    if (!Parsed)
      consumeError(Parsed.takeError());
    return createStringError(inconvertibleErrorCode(),
                             "the engine read no string");
  }
  return json::Object{{"kind", StringItemKind},
                      {"encoding", String->getString("encoding").value_or("")},
                      {"size", String->getInteger("length").value_or(0) +
                                   String->getInteger("unit").value_or(0)}};
}
} // namespace

int runItems(neverd_session_t Sess) {
  const std::string *Edits[] = {&ItemData.getValue(), &ItemString.getValue(),
                                &ItemUndefine.getValue(),
                                &ItemClear.getValue()};
  const auto Chosen =
      llvm::count_if(Edits, [](const std::string *E) { return !E->empty(); });
  if (Chosen > 1) {
    WithColor::error()
        << "--data, --string, --undefine and --clear edit one item at a time\n";
    return 1;
  }
  if (Chosen == 1) {
    const auto *Text =
        *llvm::find_if(Edits, [](const std::string *E) { return !E->empty(); });
    const std::optional<uint64_t> Addr = parseAddrArg(*Text);
    if (!Addr) {
      WithColor::error() << "invalid item address\n";
      return 1;
    }
    if (!ItemClear.empty()) {
      if (neverd_item_clear(Sess, *Addr) != 0) {
        WithColor::error() << "clear failed: " << takeLastError(Sess) << "\n";
        return 1;
      }
    } else {
      auto Row = itemRow(Sess, *Addr);
      if (!Row) {
        WithColor::error() << toString(Row.takeError()) << "\n";
        return 1;
      }
      const std::string Text = formatv("{0}", json::Value(std::move(*Row)));
      if (neverd_item_set(Sess, *Addr, Text.c_str()) != 0) {
        WithColor::error() << "define failed: " << takeLastError(Sess) << "\n";
        return 1;
      }
    }
    if (neverd_items_save(Sess) != 0) {
      WithColor::error() << "data items save failed: " << takeLastError(Sess)
                         << "\n";
      return 1;
    }
    if (!JsonOutput)
      outs() << (ItemClear.empty() ? "Defined the item at 0x"
                                   : "Cleared the item at 0x")
             << utohexstr(*Addr) << "\n";
  }
  // The items, after any edit.
  const char *Json = neverd_items_json(Sess);
  if (JsonOutput) {
    outs() << (Json ? Json : "[]") << "\n";
  } else {
    outs() << "\nData items:\n";
    outs() << format("  %-18s %-10s %8s  %s\n", "Address", "Kind", "Size",
                     "Encoding");
    outs() << "  " << std::string(50, '-') << "\n";
    size_t Count = 0;
    auto Parsed = json::parse(Json ? Json : "[]");
    if (!Parsed) {
      consumeError(Parsed.takeError());
    } else if (const json::Array *Arr = Parsed->getAsArray()) {
      for (const json::Value &V : *Arr) {
        const json::Object *Obj = V.getAsObject();
        if (!Obj)
          continue;
        outs() << format(
            "  %-18s %-10s %8lld  %s\n",
            Obj->getString("addr").value_or("").str().c_str(),
            Obj->getString("kind").value_or("").str().c_str(),
            static_cast<long long>(Obj->getInteger("size").value_or(0)),
            Obj->getString("encoding").value_or("").str().c_str());
        ++Count;
      }
    }
    if (Count == 0)
      outs() << "  (none)\n";
  }
  neverd_free_string(Json);
  return 0;
}

int runOperands(neverd_session_t Sess) {
  if (!OperandAddr.empty()) {
    const std::optional<uint64_t> Addr = parseAddrArg(OperandAddr);
    if (!Addr) {
      WithColor::error() << "invalid instruction address\n";
      return 1;
    }
    std::string Format;
    if (!OperandStruct.empty() && OperandBase.getValue() != "number" &&
        OperandBase.getValue() != "struct") {
      WithColor::error() << "--struct shows a structure offset; leave out "
                            "--base\n";
      return 1;
    }
    if (OperandStruct.empty() && OperandDelta != 0) {
      WithColor::error() << "--delta goes with --struct\n";
      return 1;
    }
    if (!OperandClear) {
      json::Object Object{{"base", OperandStruct.empty()
                                       ? OperandBase.getValue()
                                       : std::string("struct")},
                          {"negate", OperandNegate.getValue()},
                          {"invert", OperandInvert.getValue()}};
      if (!OperandStruct.empty()) {
        Object["type"] = OperandStruct.getValue();
        Object["delta"] = static_cast<int64_t>(OperandDelta.getValue());
      }
      Format = formatv("{0}", json::Value(std::move(Object))).str();
    }
    if (neverd_operand_format_set(Sess, *Addr, static_cast<int>(OperandIndex),
                                  OperandClear ? nullptr : Format.c_str()) !=
        0) {
      WithColor::error() << "format failed: " << takeLastError(Sess) << "\n";
      return 1;
    }
    if (neverd_operand_formats_save(Sess) != 0) {
      WithColor::error() << "operand formats save failed: "
                         << takeLastError(Sess) << "\n";
      return 1;
    }
    if (!JsonOutput)
      outs() << (OperandClear ? "Cleared operand " : "Formatted operand ")
             << OperandIndex << " at 0x" << utohexstr(*Addr) << "\n";
  }
  // The formats, after any edit.
  const char *Json = neverd_operand_formats_json(Sess);
  if (JsonOutput) {
    outs() << (Json ? Json : "[]") << "\n";
  } else {
    outs() << "\nOperand formats:\n";
    outs() << format("  %-18s %-8s %-10s %s\n", "Address", "Operand", "Base",
                     "Changes");
    outs() << "  " << std::string(50, '-') << "\n";
    size_t Count = 0;
    auto Parsed = json::parse(Json ? Json : "[]");
    if (!Parsed) {
      consumeError(Parsed.takeError());
    } else if (const json::Array *Rows = Parsed->getAsArray()) {
      for (const json::Value &Row : *Rows) {
        const json::Object *Object = Row.getAsObject();
        const json::Array *Operands =
            Object ? Object->getArray("operands") : nullptr;
        if (!Operands)
          continue;
        for (const json::Value &Entry : *Operands) {
          const json::Object *Operand = Entry.getAsObject();
          if (!Operand)
            continue;
          std::string Changes;
          if (Operand->getBoolean("negate").value_or(false))
            Changes += "sign ";
          if (Operand->getBoolean("invert").value_or(false))
            Changes += "bits ";
          if (std::optional<StringRef> Type = Operand->getString("type")) {
            Changes += Type->str();
            if (const int64_t Delta = Operand->getInteger("delta").value_or(0))
              Changes += ", delta " + std::to_string(Delta);
          }
          outs() << format(
              "  %-18s %-8lld %-10s %s\n",
              Object->getString("addr").value_or("").str().c_str(),
              static_cast<long long>(
                  Operand->getInteger("operand").value_or(0)),
              Operand->getString("base").value_or("").str().c_str(),
              Changes.c_str());
          ++Count;
        }
      }
    }
    if (Count == 0)
      outs() << "  (none)\n";
  }
  neverd_free_string(Json);
  return 0;
}

} // namespace neverd::cli
