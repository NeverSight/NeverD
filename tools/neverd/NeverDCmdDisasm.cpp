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

#include "NeverDCLI.h"

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

bool parseHexAddress(const std::string &Text, neverd_va_t &Addr) {
  StringRef Ref(Text);
  if (Ref.empty() || Ref.front() == '-')
    return false;
  if (Ref.consume_front("0x") || Ref.consume_front("0X")) {
    if (Ref.empty())
      return false;
  }
  if (Ref.getAsInteger(16, Addr))
    return false;
  return true;
}

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
  int Count = neverd_func_count(Sess);
  if (JsonOutput) {
    json::Array Funcs;
    for (int I = 0; I < Count; ++I) {
      const char *N = neverd_func_name(Sess, I);
      json::Object Func;
      Func["name"] = N ? N : "";
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
      outs() << format(
          "  0x%-16s %-8u %s\n", utohexstr(neverd_func_entry(Sess, I)).c_str(),
          static_cast<unsigned>(neverd_func_size(Sess, I)), N ? N : "");
      neverd_free_string(N);
    }
  }
  return 0;
}

int runDisasm(neverd_session_t Sess) {
  if (DisasmFunc.empty()) {
    WithColor::error() << "disasm requires --func <name or hex addr>\n";
    return 1;
  }

  if (JsonOutput) {
    std::string FuncId = DisasmFunc.getValue();
    int FuncIdx = findFunction(Sess, FuncId);
    if (FuncIdx < 0) {
      WithColor::error() << "disasm failed: function not found\n";
      return 1;
    }

    const char *Json =
        neverd_disasm_json(Sess, neverd_func_entry(Sess, FuncIdx), 0);
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

  unsigned Size = HexSize;
  std::vector<uint8_t> Buf(Size);
  int Got = neverd_read_bytes(Sess, Addr, Buf.data(), static_cast<int>(Size));
  if (Got <= 0) {
    WithColor::error() << "no data at address 0x" << utohexstr(Addr) << "\n";
    return 1;
  }

  if (JsonOutput) {
    outs() << "{\"addr\":\"0x" << utohexstr(Addr) << "\",\"size\":" << Got
           << ",\"bytes\":\"";
    for (int I = 0; I < Got; ++I)
      outs() << format("%02x", Buf[I]);
    outs() << "\"}\n";
  } else {
    const char *Dump = neverd_hex_dump(Sess, Addr, static_cast<int>(Size));
    if (Dump) {
      outs() << "\n" << Dump;
      neverd_free_string(Dump);
    }
  }
  return 0;
}

int runXrefs(neverd_session_t Sess) {
  neverd_va_t Target = 0;
  if (!parseHexAddress(XrefAddr.getValue(), Target)) {
    WithColor::error() << "invalid hexadecimal address\n";
    return 1;
  }
  if (!JsonOutput) {
    outs() << "XRefs for 0x" << utohexstr(Target) << ":\n";
    outs() << "(requires pipeline - running lift...)\n";
  }

  const char *Json =
      neverd_xrefs_scan(Sess, InputFile.getValue().c_str(), Target);
  if (!Json) {
    WithColor::error() << "xrefs scan failed\n";
    return 1;
  }

  if (JsonOutput) {
    outs() << Json << "\n";
  } else {
    auto Parsed = json::parse(Json);
    size_t RefCount = 0;
    if (Parsed) {
      if (auto *Arr = Parsed->getAsArray()) {
        for (const auto &V : *Arr) {
          if (auto *Obj = V.getAsObject()) {
            outs() << "  " << Obj->getString("from").value_or("") << " in "
                   << Obj->getString("func").value_or("") << " (block "
                   << Obj->getInteger("block").value_or(0) << ")\n";
            ++RefCount;
          }
        }
      }
    }
    outs() << RefCount << " references found\n";
  }
  neverd_free_string(Json);
  return 0;
}

int runCfg(neverd_session_t Sess) {
  if (DisasmFunc.empty()) {
    WithColor::error() << "cfg requires --func <name or hex addr>\n";
    return 1;
  }

  const char *InPath = InputFile.getValue().c_str();
  const char *FuncId = DisasmFunc.getValue().c_str();

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
    int FuncIdx = findFunction(Sess, FuncId);
    if (FuncIdx < 0) {
      WithColor::error() << "cfg failed: function not found\n";
      return 1;
    }
    const char *Json = neverd_cfg_json(Sess, neverd_func_entry(Sess, FuncIdx));
    if (Json) {
      outs() << Json << "\n";
      neverd_free_string(Json);
    } else {
      WithColor::error() << "cfg failed\n";
      return 1;
    }
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
