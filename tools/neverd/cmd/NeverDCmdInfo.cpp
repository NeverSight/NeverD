//===- NeverDCmdInfo.cpp - Binary metadata summary commands --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Handlers for the high-level metadata views: `info`, `headers`, and
/// `dashboard`.  Each renders a single C-API JSON blob either verbatim (with
/// --json) or as a human-readable report.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"
#include "../UnwindQuery.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace neverd::cli {

namespace {

/// The headers' `language` object as one line: `rust`, `go go1.26.5`,
/// `c++-itanium (also rust)`.
std::string describeLanguage(const json::Object &Language) {
  std::string Text = Language.getString("runtime").value_or("").str();
  if (auto Version = Language.getString("version"))
    Text += (" " + *Version).str();
  if (auto *Secondary = Language.getArray("secondary");
      Secondary && !Secondary->empty()) {
    Text += " (also";
    for (const auto &Runtime : *Secondary)
      Text += (" " + Runtime.getAsString().value_or("")).str();
    Text += ")";
  }
  return Text;
}

} // namespace

int runIdentify() {
  const char *Json = neverd_identify_json(InputFile.c_str());
  const std::string Text = Json ? Json : "";
  neverd_free_string(Json);
  if (JsonOutput) {
    outs() << Text << "\n";
    return 0;
  }
  auto Parsed = json::parse(Text);
  if (!Parsed) {
    WithColor::error() << "identify failed: " << toString(Parsed.takeError())
                       << "\n";
    return 1;
  }
  const json::Object *Root = Parsed->getAsObject();
  if (auto Error = Root->getString("error")) {
    WithColor::error() << *Error << "\n";
    return 1;
  }
  // As the load dialog lists them: the first row loadable for the file's
  // contents is the default.
  outs() << "Load file " << InputFile << " as:\n";
  bool Chosen = false;
  for (const json::Value &Value : *Root->getArray("rows")) {
    const json::Object &Row = *Value.getAsObject();
    const bool Loadable = Row.getBoolean("loadable").value_or(false);
    const bool Default =
        Loadable && !Row.getBoolean("by_name").value_or(false) && !Chosen;
    Chosen |= Default;
    outs() << (Default ? "  * " : "    ") << Row.getString("text").value_or("")
           << " [" << Row.getString("loader").value_or("") << "]";
    if (const auto Processor = Row.getString("processor");
        Processor && !Processor->empty())
      outs() << "  " << *Processor;
    if (!Loadable)
      outs() << "  (" << Row.getString("reason").value_or("") << ")";
    else if (Row.getBoolean("by_name").value_or(false))
      outs() << "  (for the file name alone)";
    outs() << "\n";
    // What a headerless file's bytes show, and the processor --loader
    // binary reads them as without --processor.
    if (Row.get("guesses")) {
      outs() << "      bytes: " << Row.getString("evidence").value_or("")
             << "\n";
      const llvm::StringRef Detected = Row.getString("detected").value_or("");
      outs() << "      processor: "
             << (Detected.empty() ? "none read; name one with --processor"
                                  : Detected)
             << "\n";
      if (const json::Object *Print = Row.getObject("fingerprint"))
        outs() << "      " << Print->getString("name").value_or("")
               << ": entry " << Print->getString("entry").value_or("")
               << ", base " << Print->getString("base").value_or("") << "\n";
      else if (const int64_t Start = Row.getInteger("code_offset").value_or(0))
        outs() << "      read it with --load-offset "
               << llvm::format_hex(Start, 0) << "\n";
    }
  }
  return 0;
}

int runInfo(neverd_session_t Sess) {
  if (InfoUnwindFrame.getNumOccurrences()) {
    if (!JsonOutput) {
      WithColor::error() << unwind_query::kRequiresJSON << '\n';
      return 1;
    }
    const auto Address = parseAddrArg(InfoUnwindFrame.getValue());
    if (!Address) {
      WithColor::error() << unwind_query::kInvalidAddress << '\n';
      return 1;
    }
    const char *Result = neverd_unwind_frame_json(Sess, *Address);
    if (!Result) {
      WithColor::error() << unwind_query::kQueryFailed << takeLastError(Sess)
                         << '\n';
      return 1;
    }
    outs() << Result << '\n';
    neverd_free_string(Result);
    return 0;
  }

  if (JsonOutput) {
    const char *Json = neverd_headers_json(Sess);
    outs() << Json << "\n";
    neverd_free_string(Json);
    return 0;
  }

  const char *Fmt = neverd_session_format_name(Sess);
  outs() << "Format: " << Fmt << (neverd_session_is_64bit(Sess) ? "64" : "32")
         << "\n";
  neverd_free_string(Fmt);
  outs() << "Base:   0x" << utohexstr(neverd_session_base_addr(Sess)) << "\n";
  outs() << "Entry:  0x" << utohexstr(neverd_session_entry_addr(Sess)) << "\n";
  // A binary file's platform, and what detection read it from.
  const char *LoadJson = neverd_session_load_options_json(Sess);
  if (auto Load = json::parse(LoadJson); Load && Load->getAsObject()) {
    const json::Object &Options = *Load->getAsObject();
    if (auto Platform = Options.getString("platform");
        Platform && *Platform != "auto") {
      outs() << "Platform: " << *Platform;
      if (Options.getString("platform_source") == "detected")
        outs() << " (read from the code: "
               << Options.getString("platform_evidence").value_or("") << ")";
      outs() << "\n";
    }
  } else if (!Load) {
    consumeError(Load.takeError());
  }
  neverd_free_string(LoadJson);

  const char *HeaderJson = neverd_headers_json(Sess);
  auto HeaderParsed = json::parse(HeaderJson ? HeaderJson : "{}");
  neverd_free_string(HeaderJson);
  if (HeaderParsed)
    if (auto *Root = HeaderParsed->getAsObject()) {
      if (auto *Target = Root->getObject("target_profile"))
        outs() << "Target: " << Target->getString("system").value_or("unknown")
               << " / " << Target->getString("domain").value_or("unknown")
               << " / " << Target->getString("runtime").value_or("unknown")
               << "\n";
      if (auto *Language = Root->getObject("language"))
        outs() << "Language: " << describeLanguage(*Language) << "\n";
      if (auto *SBF = Root->getObject("sbf"))
        outs() << "SBF:    " << SBF->getString("version_display").value_or("")
               << " / " << SBF->getString("machine_name").value_or("") << " / "
               << SBF->getString("layout").value_or("") << "\n";
      if (auto *EVM = Root->getObject("evm")) {
        outs() << "EVM:    " << EVM->getString("container").value_or("")
               << " / " << EVM->getString("hardfork").value_or("") << "\n";
        // The delegate target is the only address at which this account's
        // behavior can be read, so it belongs in the one-screen summary.
        if (auto Target = EVM->getString("delegate_target"))
          outs() << "        delegates to " << *Target << "\n";
        for (const char *Key : {"runtime_metadata", "input_metadata"})
          if (auto *Trailer = EVM->getObject(Key))
            if (auto Version = Trailer->getString("compiler_version")) {
              outs() << "        built by "
                     << Trailer->getString("language").value_or("") << " "
                     << *Version << "\n";
              break;
            }
      }
    }

  const char *SegJson = neverd_segments_json(Sess);
  auto SegParsed = json::parse(SegJson ? SegJson : "[]");
  neverd_free_string(SegJson);
  if (SegParsed) {
    if (auto *Arr = SegParsed->getAsArray()) {
      outs() << "\nSegments (" << Arr->size() << "):\n";
      for (const auto &V : *Arr) {
        auto *Obj = V.getAsObject();
        if (!Obj)
          continue;
        outs() << "  "
               << format(
                      "%-20s",
                      std::string(Obj->getString("name").value_or("")).c_str())
               << " VA=" << Obj->getString("va").value_or("")
               << " Size=" << Obj->getString("size").value_or("") << " "
               << Obj->getString("flags").value_or("") << "\n";
      }
    }
  }

  const char *SecJson = neverd_sections_json(Sess);
  auto SecParsed = json::parse(SecJson ? SecJson : "[]");
  neverd_free_string(SecJson);
  if (SecParsed) {
    if (auto *Arr = SecParsed->getAsArray()) {
      outs() << "\nSections (" << Arr->size() << "):\n";
      for (const auto &V : *Arr) {
        auto *Obj = V.getAsObject();
        if (!Obj)
          continue;
        outs() << "  "
               << format(
                      "%-20s",
                      std::string(Obj->getString("name").value_or("")).c_str())
               << " VA=" << Obj->getString("va").value_or("")
               << " Size=" << Obj->getInteger("size").value_or(0) << "\n";
      }
    }
  }

  const char *ImpJson = neverd_imports_json(Sess);
  auto ImpParsed = json::parse(ImpJson ? ImpJson : "[]");
  neverd_free_string(ImpJson);
  if (ImpParsed) {
    if (auto *Arr = ImpParsed->getAsArray()) {
      outs() << "\nImports: " << Arr->size() << "\n";
      for (const auto &V : *Arr) {
        auto *Obj = V.getAsObject();
        if (!Obj)
          continue;
        outs() << "  " << Obj->getString("module").value_or("")
               << "::" << Obj->getString("name").value_or("") << " @"
               << Obj->getString("iat_addr").value_or("") << "\n";
      }
    }
  }

  const char *ExpJson = neverd_exports_json(Sess);
  auto ExpParsed = json::parse(ExpJson ? ExpJson : "[]");
  neverd_free_string(ExpJson);
  if (ExpParsed) {
    if (auto *Arr = ExpParsed->getAsArray()) {
      outs() << "\nExports: " << Arr->size() << "\n";
      for (const auto &V : *Arr) {
        auto *Obj = V.getAsObject();
        if (!Obj)
          continue;
        outs() << "  " << Obj->getString("name").value_or("") << " @"
               << Obj->getString("addr").value_or("") << "\n";
      }
    }
  }

  outs() << "\nSymbols: " << neverd_session_symbol_count(Sess) << "\n";
  return 0;
}

int runHeaders(neverd_session_t Sess) {
  const char *Json = neverd_headers_json(Sess);
  if (JsonOutput) {
    outs() << (Json ? Json : "{}") << "\n";
    if (Json)
      neverd_free_string(Json);
    return 0;
  }

  auto Parsed = json::parse(Json ? Json : "{}");
  if (Parsed) {
    auto *Root = Parsed->getAsObject();
    if (Root) {
      outs() << "\n=== Binary Headers ===\n\n";
      outs() << format(
          "  %-20s %s\n", "File:",
          std::string(Root->getString("file_path").value_or("")).c_str());
      outs() << format(
          "  %-20s %s\n", "Format:",
          std::string(Root->getString("format").value_or("")).c_str());
      outs() << format(
          "  %-20s %s\n", "Architecture:",
          std::string(Root->getString("arch").value_or("")).c_str());
      outs() << format("  %-20s %lld-bit\n",
                       "Bitness:", Root->getInteger("bits").value_or(0));
      outs() << format(
          "  %-20s %s\n", "Entry Point:",
          std::string(Root->getString("entry").value_or("")).c_str());
      outs() << format(
          "  %-20s %s\n", "Base Address:",
          std::string(Root->getString("base").value_or("")).c_str());
      outs() << format("  %-20s %lld bytes\n",
                       "File Size:", Root->getInteger("file_size").value_or(0));
      if (auto *Language = Root->getObject("language"))
        outs() << format("  %-20s %s\n",
                         "Language:", describeLanguage(*Language).c_str());

      if (auto *SBF = Root->getObject("sbf")) {
        outs() << "\n  --- Solana SBF ---\n";
        outs() << format(
            "  %-20s %s\n", "Version:",
            SBF->getString("version_display").value_or("").str().c_str());
        outs() << format(
            "  %-20s %s (%lld)\n", "ELF Machine:",
            SBF->getString("machine_name").value_or("").str().c_str(),
            SBF->getInteger("machine").value_or(0));
        outs() << format("  %-20s %s\n", "Layout:",
                         SBF->getString("layout").value_or("").str().c_str());
      }

      if (auto *EVM = Root->getObject("evm")) {
        outs() << "\n  --- Ethereum EVM ---\n";
        outs() << format("  %-20s %s\n", "Input:",
                         EVM->getString("source").value_or("").str().c_str());
        outs() << format(
            "  %-20s %s\n", "Container:",
            EVM->getString("container").value_or("").str().c_str());
        outs() << format("  %-20s %s\n", "Hardfork:",
                         EVM->getString("hardfork").value_or("").str().c_str());
        if (auto Activated = EVM->getString("container_activated_at"))
          outs() << format("  %-20s %s (%s)\n", "Container Active:",
                           EVM->getBoolean("container_active").value_or(false)
                               ? "yes"
                               : "no",
                           Activated->str().c_str());
        if (auto Target = EVM->getString("delegate_target"))
          outs() << format("  %-20s %s\n",
                           "Delegates To:", Target->str().c_str());
        outs() << format("  %-20s %s\n", "Runtime Extracted:",
                         EVM->getBoolean("runtime_extracted").value_or(false)
                             ? "yes"
                             : "no");

        // The trailer lives in the deployment container for one compiler and
        // in the runtime code for the other, so which one carried it is a fact
        // about the build worth printing beside the version.
        for (const char *Key : {"runtime_metadata", "input_metadata"}) {
          auto *Trailer = EVM->getObject(Key);
          if (!Trailer)
            continue;
          outs() << format(
              "  %-20s %s %s (%s)\n", "Compiler:",
              Trailer->getString("language").value_or("").str().c_str(),
              Trailer->getString("compiler_version")
                  .value_or("?")
                  .str()
                  .c_str(),
              Trailer->getString("container").value_or("").str().c_str());
          if (auto *Hash = Trailer->getObject("source_hash"))
            outs() << format(
                "  %-20s %s:%s\n",
                "Source:", Hash->getString("kind").value_or("").str().c_str(),
                Hash->getString("value").value_or("").str().c_str());
          break;
        }
      }

      outs() << "\n  --- Counts ---\n";
      outs() << format("  %-20s %lld\n", "Segments:",
                       Root->getInteger("segment_count").value_or(0));
      outs() << format("  %-20s %lld\n", "Sections:",
                       Root->getInteger("section_count").value_or(0));
      outs() << format("  %-20s %lld\n", "Functions:",
                       Root->getInteger("func_count").value_or(0));
      outs() << format("  %-20s %lld\n", "Symbols:",
                       Root->getInteger("symbol_count").value_or(0));
      outs() << format("  %-20s %lld\n", "Imports:",
                       Root->getInteger("import_count").value_or(0));
      outs() << format("  %-20s %lld\n", "Exports:",
                       Root->getInteger("export_count").value_or(0));
      outs() << format("  %-20s %lld\n", "Relocations:",
                       Root->getInteger("reloc_count").value_or(0));

      auto *Dyn = Root->getObject("dynamic");
      if (Dyn && !Dyn->empty()) {
        outs() << "\n  --- Dynamic Info ---\n";
        if (auto S = Dyn->getString("soname"))
          outs() << format("  %-20s %s\n", "SO Name:", S->str().c_str());
        if (auto S = Dyn->getString("uuid"))
          outs() << format("  %-20s %s\n", "UUID:", S->str().c_str());
        if (auto S = Dyn->getString("min_os_version"))
          outs() << format("  %-20s %s\n", "Min OS:", S->str().c_str());
        if (auto S = Dyn->getString("pdb_path"))
          outs() << format("  %-20s %s\n", "PDB Path:", S->str().c_str());
        if (auto *Needed = Dyn->getArray("needed")) {
          outs() << "  Needed Libraries:\n";
          for (const auto &Lib : *Needed)
            if (auto S = Lib.getAsString())
              outs() << "    - " << *S << "\n";
        }
        if (auto *RP = Dyn->getArray("rpaths")) {
          outs() << "  RPaths:\n";
          for (const auto &P : *RP)
            if (auto S = P.getAsString())
              outs() << "    - " << *S << "\n";
        }
      }
      outs() << "\n";
    }
  }
  if (Json)
    neverd_free_string(Json);
  return 0;
}

int runDashboard(neverd_session_t Sess) {
  const char *Json = neverd_dashboard_json(Sess);
  if (JsonOutput) {
    outs() << (Json ? Json : "{}") << "\n";
    if (Json)
      neverd_free_string(Json);
    return 0;
  }

  auto Parsed = json::parse(Json ? Json : "{}");
  if (Parsed) {
    auto *Root = Parsed->getAsObject();
    if (Root) {
      auto *File = Root->getObject("file");
      if (File) {
        outs() << "\n=== File Info ===\n";
        auto PrintField = [](raw_ostream &OS, const char *Label,
                             StringRef Val) {
          OS << format("  %-12s %s\n", Label, Val.str().c_str());
        };
        PrintField(outs(), "File:", File->getString("name").value_or(""));
        PrintField(outs(), "Format:", File->getString("format").value_or(""));
        PrintField(outs(), "Arch:", File->getString("arch").value_or(""));
        if (auto Bits = File->getInteger("bits"))
          outs() << format("  %-12s %lld\n", "Bits:", *Bits);
        if (auto Size = File->getInteger("size"))
          outs() << format("  %-12s %lld bytes\n", "Size:", *Size);
        PrintField(outs(), "Endian:", File->getString("endian").value_or(""));
        PrintField(outs(), "Entry:", File->getString("entry").value_or(""));
        PrintField(outs(), "Base:", File->getString("base").value_or(""));
      }

      auto *Hashes = Root->getObject("hashes");
      if (Hashes) {
        outs() << "\n=== Hashes ===\n";
        outs() << format("  %-12s %s\n", "MD5:",
                         Hashes->getString("md5").value_or("").str().c_str());
        outs() << format(
            "  %-12s %s\n",
            "SHA256:", Hashes->getString("sha256").value_or("").str().c_str());
      }

      auto *Counts = Root->getObject("counts");
      if (Counts) {
        outs() << "\n=== Statistics ===\n";
        auto PrintCount = [](raw_ostream &OS, const char *Label,
                             const json::Object *Obj, const char *Key) {
          if (auto V = Obj->getInteger(Key))
            OS << format("  %-14s %lld\n", Label, *V);
        };
        PrintCount(outs(), "Functions:", Counts, "functions");
        PrintCount(outs(), "Imports:", Counts, "imports");
        PrintCount(outs(), "Exports:", Counts, "exports");
        PrintCount(outs(), "Strings:", Counts, "strings");
        PrintCount(outs(), "Symbols:", Counts, "symbols");
        PrintCount(outs(), "Segments:", Counts, "segments");
        PrintCount(outs(), "Sections:", Counts, "sections");
        PrintCount(outs(), "Relocations:", Counts, "relocations");
        PrintCount(outs(), "EntryPoints:", Counts, "entrypoints");
      }

      auto *Libs = Root->getArray("libraries");
      if (Libs && !Libs->empty()) {
        outs() << "\n=== Libraries ===\n";
        for (const auto &Lib : *Libs)
          if (auto S = Lib.getAsString())
            outs() << "  " << *S << "\n";
      }
      outs() << "\n";
    }
  }
  if (Json)
    neverd_free_string(Json);
  return 0;
}

} // namespace neverd::cli
