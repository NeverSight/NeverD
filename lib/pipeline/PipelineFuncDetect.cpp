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
  FuncDetector Detector;
  auto FuncEntries = Detector.detect(Img, Dec);
  LLVM_DEBUG(llvm::dbgs() << "pipeline: detected " << FuncEntries.size()
                          << " functions\n");

  if (Dbg && Dbg->hasInfo())
    mergeDebugSymbols(FuncEntries, *Dbg);

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
    if (Img.isImportStubAt(Entry)) {
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
