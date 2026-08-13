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
  std::string BmPath =
      std::filesystem::path(InputFile.getValue()).string() +
      ".neverd-bookmarks.json";

  using Bookmark = std::tuple<uint64_t, std::string, std::string>;

  auto toJson = [](ArrayRef<Bookmark> Bms) {
    json::Array Arr;
    for (const auto &[A, N, T] : Bms)
      Arr.push_back(json::Object{
          {"addr", "0x" + utohexstr(A)}, {"name", N}, {"note", T}});
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
        if (!std::isfinite(*N) || *N < 0.0 ||
            *N >= 18446744073709551616.0 || std::trunc(*N) != *N)
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
    std::error_code EC;
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
    outs() << "Added bookmark: " << Name << " @ 0x" << utohexstr(*Addr)
           << "\n";
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
    neverd_annotations_save(Sess);
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
    neverd_annotations_save(Sess);
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
  } else {
    if (RenameFrom.empty() || RenameTo.empty()) {
      WithColor::error() << "rename requires --func <old> --to <new>\n";
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

} // namespace neverd::cli
