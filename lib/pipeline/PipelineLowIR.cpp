//===- PipelineLowIR.cpp - LowIR pipeline stage --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// LowIR construction for the decompilation pipeline.
///
//===----------------------------------------------------------------------===//

#include "PipelineTrimStorage.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Parallel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

namespace {

bool hasRealOps(const LowFunc &Func) {
  for (auto &Blk : Func.Blocks)
    for (auto &Op : Blk.Ops)
      if (Op.Opcode != NdOp::NOP)
        return true;
  return false;
}

void annotateDebugInfo(LowFunc &Func, DebugContext &Dbg) {
  auto FSym = Dbg.resolveFunction(Func.Entry);
  if (!FSym)
    return;
  Func.DebugName = FSym->Name;
  Func.SourceFile = FSym->DeclLoc.File;
  Func.SourceLine = FSym->DeclLoc.Line;
  if (FSym->Size > 0)
    Func.OriginalSize = FSym->Size;
}

} // namespace

//===----------------------------------------------------------------------===//
// buildLowIR — Phase 1
//===----------------------------------------------------------------------===//

void Pipeline::buildLowIR(
    const BinaryImage &Img,
    const std::vector<std::pair<va_t, std::string>> &Candidates,
    const PipelineOptions &Opts, DebugContext *Dbg, PipelineResult &Result) {
  const size_t Total = Candidates.size();
  std::vector<LowFunc> AllLow(Total);

  // The set of all detected function entries lets each CFG builder recognise an
  // unconditional `jmp` to *another* function as a tail call (call + ret)
  // rather than following it and fusing the callee into this function's CFG.
  std::set<va_t> FuncEntries;
  for (const auto &C : Candidates)
    FuncEntries.insert(C.first);

  // Decode cost tracks a function's instruction count, which is unknown before
  // the recursive-descent build runs.  Candidates are address-sorted, so the
  // byte gap to the next entry is a cheap upper-bound proxy for a function's
  // size; scheduling the largest gaps first keeps one giant function from
  // being claimed last and serializing the tail.  The gap is clamped so an
  // outsized cross-segment gap (data between the last function and the segment
  // end) does not distort the ordering — it is only a scheduling hint and never
  // affects the decoded result.
  constexpr uint64_t kMaxDecodeWeight = 1ull << 20;
  std::vector<uint64_t> Weight(Total, kMaxDecodeWeight);
  for (size_t I = 0; I + 1 < Total; ++I) {
    uint64_t Gap = Candidates[I + 1].first - Candidates[I].first;
    Weight[I] = std::min(Gap, kMaxDecodeWeight);
  }

  parallelForEachWeighted(Weight, [&](auto Claim, size_t N) {
    Decoder LocalDec;
    if (!LocalDec.init(Img.Arch, Img.Mode))
      return;
    CFGBuilder LocalCFG;
    LocalCFG.setKnownFuncEntries(&FuncEntries);
    for (size_t I; (I = Claim()) < N;) {
      AllLow[I] = LocalCFG.build(Img, LocalDec, Candidates[I].first,
                                 Candidates[I].second);
      trimFuncStorage(AllLow[I]);
    }
  });

  // Merge each function's relocation-free PC-relative code references (x86
  // same-section `lea rip` function pointers) into the image so the emitter
  // symbolizes them.  Done single-threaded after the parallel build to avoid a
  // data race on the shared set.
  for (const auto &LF : AllLow)
    for (va_t Ref : LF.CodeRefTargets)
      Img.CodeRefTargets.insert(Ref);

  size_t FuncCount = 0;
  for (size_t I = 0; I < Total; ++I) {
    auto &Low = AllLow[I];
    auto AuditIt =
        std::find_if(Result.FunctionAudits.begin(), Result.FunctionAudits.end(),
                     [&](const PipelineFunctionAudit &Audit) {
                       return Audit.Entry == Candidates[I].first;
                     });
    if (AuditIt != Result.FunctionAudits.end()) {
      AuditIt->DecodedInstructions = Low.DecodedInstructionCount;
      AuditIt->LiftedInstructions = Low.LiftedInstructionCount;
      AuditIt->DecodeFailures = Low.DecodeFailureAddresses;
      AuditIt->UnsupportedInstructions = Low.UnsupportedInstructionAddresses;
      AuditIt->TruncatedPaths = Low.TruncatedPathAddresses;
    }

    if (Opts.MaxFunctions > 0 && FuncCount >= Opts.MaxFunctions) {
      if (AuditIt != Result.FunctionAudits.end())
        AuditIt->Disposition = PipelineFunctionDisposition::SkippedLimit;
      continue;
    }
    if (!hasRealOps(Low)) {
      if (AuditIt != Result.FunctionAudits.end())
        AuditIt->Disposition =
            Low.hasCompleteInstructionLift()
                ? PipelineFunctionDisposition::RejectedLowIR
                : PipelineFunctionDisposition::RejectedIncomplete;
      continue;
    }
    // Only an incomplete *lift* disqualifies a function.  A path that left the
    // mapped image is recorded in the audit but is not a defect in what was
    // recovered, and rejecting it would drop a function whose every
    // instruction lifted cleanly.
    if (!Low.hasCompleteInstructionLift()) {
      if (AuditIt != Result.FunctionAudits.end())
        AuditIt->Disposition = PipelineFunctionDisposition::RejectedIncomplete;
      continue;
    }

    if (Dbg && Dbg->hasInfo())
      annotateDebugInfo(Low, *Dbg);
    if (Low.OriginalSize == 0)
      Low.OriginalSize = Low.computedSize();

    Result.LowFuncs.push_back(std::move(Low));
    if (AuditIt != Result.FunctionAudits.end())
      AuditIt->HasLowIR = true;
    ++FuncCount;
  }
}

} // namespace neverd
