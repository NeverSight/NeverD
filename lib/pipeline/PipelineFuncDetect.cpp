//===- PipelineFuncDetect.cpp - Function detection and symbol merging
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Function entry-point detection, debug symbol merging, thunk stub
/// recognition, and function name map construction.
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <set>

#define DEBUG_TYPE "neverd-pipeline"

namespace neverd {

namespace {

bool isELFImportStub(const BinaryImage &Img, va_t Addr) {
  if (!Img.isELF())
    return false;

  for (const auto &Sec : Img.Sections) {
    if (!Sec.isExecutable() || !Sec.contains(Addr))
      continue;
    llvm::StringRef Name = Sec.Name;
    if (Name == section_names::elf::Plt ||
        Name.starts_with(section_names::elf::PltPrefix) ||
        Name == section_names::elf::Iplt)
      return true;
  }
  return false;
}

bool isELFRuntimeScaffold(llvm::StringRef Name) {
  return Name == "call_weak_fn" || Name == "deregister_tm_clones" ||
         Name == "register_tm_clones" || Name == "__do_global_dtors_aux" ||
         Name == "frame_dummy" || Name == "__libc_csu_init" ||
         Name == "__libc_csu_fini" || Name == "__libc_csu_irel" ||
         Name == "_dl_relocate_static_pie";
}

} // namespace

//===----------------------------------------------------------------------===//
// Debug symbol merging
//===----------------------------------------------------------------------===//

void Pipeline::mergeDebugSymbols(
    std::vector<std::pair<va_t, std::string>> &FuncEntries, DebugContext &Dbg) {
  std::map<va_t, std::string> Existing;
  for (auto &[Addr, FName] : FuncEntries)
    Existing[Addr] = FName;

  std::vector<va_t> SortedAddrs;
  SortedAddrs.reserve(Existing.size());
  for (auto &[Addr, Unused] : Existing)
    SortedAddrs.push_back(Addr);
  std::sort(SortedAddrs.begin(), SortedAddrs.end());

  auto DbgFuncs = Dbg.allFunctions();
  int Merged = 0, Added = 0, Skipped = 0;
  for (auto &DF : DbgFuncs) {
    auto It = Existing.find(DF.Addr);
    if (It != Existing.end()) {
      bool IsSynthetic = It->second.starts_with(kAutoFuncPrefix) ||
                         It->second.starts_with("func_");
      if (IsSynthetic || It->second.empty()) {
        for (auto &[EA, EN] : FuncEntries)
          if (EA == DF.Addr) {
            EN = DF.Name;
            break;
          }
        ++Merged;
      }
    } else {
      auto Pos =
          std::lower_bound(SortedAddrs.begin(), SortedAddrs.end(), DF.Addr);
      if (Pos != SortedAddrs.begin()) {
        va_t PrevFunc = *std::prev(Pos);
        va_t NextFunc = (Pos != SortedAddrs.end()) ? *Pos : UINT64_MAX;
        constexpr uint64_t kMaxOverlapDistance = limits::kMaxOverlapDistance;
        if (DF.Addr > PrevFunc && DF.Addr < NextFunc &&
            (DF.Addr - PrevFunc) < kMaxOverlapDistance) {
          LLVM_DEBUG(llvm::dbgs()
                     << "pipeline: skipping overlapping debug func " << DF.Name
                     << " @ 0x" << llvm::utohexstr(DF.Addr)
                     << " (inside detected func @ 0x"
                     << llvm::utohexstr(PrevFunc) << ")\n");
          ++Skipped;
          continue;
        }
      }
      FuncEntries.emplace_back(DF.Addr, DF.Name);
      ++Added;
    }
  }
  if (Merged || Added || Skipped)
    LLVM_DEBUG(llvm::dbgs()
               << "pipeline: debug symbols merged=" << Merged
               << " added=" << Added << " skipped=" << Skipped << "\n");
}

//===----------------------------------------------------------------------===//
// Thunk stub detection
//===----------------------------------------------------------------------===//

void Pipeline::detectThunkStubs(const std::vector<LowFunc> &LowFuncs,
                                std::map<va_t, std::string> &AllFuncNames) {
  for (auto &LF : LowFuncs) {
    if (LF.Blocks.size() != 1 || LF.Blocks[0].Ops.size() < 1)
      continue;
    auto &Ops = LF.Blocks[0].Ops;
    for (size_t I = 0; I < Ops.size(); ++I) {
      if (Ops[I].Opcode == NdOp::INDIR_BR && Ops[I].NumInputs >= 1 &&
          Ops[I].Inputs[0].isConst()) {
        auto It = AllFuncNames.find(Ops[I].Inputs[0].Offset);
        if (It != AllFuncNames.end())
          AllFuncNames[LF.Entry] = It->second;
        break;
      }
      if (Ops[I].Opcode == NdOp::LOAD && I + 1 < Ops.size() &&
          Ops[I + 1].Opcode == NdOp::INDIR_BR && Ops[I].NumInputs >= 1 &&
          Ops[I].Inputs[0].isConst()) {
        auto It = AllFuncNames.find(Ops[I].Inputs[0].Offset);
        if (It != AllFuncNames.end())
          AllFuncNames[LF.Entry] = It->second;
        break;
      }
    }
  }
}

//===----------------------------------------------------------------------===//
// Function name map
//===----------------------------------------------------------------------===//

std::map<va_t, std::string>
Pipeline::buildFuncNameMap(const BinaryImage &Img,
                           const PipelineResult &Result) {
  std::map<va_t, std::string> Names;
  for (auto &LF : Result.LowFuncs)
    Names[LF.Entry] = LF.Name;
  for (auto &Imp : Img.Imports)
    if (Imp.IATAddr != 0)
      Names[Imp.IATAddr] = Imp.Name;
  return Names;
}

//===----------------------------------------------------------------------===//
// Function detection
//===----------------------------------------------------------------------===//

std::vector<std::pair<va_t, std::string>>
Pipeline::detectFunctions(const BinaryImage &Img, Decoder &Dec,
                          const PipelineOptions &Opts, DebugContext *Dbg) {
  FuncDetector Detector;
  auto FuncEntries = Detector.detect(Img, Dec);
  LLVM_DEBUG(llvm::dbgs() << "pipeline: detected " << FuncEntries.size()
                          << " functions\n");

  if (Dbg && Dbg->hasInfo())
    mergeDebugSymbols(FuncEntries, *Dbg);

  std::set<va_t> StubAddrs;
  for (auto &Imp : Img.Imports)
    if (Imp.IATAddr != 0)
      StubAddrs.insert(Imp.IATAddr);

  std::set<va_t> RuntimeAddrs;
  if (Opts.PatchMode && Img.isELF()) {
    if (Img.DynInfo.InitAddr != 0)
      RuntimeAddrs.insert(Img.DynInfo.InitAddr);
    if (Img.DynInfo.FiniAddr != 0)
      RuntimeAddrs.insert(Img.DynInfo.FiniAddr);
    RuntimeAddrs.insert(Img.DynInfo.InitArray.begin(),
                        Img.DynInfo.InitArray.end());
    RuntimeAddrs.insert(Img.DynInfo.FiniArray.begin(),
                        Img.DynInfo.FiniArray.end());
    RuntimeAddrs.erase(0);
  }

  std::vector<std::pair<va_t, std::string>> Candidates;
  Candidates.reserve(Opts.MaxFunctions > 0
                         ? std::min(FuncEntries.size(), Opts.MaxFunctions + 64)
                         : FuncEntries.size());
  for (auto &[Entry, FName] : FuncEntries) {
    if (Opts.MaxFunctions > 0 && Candidates.size() >= Opts.MaxFunctions * 2)
      break;
    // ELF import metadata points at the GOT slot, while direct calls target
    // executable PLT stubs.  Treat the whole family of PLT sections as import
    // machinery so lazy-binding register conventions remain intact.
    if (StubAddrs.count(Entry) || isELFImportStub(Img, Entry))
      continue;
    // A process entry point follows the platform loader ABI, not an ordinary
    // C function ABI.  Recompiling ELF `_start`, for example, discards the
    // register setup that passes main/argc/argv to __libc_start_main.  Keep
    // the original entry point in patch mode; calls it makes still reach
    // rewritten functions through their installed trampolines.
    if (Opts.PatchMode && Img.isELF() && Entry == Img.Entry)
      continue;
    // ELF startup/finalization routines run under dynamic-loader and CRT
    // conventions.  Preserve both recorded dynamic entry points and their
    // standard helper functions; user code remains eligible for rewriting.
    if (Opts.PatchMode && Img.isELF() &&
        (RuntimeAddrs.count(Entry) || isELFRuntimeScaffold(FName)))
      continue;
    Candidates.push_back({Entry, FName});
  }

  return Candidates;
}

} // namespace neverd
