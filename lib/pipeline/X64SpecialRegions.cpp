//===- X64SpecialRegions.cpp - Bounded physical-state regions -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Special handling for source regions whose ordinary call ABI is unproved.
/// Selection and dispatch preserve the original machine state and call stack.
//===----------------------------------------------------------------------===//

#include "X64SpecialRegions.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/pipeline/X64ContinuationExport.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace neverd {
namespace {
std::string hex(uint64_t Value) { return "0x" + llvm::utohexstr(Value); }
llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}
} // namespace

llvm::Expected<X64SpecialRegionSelection>
selectX64SpecialRegions(const BinaryImage &Image, llvm::ArrayRef<va_t> Entries,
                        uint64_t MaxInstructions, unsigned CalleeDepth,
                        DebugContext *Debug) {
  X64SpecialRegionSelection Selection;
  std::set<va_t> Pending(Entries.begin(), Entries.end());
  std::set<va_t> Requested = Pending;
  std::set<va_t> Unique;
  // Pipeline already schedules independent functions in weighted batches.
  // Waves are dependent: do not discover or relift the same callee per caller.
  for (unsigned Depth = 0; !Pending.empty(); ++Depth) {
    PipelineOptions Options;
    Options.OnlyFunctionEntries = Pending;
    Options.DeferMedIR = true;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    auto Result = Pipeline().run(Image, Context, Options, Debug);
    if (!Result.Success)
      return invalid("selected CFG recovery failed: " + Result.Error);
    std::set<va_t> Found;
    for (const auto &Audit : Result.FunctionAudits) {
      if (!Pending.count(Audit.Entry))
        continue;
      if (!Audit.DecodeFailures.empty())
        return invalid("selected entry " + hex(Audit.Entry) +
                       " contains an undecodable instruction at " +
                       hex(Audit.DecodeFailures.front()));
      if (Audit.NativeInstructionAddresses.empty())
        return invalid("selected entry " + hex(Audit.Entry) +
                       " has no decoded region: " +
                       pipelineFunctionDispositionName(Audit.Disposition));
      Found.insert(Audit.Entry);
      llvm::json::Array PCs;
      for (va_t Address : Audit.NativeInstructionAddresses) {
        Unique.insert(Address);
        PCs.push_back(hex(Address));
        if (Unique.size() > MaxInstructions)
          return invalid("selected CFG instruction budget exceeded");
      }
      llvm::json::Object Region{
          {"entry", hex(Audit.Entry)},
          {"instructions", std::move(PCs)},
          {"lowir_disposition",
           pipelineFunctionDispositionName(Audit.Disposition)},
          {"lowir_unproved_returns", uint64_t(Audit.UnprovenReturns.size())},
          {"lowir_unsupported_instructions",
           uint64_t(Audit.UnsupportedInstructions.size())},
          {"truncated_paths", uint64_t(Audit.TruncatedPaths.size())}};
      if (CalleeDepth)
        Region["callee_depth"] = Depth;
      Selection.Regions.push_back(std::move(Region));
    }
    if (Found != Pending || Unique.empty())
      return invalid("selected entries did not produce instruction regions");
    if (Depth == CalleeDepth)
      break;
    std::set<va_t> Next;
    for (const auto &Function : Result.LowFuncs) {
      if (!Pending.count(Function.Entry))
        continue;
      for (const auto &Block : Function.Blocks)
        for (const auto &Op : Block.Ops) {
          if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
              !Op.Inputs[0].isConst())
            continue;
          const va_t Target = Op.Inputs[0].Offset;
          if (Image.findImportAt(Target) || !Image.isCodeAddress(Target) ||
              !Requested.insert(Target).second)
            continue;
          // Every admitted region must contain at least one instruction.
          // Bound the discovery frontier before running its next pipeline.
          if (Requested.size() > MaxInstructions)
            return invalid("selected CFG entry budget exceeded");
          Next.insert(Target);
        }
    }
    Pending = std::move(Next);
  }
  Selection.Addresses.assign(Unique.begin(), Unique.end());
  return Selection;
}

void emitX64SpecialRegionDriver(
    llvm::raw_ostream &OS,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units,
    bool FollowInternalCalls) {
  using Exit = analysis::X64ContinuationExitV1;
  // One index for the immutable selection; no per-instruction linear search.
  std::map<va_t, size_t> ByAddress;
  for (size_t I = 0; I < Units.size(); ++I)
    ByAddress.emplace(Units[I].Address, I);
  const auto Version = Units.front().StateVersion;
  OS << R"C(
/* Direct special-region dispatch. State and budget are accessible and disjoint
 * from each other and guest RAM. Exact instruction semantics own every effect.
 * The environment may replace all state before resuming at any selected PC.
 * Arbitrary-value slots are caller-selected per dynamic instruction.
 * A linked successor is taken only when the actual post-step PC agrees. */
)C";
  const size_t Chunk = limits::pipeline::kX64ContinuationLinkedSteps;
  const auto Continuing = [&] {
    OS << "status == " << uint64_t(Exit::Continue)
       << " || status == " << uint64_t(Exit::Jump);
    if (FollowInternalCalls)
      OS << " || status == " << uint64_t(Exit::Call)
         << " || status == " << uint64_t(Exit::Return);
  };
  // Prevent the C optimizer from rebuilding one enormous cyclic driver. Only
  // this local implementation detail is noinline; instruction steps may inline
  // into their bounded region. No machine-state fact crosses a region boundary.
  for (size_t First = 0; First < Units.size(); First += Chunk) {
    const size_t End = std::min(First + Chunk, Units.size());
    OS << "static __attribute__((noinline)) uint64_t neverd_x64_region_"
       << First / Chunk << "(neverd_x64_state_v" << Version
       << " *state, uint64_t *budget) {\n"
          "    uint64_t last = 8, status;\n"
          "dispatch:\n    switch (state->PC) {\n";
    for (size_t I = First; I < End; ++I)
      OS << "    case " << hex(Units[I].Address) << "ULL: goto step_" << I
         << ";\n";
    OS << "    default: return last;\n    }\n";
    for (size_t I = First; I < End; ++I) {
      const auto &Unit = Units[I];
      OS << "step_" << I << ":\n    if (!*budget) return 9;\n"
         << "    status = " << Unit.Function.Name << "((uint8_t *)state);\n"
         << "    if (status == " << uint64_t(Exit::NativeInstruction)
         << " || status == " << uint64_t(Exit::InvalidState)
         << ") return status;\n    --*budget;\n    if (!(";
      Continuing();
      OS << ")) return status;\n    last = status;\n";
      if (Unit.Bytes.size() <=
          std::numeric_limits<va_t>::max() - Unit.Address) {
        const va_t Next = Unit.Address + Unit.Bytes.size();
        if (const auto At = ByAddress.find(Next);
            At != ByAddress.end() && At->second >= First && At->second < End)
          OS << "    if (state->PC == " << hex(Next) << "ULL) goto step_"
             << At->second << ";\n";
      }
      OS << "    goto dispatch;\n";
    }
    OS << "}\n";
  }
  OS << "uint64_t neverd_x64_continue_v" << Version << "(neverd_x64_state_v"
     << Version
     << " *state, uint64_t *budget) {\n"
        "    uint64_t last = 8, status;\n"
        "    if (!*budget) return 9;\n"
        "    for (;;) {\n        switch (state->PC) {\n";
  for (size_t First = 0; First < Units.size(); First += Chunk) {
    const size_t End = std::min(First + Chunk, Units.size());
    for (size_t I = First; I < End; ++I)
      OS << "        case " << hex(Units[I].Address) << "ULL:\n";
    OS << "            status = neverd_x64_region_" << First / Chunk
       << "(state, budget); break;\n";
  }
  OS << "        default:\n";
  if (FollowInternalCalls)
    OS << "            if (last == " << uint64_t(Exit::Call)
       << " || last == " << uint64_t(Exit::Return) << ") return last;\n";
  OS << "            if (!*budget) return 9;\n"
     << "            return last == " << uint64_t(Exit::Jump) << " ? "
     << uint64_t(Exit::Jump) << " : 8;\n        }\n        if (!(";
  Continuing();
  OS << ")) return status;\n        last = status;\n    }\n}\n";
}

} // namespace neverd
