//===- NeverDCAPI.cpp - C API: session lifecycle and core queries
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Session lifecycle, function list, lookup helpers, raw-byte access,
/// error handling, memory management, session metadata, and version.
///
/// Additional C API domains live in sibling files:
///   NeverDCAPIDisasm.cpp      — disassembly (JSON + text)
///   NeverDCAPIDecompile.cpp   — decompile, IR dump, lift, inject
///   NeverDCAPIPatch.cpp       — patch operations
///   NeverDCAPIQuery.cpp       — info panels, xrefs, CFG, search, diff
///   NeverDCAPIPersist.cpp     — annotations, renames, FLIRT sigs
///   NeverDCAPIBench.cpp       — benchmark support
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/Common.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/debug/DWARFLoader.h"
#include "neverd/sdk/NeverDPlugin.h"

#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <iterator>

using namespace neverd;
using namespace neverd::sdk;

namespace neverd::sdk {

bool Session::synchronizeFunctions() {
  if (Img.Arch != Arch::SBF)
    return true;
  if (SBFFunctionsSynchronized)
    return true;
  if (!ensurePipeline() || !PipeResult.SBF)
    return false;

  resetFunctionsFromImage();

  const sbf::SBFProgram &Program = *PipeResult.SBF;
  for (const sbf::Function &Function : Program.High.Functions) {
    uint64_t Size = 0;
    for (size_t BlockID : Function.Blocks) {
      if (BlockID >= Program.Low.Blocks.size())
        continue;
      const sbf::BasicBlock &Block = Program.Low.Blocks[BlockID];
      Size += (Block.EndSlot - Block.StartSlot) * sbf::kInstructionSize;
    }

    auto Existing = std::find_if(
        Functions.begin(), Functions.end(),
        [&](const FuncInfo &Info) { return Info.Entry == Function.Address; });
    if (Existing == Functions.end()) {
      Functions.push_back({Function.Address, Size, Function.Name});
      Existing = std::prev(Functions.end());
    } else if (Existing->Size == 0) {
      Existing->Size = Size;
    }
    OriginalNames.try_emplace(Function.Address, Function.Name);
    if (auto Rename = Renames.find(Function.Address); Rename != Renames.end())
      Existing->Name = Rename->second;
  }
  std::sort(Functions.begin(), Functions.end(),
            [](const FuncInfo &Left, const FuncInfo &Right) {
              return Left.Entry < Right.Entry;
            });
  SBFFunctionsSynchronized = true;
  return true;
}

} // namespace neverd::sdk

// ===--------------------------------------------------------------------===//
// PipelineRunner implementation (shared by high-level C API functions)
// ===--------------------------------------------------------------------===//

bool PipelineRunner::load(const char *InputPath, std::string &Err) {
  if (!InputPath || InputPath[0] == '\0') {
    Err = "input path is empty";
    return false;
  }
  auto Path = std::filesystem::path(InputPath);
  if (!std::filesystem::exists(Path)) {
    Err = "file not found: " + Path.string();
    return false;
  }
  auto ImgOrErr = loadBinary(Path);
  if (!ImgOrErr) {
    llvm::handleAllErrors(
        ImgOrErr.takeError(),
        [&](const llvm::ErrorInfoBase &E) { Err = E.message(); });
    return false;
  }
  Img = std::move(*ImgOrErr);
  if (Img.Arch != Arch::EVM && Img.Arch != Arch::SBF)
    Dbg = DWARFDebugContext::load(Path, Img.Format);
  return true;
}

bool PipelineRunner::run(PipelineOptions Opts, std::string &Err) {
  Pipeline ThePipeline;
  Result = ThePipeline.run(Img, LLVMCtx, Opts, Dbg.get());
  if (!Result.Success) {
    Err = Result.Error.empty() ? "pipeline failed" : Result.Error;
    return false;
  }
  return true;
}

// ===--------------------------------------------------------------------===//
// Session lifecycle
// ===--------------------------------------------------------------------===//

neverd_session_t neverd_session_create(void) { return new Session(); }

void neverd_session_destroy(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return;
  S->PM.termAll();
  delete S;
}

int neverd_session_load(neverd_session_t Sess, const char *Path) {
  auto *S = toSession(Sess);
  if (!S)
    return 0;
  S->clearError();

  if (!Path || Path[0] == '\0') {
    S->setError("input path is empty");
    return 0;
  }
  auto P = std::filesystem::path(Path);
  if (!std::filesystem::exists(P)) {
    S->setError(std::string("file not found: ") + Path);
    return 0;
  }

  auto ImgOrErr = loadBinary(P);
  if (!ImgOrErr) {
    std::string Err;
    llvm::raw_string_ostream OS(Err);
    logAllUnhandledErrors(ImgOrErr.takeError(), OS);
    S->setError(Err);
    return 0;
  }

  S->Img = std::move(*ImgOrErr);
  S->FilePath = P;
  S->Loaded = true;
  S->Annotations.clear();
  S->Renames.clear();
  S->invalidatePipeline();

  if (S->Img.Arch != Arch::EVM && S->Img.Arch != Arch::SBF &&
      !S->Dec.init(S->Img.Arch, S->Img.Mode)) {
    S->setError("failed to init decoder for arch");
    S->Loaded = false;
    return 0;
  }

  neverd_annotations_load(Sess);
  neverd_renames_load(Sess);

  return 1;
}

int neverd_session_is_loaded(neverd_session_t Sess) {
  return toSession(Sess)->Loaded ? 1 : 0;
}

int neverd_session_analyze(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return (S->ensurePipeline() && S->synchronizeFunctions()) ? 1 : 0;
}

const char *neverd_session_file_path(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->Loaded ? dupStr(S->FilePath.string()) : dupStr(std::string());
}

const char *neverd_session_arch_name(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return dupStr(S->Loaded ? getArchName(S->Img.Arch) : "unknown");
}

const char *neverd_session_format_name(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return dupStr(S->Loaded ? S->Img.getFormatName() : "unknown");
}

int neverd_session_is_64bit(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return (S->Loaded && S->Img.is64Bit()) ? 1 : 0;
}

int neverd_session_bitness(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded)
    return 0;
  return static_cast<int>(getBitnessValue(S->Img.Bits));
}

// ===--------------------------------------------------------------------===//
// Function list
// ===--------------------------------------------------------------------===//

int neverd_func_count(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded || !S->synchronizeFunctions())
    return 0;
  return static_cast<int>(S->Functions.size());
}

neverd_va_t neverd_func_entry(neverd_session_t Sess, int Idx) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded || !S->synchronizeFunctions() || Idx < 0 ||
      Idx >= static_cast<int>(S->Functions.size()))
    return 0;
  return S->Functions[Idx].Entry;
}

int neverd_func_size(neverd_session_t Sess, int Idx) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded || !S->synchronizeFunctions() || Idx < 0 ||
      Idx >= static_cast<int>(S->Functions.size()))
    return 0;
  return static_cast<int>(S->Functions[Idx].Size);
}

const char *neverd_func_name(neverd_session_t Sess, int Idx) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded || !S->synchronizeFunctions() || Idx < 0 ||
      Idx >= static_cast<int>(S->Functions.size()))
    return dupStr(std::string());
  return dupStr(S->Functions[Idx].Name);
}

// ===--------------------------------------------------------------------===//
// Function lookup helpers
// ===--------------------------------------------------------------------===//

int neverd_func_find_by_name(neverd_session_t Sess, const char *Name) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded || !Name || !S->synchronizeFunctions())
    return -1;
  for (size_t I = 0; I < S->Functions.size(); ++I)
    if (S->Functions[I].Name == Name)
      return static_cast<int>(I);
  return -1;
}

int neverd_func_find_by_addr(neverd_session_t Sess, neverd_va_t Addr) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded || !S->synchronizeFunctions())
    return -1;
  for (size_t I = 0; I < S->Functions.size(); ++I)
    if (S->Functions[I].Entry == Addr)
      return static_cast<int>(I);
  return -1;
}

// ===--------------------------------------------------------------------===//
// Raw bytes
// ===--------------------------------------------------------------------===//

int neverd_read_bytes(neverd_session_t Sess, neverd_va_t Addr,
                      unsigned char *Buf, int Size) {
  auto *S = toSession(Sess);
  if (!S->Loaded || !Buf || Size <= 0)
    return 0;

  int BytesRead = 0;
  va_t Cur = Addr;
  int Remaining = Size;

  while (Remaining > 0) {
    const Segment *Seg = S->Img.getSegmentFor(Cur);
    if (!Seg)
      break;

    uint64_t OffInSeg64 = Cur - Seg->VA;
    if (OffInSeg64 >= Seg->Data.size())
      break;

    size_t OffInSeg = static_cast<size_t>(OffInSeg64);
    uint64_t VirtualAvail = Seg->Size - OffInSeg64;
    uint64_t MaterializedAvail = Seg->Data.size() - OffInSeg;
    size_t Chunk = static_cast<size_t>(
        std::min<uint64_t>(static_cast<uint64_t>(Remaining),
                           std::min(VirtualAvail, MaterializedAvail)));
    if (Chunk == 0)
      break;

    std::memcpy(Buf + BytesRead, Seg->Data.data() + OffInSeg, Chunk);
    BytesRead += static_cast<int>(Chunk);
    Cur += Chunk;
    Remaining -= static_cast<int>(Chunk);
  }
  return BytesRead;
}

// ===--------------------------------------------------------------------===//
// Error handling + memory management
// ===--------------------------------------------------------------------===//

const char *neverd_last_error(neverd_session_t Sess) {
  return dupStr(toSession(Sess)->LastError);
}

void neverd_free_string(const char *Str) { free(const_cast<char *>(Str)); }

// ===--------------------------------------------------------------------===//
// Session metadata
// ===--------------------------------------------------------------------===//

unsigned long long neverd_session_file_size(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return 0;
  std::error_code EC;
  auto Sz = std::filesystem::file_size(S->FilePath, EC);
  return EC ? 0 : static_cast<unsigned long long>(Sz);
}

neverd_va_t neverd_session_base_addr(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->Loaded ? S->Img.Base : 0;
}

neverd_va_t neverd_session_entry_addr(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->Loaded ? S->Img.Entry : 0;
}

int neverd_session_segment_count(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->Loaded ? static_cast<int>(S->Img.Segments.size()) : 0;
}

int neverd_session_section_count(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->Loaded ? static_cast<int>(S->Img.Sections.size()) : 0;
}

int neverd_session_import_count(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->Loaded ? static_cast<int>(S->Img.Imports.size()) : 0;
}

int neverd_session_export_count(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->Loaded ? static_cast<int>(S->Img.Exports.size()) : 0;
}

int neverd_session_symbol_count(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S->Loaded ? static_cast<int>(S->Img.Symbols.size()) : 0;
}

const char *neverd_hex_dump(neverd_session_t Sess, neverd_va_t Addr, int Size) {
  auto *S = toSession(Sess);
  if (!S->Loaded || Size <= 0)
    return nullptr;

  std::vector<uint8_t> Buf(static_cast<size_t>(Size));
  int Got = neverd_read_bytes(Sess, Addr, Buf.data(), Size);
  if (Got <= 0)
    return nullptr;

  std::string Out;
  llvm::raw_string_ostream OS(Out);
  for (int I = 0; I < Got; I += 16) {
    OS << "0x" << llvm::utohexstr(Addr + I) << "  ";
    int LineLen = std::min(16, Got - I);
    for (int J = 0; J < 16; ++J) {
      if (J < LineLen)
        OS << llvm::format("%02x ", Buf[I + J]);
      else
        OS << "   ";
      if (J == 7)
        OS << " ";
    }
    OS << " |";
    for (int J = 0; J < LineLen; ++J) {
      uint8_t B = Buf[I + J];
      OS << static_cast<char>((B >= 0x20 && B < 0x7F) ? B : '.');
    }
    OS << "|\n";
  }
  return dupStr(Out);
}

// ===--------------------------------------------------------------------===//
// Plugin management
// ===--------------------------------------------------------------------===//

int neverd_plugins_load_dir(neverd_session_t Sess, const char *Dir) {
  auto *S = toSession(Sess);
  if (!S || !Dir) {
    if (S)
      S->setError("plugin directory path is null");
    return 0;
  }
  S->clearError();
  const int Loaded = S->PM.loadPluginsFromDir(Dir);
  if (!S->PM.lastError().empty())
    S->setError(S->PM.lastError());
  return Loaded;
}

int neverd_plugins_load_file(neverd_session_t Sess, const char *Path) {
  auto *S = toSession(Sess);
  if (!S || !Path) {
    if (S)
      S->setError("plugin file path is null");
    return 0;
  }
  S->clearError();
  if (!S->PM.loadPluginFile(Path)) {
    S->setError(S->PM.lastError());
    return 0;
  }
  return 1;
}

const char *neverd_plugins_list_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  llvm::json::Array Arr;
  for (const auto &P : S->PM.plugins()) {
    const neverd_plugin_t &Descriptor = P.descriptor();
    llvm::json::Object Obj;
    Obj["name"] = Descriptor.Name ? Descriptor.Name : "";
    Obj["version"] = Descriptor.Version ? Descriptor.Version : "";
    Obj["author"] = Descriptor.Author ? Descriptor.Author : "";
    Obj["description"] = Descriptor.Description ? Descriptor.Description : "";
    Obj["type"] = static_cast<int64_t>(Descriptor.Type);
    Obj["kind"] = P.Runtime->kind();
    Obj["path"] = P.Path;
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

void neverd_plugins_init(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return;
  S->clearError();
  S->PM.initAll(Sess);
  if (!S->PM.lastError().empty())
    S->setError(S->PM.lastError());
}

void neverd_plugins_term(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return;
  S->clearError();
  S->PM.termAll();
  if (!S->PM.lastError().empty())
    S->setError(S->PM.lastError());
}

int neverd_plugins_run(neverd_session_t Sess, const char *Name, int Arg) {
  auto *S = toSession(Sess);
  if (!S || !Name)
    return -1;
  S->clearError();
  const int Result = S->PM.runPlugin(Name, Sess, Arg);
  if (Result == -1 && !S->PM.lastError().empty())
    S->setError(S->PM.lastError());
  return Result;
}

int neverd_plugins_count(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return S ? static_cast<int>(S->PM.plugins().size()) : 0;
}

void neverd_plugins_dispatch_event(neverd_session_t Sess, const void *Event) {
  if (!Event)
    return;
  auto *S = toSession(Sess);
  if (!S)
    return;
  S->clearError();
  S->PM.dispatchEvent(*static_cast<const neverd_event_t *>(Event));
  if (!S->PM.lastError().empty())
    S->setError(S->PM.lastError());
}

// ===--------------------------------------------------------------------===//
// Version info
// ===--------------------------------------------------------------------===//

const char *neverd_version(void) {
  return dupStr(std::string(ProjectName) + " v" + VersionString);
}

const char *neverd_project_name(void) { return dupStr(ProjectName); }

const char *neverd_version_number(void) { return dupStr(VersionString); }
