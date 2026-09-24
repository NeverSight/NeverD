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
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/pipeline/Pipeline.h"

#include <algorithm>
#include <set>

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-pipeline"

namespace neverd {

namespace {

bool isELFRuntimeScaffold(llvm::StringRef Name) {
  return llvm::StringSwitch<bool>(Name)
#define ELF_RUNTIME_SYMBOL(Symbol) .Case(Symbol, true)
#include "neverd/object/ELFRuntimeSymbols.inc"
#undef ELF_RUNTIME_SYMBOL
      .Default(false);
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
      if (isSynthesizedFuncName(It->second)) {
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
      if (Ops[I].Opcode == NdOp::LOAD &&
          Ops[I].MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          I + 1 < Ops.size() &&
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
  for (const auto &[Addr, Name] : Img.getImportAddressNames())
    Names[Addr] = Name;
  return Names;
}

//===----------------------------------------------------------------------===//
// Function detection
//===----------------------------------------------------------------------===//

std::vector<std::pair<va_t, std::string>>
Pipeline::detectFunctions(const BinaryImage &Img, Decoder &Dec,
                          const PipelineOptions &Opts, DebugContext *Dbg,
                          PipelineResult &Result) {
  std::vector<std::pair<va_t, std::string>> FuncEntries;
  if (!Opts.OnlyFunctionEntries.empty()) {
    // Single-function CLI export must not scan the whole image: call-target
    // detection on a 100k-function PE is the work we are trying to skip.
    // Catch, C++ unwind funclets, and out-of-line SEH filters are separate
    // pdata functions; include them so HighC can embed catch/dtor bodies and
    // native SEH lowering can authenticate the filter callback.
    std::set<va_t> Wanted(Opts.OnlyFunctionEntries.begin(),
                          Opts.OnlyFunctionEntries.end());
    std::vector<va_t> Work(Wanted.begin(), Wanted.end());
    for (size_t I = 0; I < Work.size(); ++I) {
      const ExceptionFunction *EH =
          Img.ExceptionMetadata.findFunction(Work[I]);
      if (!EH)
        continue;
      // Interior VAs inherit a containing pdata.  A huge or merged owner
      // must not enqueue every catch/unwind thunk in that range; keep only
      // nearby out-of-line funclets so `--func` can still attach a local
      // destructor body.
      const bool HugeOwner =
          EH->CodeRange.size() > limits::kMaxOnlyFunctionEHOwnerSize;
      auto addOutOfLine = [&](va_t Addr) {
        if (!Addr || EH->CodeRange.contains(Addr))
          return;
        // An interior VA of a foreign pdata owner would CFG-decode that
        // whole body.  MSVC template unwind actions often land inside a
        // megabyte-scale function; `--func` only needs a local start.
        const ExceptionFunction *Owner =
            Img.ExceptionMetadata.findFunction(Addr);
        if (Owner && Owner->CodeRange.Begin != Addr)
          return;
        const uint64_t Dist =
            Addr >= Work[I] ? Addr - Work[I] : Work[I] - Addr;
        if (HugeOwner && Dist > limits::kMaxOverlapDistance)
          return;
        if (Owner &&
            Owner->CodeRange.size() > limits::kMaxOnlyFunctionEHOwnerSize &&
            Dist > limits::kMaxOverlapDistance)
          return;
        if (Wanted.insert(Addr).second)
          Work.push_back(Addr);
      };
      if (EH->Cxx) {
        for (const CxxTryBlock &Try : EH->Cxx->TryBlocks)
          for (const CxxCatchHandler &Handler : Try.Handlers)
            addOutOfLine(Handler.HandlerVA);
        for (const CxxUnwindAction &Action : EH->Cxx->UnwindMap)
          addOutOfLine(Action.ActionVA);
      }
      if (EH->SEH) {
        for (const SEHScopeRecord &Scope : EH->SEH->Scopes)
          addOutOfLine(Scope.FilterOrFinallyVA);
      }
    }
    FuncEntries.reserve(Wanted.size());
    for (va_t Addr : Wanted) {
      // `--func` already chose the work set.  Do not scan every image
      // symbol to prove the VA is a "known" start: C++ unwind ActionVAs
      // are often outside RuntimeFunctionAddrs, and that walk dominates
      // single-function export on a large PE.
      const Segment *Seg = Img.getSegmentFor(Addr);
      if (!Seg || !Seg->isExecutable())
        continue;
      FuncEntries.push_back({Addr, Img.getFunctionNameAt(Addr)});
    }
    LLVM_DEBUG(llvm::dbgs() << "pipeline: only-function filter kept "
                            << FuncEntries.size() << " entries\n");
  } else {
    FuncDetector Detector;
    FuncEntries = Detector.detect(Img, Dec);
    LLVM_DEBUG(llvm::dbgs() << "pipeline: detected " << FuncEntries.size()
                            << " functions\n");
  }

  // mergeDebugSymbols also *adds* every PDB function that is not already a
  // candidate. Single-function export must not reintroduce the rest of the
  // image after OnlyFunctionEntries has already chosen the work set, but it
  // still has to replace synthesized names on the selected entries.
  if (Dbg && Dbg->hasInfo() && !Opts.OnlyFunctionEntries.empty()) {
    for (auto &[Addr, Name] : FuncEntries) {
      if (!isSynthesizedFuncName(Name))
        continue;
      if (auto DF = Dbg->functionName(Addr); DF && !DF->empty())
        Name = *DF;
    }
  }
  if (Dbg && Dbg->hasInfo() && Opts.OnlyFunctionEntries.empty()) {
    mergeDebugSymbols(FuncEntries, *Dbg);
    std::vector<std::pair<va_t, va_t>> DebugRanges;
    std::set<va_t> DebugStarts;
    for (const auto &DF : Dbg->allFunctions()) {
      DebugStarts.insert(DF.Addr);
      if (DF.Size == 0 || DF.Size > InvalidVA - DF.Addr)
        continue;
      DebugRanges.push_back({DF.Addr, DF.Addr + DF.Size});
    }
    if (!DebugRanges.empty()) {
      FuncEntries.erase(
          std::remove_if(
              FuncEntries.begin(), FuncEntries.end(),
              [&](const std::pair<va_t, std::string> &Entry) {
                if (DebugStarts.count(Entry.first))
                  return false;
                for (const auto &[Start, End] : DebugRanges)
                  if (Entry.first > Start && Entry.first < End)
                    return true;
                return false;
              }),
          FuncEntries.end());
    }
  }

  Result.FunctionAudits.clear();
  Result.FunctionAudits.reserve(FuncEntries.size());

  std::vector<std::pair<va_t, std::string>> Candidates;
  Candidates.reserve(Opts.MaxFunctions > 0
                         ? std::min(FuncEntries.size(), Opts.MaxFunctions + 64)
                         : FuncEntries.size());
  for (auto &[Entry, FName] : FuncEntries) {
    PipelineFunctionAudit Audit;
    Audit.Entry = Entry;
    Audit.Name = FName;
    // Preserve linker/dynamic-loader import veneers.  Loaders register section
    // ranges (ELF PLT, Mach-O stubs/helper), while architecture scanners map
    // exact COFF/ELF thunks back to their Import without changing IATAddr.
    if ((Opts.PatchMode || Opts.LiftMode) && Img.isImportStubAt(Entry)) {
      Audit.Disposition = PipelineFunctionDisposition::SkippedImportStub;
      Result.FunctionAudits.push_back(std::move(Audit));
      continue;
    }
    // A loader entry or lifecycle callback may follow a process/TLS/CRT ABI,
    // not an ordinary inferred C function signature.  Each loader records the
    // exact structural targets.  The ELF name table remains only a
    // compatibility fallback for old symbol-rich CRT scaffolding without
    // metadata.
    if (Opts.PatchMode && (Img.isRuntimeFunctionAt(Entry) ||
                           (Img.isELF() && isELFRuntimeScaffold(FName)))) {
      Audit.Disposition = PipelineFunctionDisposition::SkippedRuntimeScaffold;
      Result.FunctionAudits.push_back(std::move(Audit));
      continue;
    }
    if (Opts.MaxFunctions > 0 && Candidates.size() >= Opts.MaxFunctions * 2) {
      Audit.Disposition = PipelineFunctionDisposition::SkippedLimit;
      Result.FunctionAudits.push_back(std::move(Audit));
      continue;
    }
    Candidates.push_back({Entry, FName});
    Result.FunctionAudits.push_back(std::move(Audit));
  }

  return Candidates;
}

} // namespace neverd
