//===- NeverDCmdSearch.cpp - Search and signature commands ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Handlers for locating content in the binary: `search` (text or hex byte
/// pattern) and `sigs` (FLIRT signature matching from a directory, a single
/// file, or auto-detection relative to the executable).
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"
#include "../ResourceLimits.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace llvm;

namespace neverd::cli {
namespace {
cl::opt<std::string> SearchImmediate(
    "immediate",
    cl::desc("Search logical native immediates (signed decimal or 0x hex)"),
    cl::init(""), cl::sub(SearchCmd));
cl::opt<std::string> SearchFrom("from",
                                cl::desc("Exclusive search cursor (hex)"),
                                cl::init(""), cl::sub(SearchCmd));
cl::opt<bool> SearchBackward("backward",
                             cl::desc("Search in descending address order"),
                             cl::sub(SearchCmd));
} // namespace

int runSearch(neverd_session_t Sess) {
  if (!SearchImmediate.empty()) {
    if (!SearchHex.empty() || !SearchText.empty() || SearchMaxResults < 1 ||
        SearchMaxResults >
            ::neverd::limits::cli::kNeverDCmdSearchRunSearchMaxResultsLimit) {
      WithColor::error()
          << "--immediate requires one pattern and --max-results in 1..4096\n";
      return 1;
    }
    json::Object Request{{"kind", "immediate"},
                         {"pattern", SearchImmediate.getValue()},
                         {"limit", SearchMaxResults.getValue()},
                         {"backward", bool(SearchBackward)}};
    if (!SearchFrom.empty()) {
      const auto From = parseAddrArg(SearchFrom);
      if (!From) {
        WithColor::error() << "invalid --from address\n";
        return 1;
      }
      Request["from"] = "0x" + utohexstr(*From);
    }
    std::string Text;
    raw_string_ostream(Text) << json::Value(std::move(Request));
    // One SDK page builds bounded chunks; continue without changing its cursor.
    for (;;) {
      const auto *Reply = neverd_search_page(Sess, Text.c_str());
      if (!Reply) {
        WithColor::error() << takeLastError(Sess) << "\n";
        return 1;
      }
      auto Parsed = json::parse(Reply);
      neverd_free_string(Reply);
      if (!Parsed) {
        WithColor::error() << toString(Parsed.takeError()) << "\n";
        return 1;
      }
      if (Parsed->getAsObject()->getBoolean("building").value_or(false))
        continue;
      outs() << *Parsed << "\n";
      return 0;
    }
  }
  int MaxRes = static_cast<int>(SearchMaxResults.getValue());

  if (!SearchHex.empty()) {
    std::string HexStr;
    for (char Ch : SearchHex.getValue())
      if (!std::isspace(static_cast<unsigned char>(Ch)))
        HexStr += Ch;
    if (HexStr.empty() || HexStr.size() % 2 != 0) {
      WithColor::error()
          << "hex pattern must contain one or more complete bytes\n";
      return 1;
    }

    std::vector<uint8_t> Bytes;
    for (size_t I = 0; I < HexStr.size(); I += 2) {
      std::string Pair = HexStr.substr(I, 2);
      if (!std::isxdigit(static_cast<unsigned char>(Pair[0])) ||
          !std::isxdigit(static_cast<unsigned char>(Pair[1]))) {
        WithColor::error() << "invalid hex byte: " << Pair << "\n";
        return 1;
      }
      char *End = nullptr;
      unsigned long B = std::strtoul(Pair.c_str(), &End, 16);
      if (End != Pair.c_str() + 2) {
        WithColor::error() << "invalid hex byte: " << Pair << "\n";
        return 1;
      }
      Bytes.push_back(static_cast<uint8_t>(B));
    }

    const char *Json = neverd_search_bytes(
        Sess, Bytes.data(), static_cast<int>(Bytes.size()), MaxRes);
    if (JsonOutput) {
      outs() << (Json ? Json : "[]") << "\n";
    } else {
      auto Parsed = json::parse(Json ? Json : "[]");
      if (Parsed) {
        auto *Arr = Parsed->getAsArray();
        if (Arr && !Arr->empty()) {
          outs() << "\nHex pattern matches (" << Arr->size() << " hits):\n";
          outs() << format("  %-18s %s\n", "Address", "Segment");
          outs() << "  " << std::string(40, '-') << "\n";
          for (const auto &V : *Arr) {
            auto *Obj = V.getAsObject();
            if (!Obj)
              continue;
            outs() << format(
                "  %-18s %s\n",
                std::string(Obj->getString("addr").value_or("")).c_str(),
                std::string(Obj->getString("segment").value_or("")).c_str());
          }
        } else {
          outs() << "No matches found.\n";
        }
      }
    }
    if (Json)
      neverd_free_string(Json);
  } else if (!SearchText.empty()) {
    const char *Json =
        neverd_search_string(Sess, SearchText.getValue().c_str(),
                             SearchCaseSensitive ? 1 : 0, MaxRes);
    if (JsonOutput) {
      outs() << (Json ? Json : "[]") << "\n";
    } else {
      auto Parsed = json::parse(Json ? Json : "[]");
      if (Parsed) {
        auto *Arr = Parsed->getAsArray();
        if (Arr && !Arr->empty()) {
          outs() << "\nString matches (" << Arr->size() << " hits):\n";
          outs() << format("  %-18s %-12s %s\n", "Address", "Segment",
                           "Context");
          outs() << "  " << std::string(60, '-') << "\n";
          for (const auto &V : *Arr) {
            auto *Obj = V.getAsObject();
            if (!Obj)
              continue;
            outs() << format(
                "  %-18s %-12s %s\n",
                std::string(Obj->getString("addr").value_or("")).c_str(),
                std::string(Obj->getString("segment").value_or("")).c_str(),
                std::string(Obj->getString("context").value_or("")).c_str());
          }
        } else {
          outs() << "No matches found.\n";
        }
      }
    }
    if (Json)
      neverd_free_string(Json);
  } else {
    WithColor::error() << "search requires --text <pattern> or --hex <bytes>\n";
    return 1;
  }

  return 0;
}

Expected<int> applyRequestedSignatures(neverd_session_t Sess,
                                       const char *Argv0) {
  int MatchCount = -1;
  if (SigAuto || !SigBase.getValue().empty()) {
    // --sig-base names the tree; otherwise look for signatures/ next to the
    // neverd executable, then in the working directory.
    std::filesystem::path Base = SigBase.getValue();
    if (Base.empty()) {
      std::error_code EC;
      auto ExeDir = std::filesystem::canonical(std::filesystem::path(Argv0), EC)
                        .parent_path();
      Base = ExeDir / "signatures";
      if (!std::filesystem::exists(Base))
        Base = std::filesystem::current_path() / "signatures";
    }
    MatchCount = neverd_auto_apply_signatures(Sess, Base.string().c_str());
  } else if (!SigFile.getValue().empty()) {
    MatchCount = neverd_apply_signature_file(Sess, SigFile.getValue().c_str());
  } else if (!SigDir.getValue().empty()) {
    MatchCount = neverd_apply_signatures(Sess, SigDir.getValue().c_str());
  } else {
    return createStringError(inconvertibleErrorCode(),
                             "specify --auto, --sig-base <directory>, "
                             "--sig-dir <directory>, or --sig-file <file>");
  }

  if (MatchCount < 0)
    return createStringError(inconvertibleErrorCode(),
                             "signature matching failed: %s",
                             takeLastError(Sess).c_str());
  return MatchCount;
}

int runSigs(neverd_session_t Sess, const char *Argv0) {
  Expected<int> MatchCount = applyRequestedSignatures(Sess, Argv0);
  if (!MatchCount) {
    WithColor::error() << toString(MatchCount.takeError()) << "\n";
    return 1;
  }

  const char *Json = neverd_sig_matches_json(Sess);
  if (JsonOutput) {
    outs() << (Json ? Json : "[]") << "\n";
  } else {
    const char *Headers = neverd_headers_json(Sess);
    auto HeaderValue = json::parse(Headers ? Headers : "{}");
    neverd_free_string(Headers);
    if (HeaderValue) {
      if (const auto *Root = HeaderValue->getAsObject())
        if (const auto *Selection = Root->getObject("signature_selection"))
          outs() << "Signature packs: "
                 << Selection->getInteger("selected_files").value_or(0)
                 << " selected, "
                 << Selection->getInteger("skipped_files").value_or(0)
                 << " skipped by target metadata\n";
    } else {
      consumeError(HeaderValue.takeError());
    }
    outs() << format("Found %d signature matches:\n\n", *MatchCount);
    auto Parsed = json::parse(Json ? Json : "[]");
    if (Parsed) {
      if (auto *Arr = Parsed->getAsArray()) {
        outs() << format("  %-18s %-40s %s\n", "Address", "Name", "Library");
        outs() << format("  %-18s %-40s %s\n", "-------", "----", "-------");
        for (const auto &Item : *Arr) {
          if (auto *Obj = Item.getAsObject()) {
            auto Addr = Obj->getString("addr").value_or("");
            auto Name = Obj->getString("name").value_or("");
            auto Lib = Obj->getString("library").value_or("");
            outs() << format("  %-18s %-40s %s\n", Addr.str().c_str(),
                             Name.str().c_str(), Lib.str().c_str());
          }
        }
      }
    }
  }
  if (Json)
    neverd_free_string(Json);
  return 0;
}

} // namespace neverd::cli
