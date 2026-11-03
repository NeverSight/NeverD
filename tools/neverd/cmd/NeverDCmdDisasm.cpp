//===- NeverDCmdDisasm.cpp - Code inspection commands --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Handlers for the disassembly / control-flow views: `funcs`, `disasm`,
/// `hex`, `cfg`, `xrefs`, and `callgraph`.  The graph commands can additionally
/// emit Graphviz DOT or shell out to `dot` for an SVG.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace llvm;

namespace neverd::cli {

namespace {
cl::opt<std::string> RegisterQuery(
    "register-flow",
    cl::desc("Physical register query: registers, definitions, uses or value"),
    cl::init(""), cl::sub(DisasmCmd));
cl::opt<std::string> RegisterName("register", cl::desc("Decoded register name"),
                                  cl::init(""), cl::sub(DisasmCmd));
cl::opt<std::string> RegisterPC("instruction",
                                cl::desc("Instruction address (hex)"),
                                cl::init(""), cl::sub(DisasmCmd));

int findFunction(neverd_session_t Sess, const std::string &FuncId) {
  int FuncIdx = neverd_func_find_by_name(Sess, FuncId.c_str());
  if (FuncIdx >= 0)
    return FuncIdx;

  neverd_va_t Addr = 0;
  if (!parseHexAddress(FuncId, Addr))
    return -1;
  return neverd_func_find_by_addr(Sess, Addr);
}

bool renderDotToSvg(StringRef DotPath, StringRef SvgPath) {
  auto Dot = sys::findProgramByName("dot");
  if (!Dot)
    return false;

  std::vector<StringRef> Args = {*Dot, "-Tsvg", "-o", SvgPath, DotPath};
  return sys::ExecuteAndWait(*Dot, Args) == 0;
}

void writeDotQuoted(raw_ostream &OS, StringRef Text) {
  OS << '"';
  for (char Ch : Text) {
    switch (Ch) {
    case '\\':
      OS << "\\\\";
      break;
    case '"':
      OS << "\\\"";
      break;
    case '\n':
      OS << "\\n";
      break;
    case '\r':
      OS << "\\r";
      break;
    case '\t':
      OS << "\\t";
      break;
    default:
      if (static_cast<unsigned char>(Ch) >= 0x20)
        OS << Ch;
      else
        OS << '?';
      break;
    }
  }
  OS << '"';
}

} // anonymous namespace

int runFuncs(neverd_session_t Sess) {
  if ((SigAuto || !SigBase.empty() || !SigFile.empty() || !SigDir.empty()) &&
      !neverd_session_analyze(Sess)) {
    WithColor::error() << "library analysis failed: " << takeLastError(Sess)
                       << "\n";
    return 1;
  }
  int Count = neverd_func_count(Sess);
  const auto Identity = [&](int I) {
    const char *Raw = neverd_resolve_addr(Sess, neverd_func_entry(Sess, I));
    auto Value = json::parse(Raw ? Raw : "{}");
    neverd_free_string(Raw);
    if (Value && Value->getAsObject())
      return std::move(*Value->getAsObject());
    if (!Value)
      consumeError(Value.takeError());
    return json::Object{};
  };
  if (JsonOutput) {
    json::Array Funcs;
    for (int I = 0; I < Count; ++I) {
      const char *N = neverd_func_name(Sess, I);
      json::Object Func = Identity(I);
      Func["name"] = std::string(N ? N : "");
      Func["addr"] = "0x" + utohexstr(neverd_func_entry(Sess, I));
      Func["size"] = static_cast<int64_t>(neverd_func_size(Sess, I));
      Funcs.push_back(std::move(Func));
      neverd_free_string(N);
    }
    outs() << json::Value(std::move(Funcs)) << "\n";
  } else {
    outs() << "\nFunctions (" << Count << "):\n";
    outs() << format("  %-18s %-8s %s\n", "Address", "Size", "Name");
    outs() << "  " << std::string(60, '-') << "\n";
    for (int I = 0; I < Count; ++I) {
      const char *N = neverd_func_name(Sess, I);
      const auto Info = Identity(I);
      const auto Display = Info.getString("display_name");
      outs() << format("  0x%-16s %-8u %s\n",
                       utohexstr(neverd_func_entry(Sess, I)).c_str(),
                       static_cast<unsigned>(neverd_func_size(Sess, I)),
                       Display ? Display->str().c_str()
                       : N     ? N
                               : "");
      neverd_free_string(N);
    }
  }
  return 0;
}

int runDisasm(neverd_session_t Sess) {
  if (!RegisterQuery.empty()) {
    std::optional<uint64_t> Entry;
    const auto Index =
        neverd_func_find_by_name(Sess, DisasmFunc.getValue().c_str());
    if (Index >= 0)
      Entry = neverd_func_entry(Sess, Index);
    else
      Entry = parseAddrArg(DisasmFunc);
    const auto PC = parseAddrArg(RegisterPC);
    if (!Entry || !PC) {
      WithColor::error() << "register flow requires --func and --instruction\n";
      return 1;
    }
    std::string Options;
    raw_string_ostream(Options)
        << json::Value(json::Object{{"kind", RegisterQuery.getValue()},
                                    {"register", RegisterName.getValue()}});
    const auto *Reply =
        neverd_register_flow_json(Sess, *Entry, *PC, Options.c_str());
    if (!Reply) {
      WithColor::error() << takeLastError(Sess) << "\n";
      return 1;
    }
    outs() << Reply << "\n";
    neverd_free_string(Reply);
    return 0;
  }
  if (DisasmFunc.empty()) {
    WithColor::error() << "disasm requires --func <name or hex addr>\n";
    return 1;
  }

  if (JsonOutput) {
    std::string FuncId = DisasmFunc.getValue();
    int FuncIdx = findFunction(Sess, FuncId);
    // An address no listed function starts is disassembled from there, as
    // the text form does.
    const auto Entry = FuncIdx >= 0
                           ? std::optional(neverd_func_entry(Sess, FuncIdx))
                           : parseAddrArg(DisasmFunc);
    if (!Entry) {
      WithColor::error() << "disasm failed: function not found\n";
      return 1;
    }

    const char *Json = neverd_disasm_json(Sess, *Entry, 0);
    if (Json) {
      outs() << Json << "\n";
      neverd_free_string(Json);
    }
  } else {
    const char *Text = neverd_disasm_text(Sess, DisasmFunc.getValue().c_str(),
                                          DisasmAnnotate ? 1 : 0);
    if (Text) {
      outs() << "\n" << Text;
      neverd_free_string(Text);
    } else {
      WithColor::error()
          << "disasm failed: function not found or decode error\n";
      return 1;
    }
  }
  return 0;
}

int runHex(neverd_session_t Sess) {
  neverd_va_t Addr;
  if (!HexAddr.empty()) {
    if (!parseHexAddress(HexAddr.getValue(), Addr)) {
      WithColor::error() << "invalid hexadecimal address\n";
      return 1;
    }
  } else {
    Addr = neverd_session_base_addr(Sess);
  }
  const char *Encoding =
      HexTextEncoding.empty() ? nullptr : HexTextEncoding.getValue().c_str();
  const unsigned Size = HexSize;
  if (!JsonOutput) {
    // The dump goes on past unmapped bytes, which print as "??".
    const char *Dump =
        neverd_hex_dump_ex(Sess, Addr, static_cast<int>(Size), Encoding);
    if (!Dump) {
      const std::string Error = takeLastError(Sess);
      WithColor::error() << (Error.empty()
                                 ? "no data at address 0x" + utohexstr(Addr)
                                 : Error)
                         << "\n";
      return 1;
    }
    outs() << "\n" << Dump;
    neverd_free_string(Dump);
    return 0;
  }
  std::vector<uint8_t> Buf(Size);
  int Got = neverd_read_bytes(Sess, Addr, Buf.data(), static_cast<int>(Size));
  if (Got <= 0) {
    WithColor::error() << "no data at address 0x" << utohexstr(Addr) << "\n";
    return 1;
  }
  outs() << "{\"addr\":\"0x" << utohexstr(Addr) << "\",\"size\":" << Got
         << ",\"bytes\":\"";
  for (int I = 0; I < Got; ++I)
    outs() << format("%02x", Buf[I]);
  outs() << "\"";
  // With an encoding, the text of each byte as a hex view shows it.
  if (Encoding) {
    const char *Cells = neverd_decode_text_json(Buf.data(), Got, Encoding);
    if (!Cells) {
      outs() << "}\n";
      WithColor::error() << "unknown text encoding: " << Encoding << "\n";
      return 1;
    }
    auto Parsed = json::parse(Cells);
    neverd_free_string(Cells);
    if (!Parsed) {
      consumeError(Parsed.takeError());
      outs() << "}\n";
      return 1;
    }
    if (const auto *Object = Parsed->getAsObject())
      if (const auto *List = Object->get("cells"))
        outs() << ",\"cells\":" << *List;
  }
  outs() << "}\n";
  return 0;
}

int runCfg(neverd_session_t Sess) {
  if (DisasmFunc.empty()) {
    WithColor::error() << "cfg requires --func <name or hex addr>\n";
    return 1;
  }

  const char *InPath = InputFile.getValue().c_str();
  const char *FuncId = DisasmFunc.getValue().c_str();

  // Analyze the function alone, as the workbench does, so that another
  // function of the program cannot fail its graph.
  const int FuncIdx = findFunction(Sess, FuncId);
  if (FuncIdx < 0) {
    WithColor::error() << "cfg failed: function not found\n";
    return 1;
  }
  const neverd_va_t Entry = neverd_func_entry(Sess, FuncIdx);
  if (!neverd_prepare_function(Sess, Entry)) {
    WithColor::error() << "cfg failed: " << takeLastError(Sess) << "\n";
    return 1;
  }

  if (!CfgSvg.empty() || CfgDot) {
    int Styled = !CfgSvg.empty() ? 1 : 0;
    const char *Dot = neverd_cfg_dot(Sess, InPath, FuncId, Styled);
    if (!Dot) {
      WithColor::error() << "cfg failed: " << takeLastError(Sess) << "\n";
      return 1;
    }
    if (!CfgSvg.empty()) {
      std::string SvgPath = CfgSvg.getValue();
      SmallString<128> DotTmpPath;
      if (std::error_code EC =
              sys::fs::createTemporaryFile("neverd-cfg", "dot", DotTmpPath)) {
        WithColor::error() << "cannot create temp dot: " << EC.message()
                           << "\n";
        neverd_free_string(Dot);
        return 1;
      }
      {
        std::error_code EC;
        raw_fd_ostream DotFile(DotTmpPath, EC);
        if (EC) {
          WithColor::error()
              << "cannot write temp dot: " << EC.message() << "\n";
          std::error_code RemoveEC = sys::fs::remove(DotTmpPath);
          (void)RemoveEC;
          neverd_free_string(Dot);
          return 1;
        }
        DotFile << Dot;
      }
      bool Rendered = renderDotToSvg(DotTmpPath, SvgPath);
      std::error_code RemoveEC = sys::fs::remove(DotTmpPath);
      (void)RemoveEC;
      if (!Rendered) {
        WithColor::error()
            << "graphviz 'dot' failed. Install: brew install graphviz\n";
        neverd_free_string(Dot);
        return 1;
      }
      if (!JsonOutput)
        outs() << "SVG written to " << SvgPath << "\n";
    } else {
      outs() << Dot;
    }
    neverd_free_string(Dot);
    return 0;
  }

  if (JsonOutput) {
    // The graph reads "{}" when analysis fails; the error says why.
    const char *Json = neverd_cfg_json(Sess, Entry);
    const std::string Error = takeLastError(Sess);
    if (!Json || !Error.empty()) {
      if (Json)
        neverd_free_string(Json);
      WithColor::error() << "cfg failed" << (Error.empty() ? "" : ": " + Error)
                         << "\n";
      return 1;
    }
    outs() << Json << "\n";
    neverd_free_string(Json);
  } else {
    const char *Dot = neverd_cfg_dot(Sess, InPath, FuncId, 0);
    if (!Dot) {
      WithColor::error() << "cfg failed: " << takeLastError(Sess) << "\n";
      return 1;
    }
    outs() << Dot;
    neverd_free_string(Dot);
  }
  return 0;
}

int runCallGraph(neverd_session_t Sess) {
  const char *Json = neverd_callgraph_json(Sess);
  if (!Json) {
    WithColor::error() << "callgraph failed\n";
    return 1;
  }

  auto emitDot = [&](raw_ostream &DotOS, bool Styled) {
    DotOS << "digraph callgraph {\n";
    DotOS << "  rankdir=LR;\n";
    if (Styled) {
      DotOS << "  node [shape=box, fontname=\"Courier\", fontsize=10, "
               "style=filled, fillcolor=\"#252526\", fontcolor=\"#ebebeb\", "
               "color=\"#3c3c3c\"];\n";
      DotOS << "  edge [color=\"#569cd6\"];\n";
      DotOS << "  bgcolor=\"#1e1e1e\";\n";
    } else {
      DotOS << "  node [shape=box, fontname=\"Courier\", fontsize=10];\n";
    }
    auto Parsed = json::parse(Json);
    if (Parsed) {
      auto *Root = Parsed->getAsObject();
      if (Root) {
        auto *EdgesArr = Root->getArray("edges");
        if (EdgesArr) {
          std::set<std::string> NodesSeen;
          for (const auto &E : *EdgesArr) {
            auto *Obj = E.getAsObject();
            if (!Obj)
              continue;
            auto Caller = Obj->getString("caller");
            auto Callee = Obj->getString("callee");
            auto CN = Obj->getString("caller_name");
            auto CE = Obj->getString("callee_name");
            if (!Caller || !Callee || !CN || !CE)
              continue;
            if (NodesSeen.insert(Caller->str()).second) {
              DotOS << "  ";
              writeDotQuoted(DotOS, *Caller);
              DotOS << " [label=";
              writeDotQuoted(DotOS, *CN);
              DotOS << "];\n";
            }
            if (NodesSeen.insert(Callee->str()).second) {
              DotOS << "  ";
              writeDotQuoted(DotOS, *Callee);
              DotOS << " [label=";
              writeDotQuoted(DotOS, *CE);
              DotOS << "];\n";
            }
            DotOS << "  ";
            writeDotQuoted(DotOS, *Caller);
            DotOS << " -> ";
            writeDotQuoted(DotOS, *Callee);
            DotOS << ";\n";
          }
        }
      }
    }
    DotOS << "}\n";
  };

  if (!CgSvg.empty()) {
    std::string DotStr;
    raw_string_ostream DotOS(DotStr);
    emitDot(DotOS, true);

    std::string SvgPath = CgSvg.getValue();
    SmallString<128> DotTmpPath;
    if (std::error_code EC = sys::fs::createTemporaryFile("neverd-callgraph",
                                                          "dot", DotTmpPath)) {
      WithColor::error() << "cannot create temp dot: " << EC.message() << "\n";
      neverd_free_string(Json);
      return 1;
    }
    {
      std::error_code EC;
      raw_fd_ostream DotFile(DotTmpPath, EC);
      if (EC) {
        WithColor::error() << "cannot write: " << EC.message() << "\n";
        std::error_code RemoveEC = sys::fs::remove(DotTmpPath);
        (void)RemoveEC;
        neverd_free_string(Json);
        return 1;
      }
      DotFile << DotStr;
    }
    bool Rendered = renderDotToSvg(DotTmpPath, SvgPath);
    std::error_code RemoveEC = sys::fs::remove(DotTmpPath);
    (void)RemoveEC;
    if (!Rendered) {
      WithColor::error()
          << "graphviz 'dot' failed. Install: brew install graphviz\n";
      neverd_free_string(Json);
      return 1;
    }
    if (!JsonOutput)
      outs() << "SVG written to " << SvgPath << "\n";
  } else if (CgDot) {
    emitDot(outs(), false);
  } else if (JsonOutput) {
    outs() << Json << "\n";
  } else {
    outs() << "\nCall Graph:\n";
    auto Parsed = json::parse(Json);
    size_t EdgeCount = 0;
    if (Parsed) {
      auto *Root = Parsed->getAsObject();
      if (Root) {
        auto *EdgesArr = Root->getArray("edges");
        if (EdgesArr) {
          for (const auto &E : *EdgesArr) {
            auto *Obj = E.getAsObject();
            if (!Obj)
              continue;
            auto CN = Obj->getString("caller_name");
            auto CE = Obj->getString("callee_name");
            if (CN && CE) {
              outs() << "  " << *CN << " -> " << *CE << "\n";
              ++EdgeCount;
            }
          }
        }
      }
    }
    outs() << "\n" << EdgeCount << " call edges\n";
  }

  neverd_free_string(Json);
  return 0;
}

int runSymbolicExplore(neverd_session_t Sess) {
  if (DisasmFunc.empty()) {
    WithColor::error() << "sym-explore requires --func <name or hex addr>\n";
    return 1;
  }

  int FuncIdx = findFunction(Sess, DisasmFunc.getValue());
  // Keep the loader's cheap symbol lookup for ordinary images. A stripped
  // image may have no function symbols even when lifting recovers its entry;
  // run discovery and retry only if that initial lookup misses.
  if (FuncIdx < 0) {
    if (!neverd_session_analyze(Sess)) {
      WithColor::error() << "sym-explore failed: " << takeLastError(Sess)
                         << "\n";
      return 1;
    }
    FuncIdx = findFunction(Sess, DisasmFunc.getValue());
  }
  if (FuncIdx < 0) {
    WithColor::error() << "sym-explore failed: function not found\n";
    return 1;
  }

  neverd_symbolic_explore_options Options{};
  Options.struct_size = sizeof(Options);
  Options.max_paths = SymbolicMaxPaths;
  Options.max_steps = SymbolicMaxSteps;
  Options.max_block_visits = SymbolicMaxBlockVisits;
  Options.include_expressions = SymbolicExpressions ? 1 : 0;

  const char *Report = neverd_symbolic_explore_json(
      Sess, neverd_func_entry(Sess, FuncIdx), &Options);
  if (!Report) {
    WithColor::error() << "sym-explore failed: " << takeLastError(Sess) << "\n";
    return 1;
  }

  bool Ok = false;
  if (Expected<json::Value> Parsed = json::parse(Report))
    if (const json::Object *Root = Parsed->getAsObject())
      Ok = Root->getBoolean("ok").value_or(false);
  outs() << Report << "\n";
  neverd_free_string(Report);
  return Ok ? 0 : 1;
}

} // namespace neverd::cli
