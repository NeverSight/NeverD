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

int runSearch(neverd_session_t Sess) {
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
  if (SigAuto) {
    // Look for signatures/ next to the neverd executable, then cwd.
    std::error_code EC;
    auto ExeDir = std::filesystem::canonical(std::filesystem::path(Argv0), EC)
                      .parent_path();
    auto SigBase = ExeDir / "signatures";
    if (!std::filesystem::exists(SigBase))
      SigBase = std::filesystem::current_path() / "signatures";
    MatchCount = neverd_auto_apply_signatures(Sess, SigBase.string().c_str());
  } else if (!SigFile.getValue().empty()) {
    MatchCount = neverd_apply_signature_file(Sess, SigFile.getValue().c_str());
  } else if (!SigDir.getValue().empty()) {
    MatchCount = neverd_apply_signatures(Sess, SigDir.getValue().c_str());
  } else {
    return createStringError(inconvertibleErrorCode(),
                             "specify --auto, --sig-dir <directory>, or "
                             "--sig-file <file>");
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
