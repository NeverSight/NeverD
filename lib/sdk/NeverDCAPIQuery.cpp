//===- NeverDCAPIQuery.cpp - C API: info panels, analysis, search ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Information panels (imports, exports, segments, sections, symbols,
/// relocations, headers, strings, entry points, dashboard), cross-
/// references, CFG graph, call graph, address resolution, byte/string
/// search, binary diff, and standalone xref-scan / cfg-dot operations.
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/ir/NdOps.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>

using namespace neverd;
using namespace neverd::sdk;

// ===--------------------------------------------------------------------===//
// Info panels (JSON)
// ===--------------------------------------------------------------------===//

const char *neverd_imports_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr(std::string("[]"));

  llvm::json::Array Arr;
  for (const auto &Imp : S->Img.Imports) {
    llvm::json::Object Obj;
    Obj["module"] = Imp.Module;
    Obj["name"] = Imp.Name;
    Obj["ordinal"] = static_cast<int64_t>(Imp.Ordinal);
    Obj["iat_addr"] = vaHex(Imp.IATAddr);
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_exports_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr(std::string("[]"));

  llvm::json::Array Arr;
  for (const auto &Exp : S->Img.Exports) {
    llvm::json::Object Obj;
    Obj["name"] = Exp.Name;
    Obj["ordinal"] = static_cast<int64_t>(Exp.Ordinal);
    Obj["addr"] = vaHex(Exp.Addr);
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_segments_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr(std::string("[]"));

  llvm::json::Array Arr;
  for (const auto &Seg : S->Img.Segments) {
    llvm::json::Object Obj;
    Obj["name"] = Seg.Name;
    Obj["va"] = vaHex(Seg.VA);
    Obj["size"] = vaHex(Seg.Size);
    std::string Flags;
    Flags += Seg.isReadable() ? 'R' : '-';
    Flags += Seg.isWritable() ? 'W' : '-';
    Flags += Seg.isExecutable() ? 'X' : '-';
    Obj["flags"] = Flags;
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_strings_json(neverd_session_t Sess, int MinLength) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr(std::string("[]"));

  unsigned MinLen = MinLength > 0 ? static_cast<unsigned>(MinLength) : 4;
  llvm::json::Array Arr;

  for (const auto &Seg : S->Img.Segments) {
    if (Seg.isExecutable())
      continue;
    const uint8_t *Data = Seg.Data.data();
    size_t Len = Seg.Data.size();
    size_t RunStart = 0, RunLen = 0;
    for (size_t I = 0; I <= Len; ++I) {
      uint8_t B = (I < Len) ? Data[I] : 0;
      bool IsPrintable = (B >= 0x20 && B < 0x7F) || B == '\t' || B == '\n';
      if (IsPrintable) {
        if (RunLen == 0)
          RunStart = I;
        ++RunLen;
      } else {
        if (RunLen >= MinLen && B == 0) {
          va_t StrAddr = Seg.VA + RunStart;
          llvm::json::Object Obj;
          Obj["addr"] = vaHex(StrAddr);
          Obj["value"] = std::string(
              reinterpret_cast<const char *>(Data + RunStart), RunLen);
          Obj["length"] = static_cast<int64_t>(RunLen);
          Arr.push_back(std::move(Obj));
        }
        RunLen = 0;
      }
    }
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_sections_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr("[]");

  llvm::json::Array Arr;
  for (const auto &Sec : S->Img.Sections) {
    llvm::json::Object Obj;
    Obj["name"] = Sec.Name;
    Obj["segment"] = Sec.SegmentName;
    Obj["va"] = vaHex(Sec.VA);
    Obj["size"] = static_cast<int64_t>(Sec.Size);
    Obj["file_off"] = static_cast<int64_t>(Sec.FileOff);
    Obj["file_sz"] = static_cast<int64_t>(Sec.FileSz);
    std::string Flags;
    Flags += Sec.isReadable() ? 'R' : '-';
    Flags += Sec.isWritable() ? 'W' : '-';
    Flags += Sec.isExecutable() ? 'X' : '-';
    Obj["flags"] = Flags;
    Obj["alignment"] = static_cast<int64_t>(Sec.Alignment);
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_symbols_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr("[]");

  llvm::json::Array Arr;
  for (const auto &Sym : S->Img.Symbols) {
    llvm::json::Object Obj;
    Obj["name"] = Sym.Name;
    Obj["addr"] = vaHex(Sym.Addr);
    Obj["size"] = static_cast<int64_t>(Sym.Size);
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_relocs_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr("[]");

  llvm::json::Array Arr;
  for (const auto &Rel : S->Img.Relocations) {
    llvm::json::Object Obj;
    Obj["addr"] = vaHex(Rel.Address);
    Obj["type"] = static_cast<int64_t>(Rel.Type);
    Obj["symbol"] = Rel.SymbolName;
    Obj["section"] = Rel.SectionName;
    Obj["addend"] = Rel.Addend;
    Obj["sym_index"] = static_cast<int64_t>(Rel.SymbolIndex);
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_headers_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr("{}");

  llvm::json::Object Root;
  Root["entry"] = vaHex(S->Img.Entry);
  Root["base"] = vaHex(S->Img.Base);
  Root["arch"] = getArchName(S->Img.Arch);
  Root["format"] = S->Img.getFormatName();
  Root["bits"] = S->Img.is64Bit() ? 64 : 32;
  Root["file_path"] = S->FilePath.string();

  std::error_code EC;
  auto FileSz = std::filesystem::file_size(S->FilePath, EC);
  Root["file_size"] = EC ? 0 : static_cast<int64_t>(FileSz);

  Root["segment_count"] = static_cast<int64_t>(S->Img.Segments.size());
  Root["section_count"] = static_cast<int64_t>(S->Img.Sections.size());
  Root["func_count"] = static_cast<int64_t>(S->Functions.size());
  Root["symbol_count"] = static_cast<int64_t>(S->Img.Symbols.size());
  Root["import_count"] = static_cast<int64_t>(S->Img.Imports.size());
  Root["export_count"] = static_cast<int64_t>(S->Img.Exports.size());
  Root["reloc_count"] = static_cast<int64_t>(S->Img.Relocations.size());
  Root["base_reloc_count"] =
      static_cast<int64_t>(S->Img.BaseRelocations.size());

  llvm::json::Object Dyn;
  const auto &DI = S->Img.DynInfo;
  if (!DI.SOName.empty())
    Dyn["soname"] = DI.SOName;
  if (!DI.NeededLibs.empty()) {
    llvm::json::Array Needed;
    for (const auto &Lib : DI.NeededLibs)
      Needed.push_back(Lib);
    Dyn["needed"] = std::move(Needed);
  }
  if (!DI.RPaths.empty()) {
    llvm::json::Array RP;
    for (const auto &P : DI.RPaths)
      RP.push_back(P);
    Dyn["rpaths"] = std::move(RP);
  }
  if (DI.InitAddr)
    Dyn["init_addr"] = vaHex(DI.InitAddr);
  if (DI.FiniAddr)
    Dyn["fini_addr"] = vaHex(DI.FiniAddr);
  if (!DI.PreinitArray.empty()) {
    llvm::json::Array PA;
    for (auto A : DI.PreinitArray)
      PA.push_back(vaHex(A));
    Dyn["preinit_array"] = std::move(PA);
  }
  if (!DI.InitArray.empty()) {
    llvm::json::Array IA;
    for (auto A : DI.InitArray)
      IA.push_back(vaHex(A));
    Dyn["init_array"] = std::move(IA);
  }
  if (!DI.FiniArray.empty()) {
    llvm::json::Array FA;
    for (auto A : DI.FiniArray)
      FA.push_back(vaHex(A));
    Dyn["fini_array"] = std::move(FA);
  }
  if (!DI.PDBPath.empty())
    Dyn["pdb_path"] = DI.PDBPath;
  if (!DI.UUID.empty())
    Dyn["uuid"] = DI.UUID;
  if (!DI.MinOSVersion.empty())
    Dyn["min_os_version"] = DI.MinOSVersion;
  if (DI.SecurityCookieRVA)
    Dyn["security_cookie_rva"] = vaHex(DI.SecurityCookieRVA);
  if (DI.GuardCFCheckFunctionRVA)
    Dyn["guard_cf_check_rva"] = vaHex(DI.GuardCFCheckFunctionRVA);
  Root["dynamic"] = std::move(Dyn);

  return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
}

const char *neverd_entrypoints_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded) {
    if (S)
      S->setError("session not loaded");
    return dupStr("[]");
  }
  const auto &Img = S->Img;
  const auto &DI = Img.DynInfo;

  llvm::json::Array Arr;

  auto addEntry = [&](const char *Type, neverd::va_t Addr,
                      const std::string &Name) {
    if (!Addr)
      return;
    llvm::json::Object O;
    O["type"] = Type;
    O["addr"] = vaHex(Addr);
    O["name"] = Name;
    Arr.push_back(std::move(O));
  };

  addEntry("entry", Img.Entry, Img.getFunctionNameAt(Img.Entry));
  addEntry("init", DI.InitAddr, Img.getFunctionNameAt(DI.InitAddr));
  addEntry("fini", DI.FiniAddr, Img.getFunctionNameAt(DI.FiniAddr));

  for (va_t Addr : DI.PreinitArray)
    addEntry("preinit_array", Addr, Img.getFunctionNameAt(Addr));
  for (va_t Addr : DI.InitArray)
    addEntry("init_array", Addr, Img.getFunctionNameAt(Addr));
  for (va_t Addr : DI.FiniArray)
    addEntry("fini_array", Addr, Img.getFunctionNameAt(Addr));

  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_dashboard_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr("{}");

  llvm::json::Object Root;

  llvm::json::Object File;
  File["path"] = S->FilePath.string();
  File["name"] = S->FilePath.filename().string();
  File["format"] = S->Img.getFormatName();
  File["arch"] = getArchName(S->Img.Arch);
  File["bits"] = S->Img.is64Bit() ? 64 : 32;
  File["entry"] = vaHex(S->Img.Entry);
  File["base"] = vaHex(S->Img.Base);
  File["endian"] = "LE";
  std::error_code EC;
  auto FileSz = std::filesystem::file_size(S->FilePath, EC);
  File["size"] = EC ? 0 : static_cast<int64_t>(FileSz);
  Root["file"] = std::move(File);

  llvm::json::Object Hashes;
  std::ifstream Ifs(S->FilePath, std::ios::binary);
  if (Ifs.is_open()) {
    llvm::MD5 Md5;
    llvm::SHA256 Sha;
    char Buf[8192];
    while (Ifs.read(Buf, sizeof(Buf)) || Ifs.gcount() > 0) {
      auto Count = static_cast<size_t>(Ifs.gcount());
      Md5.update(llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Buf),
                                         Count));
      Sha.update(llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Buf),
                                         Count));
    }
    llvm::MD5::MD5Result Md5Res;
    Md5.final(Md5Res);
    Hashes["md5"] = Md5Res.digest().str().lower();
    auto Sha256Res = Sha.final();
    std::string Sha256Hex;
    const char Digits[] = "0123456789abcdef";
    for (auto B : Sha256Res) {
      Sha256Hex += Digits[(B >> 4) & 0xF];
      Sha256Hex += Digits[B & 0xF];
    }
    Hashes["sha256"] = Sha256Hex;
  }
  Root["hashes"] = std::move(Hashes);

  llvm::json::Object Counts;
  Counts["functions"] = static_cast<int64_t>(S->Functions.size());
  Counts["imports"] = static_cast<int64_t>(S->Img.Imports.size());
  Counts["exports"] = static_cast<int64_t>(S->Img.Exports.size());
  int64_t StringCount = 0;
  for (const auto &Seg : S->Img.Segments) {
    if (Seg.isExecutable())
      continue;
    const uint8_t *Data = Seg.Data.data();
    size_t Len = Seg.Data.size();
    size_t RunLen = 0;
    for (size_t I = 0; I <= Len; ++I) {
      uint8_t B = (I < Len) ? Data[I] : 0;
      bool IsPrintable = (B >= 0x20 && B < 0x7F) || B == '\t' || B == '\n';
      if (IsPrintable) {
        ++RunLen;
      } else {
        if (RunLen >= 4 && B == 0)
          ++StringCount;
        RunLen = 0;
      }
    }
  }
  Counts["strings"] = StringCount;
  Counts["symbols"] = static_cast<int64_t>(S->Img.Symbols.size());
  Counts["segments"] = static_cast<int64_t>(S->Img.Segments.size());
  Counts["sections"] = static_cast<int64_t>(S->Img.Sections.size());
  Counts["relocations"] = static_cast<int64_t>(S->Img.Relocations.size());
  Counts["entrypoints"] = static_cast<int64_t>(
      (S->Img.Entry ? 1 : 0) + S->Img.DynInfo.PreinitArray.size() +
      S->Img.DynInfo.InitArray.size() + S->Img.DynInfo.FiniArray.size() +
      (S->Img.DynInfo.InitAddr ? 1 : 0) + (S->Img.DynInfo.FiniAddr ? 1 : 0));
  Root["counts"] = std::move(Counts);

  llvm::json::Array Libs;
  for (const auto &Lib : S->Img.DynInfo.NeededLibs)
    Libs.push_back(Lib);
  Root["libraries"] = std::move(Libs);

  return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
}

// ===--------------------------------------------------------------------===//
// XRefs
// ===--------------------------------------------------------------------===//

const char *neverd_xrefs_to_json(neverd_session_t Sess, neverd_va_t Addr) {
  auto *S = toSession(Sess);
  S->clearError();
  if (!S->ensurePipeline())
    return dupStr(std::string("[]"));

  llvm::json::Array Arr;
  for (const auto &F : S->PipeResult.LowFuncs)
    for (const auto &B : F.Blocks)
      for (const auto &Op : B.Ops)
        for (int I = 0; I < Op.NumInputs; ++I)
          if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == Addr) {
            llvm::json::Object Obj;
            Obj["from"] = vaHex(Op.Addr);
            Obj["func"] = F.Name;
            Obj["block"] = B.Id;
            Arr.push_back(std::move(Obj));
          }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_xrefs_from_json(neverd_session_t Sess, neverd_va_t Addr) {
  auto *S = toSession(Sess);
  S->clearError();
  if (!S->ensurePipeline())
    return dupStr(std::string("[]"));

  llvm::json::Array Arr;
  for (const auto &F : S->PipeResult.LowFuncs)
    for (const auto &B : F.Blocks)
      for (const auto &Op : B.Ops)
        if (Op.Addr == Addr)
          for (int I = 0; I < Op.NumInputs; ++I)
            if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset != 0) {
              llvm::json::Object Obj;
              Obj["to"] = vaHex(Op.Inputs[I].Offset);
              Obj["func"] = F.Name;
              Obj["opcode"] = std::string(ndOpName(Op.Opcode));
              Arr.push_back(std::move(Obj));
            }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_xrefs_scan(neverd_session_t Sess, const char *InputPath,
                              neverd_va_t Target) {
  PipelineRunner R;
  std::string Err;
  auto *S = static_cast<Session *>(Sess);
  if (!R.load(InputPath, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }
  PipelineOptions Opts;
  if (!R.run(Opts, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  llvm::json::Array Arr;
  for (const auto &F : R.Result.LowFuncs)
    for (const auto &B : F.Blocks)
      for (const auto &Op : B.Ops)
        for (int i = 0; i < Op.NumInputs; ++i)
          if (Op.Inputs[i].isConst() &&
              Op.Inputs[i].Offset == static_cast<va_t>(Target)) {
            llvm::json::Object Obj;
            Obj["from"] = vaHex(Op.Addr);
            Obj["func"] = F.Name;
            Obj["block"] = static_cast<int64_t>(B.Id);
            Arr.push_back(std::move(Obj));
          }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

// ===--------------------------------------------------------------------===//
// CFG graph
// ===--------------------------------------------------------------------===//

const char *neverd_cfg_json(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  S->clearError();
  if (!S->ensurePipeline())
    return dupStr(std::string("{}"));

  if (S->PipeResult.EVM) {
    if (FuncEntry != 0) {
      S->setError("EVM program entry is pc 0");
      return dupStr(std::string("{}"));
    }
    llvm::json::Array Nodes;
    llvm::json::Array Edges;
    for (const auto &Block : S->PipeResult.EVM->Low.Blocks) {
      llvm::json::Object Node;
      Node["id"] = static_cast<int64_t>(Block.StartPC);
      Node["start"] = vaHex(Block.StartPC);
      Node["end"] = vaHex(Block.EndPC);
      Node["insn_count"] = static_cast<int64_t>(Block.InstructionCount);
      Node["reachable"] = Block.Reachable;
      llvm::json::Array Lines;
      for (size_t I = Block.FirstInstruction;
           I < Block.FirstInstruction + Block.InstructionCount; ++I) {
        const auto &Instruction = S->PipeResult.EVM->Low.Instructions[I];
        Lines.push_back(vaHex(Instruction.PC) + ": " +
                        std::string(Instruction.Info.Name));
      }
      Node["disasm"] = std::move(Lines);
      Nodes.push_back(std::move(Node));
      for (const auto &Successor : Block.Successors) {
        llvm::json::Object Edge;
        Edge["from"] = static_cast<int64_t>(Block.StartPC);
        if (Successor.Target)
          Edge["to"] = static_cast<int64_t>(*Successor.Target);
        else
          Edge["to"] = nullptr;
        switch (Successor.Kind) {
        case evm::EdgeKind::ConditionalTrue:
          Edge["type"] = "true";
          break;
        case evm::EdgeKind::ConditionalFalse:
          Edge["type"] = "false";
          break;
        case evm::EdgeKind::Indirect:
          Edge["type"] = "indirect";
          break;
        default:
          Edge["type"] = "unconditional";
          break;
        }
        Edges.push_back(std::move(Edge));
      }
    }
    llvm::json::Object Root;
    Root["name"] = kEVMEntrySymbolName;
    Root["entry"] = "0x0";
    Root["nodes"] = std::move(Nodes);
    Root["edges"] = std::move(Edges);
    return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
  }

  const LowFunc *F = S->findLowFunc(FuncEntry);
  if (!F) {
    S->setError("function not found");
    return dupStr(std::string("{}"));
  }

  llvm::json::Array Nodes;
  llvm::json::Array Edges;

  for (const auto &B : F->Blocks) {
    llvm::json::Object Node;
    Node["id"] = B.Id;
    Node["start"] = vaHex(B.StartAddr);
    Node["end"] = vaHex(B.EndAddr);
    Node["insn_count"] = static_cast<int64_t>(B.Ops.size());

    llvm::json::Array DisasmLines;
    va_t Cur = B.StartAddr;
    while (Cur < B.EndAddr) {
      const Segment *Seg = S->Img.getSegmentFor(Cur);
      if (!Seg)
        break;
      uint64_t Off64 = Cur - Seg->VA;
      if (Off64 >= Seg->Data.size())
        break;
      size_t Off = static_cast<size_t>(Off64);
      size_t Avail = static_cast<size_t>(std::min<uint64_t>(
          16, std::min<uint64_t>(B.EndAddr - Cur,
                                 std::min<uint64_t>(Seg->Size - Off64,
                                                    Seg->Data.size() - Off))));
      if (Avail == 0)
        break;
      const uint8_t *Bytes = Seg->Data.data() + Off;
      DecodedInsn DI;
      int Sz = S->Dec.decodeOne(Bytes, Avail, Cur, DI);
      if (Sz <= 0)
        break;
      std::string Line = vaHex(Cur) + ": " + (DI.Raw ? DI.Raw->mnemonic : "") +
                         " " + (DI.Raw ? DI.Raw->op_str : "");
      DisasmLines.push_back(Line);
      Cur += Sz;
    }
    Node["disasm"] = std::move(DisasmLines);
    Nodes.push_back(std::move(Node));

    for (size_t I = 0; I < B.Succs.size(); ++I) {
      llvm::json::Object Edge;
      Edge["from"] = B.Id;
      Edge["to"] = B.Succs[I];
      if (B.Succs.size() == 2)
        Edge["type"] = (I == 0) ? "true" : "false";
      else
        Edge["type"] = "unconditional";
      Edges.push_back(std::move(Edge));
    }
  }

  llvm::json::Object Root;
  Root["name"] = F->Name;
  Root["entry"] = vaHex(F->Entry);
  Root["nodes"] = std::move(Nodes);
  Root["edges"] = std::move(Edges);
  return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
}

const char *neverd_cfg_dot(neverd_session_t Sess, const char *InputPath,
                           const char *FuncNameOrAddr, int Styled) {
  PipelineRunner R;
  std::string Err;
  auto *S = static_cast<Session *>(Sess);
  if (!R.load(InputPath, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }
  PipelineOptions Opts;
  if (S) {
    Opts.EVMFork = S->EVMFork;
    Opts.EVMStrict = S->EVMStrict;
  }
  if (!R.run(Opts, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  if (R.Result.EVM) {
    std::string Buf;
    llvm::raw_string_ostream OS(Buf);
    OS << "digraph cfg {\n"
          "  node [shape=box, fontname=\"Courier\", fontsize=10];\n";
    for (const auto &Block : R.Result.EVM->Low.Blocks)
      OS << "  bb" << Block.StartPC << " [label=\"PC 0x"
         << llvm::utohexstr(Block.StartPC) << "\"];\n";
    for (const auto &Block : R.Result.EVM->Low.Blocks)
      for (const auto &Edge : Block.Successors)
        if (Edge.Target)
          OS << "  bb" << Block.StartPC << " -> bb" << *Edge.Target
             << ";\n";
    OS << "}\n";
    return dupStr(Buf);
  }

  std::string FN(FuncNameOrAddr ? FuncNameOrAddr : "");
  const LowFunc *Target = nullptr;
  va_t TargetAddr = 0;
  bool HasTargetAddr = false;
  llvm::StringRef AddrRef(FN);
  if (!AddrRef.empty() && AddrRef.front() != '-') {
    if (AddrRef.consume_front("0x") || AddrRef.consume_front("0X"))
      HasTargetAddr = !AddrRef.empty();
    else
      HasTargetAddr = true;
    if (HasTargetAddr)
      HasTargetAddr = !AddrRef.getAsInteger(16, TargetAddr);
  }
  for (const auto &F : R.Result.LowFuncs)
    if (F.Name == FN || (HasTargetAddr && F.Entry == TargetAddr)) {
      Target = &F;
      break;
    }
  if (!Target) {
    if (S)
      S->setError("function not found: " + FN);
    return nullptr;
  }

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  OS << "digraph cfg {\n";
  if (Styled) {
    OS << "  node [shape=box, fontname=\"Courier\", fontsize=10, "
          "style=filled, fillcolor=\"#252526\", fontcolor=\"#ebebeb\", "
          "color=\"#3c3c3c\"];\n";
    OS << "  edge [color=\"#569cd6\"];\n";
    OS << "  bgcolor=\"#1e1e1e\";\n";
  } else {
    OS << "  node [shape=box, fontname=\"Courier\", fontsize=10];\n";
  }
  for (const auto &B : Target->Blocks)
    OS << "  bb" << B.Id << " [label=\"BB" << B.Id << " (0x"
       << llvm::utohexstr(B.StartAddr) << ")\"];\n";
  for (const auto &B : Target->Blocks)
    for (size_t J = 0; J < B.Succs.size(); ++J) {
      std::string Color = Styled ? "\"#569cd6\"" : "blue";
      if (B.Succs.size() == 2)
        Color = (J == 0) ? (Styled ? "\"#4ec9b0\"" : "green")
                         : (Styled ? "\"#f44747\"" : "red");
      OS << "  bb" << B.Id << " -> bb" << B.Succs[J] << " [color=" << Color
         << "];\n";
    }
  OS << "}\n";
  return dupStr(Buf);
}

// ===--------------------------------------------------------------------===//
// Call graph
// ===--------------------------------------------------------------------===//

const char *neverd_callgraph_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->ensurePipeline())
    return dupStr("{\"nodes\":[],\"edges\":[]}");

  std::map<va_t, std::string> FuncNames;
  for (const auto &F : S->Functions)
    FuncNames[F.Entry] = F.Name;

  llvm::json::Array Nodes;
  for (const auto &F : S->Functions) {
    llvm::json::Object N;
    N["name"] = F.Name;
    N["addr"] = vaHex(F.Entry);
    N["size"] = static_cast<int64_t>(F.Size);
    Nodes.push_back(std::move(N));
  }

  std::set<std::pair<va_t, va_t>> Seen;
  llvm::json::Array Edges;
  for (const auto &LF : S->PipeResult.LowFuncs) {
    for (const auto &B : LF.Blocks) {
      for (const auto &Op : B.Ops) {
        if (Op.Opcode != NdOp::CALL)
          continue;
        if (Op.NumInputs < 1 || !Op.Inputs[0].isConst())
          continue;
        va_t Tgt = Op.Inputs[0].Offset;
        if (FuncNames.find(Tgt) == FuncNames.end())
          continue;
        auto Key = std::make_pair(LF.Entry, Tgt);
        if (!Seen.insert(Key).second)
          continue;
        llvm::json::Object E;
        E["caller"] = vaHex(LF.Entry);
        E["callee"] = vaHex(Tgt);
        E["caller_name"] = LF.Name;
        E["callee_name"] = FuncNames[Tgt];
        Edges.push_back(std::move(E));
      }
    }
  }

  llvm::json::Object Result;
  Result["nodes"] = std::move(Nodes);
  Result["edges"] = std::move(Edges);
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}

// ===--------------------------------------------------------------------===//
// Address resolution
// ===--------------------------------------------------------------------===//

const char *neverd_resolve_addr(neverd_session_t Sess, neverd_va_t Addr) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return nullptr;

  llvm::json::Object Obj;

  for (const auto &F : S->Functions) {
    if (F.Entry == Addr) {
      Obj["type"] = "function";
      Obj["name"] = F.Name;
      Obj["addr"] = vaHex(F.Entry);
      return dupStr(jsonToString(llvm::json::Value(std::move(Obj))));
    }
  }

  if (const Import *Imp = S->Img.findImportAt(Addr)) {
    Obj["type"] = "import";
    Obj["name"] = Imp->Name;
    Obj["module"] = Imp->Module;
    Obj["addr"] = vaHex(Addr);
    return dupStr(jsonToString(llvm::json::Value(std::move(Obj))));
  }

  for (const auto &Exp : S->Img.Exports) {
    if (Exp.Addr == Addr) {
      Obj["type"] = "export";
      Obj["name"] = Exp.Name;
      Obj["addr"] = vaHex(Addr);
      return dupStr(jsonToString(llvm::json::Value(std::move(Obj))));
    }
  }

  for (const auto &Seg : S->Img.Segments) {
    if (!Seg.contains(Addr))
      continue;
    if (Seg.isExecutable())
      continue;
    uint64_t Off64 = Addr - Seg.VA;
    if (Off64 < Seg.Data.size()) {
      size_t Off = static_cast<size_t>(Off64);
      const uint8_t *P = Seg.Data.data() + Off;
      size_t Remain = Seg.Data.size() - Off;
      size_t Len = 0;
      while (Len < Remain && Len < 256 && P[Len] >= 0x20 && P[Len] < 0x7f)
        ++Len;
      if (Len >= 4 && (Len >= Remain || P[Len] == 0)) {
        Obj["type"] = "string";
        Obj["value"] = std::string(reinterpret_cast<const char *>(P), Len);
        Obj["addr"] = vaHex(Addr);
        return dupStr(jsonToString(llvm::json::Value(std::move(Obj))));
      }
    }
  }

  return nullptr;
}

// ===--------------------------------------------------------------------===//
// Search
// ===--------------------------------------------------------------------===//

const char *neverd_search_bytes(neverd_session_t Sess, const unsigned char *Pat,
                                int PatLen, int MaxResults) {
  auto *S = toSession(Sess);
  if (!S->Loaded || !Pat || PatLen <= 0)
    return dupStr("[]");

  int Limit = MaxResults > 0 ? MaxResults : 256;
  llvm::json::Array Arr;

  for (const auto &Seg : S->Img.Segments) {
    if (Seg.Data.size() < static_cast<size_t>(PatLen))
      continue;
    size_t End = Seg.Data.size() - PatLen;
    for (size_t I = 0; I <= End; ++I) {
      if (std::memcmp(Seg.Data.data() + I, Pat, PatLen) == 0) {
        llvm::json::Object Hit;
        Hit["addr"] = vaHex(Seg.VA + I);
        Hit["segment"] = Seg.Name;
        Hit["offset"] = static_cast<int64_t>(I);
        Arr.push_back(std::move(Hit));
        if (static_cast<int>(Arr.size()) >= Limit)
          break;
      }
    }
    if (static_cast<int>(Arr.size()) >= Limit)
      break;
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_search_string(neverd_session_t Sess, const char *Pattern,
                                 int CaseSensitive, int MaxResults) {
  auto *S = toSession(Sess);
  if (!S->Loaded || !Pattern || Pattern[0] == '\0')
    return dupStr("[]");

  int Limit = MaxResults > 0 ? MaxResults : 256;
  std::string Pat(Pattern);
  std::string PatLower;
  if (!CaseSensitive) {
    PatLower.resize(Pat.size());
    std::transform(Pat.begin(), Pat.end(), PatLower.begin(), [](char Ch) {
      return static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
    });
  }

  llvm::json::Array Arr;

  for (const auto &Seg : S->Img.Segments) {
    if (Seg.Data.size() < Pat.size())
      continue;
    size_t End = Seg.Data.size() - Pat.size();
    for (size_t I = 0; I <= End; ++I) {
      bool Match;
      if (CaseSensitive) {
        Match = std::memcmp(Seg.Data.data() + I, Pat.data(), Pat.size()) == 0;
      } else {
        Match = true;
        for (size_t J = 0; J < Pat.size(); ++J) {
          unsigned char Hay = static_cast<unsigned char>(
              std::tolower(static_cast<unsigned char>(Seg.Data[I + J])));
          if (Hay != static_cast<unsigned char>(PatLower[J])) {
            Match = false;
            break;
          }
        }
      }
      if (Match) {
        llvm::json::Object Hit;
        Hit["addr"] = vaHex(Seg.VA + I);
        Hit["segment"] = Seg.Name;
        size_t CtxEnd = std::min(I + Pat.size() + 32, Seg.Data.size());
        std::string Ctx(reinterpret_cast<const char *>(Seg.Data.data() + I),
                        std::min(CtxEnd - I, static_cast<size_t>(64)));
        for (auto &C : Ctx)
          if (static_cast<unsigned char>(C) < 0x20 || C == 0x7f)
            C = '.';
        Hit["context"] = Ctx;
        Arr.push_back(std::move(Hit));
        if (static_cast<int>(Arr.size()) >= Limit)
          break;
      }
    }
    if (static_cast<int>(Arr.size()) >= Limit)
      break;
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

// ===--------------------------------------------------------------------===//
// Binary diff
// ===--------------------------------------------------------------------===//

const char *neverd_diff_functions(neverd_session_t SessA,
                                  neverd_session_t SessB) {
  auto *A = toSession(SessA);
  auto *B = toSession(SessB);
  if (!A->Loaded || !B->Loaded)
    return dupStr("{}");

  std::map<std::string, int> MapA, MapB;
  for (int I = 0; I < static_cast<int>(A->Functions.size()); ++I)
    MapA[A->Functions[I].Name] = I;
  for (int I = 0; I < static_cast<int>(B->Functions.size()); ++I)
    MapB[B->Functions[I].Name] = I;

  llvm::json::Array Matched, Added, Removed;

  for (const auto &[Name, IdxA] : MapA) {
    auto It = MapB.find(Name);
    if (It != MapB.end()) {
      int IdxB = It->second;
      llvm::json::Object Obj;
      Obj["name"] = Name;
      Obj["addr_a"] = vaHex(A->Functions[IdxA].Entry);
      Obj["addr_b"] = vaHex(B->Functions[IdxB].Entry);
      Obj["size_a"] = static_cast<int64_t>(A->Functions[IdxA].Size);
      Obj["size_b"] = static_cast<int64_t>(B->Functions[IdxB].Size);
      Obj["same_size"] = (A->Functions[IdxA].Size == B->Functions[IdxB].Size);
      Matched.push_back(std::move(Obj));
    } else {
      llvm::json::Object Obj;
      Obj["name"] = Name;
      Obj["addr"] = vaHex(A->Functions[IdxA].Entry);
      Obj["size"] = static_cast<int64_t>(A->Functions[IdxA].Size);
      Removed.push_back(std::move(Obj));
    }
  }

  for (const auto &[Name, IdxB] : MapB) {
    if (MapA.find(Name) == MapA.end()) {
      llvm::json::Object Obj;
      Obj["name"] = Name;
      Obj["addr"] = vaHex(B->Functions[IdxB].Entry);
      Obj["size"] = static_cast<int64_t>(B->Functions[IdxB].Size);
      Added.push_back(std::move(Obj));
    }
  }

  llvm::json::Object Result;
  Result["matched"] = std::move(Matched);
  Result["added"] = std::move(Added);
  Result["removed"] = std::move(Removed);
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}

const char *neverd_diff_decompile(neverd_session_t SessA, neverd_va_t EntryA,
                                  neverd_session_t SessB, neverd_va_t EntryB) {
  const char *CodeA = neverd_decompile(SessA, EntryA);
  const char *CodeB = neverd_decompile(SessB, EntryB);

  llvm::json::Object Result;
  Result["code_a"] = CodeA ? CodeA : "";
  Result["code_b"] = CodeB ? CodeB : "";
  bool Identical = CodeA && CodeB && std::strcmp(CodeA, CodeB) == 0;
  Result["identical"] = Identical;

  if (CodeA)
    free(const_cast<char *>(CodeA));
  if (CodeB)
    free(const_cast<char *>(CodeB));

  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}
