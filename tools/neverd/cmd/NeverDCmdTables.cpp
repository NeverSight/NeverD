//===- NeverDCmdTables.cpp - Flat listing commands -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Handlers that print a single flat table backed by a *_json C-API call:
/// `imports`, `exports`, `segments`, `sections`, `symbols`, `relocs`,
/// `entrypoints`, `switches`, and `strings`.  With --json the raw blob is
/// echoed; otherwise it is formatted as an aligned table.
///
//===----------------------------------------------------------------------===//

#include "../ExportTable.h"
#include "../NeverDCLI.h"
#include "../ResourceLimits.h"

#include "neverd/support/CaseList.h"

#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <map>

using namespace llvm;

namespace neverd::cli {

int runImports(neverd_session_t Sess) {
  const char *Json = neverd_imports_json(Sess);
  if (JsonOutput) {
    outs() << (Json ? Json : "[]") << "\n";
  } else {
    auto Parsed = json::parse(Json ? Json : "[]");
    if (Parsed) {
      if (auto *Arr = Parsed->getAsArray()) {
        outs() << "\nImports (" << Arr->size() << "):\n";
        outs() << format("  %-20s %-30s %-8s %s\n", "Module", "Name", "Ordinal",
                         "IAT Addr");
        outs() << "  " << std::string(72, '-') << "\n";
        for (const auto &V : *Arr) {
          auto *Obj = V.getAsObject();
          if (!Obj)
            continue;
          outs() << format(
              "  %-20s %-30s %-8lld %s\n",
              std::string(Obj->getString("module").value_or("")).c_str(),
              std::string(Obj->getString("name").value_or("")).c_str(),
              Obj->getInteger("ordinal").value_or(0),
              std::string(Obj->getString("iat_addr").value_or("")).c_str());
        }
      }
    }
  }
  neverd_free_string(Json);
  return 0;
}

int runExports(neverd_session_t Sess) {
  const char *Json = neverd_exports_json(Sess);
  int Status = 0;
  if (JsonOutput) {
    outs() << (Json ? Json : "[]") << "\n";
  } else if (auto Err = printExportTable(Json ? Json : "[]", outs())) {
    WithColor::error(errs(), "exports") << toString(std::move(Err)) << '\n';
    Status = 1;
  }
  neverd_free_string(Json);
  return Status;
}

int runSegments(neverd_session_t Sess) {
  const char *Json = neverd_segments_json(Sess);
  if (JsonOutput) {
    outs() << (Json ? Json : "[]") << "\n";
  } else {
    auto Parsed = json::parse(Json ? Json : "[]");
    if (Parsed) {
      if (auto *Arr = Parsed->getAsArray()) {
        outs() << "\nSegments (" << Arr->size() << "):\n";
        outs() << format("  %-20s %-18s %-12s %s\n", "Name", "VA", "Size",
                         "Flags");
        outs() << "  " << std::string(60, '-') << "\n";
        for (const auto &V : *Arr) {
          auto *Obj = V.getAsObject();
          if (!Obj)
            continue;
          outs() << format(
              "  %-20s %-18s %-12s %s\n",
              std::string(Obj->getString("name").value_or("")).c_str(),
              std::string(Obj->getString("va").value_or("")).c_str(),
              std::string(Obj->getString("size").value_or("")).c_str(),
              std::string(Obj->getString("flags").value_or("")).c_str());
        }
      }
    }
  }
  neverd_free_string(Json);
  return 0;
}

int runSections(neverd_session_t Sess) {
  const char *Json = neverd_sections_json(Sess);
  if (JsonOutput) {
    outs() << (Json ? Json : "[]") << "\n";
  } else {
    auto Parsed = json::parse(Json ? Json : "[]");
    if (Parsed) {
      auto *Arr = Parsed->getAsArray();
      if (Arr && !Arr->empty()) {
        outs() << format("  %-20s %-12s %-18s %-10s %-10s %-10s %s\n", "Name",
                         "Segment", "VA", "Size", "FileOff", "FileSz", "Flags");
        outs() << "  " << std::string(90, '-') << "\n";
        for (const auto &V : *Arr) {
          auto *Obj = V.getAsObject();
          if (!Obj)
            continue;
          outs() << format(
              "  %-20s %-12s %-18s %-10lld %-10lld %-10lld %s\n",
              std::string(Obj->getString("name").value_or("")).c_str(),
              std::string(Obj->getString("segment").value_or("")).c_str(),
              std::string(Obj->getString("va").value_or("")).c_str(),
              Obj->getInteger("size").value_or(0),
              Obj->getInteger("file_off").value_or(0),
              Obj->getInteger("file_sz").value_or(0),
              std::string(Obj->getString("flags").value_or("")).c_str());
        }
        outs() << "\n  " << Arr->size() << " sections.\n";
      }
    }
  }
  if (Json)
    neverd_free_string(Json);
  return 0;
}

int runSymbols(neverd_session_t Sess) {
  const char *Json = neverd_symbols_json(Sess);
  if (JsonOutput) {
    outs() << (Json ? Json : "[]") << "\n";
  } else {
    auto Parsed = json::parse(Json ? Json : "[]");
    if (Parsed) {
      auto *Arr = Parsed->getAsArray();
      if (Arr && !Arr->empty()) {
        outs() << format("  %-18s %-10s %s\n", "Address", "Size", "Name");
        outs() << "  " << std::string(60, '-') << "\n";
        for (const auto &V : *Arr) {
          auto *Obj = V.getAsObject();
          if (!Obj)
            continue;
          outs() << format(
              "  %-18s %-10lld %s\n",
              std::string(Obj->getString("addr").value_or("")).c_str(),
              Obj->getInteger("size").value_or(0),
              std::string(Obj->getString("name").value_or("")).c_str());
        }
        outs() << "\n  " << Arr->size() << " symbols.\n";
      }
    }
  }
  if (Json)
    neverd_free_string(Json);
  return 0;
}

int runRelocs(neverd_session_t Sess) {
  const char *Json = neverd_relocs_json(Sess);
  if (JsonOutput) {
    outs() << (Json ? Json : "[]") << "\n";
  } else {
    auto Parsed = json::parse(Json ? Json : "[]");
    if (Parsed) {
      auto *Arr = Parsed->getAsArray();
      if (Arr && !Arr->empty()) {
        outs() << format("  %-18s %-6s %-20s %-20s %s\n", "Address", "Type",
                         "Symbol", "Section", "Addend");
        outs() << "  " << std::string(80, '-') << "\n";
        for (const auto &V : *Arr) {
          auto *Obj = V.getAsObject();
          if (!Obj)
            continue;
          outs() << format(
              "  %-18s %-6lld %-20s %-20s %lld\n",
              std::string(Obj->getString("addr").value_or("")).c_str(),
              Obj->getInteger("type").value_or(0),
              std::string(Obj->getString("symbol").value_or("")).c_str(),
              std::string(Obj->getString("section").value_or("")).c_str(),
              Obj->getInteger("addend").value_or(0));
        }
        outs() << "\n  " << Arr->size() << " relocations.\n";
      } else {
        outs() << "No relocations.\n";
      }
    }
  }
  if (Json)
    neverd_free_string(Json);
  return 0;
}

int runEntryPoints(neverd_session_t Sess) {
  const char *Json = neverd_entrypoints_json(Sess);
  if (JsonOutput) {
    outs() << (Json ? Json : "[]") << "\n";
  } else {
    auto Parsed = json::parse(Json ? Json : "[]");
    if (Parsed) {
      auto *Arr = Parsed->getAsArray();
      if (Arr) {
        outs() << "\nEntry Points:\n";
        outs() << format("  %-14s %-18s %s\n", "Type", "Address", "Name");
        outs() << format("  %-14s %-18s %s\n", "----", "-------", "----");
        for (const auto &V : *Arr) {
          auto *Obj = V.getAsObject();
          if (!Obj)
            continue;
          auto Type = Obj->getString("type").value_or("");
          auto Addr = Obj->getString("addr").value_or("");
          auto Name = Obj->getString("name").value_or("");
          outs() << format("  %-14s %-18s %s\n", Type.str().c_str(),
                           Addr.str().c_str(), Name.str().c_str());
        }
        outs() << "\n" << Arr->size() << " entry point(s)\n";
      }
    }
  }
  if (Json)
    neverd_free_string(Json);
  return 0;
}

int runSwitches(neverd_session_t Sess) {
  if (!neverd_session_analyze(Sess)) {
    WithColor::error() << "analysis failed: " << takeLastError(Sess) << "\n";
    return 1;
  }
  json::Array Switches;
  for (std::optional<neverd_va_t> Cursor = 0; Cursor;) {
    const char *Raw = neverd_switches_json(
        Sess, *Cursor, ::neverd::limits::cli::kNeverDCmdTablesFunctionsPerPage);
    if (!Raw) {
      WithColor::error() << "switches: " << takeLastError(Sess) << "\n";
      return 1;
    }
    auto Page = json::parse(Raw);
    neverd_free_string(Raw);
    const json::Object *Object = Page ? Page->getAsObject() : nullptr;
    if (!Object) {
      if (!Page)
        consumeError(Page.takeError());
      WithColor::error() << "switches: the engine returned invalid JSON\n";
      return 1;
    }
    if (const json::Array *Rows = Object->getArray("switches"))
      for (const json::Value &Row : *Rows)
        Switches.push_back(Row);
    Cursor.reset();
    if (const auto Next = Object->getString("next_entry")) {
      neverd_va_t Entry = 0;
      if (!parseHexAddress(Next->str(), Entry)) {
        WithColor::error() << "switches: the engine returned an invalid "
                              "next entry\n";
        return 1;
      }
      Cursor = Entry;
    }
  }
  if (JsonOutput) {
    outs() << json::Value(json::Object{{"switches", std::move(Switches)}})
           << "\n";
    return 0;
  }
  outs() << "\nSwitches (" << Switches.size() << "):\n";
  for (const json::Value &Row : Switches) {
    const json::Object *Switch = Row.getAsObject();
    if (!Switch)
      continue;
    const auto text = [&](StringRef Key) {
      return Switch->getString(Key).value_or("-").str();
    };
    const json::Array *Targets = Switch->getArray("targets");
    outs() << format("  jump %-18s table %-18s %4zu cases  %-14s function %s\n",
                     text("jump").c_str(), text("table").c_str(),
                     Targets ? Targets->size() : size_t(0),
                     text("form").c_str(), text("function").c_str());
    if (!Targets)
      continue;
    // Each target with the table indexes that reach it, in target order.
    std::map<std::string, std::vector<int64_t>> Indexes;
    for (const json::Value &Target : *Targets)
      if (const json::Array *Fields = Target.getAsArray();
          Fields && Fields->size() == 3)
        if (const auto To = (*Fields)[0].getAsString())
          if (const auto Index = (*Fields)[1].getAsInteger())
            Indexes[To->str()].push_back(*Index);
    for (const auto &[To, Values] : Indexes)
      outs() << format("    %-18s %s %s\n", To.c_str(),
                       Values.size() == 1 ? "index  " : "indexes",
                       formatCaseList(Values).c_str());
  }
  return 0;
}

} // namespace neverd::cli
