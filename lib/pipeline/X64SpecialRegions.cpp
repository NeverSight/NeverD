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

#include "X64RtmAbortRegions.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86HleEncoding.h"
#include "neverd/lift/X86/X86HypercallEncoding.h"
#include "neverd/lift/X86/X86MpxEncoding.h"
#include "neverd/lift/X86/X86SnpEncoding.h"
#include "neverd/lift/X86/X86SvmEncoding.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/pipeline/X64ContinuationExport.h"
#include "neverd/support/SortedSearch.h"

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
llvm::ArrayRef<uint8_t> bytesAt(const BinaryImage &Image, va_t Address) {
  const auto *Segment = Image.getSegmentFor(Address);
  if (!Segment || Address < Segment->VA ||
      Address - Segment->VA >= Segment->Data.size())
    return {};
  return llvm::ArrayRef(Segment->Data).drop_front(Address - Segment->VA);
}
} // namespace

llvm::Expected<X64SpecialRegionSelection> selectX64SpecialRegions(
    const BinaryImage &Image, llvm::ArrayRef<va_t> Entries,
    const analysis::X64ContinuationSourceOptions &SourceOptions,
    unsigned CalleeDepth, bool FollowDirectJumps, bool NativeRtmAbortRegions,
    DebugContext *Debug) {
  X64SpecialRegionSelection Selection;
  Decoder Decode;
  if ((CalleeDepth || FollowDirectJumps) && !Decode.init(Image))
    return invalid("selected CFG transfer decoder initialization failed");
  const uint64_t MaxInstructions = SourceOptions.MaxInstructions;
  std::map<unsigned, std::set<va_t>> Frontier;
  std::map<va_t, unsigned> Requested;
  const auto Enqueue = [&](va_t Address, unsigned Depth) -> llvm::Error {
    auto [It, New] = Requested.try_emplace(Address, Depth);
    if (!New) {
      if (It->second <= Depth)
        return llvm::Error::success();
      Frontier[It->second].erase(Address);
      It->second = Depth;
    }
    if (Requested.size() > MaxInstructions)
      return invalid("selected CFG entry budget exceeded");
    Frontier[Depth].insert(Address);
    return llvm::Error::success();
  };
  for (va_t Entry : Entries)
    if (auto Error = Enqueue(Entry, 0))
      return std::move(Error);
  std::set<va_t> Unique;
  // Pipeline already schedules independent functions in weighted batches.
  // Waves are dependent: do not discover or relift the same callee per caller.
  while (!Frontier.empty()) {
    const unsigned Depth = Frontier.begin()->first;
    auto Pending = std::move(Frontier.begin()->second);
    Frontier.erase(Frontier.begin());
    if (Pending.empty())
      continue;
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
      llvm::json::Array DirectJumps;
      std::vector<va_t> RegionAddresses;
      if (FollowDirectJumps) {
        RegionAddresses = Audit.NativeInstructionAddresses;
        std::sort(RegionAddresses.begin(), RegionAddresses.end());
      }
      for (va_t Address : Audit.NativeInstructionAddresses) {
        Unique.insert(Address);
        PCs.push_back(hex(Address));
        if (Unique.size() > MaxInstructions)
          return invalid("selected CFG instruction budget exceeded");
        if (Depth < CalleeDepth || FollowDirectJumps) {
          // Rejected bodies retain audited instruction starts but no LowFunc.
          // Use the decoder's original direct-call classification, never an
          // optimized CALL or a register constant propagated across VMEXIT.
          const auto Bytes = bytesAt(Image, Address);
          DecodedInsn Instruction;
          if (!Decode.selectMode(Image, Address) ||
              !Decode.decodeOneForLift(Bytes.data(), Bytes.size(), Address,
                                       Instruction))
            return invalid("selected transfer decoding failed at " +
                           hex(Address));
          if (Depth < CalleeDepth) {
            const va_t Target = Decode.directCallTarget(Instruction);
            if (Target != InvalidVA && !Image.findImportAt(Target) &&
                Image.isCodeAddress(Target))
              if (auto Error = Enqueue(Target, Depth + 1))
                return std::move(Error);
          }
          if (FollowDirectJumps && Instruction.Id == X86_INS_JMP) {
            // The architecture lifter owns the target, including displacement
            // width and PC arithmetic. Never inherit an ordinary tail-call
            // rewrite or propagate an indirect register/slot target here.
            std::vector<LowOp> Operations;
            Decode.liftToLow(Instruction, Operations);
            for (const LowOp &Op : Operations)
              if (Op.Opcode == NdOp::BRANCH && Op.NumInputs == 1 &&
                  Op.Inputs[0].isConst()) {
                const va_t Target = Op.Inputs[0].Offset;
                if (!sorted::contains64(RegionAddresses.data(),
                                        RegionAddresses.size(), Target) &&
                    !Image.findImportAt(Target) &&
                    Image.isCodeAddress(Target)) {
                  if (auto Error = Enqueue(Target, Depth))
                    return std::move(Error);
                  DirectJumps.push_back(llvm::json::Object{
                      {"instruction", hex(Address)}, {"target", hex(Target)}});
                }
              }
          }
        }
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
      if (!DirectJumps.empty())
        Region["direct_jump_successors"] = std::move(DirectJumps);
      if (NativeRtmAbortRegions) {
        llvm::json::Array Resumes;
        for (va_t Address : Audit.NativeInstructionAddresses)
          if (auto Abort = readX64RtmAbortRegion(
                  Image, Address, *SourceOptions.PagingAddressBits);
              Abort && Image.isCodeAddress(Abort->Fallback)) {
            if (auto Error = Enqueue(Abort->Fallback, Depth))
              return std::move(Error);
            Resumes.push_back(hex(Abort->Fallback));
          }
        if (!Resumes.empty())
          Region["rtm_abort_successors"] = std::move(Resumes);
      }
      if (SourceOptions.Profile ==
          analysis::X64ContinuationProfile::NativeSvmNoFaultV10) {
        llvm::json::Array Resumes;
        for (va_t Address : Audit.UnsupportedInstructions) {
          const auto Bytes = bytesAt(Image, Address);
          for (unsigned Size : {3u, 4u}) {
            if (Bytes.size() < Size ||
                !x64VmrunAddressWidth(Bytes.take_front(Size)))
              continue;
            // The raw reader revalidates this same encoding before emission.
            // This extends selection only; no LowIR or ordinary ABI is
            // admitted.
            const va_t Resume = Address + Size;
            if (Image.isCodeAddress(Resume)) {
              if (auto Error = Enqueue(Resume, Depth))
                return std::move(Error);
              Resumes.push_back(hex(Resume));
            }
            break;
          }
        }
        if (!Resumes.empty())
          Region["vmexit_successors"] = std::move(Resumes);
      }
      if (SourceOptions.NativeHleSteps) {
        llvm::json::Array Resumes;
        for (va_t Address : Audit.UnsupportedInstructions) {
          const auto Bytes = bytesAt(Image, Address);
          for (unsigned Size = 1; Size <= 15 && Size <= Bytes.size(); ++Size) {
            if (!x86HleEncoding(Bytes.take_front(Size)))
              continue;
            const va_t Resume = Address + Size;
            if (Image.isCodeAddress(Resume)) {
              if (auto Error = Enqueue(Resume, Depth))
                return std::move(Error);
              Resumes.push_back(hex(Resume));
            }
            break;
          }
        }
        if (!Resumes.empty())
          Region["hle_successors"] = std::move(Resumes);
      }
      if (SourceOptions.NativeSnpInstructions ||
          SourceOptions.NativeSevGuestExit) {
        llvm::json::Array Resumes;
        for (va_t Address : Audit.UnsupportedInstructions) {
          const auto Bytes = bytesAt(Image, Address);
          for (unsigned Size : {4u, 5u}) {
            if (Bytes.size() < Size)
              continue;
            const auto Snp = x86SnpEncoding(Bytes.take_front(Size));
            if (!Snp || !(Snp->Id == Intrinsic::X86SevVmgexit
                              ? SourceOptions.NativeSevGuestExit
                              : SourceOptions.NativeSnpInstructions))
              continue;
            const va_t Resume = Address + Size;
            if (Image.isCodeAddress(Resume)) {
              if (auto Error = Enqueue(Resume, Depth))
                return std::move(Error);
              Resumes.push_back(hex(Resume));
            }
            break;
          }
        }
        if (!Resumes.empty())
          Region["snp_sev_successors"] = std::move(Resumes);
      }
      if (SourceOptions.NativeKvmHypercalls ||
          SourceOptions.NativeHyperVHypercallEntry) {
        llvm::json::Array Resumes;
        for (va_t Address : Audit.UnsupportedInstructions) {
          const auto Bytes = bytesAt(Image, Address);
          if (Bytes.size() < 3 || !x86HypercallEncoding(Bytes.take_front(3)))
            continue;
          const va_t Resume = Address + 3;
          if (Image.isCodeAddress(Resume)) {
            if (auto Error = Enqueue(Resume, Depth))
              return std::move(Error);
            Resumes.push_back(hex(Resume));
          }
        }
        if (!Resumes.empty())
          Region["hypercall_successors"] = std::move(Resumes);
      }
      if (SourceOptions.Profile ==
          analysis::X64ContinuationProfile::MpxNoFaultV11) {
        llvm::json::Array Resumes;
        for (va_t Address : Audit.UnsupportedInstructions) {
          const auto Mpx =
              decodeX86MpxEncoding(bytesAt(Image, Address), Arch::X64);
          if (!Mpx || Mpx->Id == X86_INS_NOP)
            continue;
          const va_t Resume = Address + Mpx->Size;
          if (Image.isCodeAddress(Resume)) {
            if (auto Error = Enqueue(Resume, Depth))
              return std::move(Error);
            Resumes.push_back(hex(Resume));
          }
        }
        if (!Resumes.empty())
          Region["mpx_successors"] = std::move(Resumes);
      }
      Selection.Regions.push_back(std::move(Region));
    }
    if (Found != Pending || Unique.empty())
      return invalid("selected entries did not produce instruction regions");
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
