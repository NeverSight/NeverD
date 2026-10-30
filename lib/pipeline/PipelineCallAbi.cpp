//===- PipelineCallAbi.cpp - Module call-ABI recovery --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recovery of every call's arguments and every forwarder's passed-through
/// parameters from the setup each caller writes, bounded by its callee's
/// recovered signature (recoverCallAbi).  The LLVM route uses it for every
/// convention, and the HighIR route for each convention that has no callee
/// summaries (CallArgumentConvention::ArgumentsFromCallSetup).
///
//===----------------------------------------------------------------------===//

#include "PipelineCallAbiDetail.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/IndirectCallSlot.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/ir/med/MedReturnArguments.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"
#include "neverd/loader/ReadOnlyBytesScope.h"
#include "neverd/pipeline/Pipeline.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

namespace {
/// The entries callee lifts know: the function symbols of \p Img that are
/// not boundary guesses.
std::set<va_t> calleeLiftEntries(const BinaryImage &Img) {
  std::set<va_t> Entries;
  for (const auto &Symbol : Img.Symbols)
    if (Symbol.IsFunc && !Symbol.IsBoundaryGuess)
      Entries.insert(Symbol.Addr);
  return Entries;
}

/// The argument counts of prototyped imports that the conversion of an
/// omitted callee reads (PipelineResult::OmittedCalleeArgumentCounts).
const std::map<va_t, int> &
omittedCalleeArgumentCounts(const PipelineResult &Result) {
  return Result.OmittedCalleeArgumentCounts
             ? *Result.OmittedCalleeArgumentCounts
             : Result.CallEntryArgumentCounts;
}
} // namespace

OmittedCalleeMemo::SummaryRead
OmittedCalleeMemo::readAt(const PipelineResult &Result, va_t Key) {
  SummaryRead Read;
  Read.Key = Key;
  if (const auto It = Result.CallMayWriteGPRs.find(Key);
      It != Result.CallMayWriteGPRs.end())
    Read.MayWrite = It->second;
  if (const auto It = Result.CallEntryReadGPRs.find(Key);
      It != Result.CallEntryReadGPRs.end())
    Read.EntryReads = It->second;
  if (const auto It = Result.CallEntryStackArgs.find(Key);
      It != Result.CallEntryStackArgs.end())
    Read.StackArgs = It->second;
  Read.StackSlot = Result.CallEntryStackSlots.count(Key) != 0;
  const std::map<va_t, int> &Counts = omittedCalleeArgumentCounts(Result);
  if (const auto It = Counts.find(Key); It != Counts.end())
    Read.ArgumentCount = It->second;
  return Read;
}

bool OmittedCalleeMemo::Conversion::readsHold(
    const PipelineResult &Result) const {
  return std::all_of(Reads.begin(), Reads.end(), [&](const SummaryRead &Read) {
    return readAt(Result, Read.Key) == Read;
  });
}

const OmittedCalleeMemo::Conversion *
OmittedCalleeMemo::Callee::conversionFor(const PipelineResult &Result) {
  const auto Holds =
      std::find_if(Conversions.begin(), Conversions.end(),
                   [&](const Conversion &C) { return C.readsHold(Result); });
  if (Holds == Conversions.end())
    return nullptr;
  std::rotate(Conversions.begin(), Holds, std::next(Holds));
  return &Conversions.front();
}

void OmittedCalleeMemo::adopt(const BinaryImage &Img,
                              const PipelineResult &Result) {
  if (Image == &Img && SymbolData == Img.Symbols.data() &&
      SymbolCount == Img.Symbols.size() &&
      CallArgumentSources == Result.CallArgumentSources)
    return;
  Image = &Img;
  SymbolData = Img.Symbols.data();
  SymbolCount = Img.Symbols.size();
  CallArgumentSources = Result.CallArgumentSources;
  FunctionEntries = calleeLiftEntries(Img);
  PointerCallees = collectPointerVariableCallees(Img);
  CodeOwners.reset();
  AbsoluteRelocationRoots.reset();
  RuntimeRoutineCalls.reset();
  CodePointerTargets.reset();
  Callees.clear();
  Bytes = 0;
}

const ExecutableCodeOwnerIndex &OmittedCalleeMemo::codeOwners() {
  if (!CodeOwners)
    CodeOwners.emplace(*Image);
  return *CodeOwners;
}

const detail::AbsoluteRelocationRootIndex &
OmittedCalleeMemo::absoluteRelocationRoots() {
  if (!AbsoluteRelocationRoots)
    AbsoluteRelocationRoots =
        std::make_shared<const detail::AbsoluteRelocationRootIndex>(*Image);
  return *AbsoluteRelocationRoots;
}

const std::map<va_t, Intrinsic> &OmittedCalleeMemo::runtimeRoutineCalls() {
  if (!RuntimeRoutineCalls)
    RuntimeRoutineCalls = runtimeRoutineCallsOf(*Image, Image->Arch);
  return *RuntimeRoutineCalls;
}

const std::vector<va_t> &OmittedCalleeMemo::codePointerTargets() {
  if (!CodePointerTargets)
    CodePointerTargets = codePointerTargetsOf(*Image);
  return *CodePointerTargets;
}

OmittedCalleeMemo::Conversion *
OmittedCalleeMemo::conversionOf(const MedFunc &Body) {
  const auto Found = Callees.find(Body.Entry);
  if (Found == Callees.end())
    return nullptr;
  for (Conversion &Each : Found->second.Conversions)
    if (Each.Body.get() == &Body)
      return &Each;
  return nullptr;
}

void OmittedCalleeMemo::keepProbe(Conversion &Holder, Probe Kept) {
  Kept.Bytes =
      sizeof(Probe) + Kept.Reads.size() * sizeof(ProbeRead) +
      (Kept.Gives.FPRegs.size() + Kept.Gives.IntegerArgumentRegisters.size()) *
          sizeof(uint64_t);
  for (const ProbeRead &Read : Kept.Reads)
    Kept.Bytes += (Read.Name ? Read.Name->size() : 0) +
                  ((Read.FPRegs ? Read.FPRegs->size() : 0) +
                   (Read.IntRegs ? Read.IntRegs->size() : 0)) *
                      sizeof(uint64_t);
  if (Holder.Probes.size() == MaxProbes) {
    Holder.Bytes -= Holder.Probes.back().Bytes;
    Bytes -= Holder.Probes.back().Bytes;
    Holder.Probes.pop_back();
  }
  Holder.Bytes += Kept.Bytes;
  Bytes += Kept.Bytes;
  Holder.Probes.insert(Holder.Probes.begin(), std::move(Kept));
}

OmittedCalleeMemo::Callee *OmittedCalleeMemo::find(va_t Entry) {
  Callee &Found = Callees[Entry];
  Found.LastUsed = ++Clock;
  return &Found;
}

const OmittedCalleeMemo::Conversion &
OmittedCalleeMemo::keep(Callee &Holder, Conversion Converted) {
  if (Converted.Body) {
    Converted.Bytes = sizeof(MedFunc);
    for (const MedBlock &Block : Converted.Body->Blocks)
      Converted.Bytes += sizeof(MedBlock) + Block.Ops.size() * sizeof(MedOp) +
                         Block.Phis.size() * sizeof(PhiNode);
    if (const auto &Summary = Converted.Summary)
      Converted.Bytes +=
          sizeof(CalleeSummary) +
          (Summary->IntegerArgumentRegisters.size() + Summary->FPRegs.size()) *
              sizeof(uint64_t);
    Converted.Bytes +=
        (Converted.CalleesRead.size() + Converted.DirectCallTargets.size()) *
        sizeof(va_t);
  }
  if (Holder.Conversions.size() == MaxConversions) {
    Bytes -= Holder.Conversions.back().Bytes;
    Holder.Conversions.pop_back();
  }
  Bytes += Converted.Bytes;
  Holder.Conversions.insert(Holder.Conversions.begin(), std::move(Converted));
  if (Bytes > MaxBytes) {
    // A callee's lift stays: it holds no body.
    std::vector<std::pair<uint64_t, va_t>> Order;
    for (const auto &[Entry, Read] : Callees)
      if (&Read != &Holder && !Read.Conversions.empty())
        Order.emplace_back(Read.LastUsed, Entry);
    std::sort(Order.begin(), Order.end());
    for (const auto &[Used, Entry] : Order) {
      if (Bytes <= MaxBytes / 4 * 3)
        break;
      Callee &Gone = Callees.at(Entry);
      for (const Conversion &Each : Gone.Conversions)
        Bytes -= Each.Bytes;
      Gone.Conversions.clear();
    }
  }
  return Holder.Conversions.front();
}

namespace {

/// What \p MF's own parameters say of the calls to it before recovery.
OmittedCalleeMemo::CalleeSummary summarizeCallee(const MedFunc &MF,
                                                 const BinaryImage &Img);

/// The callees a run reads the signature bodies of for \p F, in order: its
/// direct callees and the routines the function-pointer variables it calls
/// through start at (omittedDirectCalleeSignatures).
std::vector<va_t> calleesToRead(const MedFunc &F, const BinaryImage &Img,
                                const PipelineResult &Result) {
  std::vector<va_t> Callees;
  // A call through a function-pointer variable passes the arguments of the
  // routine of this image the variable starts at (CallArgumentSources).
  MedDefinitionIndex Definitions(F);
  auto lookupDefinition = [&](const MedVar &V) { return Definitions.find(V); };
  // A call to the stub of an import the loader starts at this image's own
  // routine passes that routine's arguments.
  for (const MedBlock &B : F.Blocks)
    for (const MedOp &Op : B.Ops)
      if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst())
        Callees.push_back(importOwnRoutine(Img, &Result.CallArgumentSources,
                                           Op.Inputs[0].ConstVal)
                              .value_or(Op.Inputs[0].ConstVal));
      else if (Op.Opcode == NdOp::INDIR_CALL &&
               !Result.CallArgumentSources.empty())
        if (const std::optional<va_t> Slot =
                indirectCallSlot(F, Op, lookupDefinition))
          if (const auto Source = Result.CallArgumentSources.find(*Slot);
              Source != Result.CallArgumentSources.end() &&
              Source->second != *Slot)
            Callees.push_back(Source->second);
  return Callees;
}

/// The target of each direct call of \p F, in order.
std::vector<va_t> directCallTargets(const MedFunc &F) {
  std::vector<va_t> Targets;
  for (const auto &Blk : F.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
          Op.Inputs[0].isConst())
        Targets.push_back(Op.Inputs[0].ConstVal);
  return Targets;
}

/// Releases the spare capacity of \p Med's containers, which grew by
/// doubling while it was built.  Its contents do not change.
void shrinkToFit(MedFunc &Med) {
  for (MedBlock &Block : Med.Blocks) {
    for (MedOp &Op : Block.Ops)
      Op.IntrinsicOutputs.shrink_to_fit();
    for (PhiNode &Phi : Block.Phis)
      Phi.Args.shrink_to_fit();
    Block.Ops.shrink_to_fit();
    Block.Phis.shrink_to_fit();
    Block.Succs.shrink_to_fit();
    Block.Preds.shrink_to_fit();
  }
  Med.Blocks.shrink_to_fit();
}

/// A selected function still calls the image's other native functions. Use
/// the normal parameter detector for their bodies instead of assigning the
/// default register order merely because the user omitted their output.
/// These bounded support bodies never become emitted definitions.
/// Reads callees' signature bodies for a run under \p Result's call
/// summaries: their lifts with the image's function symbols as the known
/// entries, and their conversions, into \p Result's memo where it has room.
class CalleeReader {
public:
  /// \p Result's memo, where it has one, has adopted \p Img.
  CalleeReader(const BinaryImage &Img, const PipelineResult &Result)
      : Img(Img), Result(Result), Memo(Result.CalleeSignatureMemo.get()),
        ReadScope(Img), StackPops(Img), StackPopScope(StackPops) {
    if (!Memo)
      OwnEntries = calleeLiftEntries(Img);
    Entries = Memo ? &Memo->functionEntries() : &OwnEntries;
    Builder.setKnownFuncEntries(Entries);
    // The callees' builds prove each x87 call effect from the same callee
    // graphs: build each once.  A 32-bit printf engine in a call cycle with
    // the heap reaches every build here, and its jump tables take seconds.
    Builder.setX87CallGraphCache(createX87CallGraphCache());
  }
  bool init() { return Dec.init(Img); }
  /// Whether \p Target is a callee the read takes: a function of this image.
  bool reads(va_t Target) const {
    return Img.isFunctionEntryCandidateAt(Target) &&
           !Img.findImportAt(Target) && !Img.isImportStubAt(Target) &&
           (Entries->count(Target) || Img.hasKnownFunctionEntryAt(Target)) &&
           Img.hasExecutableCodeOwnerAt(Target);
  }
  /// Where \p Target's reading is kept: the memo's, or \p Local where the
  /// run keeps none.
  OmittedCalleeMemo::Callee &callee(va_t Target,
                                    OmittedCalleeMemo::Callee &Local) {
    return Memo ? *Memo->find(Target) : Local;
  }
  /// Lifts \p Target into \p Low, once.
  const LowFunc &lift(va_t Target, std::optional<LowFunc> &Low) {
    if (!Low) {
      // The lifts look the image's code owners and absolute relocations up
      // in one index each: the memo's, or ones built for the first lift of
      // this read.
      if (!OwnersLent) {
        Builder.setExecutableCodeOwnerIndex(Memo ? &Memo->codeOwners()
                                                 : &Owners.emplace(Img));
        Builder.setAbsoluteRelocationRootIndex(
            Memo ? &Memo->absoluteRelocationRoots()
                 : &RelocationRoots.emplace(Img));
        OwnersLent = true;
      }
      Low.emplace(
          Builder.build(Img, Dec, Target, Img.getFunctionNameAt(Target)));
    }
    return *Low;
  }
  /// Reads \p Target's lift into \p Read: the operations of its blocks.
  void readLift(va_t Target, OmittedCalleeMemo::Callee &Read,
                std::optional<LowFunc> &Low) {
    if (Read.Lifted)
      return;
    const LowFunc &Lifted = lift(Target, Low);
    if (Lifted.hasCompleteLiftCoverage() &&
        Lifted.UnsafeIndirectBranchAddresses.empty()) {
      Read.BlockOps.emplace();
      for (const auto &Block : Lifted.Blocks)
        Read.BlockOps->push_back(Block.Ops.size());
    }
    Read.Lifted = true;
  }
  /// The conversion of \p Target's lift the run's summaries hold for,
  /// converting it where none does.  \p Kept says \p Read is the memo's.
  const OmittedCalleeMemo::Conversion &
  conversion(va_t Target, OmittedCalleeMemo::Callee &Read, bool Kept,
             std::optional<LowFunc> &Low) {
    // A conversion that read summaries this run states otherwise serves
    // none of its reads: the callee is converted again.
    if (const OmittedCalleeMemo::Conversion *Held = Read.conversionFor(Result))
      return *Held;
    const LowFunc &Lifted = lift(Target, Low);
    std::shared_ptr<const MedFunc> Body;
    // The callee's parameters read the entry-read summary of its own entry.
    std::set<va_t> Keys{Target};
    try {
      LowToMedConverter Converter;
      Converter.setBinaryImage(&Img);
      Converter.setSourceCallHintsEnabled(false);
      Converter.setCallMayWriteGPRs(&Result.CallMayWriteGPRs);
      Converter.setCallEntryReadGPRs(&Result.CallEntryReadGPRs);
      Converter.setCallArgumentSources(&Result.CallArgumentSources);
      Converter.setCallEntryStackArgs(&Result.CallEntryStackArgs);
      Converter.setCallEntryStackSlots(&Result.CallEntryStackSlots);
      Converter.setCallEntryArgumentCounts(
          &omittedCalleeArgumentCounts(Result));
      Converter.setCallSummaryKeysRead(&Keys);
      Converter.setRuntimeRoutineCalls(&runtimeRoutineCalls());
      Converter.setCodePointerTargets(&codePointerTargets());
      MedFunc Med = Converter.convert(Lifted, Img.Arch, Img.abiFormat());
      size_t MedOperations = 0;
      for (const auto &Block : Med.Blocks)
        MedOperations += Block.Ops.size();
      if (Med.Blocks.size() <= limits::kMaxStructurableMedBlocks &&
          MedOperations <= static_cast<size_t>(limits::kMaxSSANodes)) {
        inferMedTypes(Med, Img.Arch);
        // The memo keeps the body: drop the spare capacity its vectors grew
        // by while it was built.
        if (Memo && Kept)
          shrinkToFit(Med);
        Body = std::make_shared<const MedFunc>(std::move(Med));
      }
    } catch (const LowToMedConversionError &) {
      // An unsupported callee's body supplies no signature evidence.
    }
    OmittedCalleeMemo::Conversion Converted{std::move(Body), {}};
    for (const va_t Key : Keys)
      Converted.Reads.push_back(OmittedCalleeMemo::readAt(Result, Key));
    if (Memo && Kept) {
      // Every run that reads the body reads these of it.
      if (Converted.Body) {
        Converted.Summary = summarizeCallee(*Converted.Body, Img);
        Converted.CalleesRead = calleesToRead(*Converted.Body, Img, Result);
        Converted.DirectCallTargets = directCallTargets(*Converted.Body);
      }
      return Memo->keep(Read, std::move(Converted));
    }
    Read.Conversions.insert(Read.Conversions.begin(), std::move(Converted));
    return Read.Conversions.front();
  }

private:
  /// The runtime routines every conversion reads: the memo's, or the
  /// reader's own, read once.
  const std::map<va_t, Intrinsic> &runtimeRoutineCalls() {
    if (Memo)
      return Memo->runtimeRoutineCalls();
    if (!OwnRuntimeRoutineCalls)
      OwnRuntimeRoutineCalls = runtimeRoutineCallsOf(Img, Img.Arch);
    return *OwnRuntimeRoutineCalls;
  }
  /// The code pointer targets every conversion reads, likewise.
  const std::vector<va_t> &codePointerTargets() {
    if (Memo)
      return Memo->codePointerTargets();
    if (!OwnCodePointerTargets)
      OwnCodePointerTargets = codePointerTargetsOf(Img);
    return *OwnCodePointerTargets;
  }

  const BinaryImage &Img;
  const PipelineResult &Result;
  OmittedCalleeMemo *Memo;
  // Every supplemental lift reads this unchanged image. Keep the same
  // immutable fixup inventory and independently charged stack answers across
  // its callees, as the ordinary LowIR worker does across its functions.
  ReadOnlyBytesScope ReadScope;
  RegistrationStackPopMemo StackPops;
  RegistrationStackPopMemoScope StackPopScope;
  std::set<va_t> OwnEntries;
  const std::set<va_t> *Entries = nullptr;
  Decoder Dec;
  std::optional<ExecutableCodeOwnerIndex> Owners;
  std::optional<detail::AbsoluteRelocationRootIndex> RelocationRoots;
  bool OwnersLent = false;
  std::optional<std::map<va_t, Intrinsic>> OwnRuntimeRoutineCalls;
  std::optional<std::vector<va_t>> OwnCodePointerTargets;
  CFGBuilder Builder;
};

std::vector<std::shared_ptr<const MedFunc>>
omittedDirectCalleeSignatures(const BinaryImage &Img,
                              const PipelineResult &Result) {
  const auto *Convention = callArgumentConvention(Img.Arch, Img.abiFormat());
  if (!Convention || !Convention->ArgumentsFromCallSetup || Img.IsRelocatable)
    return {};
  std::set<va_t> Present, Queued;
  std::queue<va_t> Pending;
  // Each callee is read as an earlier run of the image read it, where the
  // run shares what those read.
  if (OmittedCalleeMemo *Memo = Result.CalleeSignatureMemo.get())
    Memo->adopt(Img, Result);
  CalleeReader Reader(Img, Result);
  for (const MedFunc &F : Result.MedFuncs)
    Present.insert(F.Entry);
  auto enqueue = [&](va_t Target) {
    if (!Present.count(Target) && Reader.reads(Target) &&
        Queued.insert(Target).second)
      Pending.push(Target);
  };
  auto enqueueCallees = [&](llvm::ArrayRef<va_t> Callees) {
    for (const va_t Callee : Callees)
      enqueue(Callee);
  };
  for (const MedFunc &F : Result.MedFuncs)
    enqueueCallees(calleesToRead(F, Img, Result));
  std::vector<std::shared_ptr<const MedFunc>> Signatures;
  if (Pending.empty())
    return Signatures;
  if (!Reader.init())
    return Signatures;
  size_t Inspected = 0;
  size_t Operations = 0;
  // Visit the selected functions' direct callees before their descendants.
  // A deep lower-address chain must not consume the bounded support budget
  // before another direct callee can supply its signature.
  while (!Pending.empty() && Inspected < limits::kMaxCallEffectExtraLifts) {
    ++Inspected;
    const va_t Target = Pending.front();
    Pending.pop();
    Present.insert(Target);
    OmittedCalleeMemo::Callee Local;
    OmittedCalleeMemo::Callee &Read = Reader.callee(Target, Local);
    std::optional<LowFunc> Low;
    Reader.readLift(Target, Read, Low);
    if (!Read.BlockOps)
      continue;
    for (const size_t BlockOps : *Read.BlockOps) {
      if (BlockOps > limits::kMaxSSAFunctionOps - Operations)
        return Signatures;
      Operations += BlockOps;
    }
    const OmittedCalleeMemo::Conversion &Conversion =
        Reader.conversion(Target, Read, &Read != &Local, Low);
    if (!Conversion.Body)
      continue;
    if (Conversion.Summary)
      enqueueCallees(Conversion.CalleesRead);
    else
      enqueueCallees(calleesToRead(*Conversion.Body, Img, Result));
    Signatures.push_back(Conversion.Body);
  }
  return Signatures;
}

// Variadic overflow parameters are finalized only after call recovery.  Keep
// their interim arity open so neither recovery pass truncates the caller's
// recovered stack tail to the currently known fixed prefix.
int callRecoveryTotalArity(const MedFunc &Func, int MaxParamIndex) {
  return Func.IsVariadic ? limits::kMaxCallArgs : MaxParamIndex + 1;
}

int integerParameterIndex(const MedVar &Param,
                          const IntegerArgumentLayout &Layout) {
  if (Param.RegOff != kNoParamReg)
    return Layout.registerIndex(Param.RegOff);
  return Param.Kind == MedVar::Param ? Param.Id : -1;
}

/// Recovery may collect local spills beyond a callee's final ordinary
/// signature. A checked x87 argument follows that entire recovered prefix:
/// remove surplus ordinary arguments before it, never the argument itself.
void boundRecoveredArguments(MedCallInfo &Call, const MedFunc &Callee) {
  if (Callee.IsVariadic || Callee.SourceParametersBound ||
      Call.Args.size() == Callee.Params.size())
    return;
  if (Callee.ConsumesX87Argument && !Callee.Params.empty() &&
      !Call.Args.empty()) {
    if (Call.Args.size() > Callee.Params.size())
      Call.Args.erase(Call.Args.begin() + Callee.Params.size() - 1,
                      Call.Args.end() - 1);
    // A missing ordinary value cannot move the x87 argument into its slot.
    // Keep the missing value explicit for source views and native refusal.
    while (Call.Args.size() < Callee.Params.size()) {
      const MedVar &Param = Callee.Params[Call.Args.size() - 1];
      Call.Args.insert(Call.Args.end() - 1,
                       MedVar::makeUnspecified(Param.Size, Param.TheArch));
    }
  } else if (Call.Args.size() > Callee.Params.size()) {
    Call.Args.resize(Callee.Params.size());
  }
}

/// The integer register arguments a call to \p Func passes through: its
/// register parameters through \p MaxRegisterIndex, but only the named ones
/// of a variadic function whose save area spills the rest.
int callRegisterArity(const MedFunc &Func, int MaxRegisterIndex) {
  const int Arity = MaxRegisterIndex + 1;
  return Func.IsVariadic && Func.VariadicFirstRegister >= 0
             ? std::min(Arity, Func.VariadicFirstRegister)
             : Arity;
}

/// Count FP arguments a callee takes from its entry-block live-in self-copies
/// (`COPY D0,D0; COPY D1,D1; ...` before any real body).  Used to prime
/// CalleeFPArity for intra-module callees (e.g. `mkD2`) before recoverCallAbi
/// runs, so a tail-call struct-return forwarder (`fwdD2`) can recover its
/// forwarded d0/d1 live-ins (KnownFPCallee gate in recoverCallAbi).
int countEntryLiveInFPArgs(const MedFunc &MF, const TargetRegInfo &TRI,
                           BinaryFormat Format) {
  if (MF.Blocks.empty())
    return 0;
  int Count = 0;
  for (const auto &O : MF.Blocks.front().Ops) {
    if (O.Opcode == NdOp::COPY && O.NumInputs >= 1 &&
        O.Inputs[0].Kind == MedVar::Reg &&
        O.Inputs[0].RegOff == O.Output.RegOff &&
        TRI.isFPArgReg(O.Output.RegOff, Format))
      ++Count;
    else if (O.Opcode == NdOp::COPY && TRI.isLinkRegister(O.Output.RegOff))
      continue;
    else
      break;
  }
  return Count;
}

OmittedCalleeMemo::CalleeSummary summarizeCallee(const MedFunc &MF,
                                                 const BinaryImage &Img) {
  OmittedCalleeMemo::CalleeSummary Summary;
  const auto &TRI = getTargetRegInfo(Img.Arch);
  const auto FPParamRegs = TRI.floatingParamRegs(Img.abiFormat());
  // Internal x86/x86-64 scalar float/double, AArch64, and ARM hard-float
  // calls return through XMM0/V0/D0.  External i386 cdecl calls are excluded
  // by recoverCallAbi's relocation/import gates; x86 long double uses x87.
  auto modelsScalarFPReturnInVectorReg = [&](uint16_t Size) {
    if (TRI.WideFloatsReturnInX87 && Size > 8)
      return false;
    return TRI.isVectorReg(TRI.fpReturnModelReg());
  };
  int MaxRegIdx = -1, MaxIdx = -1;
  const IntegerArgumentLayout IntegerLayout = integerArgumentLayoutOf(MF, TRI);
  Summary.IntegerArgumentRegisters = MF.IntegerArgumentRegisters;
  // The exact FP-argument register offsets, in ABI order.  ARM `float` args
  // land in the single-width S registers (s0,s1,..) and `double` args in
  // the D registers (d0,d1,..); recording the layout lets the caller
  // recover FP arguments at the registers the callee actually reads (s1 !=
  // d1).
  std::vector<uint64_t> &FPRegs = Summary.FPRegs;
  const uint64_t IRR = TRI.indirectResultReg();
  for (const auto &P : MF.Params) {
    if (IRR != 0 && P.RegOff == IRR) {
      // Hidden indirect-result (sret) pointer (AArch64 x8): not an ordinary
      // integer/FP/stack argument; recorded separately.
      Summary.HasSret = true;
    } else if (P.RegOff != kNoParamReg &&
               TRI.isFPArgReg(P.RegOff, Img.abiFormat())) {
      // Floating-point/vector argument register: counted separately.
      FPRegs.push_back(P.RegOff);
    } else {
      const int Index = integerParameterIndex(P, IntegerLayout);
      if (P.RegOff != kNoParamReg)
        MaxRegIdx = std::max(MaxRegIdx, Index);
      MaxIdx = std::max(MaxIdx, Index);
    }
  }
  for (const auto &Blk : MF.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
          Op.Inputs[0].isConst()) {
        if (const Import *Imp = Img.findImportAt(Op.Inputs[0].ConstVal)) {
          if (libc::isVaListConsumer(stripLeadingUnderscores(Imp->Name)))
            Summary.ConsumesVaList = true;
        }
      }
  // A struct return field the function passes through unchanged is an
  // argument its callers pass in that register, and so is each argument
  // register of the field's class before it.
  unsigned ReturnedFPArgs = 0;
  for (const MedReturnReg &Field : MF.MultiReturn) {
    if (!returnsArgumentUnchanged(MF, Field, TRI, Img.abiFormat()))
      continue;
    if (Field.IsFP) {
      ReturnedFPArgs = std::max(
          ReturnedFPArgs,
          *argumentRegisterIndex(TRI, Img.abiFormat(), Field.RegOff, true) + 1);
    } else if (const int Idx = IntegerLayout.registerIndex(Field.RegOff);
               Idx >= 0) {
      MaxRegIdx = std::max(MaxRegIdx, Idx);
      MaxIdx = std::max(MaxIdx, Idx);
    }
  }
  std::sort(FPRegs.begin(), FPRegs.end());
  Summary.RegArity = callRegisterArity(MF, MaxRegIdx);
  Summary.TotalArity = callRecoveryTotalArity(MF, MaxIdx);
  Summary.IsVariadic = MF.IsVariadic;
  Summary.VectorArguments = MF.takesVectorArgumentRegisters();
  int FpArity = static_cast<int>(FPRegs.size());
  // Params are not recovered yet (recoverCallAbi runs later), so fall back
  // to entry live-in self-copies for intra-module FP callees like `mkD2`.
  // Without this, tail-call struct-return forwarders (`fwdD2`) miss the
  // KnownFPCallee gate and pass 0.0 for forwarded d0/d1.
  if (FpArity == 0) {
    FpArity = countEntryLiveInFPArgs(MF, TRI, Img.abiFormat());
    if (FpArity > 0)
      FPRegs.assign(FPParamRegs.begin(), FPParamRegs.begin() + FpArity);
  }
  if (ReturnedFPArgs > 0) {
    for (unsigned K = 0; K < ReturnedFPArgs && K < FPParamRegs.size(); ++K)
      if (std::find(FPRegs.begin(), FPRegs.end(), FPParamRegs[K]) ==
          FPRegs.end())
        FPRegs.push_back(FPParamRegs[K]);
    std::sort(FPRegs.begin(), FPRegs.end());
    FpArity = std::max(FpArity, static_cast<int>(FPRegs.size()));
  }
  Summary.FPArity = FpArity;
  const uint16_t ReturnSize =
      MF.ReturnType && MF.ReturnType->Size ? MF.ReturnType->Size : 8;
  const bool ReturnsScalarFP = MF.ReturnType &&
                               MF.ReturnType->Kind == NdTypeKind::Float &&
                               MF.MultiReturn.empty() && !MF.FPReturnViaX87 &&
                               modelsScalarFPReturnInVectorReg(ReturnSize);
  Summary.FPReturnSize = ReturnsScalarFP ? ReturnSize : 0;
  return Summary;
}

/// The value \p Map holds under \p Key, where it holds one.
template <typename Value>
std::optional<Value> valueAt(const std::map<va_t, Value> &Map, va_t Key) {
  const auto Found = Map.find(Key);
  if (Found == Map.end())
    return std::nullopt;
  return Found->second;
}

/// Reach a fixed point for parameters forwarded only through direct calls.
///
/// Optimized wrappers can consume an incoming argument solely by passing the
/// register on to another function.  Their initial parameter scan therefore
/// reports arity zero; recoverCallAbi surfaces the live-in only after the
/// callee's arity is known.  A single global pass is order-dependent for a
/// chain such as `outer -> middle -> leaf`: leaf seeds x0, then middle and
/// outer each need a later visit.  Probe copies let us propagate those arities
/// without repeatedly mutating real CallInfos or inserting call-lane helper
/// ops.  The worklist revisits only direct callers of a function whose
/// signature grew.  A body \p Kept holds the memo's conversion of takes the
/// probe the conversion keeps whose reads the summaries hold: one of an
/// earlier run, around another selection, gives what a probe would.
void propagateForwardedCallArities(
    const std::vector<const MedFunc *> &Funcs,
    llvm::ArrayRef<OmittedCalleeMemo::Conversion *> Kept,
    OmittedCalleeMemo *Memo, Arch TheArch,
    const std::map<va_t, std::string> &FuncNames, const BinaryImage &Img,
    std::map<va_t, int> &CalleeRegArity, std::map<va_t, int> &CalleeTotalArity,
    std::map<va_t, int> &CalleeFPArity,
    const std::map<va_t, uint16_t> &CalleeFPReturnSize,
    std::map<va_t, std::vector<uint64_t>> &CalleeFPRegs,
    const std::map<va_t, bool> &CalleeHasSret,
    std::map<va_t, bool> &CalleeIsVariadic,
    const std::map<va_t, bool> &CalleeConsumesVaList,
    std::map<va_t, std::vector<uint64_t>> &CalleeIntRegs,
    const std::set<va_t> &VectorArgumentCallees,
    const std::map<va_t, PointerVariableCallee> &PointerCallees,
    const std::set<va_t> &PrototypeArgumentCallees,
    const RelocationSymbolIndex &RelocationSymbols) {
  if (Funcs.empty())
    return;

  std::map<va_t, std::vector<size_t>> DirectCallers;
  std::set<va_t> Entries;
  for (const MedFunc *MF : Funcs)
    Entries.insert(MF->Entry);
  for (size_t I = 0; I < Funcs.size(); ++I) {
    const OmittedCalleeMemo::Conversion *Held =
        I < Kept.size() ? Kept[I] : nullptr;
    std::vector<va_t> Own;
    if (!Held || !Held->Summary)
      Own = directCallTargets(*Funcs[I]);
    for (const va_t Target :
         Held && Held->Summary ? Held->DirectCallTargets : Own)
      if (Entries.count(Target) != 0)
        DirectCallers[Target].push_back(I);
  }

  std::queue<size_t> Work;
  std::vector<bool> Queued(Funcs.size(), true);
  for (size_t I = 0; I < Funcs.size(); ++I)
    Work.push(I);

  const auto &TRI = getTargetRegInfo(TheArch);
  // Every summary, user prototype and name the run holds under \p Key.
  auto readAt = [&](va_t Key) {
    OmittedCalleeMemo::ProbeRead Read;
    Read.Key = Key;
    Read.Name = valueAt(FuncNames, Key);
    Read.RegArity = valueAt(CalleeRegArity, Key);
    Read.TotalArity = valueAt(CalleeTotalArity, Key);
    Read.FPArity = valueAt(CalleeFPArity, Key);
    Read.FPReturnSize = valueAt(CalleeFPReturnSize, Key);
    Read.FPRegs = valueAt(CalleeFPRegs, Key);
    Read.HasSret = valueAt(CalleeHasSret, Key);
    Read.IsVariadic = valueAt(CalleeIsVariadic, Key);
    Read.ConsumesVaList = valueAt(CalleeConsumesVaList, Key);
    Read.IntRegs = valueAt(CalleeIntRegs, Key);
    Read.VectorArguments = VectorArgumentCallees.count(Key) != 0;
    Read.PrototypeArguments = PrototypeArgumentCallees.count(Key) != 0;
    return Read;
  };
  // Whether the run holds what \p Read read under its key: readAt(Key) ==
  // Read, without copying what the run holds.
  auto holds = [&](const OmittedCalleeMemo::ProbeRead &Read) {
    const va_t Key = Read.Key;
    auto same = [Key](const auto &Map, const auto &Value) {
      const auto Found = Map.find(Key);
      return Found == Map.end() ? !Value : Value && *Value == Found->second;
    };
    return same(CalleeRegArity, Read.RegArity) &&
           same(CalleeTotalArity, Read.TotalArity) &&
           same(CalleeIsVariadic, Read.IsVariadic) &&
           same(CalleeFPArity, Read.FPArity) &&
           same(CalleeFPReturnSize, Read.FPReturnSize) &&
           same(CalleeHasSret, Read.HasSret) &&
           same(CalleeConsumesVaList, Read.ConsumesVaList) &&
           same(CalleeFPRegs, Read.FPRegs) &&
           same(CalleeIntRegs, Read.IntRegs) &&
           (VectorArgumentCallees.count(Key) != 0) == Read.VectorArguments &&
           (PrototypeArgumentCallees.count(Key) != 0) ==
               Read.PrototypeArguments &&
           same(FuncNames, Read.Name);
  };
  // What recovering a copy of Funcs[I] under the summaries gives, and the
  // summaries the recovery leaves at its entry, or a kept probe's where
  // they read the same.
  auto probe = [&](size_t I) {
    const MedFunc &Body = *Funcs[I];
    OmittedCalleeMemo::Conversion *Holder = I < Kept.size() ? Kept[I] : nullptr;
    if (Holder)
      for (auto It = Holder->Probes.begin(); It != Holder->Probes.end(); ++It) {
        if (!std::all_of(It->Reads.begin(), It->Reads.end(), holds))
          continue;
        std::rotate(Holder->Probes.begin(), It, std::next(It));
        const OmittedCalleeMemo::ProbeSignature &Gives =
            Holder->Probes.front().Gives;
        if (Gives.EntryRegArity)
          CalleeRegArity[Body.Entry] = *Gives.EntryRegArity;
        if (Gives.EntryTotalArity)
          CalleeTotalArity[Body.Entry] = *Gives.EntryTotalArity;
        if (Gives.EntryIsVariadic)
          CalleeIsVariadic[Body.Entry] = *Gives.EntryIsVariadic;
        return Gives;
      }
    // The recovery writes summaries at the entry: read them before it runs.
    const OmittedCalleeMemo::ProbeRead EntryRead = readAt(Body.Entry);
    MedFunc Probe = Body;
    std::vector<va_t> Keys;
    recoverCallAbi(Probe, TheArch, FuncNames, &Img, &CalleeRegArity,
                   &CalleeTotalArity, &CalleeFPArity, &CalleeFPReturnSize,
                   &CalleeFPRegs, &CalleeHasSret, &CalleeIsVariadic,
                   &CalleeConsumesVaList, /*FrameLocalLeafCallees=*/nullptr,
                   &CalleeIntRegs, &VectorArgumentCallees, &PointerCallees,
                   &PrototypeArgumentCallees, &RelocationSymbols,
                   Holder ? &Keys : nullptr);
    int MaxRegIdx = -1;
    int MaxIdx = -1;
    OmittedCalleeMemo::ProbeSignature Gives;
    const uint64_t IRR = TRI.indirectResultReg();
    const IntegerArgumentLayout ProbeLayout =
        integerArgumentLayoutOf(Probe, TRI);
    for (const auto &P : Probe.Params) {
      if (IRR != 0 && P.RegOff == IRR)
        continue;
      if (P.RegOff != kNoParamReg &&
          TRI.isFPArgReg(P.RegOff, Img.abiFormat())) {
        Gives.FPRegs.push_back(P.RegOff);
      } else {
        const int Index = integerParameterIndex(P, ProbeLayout);
        if (P.RegOff != kNoParamReg)
          MaxRegIdx = std::max(MaxRegIdx, Index);
        MaxIdx = std::max(MaxIdx, Index);
      }
    }
    std::sort(Gives.FPRegs.begin(), Gives.FPRegs.end());
    Gives.RegisterArity = callRegisterArity(Probe, MaxRegIdx);
    Gives.MaxArgumentIndex = MaxIdx;
    Gives.RecoveryTotalArity = callRecoveryTotalArity(Probe, MaxIdx);
    Gives.IsVariadic = Probe.IsVariadic;
    Gives.IntegerArgumentRegisters = Probe.IntegerArgumentRegisters;
    Gives.EntryRegArity = valueAt(CalleeRegArity, Body.Entry);
    Gives.EntryTotalArity = valueAt(CalleeTotalArity, Body.Entry);
    Gives.EntryIsVariadic = valueAt(CalleeIsVariadic, Body.Entry);
    if (Holder) {
      // The recovery wrote summaries only at the entry, read before it ran:
      // the others hold what it read.
      OmittedCalleeMemo::Probe Recorded{{EntryRead}, Gives};
      std::sort(Keys.begin(), Keys.end());
      Keys.erase(std::unique(Keys.begin(), Keys.end()), Keys.end());
      for (const va_t Key : Keys)
        if (Key != Body.Entry)
          Recorded.Reads.push_back(readAt(Key));
      Memo->keepProbe(*Holder, std::move(Recorded));
    }
    return Gives;
  };
  while (!Work.empty()) {
    const size_t I = Work.front();
    Work.pop();
    Queued[I] = false;

    const va_t Entry = Funcs[I]->Entry;
    const int PreviousRegArity = CalleeRegArity[Entry];
    const int PreviousTotalArity = CalleeTotalArity[Entry];
    const bool PreviousVariadic =
        CalleeIsVariadic.count(Entry) != 0 && CalleeIsVariadic.at(Entry);
    const OmittedCalleeMemo::ProbeSignature Probe = probe(I);
    const bool IsVariadicPublic =
        Probe.IsVariadic ||
        (CalleeIsVariadic.count(Entry) != 0 && CalleeIsVariadic.at(Entry));
    const int TotalArity = IsVariadicPublic ? Probe.MaxArgumentIndex + 1
                                            : Probe.RecoveryTotalArity;
    const int FPArity = static_cast<int>(Probe.FPRegs.size());
    if (IsVariadicPublic)
      CalleeIsVariadic[Entry] = true;
    // A routine found variadic only here, a va_list wrapper, keeps its arity
    // open for the overflow its callers pass, as one whose prologue says so
    // does (callRecoveryTotalArity).
    if (IsVariadicPublic && !PreviousVariadic)
      CalleeTotalArity[Entry] =
          std::max(CalleeTotalArity[Entry], limits::kMaxCallArgs);
    bool Grew = CalleeRegArity[Entry] > PreviousRegArity ||
                CalleeTotalArity[Entry] > PreviousTotalArity ||
                (CalleeIsVariadic.count(Entry) != 0 &&
                 CalleeIsVariadic.at(Entry) != PreviousVariadic);
    // A forwarder takes its parameters in the order of the function it passes
    // them to, and its callers pass them so.
    if (!Probe.IntegerArgumentRegisters.empty() &&
        CalleeIntRegs[Entry] != Probe.IntegerArgumentRegisters) {
      CalleeIntRegs[Entry] = Probe.IntegerArgumentRegisters;
      Grew = true;
    }
    if (Probe.RegisterArity > CalleeRegArity[Entry]) {
      CalleeRegArity[Entry] = Probe.RegisterArity;
      Grew = true;
    }
    if (!IsVariadicPublic) {
      if (TotalArity > CalleeTotalArity[Entry]) {
        CalleeTotalArity[Entry] = TotalArity;
        Grew = true;
      }
      if (FPArity > CalleeFPArity[Entry]) {
        CalleeFPArity[Entry] = FPArity;
        CalleeFPRegs[Entry] = Probe.FPRegs;
        Grew = true;
      }
    }
    if (!Grew)
      continue;

    auto CallerIt = DirectCallers.find(Entry);
    if (CallerIt == DirectCallers.end())
      continue;
    for (size_t Caller : CallerIt->second)
      if (!Queued[Caller]) {
        Queued[Caller] = true;
        Work.push(Caller);
      }
  }
}

} // namespace

AnalysisCacheWarmup warmOmittedCallees(const BinaryImage &Img,
                                       const PipelineResult &Summaries,
                                       llvm::ArrayRef<va_t> Functions) {
  AnalysisCacheWarmup Warmup;
  OmittedCalleeMemo *Memo = Summaries.CalleeSignatureMemo.get();
  const auto *Convention = callArgumentConvention(Img.Arch, Img.abiFormat());
  if (!Memo || !Convention || !Convention->ArgumentsFromCallSetup ||
      Img.IsRelocatable)
    return Warmup;
  Memo->adopt(Img, Summaries);
  CalleeReader Reader(Img, Summaries);
  if (!Reader.init())
    return Warmup;
  // Warming fills three quarters of the memo, and leaves the rest to the
  // callees selections read: bodies past it would only push those out.
  constexpr size_t WarmedBytes = OmittedCalleeMemo::MaxBytes / 4 * 3;
  // A body past the SSA node bound is no signature (conversion), so warming
  // converts lifts up to that many operations; a selection converts a larger
  // callee where it first calls it.
  constexpr size_t MaxWarmedOperations =
      static_cast<size_t>(limits::kMaxSSANodes);
  for (const va_t Target : Functions) {
    if (!Reader.reads(Target))
      continue;
    if (Memo->bytes() >= WarmedBytes) {
      Warmup.Full = true;
      break;
    }
    OmittedCalleeMemo::Callee &Read = *Memo->find(Target);
    std::optional<LowFunc> Low;
    Reader.readLift(Target, Read, Low);
    if (Read.BlockOps &&
        std::accumulate(Read.BlockOps->begin(), Read.BlockOps->end(),
                        size_t{0}) <= MaxWarmedOperations)
      (void)Reader.conversion(Target, Read, /*Kept=*/true, Low);
    ++Warmup.Prepared;
  }
  return Warmup;
}

void recoverModuleCallAbi(const BinaryImage &Img, PipelineResult &Result,
                          const std::map<va_t, std::string> &AllFuncNames) {
  const CallArgumentConvention *Convention =
      callArgumentConvention(Img.Arch, Img.abiFormat());
  std::map<va_t, int> CalleeRegArity;
  std::map<va_t, int> CalleeTotalArity;
  std::map<va_t, int> CalleeFPArity;
  std::map<va_t, std::vector<uint64_t>> CalleeFPRegs;
  std::map<va_t, uint16_t> CalleeFPReturnSize;
  std::map<va_t, bool> CalleeHasSret;
  std::map<va_t, bool> CalleeIsVariadic;
  std::set<va_t> VectorArgumentCallees;
  std::map<va_t, bool> CalleeConsumesVaList;
  // The integer argument registers of each function that takes them in its
  // own order (MedFunc::IntegerArgumentRegisters).
  std::map<va_t, std::vector<uint64_t>> CalleeIntRegs;
  std::vector<va_t> Functions;
  for (const MedFunc &F : Result.MedFuncs)
    Functions.push_back(F.Entry);
  // The memo of the image's callees, where the run shares one, keeps the
  // image's function-pointer variables and the probes of its bodies.
  OmittedCalleeMemo *Memo = Result.CalleeSignatureMemo.get();
  if (Memo)
    Memo->adopt(Img, Result);
  if (!Result.OmittedCallees || Result.OmittedCallees->Functions != Functions)
    Result.OmittedCallees =
        std::make_shared<const OmittedCalleeBodies>(OmittedCalleeBodies{
            Functions, omittedDirectCalleeSignatures(Img, Result)});
  // The passes before recovery read the kept bodies in place.
  const std::vector<std::shared_ptr<const MedFunc>> &OmittedCallees =
      Result.OmittedCallees->Bodies;
  // What a call through each function-pointer variable passes on.
  std::map<va_t, PointerVariableCallee> OwnPointerCallees;
  if (!Memo)
    OwnPointerCallees = collectPointerVariableCallees(Img);
  const std::map<va_t, PointerVariableCallee> &PointerCallees =
      Memo ? Memo->pointerCallees() : OwnPointerCallees;
  // The relocation that names each call's callee, where one does.
  const RelocationSymbolIndex RelocationSymbols(Img);
  std::vector<const MedFunc *> SignatureBodies;
  for (const MedFunc &F : Result.MedFuncs)
    SignatureBodies.push_back(&F);
  for (const auto &F : OmittedCallees)
    SignatureBodies.push_back(F.get());
  // The memo's conversions of the omitted bodies, which keep their forwarder
  // probes.
  std::vector<OmittedCalleeMemo::Conversion *> KeptBodies(
      Result.MedFuncs.size());
  for (const auto &F : OmittedCallees)
    KeptBodies.push_back(Memo ? Memo->conversionOf(*F) : nullptr);
  for (size_t I = 0; I < SignatureBodies.size(); ++I) {
    const MedFunc &MF = *SignatureBodies[I];
    std::optional<OmittedCalleeMemo::CalleeSummary> Own;
    const OmittedCalleeMemo::CalleeSummary &Summary =
        KeptBodies[I] && KeptBodies[I]->Summary
            ? *KeptBodies[I]->Summary
            : Own.emplace(summarizeCallee(MF, Img));
    if (!Summary.IntegerArgumentRegisters.empty())
      CalleeIntRegs[MF.Entry] = Summary.IntegerArgumentRegisters;
    CalleeConsumesVaList[MF.Entry] = Summary.ConsumesVaList;
    CalleeRegArity[MF.Entry] = Summary.RegArity;
    CalleeTotalArity[MF.Entry] = Summary.TotalArity;
    CalleeHasSret[MF.Entry] = Summary.HasSret;
    CalleeIsVariadic[MF.Entry] = Summary.IsVariadic;
    if (Summary.VectorArguments)
      VectorArgumentCallees.insert(MF.Entry);
    CalleeFPArity[MF.Entry] = Summary.FPArity;
    CalleeFPRegs[MF.Entry] = Summary.FPRegs;
    CalleeFPReturnSize[MF.Entry] = Summary.FPReturnSize;
  }

  // A pure tail-call forwarder `T f(args){return g(args);}` lowers at -O2 to a
  // lone `b g`.  It carries its incoming arguments straight into the call, so
  // it has no parameters of its own here and the per-function FP arity above is
  // 0. Inherit the callee g's scalar-FP return type and FP argument arity for
  // such a forwarder so (a) the function's return type is floating-point
  // (recoverCallAbi rewires the tail call's result register to the FP return
  // register) and (b) a CALLER of it recovers the forwarded FP arguments
  // instead of padding 0.0.  The callee g is a libc import (signature from
  // libcArity) or an intra-module function (signature from its recovered
  // MedFunc).  Gated to a genuine forwarder whose FP argument registers are
  // live-in (never written), so a function that locally computes the callee's
  // FP arguments is left untouched.
  {
    const auto &TRI = getTargetRegInfo(Img.Arch);
    const auto FPParamRegs = TRI.floatingParamRegs(Img.abiFormat());
    std::map<va_t, const MedFunc *> ByEntry;
    for (const MedFunc *MF : SignatureBodies)
      ByEntry[MF->Entry] = MF;
    if (!FPParamRegs.empty())
      for (auto &MF : Result.MedFuncs) {
        // Only a pure forwarder, which has no parameters of its own recovered
        // yet (its incoming arguments flow straight into the tail call).
        if (!MF.Params.empty())
          continue;
        const MedOp *CallOp = nullptr;
        for (const auto &Blk : MF.Blocks) {
          for (size_t I = 0; I + 1 < Blk.Ops.size(); ++I) {
            const auto &Op = Blk.Ops[I];
            if (Op.Opcode != NdOp::CALL || Op.NumInputs < 1 ||
                !Op.Inputs[0].isConst() || Op.Output.Kind != MedVar::Reg)
              continue;
            const auto &Ret = Blk.Ops[I + 1];
            if (Ret.Opcode == NdOp::RETURN && Ret.NumInputs >= 1 &&
                Ret.Inputs[0].Kind == MedVar::Reg &&
                Ret.Inputs[0].RegOff == Op.Output.RegOff) {
              CallOp = &Op;
              break;
            }
          }
          if (CallOp)
            break;
        }
        if (!CallOp)
          continue;
        va_t Target = CallOp->Inputs[0].ConstVal;
        // Resolve the callee's integer + FP argument counts and scalar-FP
        // return width (libc import via libcArity, intra-module via its
        // recovered MedFunc).  All three must reach the forwarder's signature,
        // or a caller misassembles the forwarded arguments (a mixed `double
        // f(int n,double x) {return g(n,x);}` with only the FP arity propagated
        // would disagree with its own declared int+FP signature).
        int IntArgs = 0, FpArgs = 0;
        uint16_t FpRetSize = 0; // 0 = callee does not return a scalar FP value
        if (const Import *Imp = Img.findImportAt(Target)) {
          if (auto Sig = libc::libcArityForSymbol(Imp->Name,
                                                  libc::libcVariantOf(Img))) {
            if (!Sig->FpRetComplex) {
              IntArgs = Sig->IntArgs;
              FpArgs = Sig->FpArgs;
              bool ScalarFPRet = (Sig->FpArgs > 0 && Sig->IntArgs == 0) ||
                                 Sig->FpRet || Sig->FpRetLongDouble;
              if (ScalarFPRet)
                FpRetSize = Sig->FpIsFloat ? 4 : 8;
            }
          }
        } else if (auto It = ByEntry.find(Target); It != ByEntry.end()) {
          const MedFunc *G = It->second;
          IntArgs = CalleeRegArity[Target]; // g's integer-argument count
          FpArgs = CalleeFPArity[Target];   // g's FP-argument count
          if (FpArgs == 0) {
            FpArgs = countEntryLiveInFPArgs(*G, TRI, Img.abiFormat());
            if (FpArgs > 0) {
              CalleeFPArity[Target] = FpArgs;
              CalleeFPRegs[Target] = std::vector<uint64_t>(
                  FPParamRegs.begin(), FPParamRegs.begin() + FpArgs);
            }
          }
          if (G->ReturnType && G->ReturnType->Kind == NdTypeKind::Float &&
              G->MultiReturn.empty() && !G->FPReturnViaX87)
            FpRetSize = G->ReturnType->Size ? G->ReturnType->Size : 8;
        }
        if (IntArgs <= 0 && FpArgs <= 0 && FpRetSize == 0)
          continue; // nothing to inherit
        int NFp = std::min<int>(FpArgs, static_cast<int>(FPParamRegs.size()));
        const auto IntegerRegs =
            TRI.integerArgumentLayout(MF.CC == CallingConv::Win64).Registers;
        int NInt = std::min<int>(IntArgs, static_cast<int>(IntegerRegs.size()));
        // Every forwarded argument register must be live-in (genuine forwarder,
        // not a function that computes the callee's arguments locally).  The
        // forwarding CALL itself is excluded: its output is the integer return
        // register, which on AArch64/x86-64 aliases the first integer argument
        // register (x0 / rax-vs-rdi differ, but x0==arg0==ret on AArch64), so
        // the call's result write must not be mistaken for an argument write.
        auto regWrittenInF = [&](uint64_t Reg) {
          for (const auto &B : MF.Blocks) {
            for (const auto &O : B.Ops)
              if (&O != CallOp && O.Output.Kind == MedVar::Reg &&
                  O.Output.Size > 0 && O.Output.RegOff == Reg)
                return true;
            for (const auto &Ph : B.Phis)
              if (Ph.Output.Kind == MedVar::Reg && Ph.Output.RegOff == Reg)
                return true;
          }
          return false;
        };
        bool AllLiveIn = true;
        for (int K = 0; K < NFp && AllLiveIn; ++K)
          if (regWrittenInF(FPParamRegs[K]))
            AllLiveIn = false;
        for (int K = 0; K < NInt && AllLiveIn; ++K)
          if (regWrittenInF(IntegerRegs[K]))
            AllLiveIn = false;
        if (!AllLiveIn)
          continue;
        if (NFp > 0) {
          CalleeFPArity[MF.Entry] = NFp;
          CalleeFPRegs[MF.Entry] = std::vector<uint64_t>(
              FPParamRegs.begin(), FPParamRegs.begin() + NFp);
        }
        if (NInt > 0) {
          CalleeRegArity[MF.Entry] = NInt;
          if (CalleeTotalArity[MF.Entry] < NInt)
            CalleeTotalArity[MF.Entry] = NInt;
        }
        // Inherit a scalar FP return so the function and its callers treat the
        // result as floating-point; recoverCallAbi rewires the tail call's
        // result register to the FP return register.
        if (FpRetSize && !MF.FPReturnViaX87) {
          MF.ReturnType = NdType::makeFloat(FpRetSize);
          CalleeFPReturnSize[MF.Entry] =
              !(TRI.WideFloatsReturnInX87 && FpRetSize > 8) &&
                      TRI.isVectorReg(TRI.fpReturnModelReg())
                  ? FpRetSize
                  : 0;
        }
      }
  }

  propagateForwardedCallArities(
      SignatureBodies, KeptBodies, Memo, Img.Arch, AllFuncNames, Img,
      CalleeRegArity, CalleeTotalArity, CalleeFPArity, CalleeFPReturnSize,
      CalleeFPRegs, CalleeHasSret, CalleeIsVariadic, CalleeConsumesVaList,
      CalleeIntRegs, VectorArgumentCallees, PointerCallees,
      Result.CallPrototypeArguments, RelocationSymbols);

  const auto FrameLocalLeafCallees =
      Convention && Convention->TargetSpillsSurviveLeafCalls
          ? findFrameLocalLeafCallees(Result.MedFuncs, Img.Arch)
          : std::set<va_t>{};
  auto recover = [&](MedFunc &MF) {
    recoverCallAbi(MF, Img.Arch, AllFuncNames, &Img, &CalleeRegArity,
                   &CalleeTotalArity, &CalleeFPArity, &CalleeFPReturnSize,
                   &CalleeFPRegs, &CalleeHasSret, &CalleeIsVariadic,
                   &CalleeConsumesVaList, &FrameLocalLeafCallees,
                   &CalleeIntRegs, &VectorArgumentCallees, &PointerCallees,
                   &Result.CallPrototypeArguments, &RelocationSymbols);
  };
  for (MedFunc &F : Result.MedFuncs)
    recover(F);
  // An omitted body's recovery gives the bound below on the run's direct
  // calls to it, and the bodies after it read the arities and variadic
  // marks it leaves.  Past the last body those calls reach, only the second
  // regparm pass, which reads every body, reads what recovery gives.
  size_t Recovered = OmittedCallees.size();
  if (!Convention || !Convention->RegparmOnlyForInternalCalls) {
    std::set<va_t> Called;
    for (const MedFunc &F : Result.MedFuncs)
      for (const MedCallInfo &CI : F.CallInfos)
        if (!CI.IsIndirect)
          Called.insert(CI.TargetAddr);
    while (Recovered && !Called.count(OmittedCallees[Recovered - 1]->Entry))
      --Recovered;
  }
  // Recovery changes the bodies it recovers: copies of the kept ones.
  std::vector<MedFunc> RecoveredCallees;
  RecoveredCallees.reserve(Recovered);
  for (size_t I = 0; I < Recovered; ++I) {
    RecoveredCallees.push_back(*OmittedCallees[I]);
    recover(RecoveredCallees.back());
  }
  // The bodies as recovery left them.
  std::vector<MedFunc *> RecoveredBodies;
  for (MedFunc &F : Result.MedFuncs)
    RecoveredBodies.push_back(&F);
  for (MedFunc &F : RecoveredCallees)
    RecoveredBodies.push_back(&F);

  // Regparm two-pass call recovery (i386): the first pass promotes forwarder
  // register params (PromoteParams).  Recompute CalleeRegArity from the
  // now-promoted params and re-run: forwarders now have CalleeRegArgs > 0, so
  // the cdecl clearing (CalleeRegArgs == 0) no longer fires for them, while
  // true cdecl callees remain at 0 and get their stack arguments correctly
  // indexed.
  if (Convention && Convention->RegparmOnlyForInternalCalls) {
    const auto &TRI2 = getTargetRegInfo(Img.Arch);
    // Omitted support bodies retain the same contract in both passes.
    auto CRA2 = CalleeRegArity, CTA2 = CalleeTotalArity;
    auto CalleeIntRegs2 = CalleeIntRegs;
    for (const MedFunc *Body : RecoveredBodies) {
      const MedFunc &MF = *Body;
      int MaxRI = -1, MaxI = -1;
      const IntegerArgumentLayout Layout = integerArgumentLayoutOf(MF, TRI2);
      for (const auto &P : MF.Params) {
        const int Index = integerParameterIndex(P, Layout);
        if (P.RegOff != kNoParamReg)
          MaxRI = std::max(MaxRI, Index);
        MaxI = std::max(MaxI, Index);
      }
      CRA2[MF.Entry] = MaxRI + 1;
      CTA2[MF.Entry] = callRecoveryTotalArity(MF, MaxI);
      if (!MF.IntegerArgumentRegisters.empty())
        CalleeIntRegs2[MF.Entry] = MF.IntegerArgumentRegisters;
    }
    for (MedFunc *Body : RecoveredBodies) {
      MedFunc &MF = *Body;
      recoverCallAbi(MF, Img.Arch, AllFuncNames, &Img, &CRA2, &CTA2,
                     &CalleeFPArity, &CalleeFPReturnSize, &CalleeFPRegs,
                     &CalleeHasSret, &CalleeIsVariadic, &CalleeConsumesVaList,
                     /*FrameLocalLeafCallees=*/nullptr, &CalleeIntRegs2,
                     &VectorArgumentCallees, &PointerCallees,
                     &Result.CallPrototypeArguments, &RelocationSymbols);
    }
  }
  // Forwarder promotion can insert ordinary parameters before the explicit
  // binary80 parameter. Refresh the typed signature after its order settles;
  // otherwise an emitter falls back to an integer type for the i80 payload.
  for (MedFunc *Body : RecoveredBodies)
    if (Body->ConsumesX87Argument &&
        Body->TypedParams.size() != Body->Params.size())
      inferMedTypes(*Body, Img.Arch);
  // An omitted definition cannot give the emitter its parameter count.
  // Apply the same final signature bound used for an emitted callee after
  // forwarder promotion, so incidental live registers do not become extra
  // arguments merely because output was restricted to its caller.
  std::map<va_t, const MedFunc *> SignatureBounds;
  for (size_t I = 0; I < OmittedCallees.size(); ++I) {
    const MedFunc &F = I < Recovered ? RecoveredCallees[I] : *OmittedCallees[I];
    if (!F.IsVariadic && !F.SourceParametersBound)
      SignatureBounds.emplace(F.Entry, &F);
  }
  // Consumers have an explicit final argument outside the ordinary ABI
  // scan. Bind it after forwarder promotion has settled ordinary parameters,
  // before either the LLVM or HighIR emitter sees a truncated prefix.
  for (const MedFunc &F : Result.MedFuncs)
    if (F.ConsumesX87Argument)
      SignatureBounds.emplace(F.Entry, &F);
  for (MedFunc &F : Result.MedFuncs)
    for (MedCallInfo &CI : F.CallInfos) {
      auto It = CI.IsIndirect ? SignatureBounds.end()
                              : SignatureBounds.find(CI.TargetAddr);
      if (It != SignatureBounds.end())
        boundRecoveredArguments(CI, *It->second);
    }
}

void matchCallsToCalleeSignatures(const BinaryImage &Img,
                                  PipelineResult &Result) {
  std::map<va_t, const MedFunc *> ByEntry;
  for (const MedFunc &MF : Result.MedFuncs)
    ByEntry.emplace(MF.Entry, &MF);
  for (MedFunc &MF : Result.MedFuncs)
    for (MedCallInfo &CI : MF.CallInfos) {
      const auto It =
          CI.IsIndirect ? ByEntry.end() : ByEntry.find(CI.TargetAddr);
      if (It == ByEntry.end())
        continue;
      const MedFunc &Callee = *It->second;
      if (Callee.IsVariadic || Callee.SourceParametersBound ||
          Img.findImportAt(Callee.Entry))
        continue;
      boundRecoveredArguments(CI, Callee);
      // The callee reads the scalar in an argument register's low lane: an
      // SSE register's other bytes, which a scalar instruction leaves as they
      // were, pass nothing.
      CI.ArgReadBytes.assign(CI.Args.size(), 0);
      for (size_t K = 0; K < CI.Args.size(); ++K)
        if (const MedVar &Arg = CI.Args[K]; Arg.Kind == MedVar::Reg)
          if (const auto Scalar = Callee.FPParamScalarBytes.find(Arg.RegOff);
              Scalar != Callee.FPParamScalarBytes.end() &&
              Scalar->second < Arg.Size)
            CI.ArgReadBytes[K] = Scalar->second;
    }
}

} // namespace neverd
