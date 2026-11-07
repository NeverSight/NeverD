//===- RegistrationPhysicalFrame.cpp - Shared C++ frame replay -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Replay expanded helpers and bind their exact current chain operations.
//===----------------------------------------------------------------------===//

#include "RegistrationPhysicalFrame.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/RegistrationSharedCall.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86RegistrationSEHThread.h"

#include "llvm/ADT/ScopeExit.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace neverd {
namespace {

bool complete(const RegistrationStateAnalysis &S) {
  return S.Complete && S.CallbackStatesComplete &&
         S.RegistrationLifetimeComplete && S.ChainOperationsComplete &&
         S.IncomingFrameAccessesComplete && S.CallFrameEffectsComplete &&
         S.CleanupFrameEffectsComplete && S.CxxContinuationsComplete &&
         S.CxxCatchObjectsComplete && S.RuntimeObjectAccessesComplete &&
         S.ImageReadsComplete;
}

bool same(const RegistrationChainAccess &A, const RegistrationChainAccess &B) {
  return A.Address == B.Address && A.EndAddress == B.EndAddress &&
         A.OpSeq == B.OpSeq && A.AccessKind == B.AccessKind;
}

bool same(const RegistrationBlockState &A, const RegistrationBlockState &B) {
  auto Key = [](const auto &S) {
    return std::tie(S.BlockId, S.Range.Begin, S.Range.End, S.levels(),
                    S.Unknown, S.CallbackOnly, S.CanDispatch,
                    S.CxxMinimumTryLevel, S.Reached, S.CxxSearches,
                    S.CxxCatchStacks);
  };
  return Key(A) == Key(B);
}

using TempKey = std::pair<int, int>;
using TempDefinition = std::pair<const MedBlock *, const MedOp *>;
using TempDefinitions = std::map<TempKey, TempDefinition>;

bool segmentOffset(const MedBlock &Block, const MedOp &Access,
                   const TempDefinitions &Definitions, uint32_t Offset,
                   size_t &Work) {
  MedVar Value = Access.Inputs[0];
  const MedOp *Before = &Access;
  // The ordinary x86 lifter materializes a fixed FS offset as COPY, optionally
  // followed by a 32-to-64-bit zero extension. Exceptional CFGs need not
  // have folded these transports. Only unique, earlier definitions in the
  // same source instruction can supply this offset; no PHI or input can.
  for (unsigned Step = 0; Step != 3; ++Step) {
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        (Value.Size != 4 && Value.Size != 8))
      return false;
    if (Value.isConst())
      return Value.ConstVal == Offset;
    if (Value.Kind != MedVar::Temp)
      return false;
    const auto It = Definitions.find({Value.Id, Value.SSAVer});
    if (It == Definitions.end() || It->second.first != &Block ||
        !It->second.second)
      return false;
    const auto *Def = It->second.second;
    if (Def >= Before || Def->Dead || Def->Addr != Access.Addr ||
        Def->OriginSeq < 0 || Def->OriginSeq >= Before->OriginSeq ||
        Def->Output.Size != Value.Size || Def->NumInputs != 1 ||
        Def->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Def->MemoryOrdering != NdMemoryOrdering::None ||
        !Def->IntrinsicOutputs.empty() ||
        !((Def->Opcode == NdOp::COPY && Def->Inputs[0].Size == Value.Size) ||
          (Def->Opcode == NdOp::INT_ZEXT && Value.Size == 8 &&
           Def->Inputs[0].Size == 4)))
      return false;
    Value = Def->Inputs[0];
    Before = Def;
  }
  return false;
}

bool matchesSourceChain(const RegistrationStateAnalysis &Source,
                        const X86RegistrationCxxPhysicalFrame &Frame) {
  return complete(Source) && Source.CxxPrologue &&
         *Source.CxxPrologue == Frame.Entry &&
         x86_registration::matchesPhysicalChain(
             Source.ChainAccesses,
             {Frame.Entry.Address, Frame.Entry.EndAddress, Frame.Entry.OpSeq,
              RegistrationChainAccess::Kind::Install},
             Frame.Exits, Frame.ChainAccesses);
}

} // namespace

namespace x86_registration {
bool matchesPhysicalChain(
    const std::vector<RegistrationChainAccess> &Source,
    const RegistrationChainAccess &Entry,
    const std::vector<X86RegistrationPhysicalExit> &SourceExits,
    const std::vector<RegistrationChainAccess> &Projected) {
  if (Source.size() > limits::kMaxRegistrationEHRecords ||
      Projected.size() != Source.size() + 1 ||
      SourceExits.size() > Source.size())
    return false;
  using Key = std::pair<va_t, int>;
  std::map<Key, const RegistrationChainAccess *> Physical;
  std::map<va_t, std::vector<const RegistrationChainAccess *>> At;
  for (const auto &Access : Projected) {
    if (Access.Address >= Access.EndAddress || Access.OpSeq < 0 ||
        !Physical.emplace(Key{Access.Address, Access.OpSeq}, &Access).second)
      return false;
    At[Access.Address].push_back(&Access);
  }
  std::map<va_t, const X86RegistrationPhysicalExit *> Exits;
  for (const auto &Exit : SourceExits)
    if (Exit.Source.Address == Entry.Address ||
        Exit.Source.AccessKind != RegistrationChainAccess::Kind::Remove ||
        !Exits.emplace(Exit.Source.Address, &Exit).second)
      return false;
  bool SeenEntry = false;
  for (const auto &Access : Source) {
    if (Access.Address == Entry.Address) {
      if (SeenEntry || Access.EndAddress != Entry.EndAddress ||
          Access.OpSeq != Entry.OpSeq ||
          Access.AccessKind != RegistrationChainAccess::Kind::Install ||
          At[Access.Address].size() != 2)
        return false;
      bool Read = false, Install = false;
      for (const auto *Effect : At[Access.Address]) {
        if (Effect->EndAddress != Access.EndAddress ||
            !Physical.erase({Effect->Address, Effect->OpSeq}))
          return false;
        Read |= Effect->AccessKind ==
                RegistrationChainAccess::Kind::ReadPreviousHead;
        Install |= Effect->AccessKind == RegistrationChainAccess::Kind::Install;
      }
      if (!Read || !Install)
        return false;
      SeenEntry = true;
    } else if (const auto Exit = Exits.find(Access.Address);
               Exit != Exits.end()) {
      if (!same(Access, Exit->second->Source) || At[Access.Address].size() != 1)
        return false;
      const auto *Effect = At[Access.Address].front();
      if (Effect->EndAddress != Access.EndAddress ||
          Effect->AccessKind != RegistrationChainAccess::Kind::Remove ||
          !Physical.erase({Effect->Address, Effect->OpSeq}))
        return false;
      Exits.erase(Exit);
    } else {
      const auto Found = Physical.find({Access.Address, Access.OpSeq});
      if (Found == Physical.end() || !same(*Found->second, Access))
        return false;
      Physical.erase(Found);
    }
  }
  return SeenEntry && Physical.empty() && Exits.empty();
}

std::shared_ptr<const X86RegistrationCxxPhysicalFrame>
analyzePhysicalCxxFrame(const BinaryImage &Image, const LowFunc &Source,
                        LowFunc &Physical) {
  if (!Source.RegistrationStates || !complete(*Source.RegistrationStates) ||
      Physical.Blocks.size() != Source.Blocks.size())
    return nullptr;
  const auto Entry = getCheckedX86RegistrationCxxPrologueCall(Image, Source);
  if (!Entry)
    return nullptr;
  const auto Handler =
      coff_loader::getCheckedX86CxxHandlerABI(Image, *Source.ExceptionMetadata);
  // The output cookie ABI of guarded handlers remains a separate obligation.
  if (!Handler || !Handler->CookieGuards.empty())
    return nullptr;
  const auto &States = *Source.RegistrationStates;
  auto Frame = std::make_shared<X86RegistrationCxxPhysicalFrame>();
  Frame->Entry = *States.CxxPrologue;
  Frame->EntryCall = *Entry;
  std::vector<std::pair<LowInstructionBoundary *, LowInstructionBoundary>>
      Saved;
  llvm::scope_exit Restore([&] {
    for (auto &[At, Original] : Saved)
      *At = Original;
  });
  for (size_t I = 0; I != Source.Blocks.size(); ++I) {
    const auto &Block = Source.Blocks[I];
    auto &Expanded = Physical.Blocks[I];
    if (Expanded.StartAddr != Block.StartAddr ||
        Expanded.EndAddr != Block.EndAddr ||
        Expanded.InstructionBoundaries.size() !=
            Block.InstructionBoundaries.size())
      return nullptr;
    if (Block.InstructionBoundaries.empty())
      continue;
    const auto Exit =
        getCheckedX86RegistrationCxxEpilogueCall(Image, Source, Block);
    const bool IsEntry = Block.EndAddr == Entry->Caller.End &&
                         Block.StartAddr == Entry->Caller.Begin;
    if (!IsEntry && (!Exit || Exit->CookieCheck))
      continue;
    if (Expanded.Ops.empty() || Expanded.Ops.back().Opcode == NdOp::CALL)
      return nullptr;
    if (Exit) {
      const auto &Op = Block.Ops.back();
      Frame->Exits.push_back({{Op.Addr, Block.EndAddr, Op.Seq,
                               RegistrationChainAccess::Kind::Remove},
                              Exit->Helper.Begin});
    }
    // The execution view contains explicit memory/register effects, so its
    // helper boundary is no longer a CALL. Keep the original boundary intact
    // for every source consumer, including after a failed replay.
    auto &Boundary = Expanded.InstructionBoundaries.back();
    Saved.emplace_back(&Boundary, Boundary);
    Boundary.Control = LowInstructionControl::None;
    Boundary.ControlFlags = LowInstructionControlFlag::None;
    Boundary.Immediate.reset();
  }
  auto Replayed = analyzeRegistrationStates(
      Physical, Entry->SecurityCookieVA, 0, &States.CalleeContracts,
      &States.CleanupContracts, nullptr, nullptr, nullptr, &*Handler);
  if (!complete(Replayed) || Replayed.CxxPrologue ||
      Replayed.Blocks.size() != States.Blocks.size() ||
      !std::equal(Replayed.Blocks.begin(), Replayed.Blocks.end(),
                  States.Blocks.begin(),
                  [](const auto &A, const auto &B) { return same(A, B); }) ||
      Replayed.IncomingFrameAccesses != States.IncomingFrameAccesses ||
      Replayed.CallFrameEffects != States.CallFrameEffects ||
      Replayed.CleanupFrameEffects != States.CleanupFrameEffects ||
      Replayed.CxxContinuations != States.CxxContinuations ||
      Replayed.CxxCatchObjects != States.CxxCatchObjects ||
      Replayed.RuntimeObjectAccesses != States.RuntimeObjectAccesses ||
      Replayed.ImageReads.size() != States.ImageReads.size() ||
      !std::equal(Replayed.ImageReads.begin(), Replayed.ImageReads.end(),
                  States.ImageReads.begin(), [](const auto &A, const auto &B) {
                    return A.Begin == B.Begin && A.End == B.End;
                  }))
    return nullptr;
  Frame->ChainAccesses = std::move(Replayed.ChainAccesses);
  return matchesSourceChain(States, *Frame) ? Frame : nullptr;
}
bool hasCurrentPhysicalChain(
    const MedFunc &Function,
    const std::vector<RegistrationChainAccess> &ChainAccesses) {
  const auto Threads = getX86RegistrationSEHThreadAccesses(Function);
  if (!Threads)
    return false;
  std::map<std::pair<va_t, int>, const RegistrationChainAccess *> Remaining;
  for (const auto &Access : ChainAccesses)
    Remaining.emplace(std::make_pair(Access.Address, Access.OpSeq), &Access);
  size_t Work = 0;
  TempDefinitions Definitions;
  auto Define = [&](const MedVar &Value, const MedBlock &Block,
                    const MedOp *Op) {
    if (Value.Kind != MedVar::Temp || !Value.Size || Value.Id < 0)
      return;
    const auto [It, Added] = Definitions.emplace(
        TempKey{Value.Id, Value.SSAVer}, TempDefinition{&Block, Op});
    if (!Added)
      It->second.second = nullptr;
  };
  for (const auto &Block : Function.Blocks) {
    for (const auto &Phi : Block.Phis) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      Define(Phi.Output, Block, nullptr);
    }
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      Define(Op.Output, Block, &Op);
      for (const auto &Output : Op.IntrinsicOutputs) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return false;
        Define(Output, Block, nullptr);
      }
    }
  }
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      if (Op.MemoryAddressSpace != NdMemoryAddressSpace::X86FS)
        continue;
      const auto Found = Remaining.find({Op.Addr, Op.OriginSeq});
      const auto Thread = Threads->find({Op.Addr, Op.OriginSeq});
      if (Thread != Threads->end()) {
        // Source-bound Self loads are ordinary thread reads, never chain
        // effects. Recheck their current materialized offset independently.
        if (Found != Remaining.end() || Op.Dead || Op.Addr < Block.StartAddr ||
            Op.Addr >= Block.EndAddr ||
            Thread->second.Field != RegistrationSEHThreadAccess::Kind::Self ||
            !segmentOffset(Block, Op, Definitions,
                           RegistrationSEHThreadAccess::SelfOffset, Work))
          return false;
        continue;
      }
      const bool Load = Op.Opcode == NdOp::LOAD;
      if (Found == Remaining.end() || Op.Dead || Op.Addr < Block.StartAddr ||
          Found->second->EndAddress > Block.EndAddr ||
          (!Load && Op.Opcode != NdOp::STORE) ||
          Op.NumInputs != (Load ? 1u : 2u) ||
          !segmentOffset(Block, Op, Definitions, 0, Work) ||
          (Load ? Op.Output.Size : Op.Inputs[1].Size) != 4 ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          Load != (Found->second->AccessKind ==
                       RegistrationChainAccess::Kind::ReadPreviousHead ||
                   Found->second->AccessKind ==
                       RegistrationChainAccess::Kind::ReadInstalledHead))
        return false;
      Remaining.erase(Found);
    }
  return Remaining.empty();
}
} // namespace x86_registration

const std::vector<RegistrationChainAccess> *
getX86RegistrationCxxChainAccesses(const MedFunc &Function) {
  if (!Function.RegistrationStates)
    return nullptr;
  const auto &States = *Function.RegistrationStates;
  if (!States.CxxPrologue)
    return Function.RegistrationPhysicalFrame ? nullptr : &States.ChainAccesses;
  const auto *Frame = Function.RegistrationPhysicalFrame.get();
  if (!Frame || !Function.ExceptionMetadata ||
      Function.Entry != Function.ExceptionMetadata->CodeRange.Begin ||
      !Function.ExceptionMetadata->Registration ||
      Function.ExceptionMetadata->Registration->RealignedFrame ||
      !matchesSourceChain(States, *Frame))
    return nullptr;
  return x86_registration::hasCurrentPhysicalChain(Function,
                                                   Frame->ChainAccesses)
             ? &Frame->ChainAccesses
             : nullptr;
}

bool hasCheckedX86RegistrationCxxPhysicalFrame(const BinaryImage &Image,
                                               const MedFunc &Function) {
  if (!getX86RegistrationCxxChainAccesses(Function))
    return false;
  if (!Function.RegistrationPhysicalFrame)
    return true;
  const auto &Frame = *Function.RegistrationPhysicalFrame;
  const auto &EH = *Function.ExceptionMetadata;
  const auto Entry = coff_loader::getCheckedX86CxxPrologueCall(Image, EH);
  const auto Handler = coff_loader::getCheckedX86CxxHandlerABI(Image, EH);
  if (!Entry || !Handler || !Handler->CookieGuards.empty() ||
      Entry->Caller.Begin != Function.Entry ||
      Entry->CallVA != Frame.Entry.Address ||
      Entry->Caller.End != Frame.Entry.EndAddress ||
      Entry->Helper.Begin != Frame.Entry.HelperVA ||
      Entry->PostCallStackOffset != Frame.Entry.StackOffset ||
      Entry->Caller.Begin != Frame.EntryCall.Caller.Begin ||
      Entry->Caller.End != Frame.EntryCall.Caller.End ||
      Entry->Helper.Begin != Frame.EntryCall.Helper.Begin ||
      Entry->Helper.End != Frame.EntryCall.Helper.End ||
      Entry->CallVA != Frame.EntryCall.CallVA ||
      Entry->HandlerVA != Frame.EntryCall.HandlerVA ||
      Entry->SecurityCookieVA != Frame.EntryCall.SecurityCookieVA ||
      Entry->AllocationBytes != Frame.EntryCall.AllocationBytes ||
      Entry->PostCallStackOffset != Frame.EntryCall.PostCallStackOffset ||
      Entry->Writes != Frame.EntryCall.Writes)
    return false;
  for (const auto &Exit : Frame.Exits) {
    const auto Fresh = coff_loader::getCheckedX86CxxEpilogueCall(
        Image, EH, Exit.Source.Address);
    if (!Fresh || Fresh->CookieCheck ||
        Fresh->Caller.End != Exit.Source.EndAddress ||
        Fresh->Helper.Begin != Exit.HelperVA)
      return false;
  }
  return true;
}

} // namespace neverd
