//===- PipelineAnalysis.cpp - Display-only function admission ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve checked callback code and decoded operations for analysis while
/// retaining every refusal of a native state/ABI proof.
///
//===----------------------------------------------------------------------===//

#include "PipelineAnalysis.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/pipeline/Pipeline.h"

#include <cstddef>
#include <set>

namespace neverd::pipeline_detail {
namespace {
bool enabled(const BinaryImage &Image, const PipelineOptions &Options) {
  return Options.allowsAnalysisRecovery() && Image.Arch == Arch::X86 &&
         Image.isCOFF() && Image.Bits == Bitness::Bits32;
}
} // namespace

std::set<va_t> analysisCallbackEntries(const BinaryImage &Image,
                                       const PipelineOptions &Options) {
  std::set<va_t> Entries;
  if (!enabled(Image, Options))
    return Entries;
  const auto Add = [&](const ExceptionFunction &Owner) {
    if (!Owner.Registration || !Owner.Cxx)
      return;
    const auto Sources =
        coff_loader::getCheckedX86CxxCallbackPointerSources(Image, Owner);
    if (!Sources)
      return;
    const auto AddTarget = [&](va_t Target) {
      if (!Owner.ownsCode(Target) && Image.isFunctionEntryCandidateAt(Target) &&
          readImmutableCodeBytes(Image, Target, 1))
        Entries.insert(Target);
    };
    for (const auto &[Slot, Target] : *Sources) {
      (void)Slot;
      AddTarget(Target);
    }
    // The installed language-handler thunk is code too, but the metadata's
    // address alone is not permission to introduce a root there.
    if (const auto Handler =
            coff_loader::getCheckedX86CxxHandlerPointerSource(Image, Owner))
      AddTarget(Handler->second);
  };
  if (Options.OnlyFunctionEntries.empty()) {
    for (const auto &Owner : Image.ExceptionMetadata.Functions)
      Add(Owner);
  } else {
    std::set<const ExceptionFunction *> Seen;
    for (va_t Entry : Options.OnlyFunctionEntries)
      if (const auto *Owner = Image.ExceptionMetadata.findFunction(Entry);
          Owner && Owner->CodeRange.Begin == Entry && Seen.insert(Owner).second)
        Add(*Owner);
  }
  return Entries;
}

std::set<va_t> analysisFinallyInteriors(const BinaryImage &Image,
                                        const PipelineOptions &Options) {
  std::set<va_t> Interiors;
  if (!enabled(Image, Options))
    return Interiors;
  for (const ExceptionFunction &Owner : Image.ExceptionMetadata.Functions) {
    if (!Owner.Registration || Owner.Cxx)
      continue;
    for (const RegistrationScopeRecord &Scope : Owner.Registration->Scopes) {
      if (!Scope.IsFinally)
        continue;
      // Each `mov r, [ebp + disp]` the handler starts with, of either width.
      for (va_t At = Scope.HandlerVA; Owner.ownsCode(At);) {
        const auto Code = readImmutableCodeBytes(Image, At, 2);
        if (!Code || ((*Code)[0] != 0x8b && (*Code)[0] != 0x8a))
          break;
        const uint8_t Form = (*Code)[1] & 0xc7;
        if (Form != 0x45 && Form != 0x85)
          break;
        At += Form == 0x45 ? 3 : 6;
        Interiors.insert(At);
      }
    }
  }
  return Interiors;
}

bool hasAnalysisInstructionBody(const LowFunc &Function) {
  if (!Function.DecodedInstructionCount ||
      Function.DecodedInstructionCount != Function.LiftedInstructionCount ||
      !Function.DecodeFailureAddresses.empty() ||
      !Function.UnprovenReturnAddresses.empty())
    return false;
  // A post-lift proof can decline an operation that is still useful to show
  // (for example, an x87 load whose empty destination tag is unknown). It
  // cannot authorize a missing instruction or a placeholder for one.
  std::set<va_t> Pending(Function.UnsupportedInstructionAddresses.begin(),
                         Function.UnsupportedInstructionAddresses.end());
  if (Pending.empty())
    return true;
  for (const auto &Block : Function.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries)
      if (Boundary.OpCount && Boundary.FirstOp <= Block.Ops.size() &&
          Boundary.OpCount <= Block.Ops.size() - Boundary.FirstOp &&
          Pending.count(Boundary.Address)) {
        for (size_t I = Boundary.FirstOp;
             I < Boundary.FirstOp + Boundary.OpCount; ++I)
          if (Block.Ops[I].Opcode != NdOp::NOP) {
            Pending.erase(Boundary.Address);
            break;
          }
      }
  return Pending.empty();
}

void prepareAnalysisBody(const BinaryImage &Image,
                         const PipelineOptions &Options,
                         const std::set<va_t> &Callbacks, LowFunc &Function) {
  if (!enabled(Image, Options))
    return;
  if (Callbacks.count(Function.Entry) ||
      (!Function.hasCompleteInstructionLift() &&
       hasAnalysisInstructionBody(Function)))
    Function.AnalysisOnly = true;
  if (!Function.AnalysisOnly)
    for (const auto &Block : Function.Blocks)
      for (const auto &Operation : Block.Ops)
        if (Operation.X87ImportArguments)
          Function.AnalysisOnly = true;
  if (Function.AnalysisOnly)
    for (auto &Table : Function.JumpTables) {
      Table.AnalysisOnly = true;
      Table.AuthenticatedTableLoads.clear();
      Table.SuppressibleRelocationSlots.clear();
    }
}

} // namespace neverd::pipeline_detail
