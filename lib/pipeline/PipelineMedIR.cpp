//===- PipelineMedIR.cpp - MedIR pipeline stage --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// LowIR-to-MedIR conversion for the decompilation pipeline.
///
//===----------------------------------------------------------------------===//

#include "PipelineCallAbiDetail.h"
#include "PipelineIncomingStack.h"
#include "PipelineLowIRDetail.h"
#include "PipelineMedAudit.h"
#include "PipelineRegisterCalls.h"
#include "PipelineSEHRuntimeMemory.h"
#include "PipelineSourceStackAccesses.h"
#include "PipelineTrimStorage.h"
#include "X86/X64CallRCX.h"
#include "X86/X64FillReturn.h"

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceCallABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/ir/low/SourceCallOccurrence.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/ir/med/MedCallReturnReads.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Diagnostic.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-pipeline"

namespace neverd {

MedStackProjection medStackProjection(const PipelineOptions &Options) {
  return !Options.MedAnalysisOnly && !Options.PatchMode &&
                 (!Options.LiftMode || Options.SourceProjection)
             ? MedStackProjection::Source
             : MedStackProjection::Machine;
}

bool refusalFailsRun(const PipelineOptions &Options) {
  return !Options.OnlyFunctionEntries.empty() && !Options.OmitRefusedFunctions;
}

void prepareMedStackAccesses(MedFunc &Func, const LowFunc &Source,
                             const BinaryImage &Image,
                             MedStackProjection Projection,
                             const MedCallReturnRanges *CallReturns,
                             bool WaitForCalleeFacts,
                             bool CallFreeMachineSource) {
  Func.EntryKind = functionEntryKind(Func, Image.stackEntryKindAt(Func.Entry));
  // Tentative display bodies retain their physical operations. Native source
  // frame proofs cannot reject that view or turn it into an executable ABI.
  if (Projection != MedStackProjection::Source || Func.AnalysisOnly)
    return;
  simplifyProvenStackAlignment(Func, Image.Arch, Image.abiFormat(),
                               Func.EntryKind);
  // Loads of CALL return slots wait for settled calls and returns
  // (refuseObservedCallReturnReads); other refusals are final now.
  std::string Diagnostic;
  Func.CallReturnReads.clear();
  bool NeedsCalleeFacts = false;
  if (!CallFreeMachineSource &&
      !preserveSourceCallReturnWrites(
          Func, Image, Diagnostic, limits::ir::kSourceCallReturnReadWork,
          &Source, CallReturns, &Func.CallReturnReads,
          WaitForCalleeFacts ? &NeedsCalleeFacts : nullptr))
    throw LowToMedConversionError(Diagnostic);
  // A callee's return range may bound the dynamic stack below such a CALL and
  // prove the load. Retry with those facts instead of deferring it now.
  if (NeedsCalleeFacts && !Func.CallReturnReads.empty())
    throw LowToMedConversionError(
        "source frame in " + Func.Name +
        ": a CALL return-slot proof waits for callee return ranges");
  markSourceStackAccesses(Func, Image.Arch, Image.abiFormat(), Func.EntryKind,
                          &Image);
}

MedCallReturnRanges sourceCallReturnRanges(const BinaryImage &Image,
                                           const PipelineResult &Result) {
  MedCallReturnRanges Ranges;
  if (!getTargetRegInfo(Image.Arch).ReturnOperandIsValue)
    return Ranges;
  const size_t Total = Result.LowFuncs.size();
  std::vector<std::optional<MedIntegerReturnRange>> Proven(Total);
  std::vector<uint64_t> Weight(Total, 1);
  for (size_t I = 0; I < Total; ++I)
    for (const auto &Block : Result.LowFuncs[I].Blocks)
      Weight[I] += Block.Ops.size();
  parallelForEachWeighted(Weight, [&](auto Claim, size_t N) {
    for (size_t I; (I = Claim()) < N;)
      Proven[I] =
          integerReturnRange(Result.MedFuncs[I], Result.LowFuncs[I], Image);
  });
  for (size_t I = 0; I < Total; ++I)
    if (Proven[I])
      Ranges.try_emplace(Result.LowFuncs[I].Entry, *Proven[I]);
  return Ranges;
}

SourceStackScan sourceStackScanOf(const MedFunc &Func,
                                  const BinaryImage &Image) {
  SourceStackScan Scan;
  const auto &TRI = getTargetRegInfo(Image.Arch);
  const auto Arguments = TRI.integerArgumentLayout(Image.abiFormat());
  Scan.Candidate = Func.Params.size() > Arguments.Registers.size();
  for (const auto &Block : Func.Blocks) {
    Scan.Weight += Block.Ops.size();
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst() &&
          !Op.SourceCallHint)
        Scan.Calls.push_back(Op.Inputs[0].ConstVal);
      Scan.Candidate |= forwardsSourceEntryRegister(Op, Arguments);
      if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2)
        Scan.Candidate |= Op.Inputs[1].Kind == MedVar::Reg &&
                          Op.Inputs[1].SSAVer == 0 &&
                          Arguments.registerIndex(Op.Inputs[1].RegOff) >= 0;
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
        Scan.Candidate |= Op.Inputs[0].Kind == MedVar::Reg &&
                          Op.Inputs[0].SSAVer == 0 &&
                          Op.Inputs[0].Size < TRI.PointerSize &&
                          Arguments.registerIndex(Op.Inputs[0].RegOff) >= 0;
    }
  }
  return Scan;
}

std::shared_ptr<OmittedCalleeMemo::Conversion::PreparedSource>
prepareSourceStackBody(const MedFunc &Source, const BinaryImage &Image,
                       bool KeepPrologueSaves) {
  using PreparedSource = OmittedCalleeMemo::Conversion::PreparedSource;
  // Marked where it is kept: the spills name its operations.
  auto Prepared = std::make_shared<PreparedSource>(PreparedSource{Source, {}});
  MedFunc &Func = Prepared->Body;
  markSourceStackAccesses(Func, Image.Arch, Image.abiFormat(), Func.EntryKind,
                          &Image, &Prepared->OverwrittenSpills);
  // Selected bodies also carry this phase's complete borrowed-frame proof.
  // Keep that source fact, but never promote it into an overwrite receipt.
  if (KeepPrologueSaves)
    for (size_t B = 0; B < Source.Blocks.size(); ++B)
      for (size_t O = 0; O < Source.Blocks[B].Ops.size(); ++O)
        Func.Blocks[B].Ops[O].PrologueSave |=
            Source.Blocks[B].Ops[O].PrologueSave;
  // Every dependency wave, and every later run's, queries the prepared
  // body's value readers.
  Prepared->ValueReads = valueReadsOf(Func);
  return Prepared;
}

namespace {

using SourceValueKey = std::tuple<int, int, int>;

SourceValueKey sourceValueKey(const MedVar &V) {
  return {static_cast<int>(V.Kind), V.Id, V.SSAVer};
}

/// The copy that names the value a register holds at entry.
bool namesEntryValue(const MedOp &Op) {
  return Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
         Op.Output.Kind == MedVar::Reg && Op.Inputs[0] == Op.Output;
}

/// The argument positions of \p Func that its body never reads: the register
/// positions first, then the stack slots, as its callers number them.  A
/// convention that aligns a wide argument (PairAlignedWideArguments) leaves
/// the register or stack lane before it unused, and a caller stores nothing
/// there, so the position holds whatever earlier code left.
///
/// A position is unread when no operation, join or recovered call argument of
/// the body names its entry value, and nothing can take the value another
/// way.  A register position needs every call of the body to be one whose
/// arguments recovery lists, and the register to carry no result the function
/// could return unwritten.  A stack position needs every address the body
/// derives from the entry stack pointer to keep a known offset and to be used
/// only to address a load or store: a load from the incoming area reads its
/// slots, and an address that escapes, a tail call, which passes the incoming
/// area on, or a slot the function keeps as a mutable home ends the proof.
/// Variadic, source-bound and incompletely lifted bodies prove nothing.
std::set<unsigned> unreadArgumentPositions(const MedFunc &Func,
                                           const BinaryImage &Image,
                                           size_t &Work) {
  const auto &TRI = getTargetRegInfo(Image.Arch);
  const IntegerArgumentLayout Layout =
      TRI.integerArgumentLayout(Image.abiFormat());
  const size_t Registers = Layout.Registers.size();
  if (Func.Blocks.empty() || !Func.Blocks.front().Preds.empty() ||
      Func.Blocks.front().StartAddr != Func.Entry ||
      !Func.CompleteLiftCoverage ||
      !Func.UnsafeIndirectBranchAddresses.empty() || Func.SkippedSSA ||
      Func.AnalysisOnly || Func.AnalystStackFrame || Func.SourceTypeHint ||
      Func.SourceParametersBound || Func.HasUnresolvedSEHFrame ||
      Func.EntryKind != StackEntryKind::Call || Func.IsVariadic ||
      Func.VaListDisplacement || Func.VaRegisterSaveArea ||
      !Func.IntegerArgumentRegisters.empty() ||
      !Func.FPParamScalarBytes.empty() || Func.ConsumesX87Argument ||
      Func.Params.empty() ||
      Func.Params.size() >
          static_cast<size_t>(limits::ir::kMaxSourceABIParameters) ||
      !Layout.SlotBytes)
    return {};
  // Position I is the Ith register of the layout, and past them a stack
  // slot.  A register placeholder (MedVar::Id below zero) is one the body's
  // own recovery found no read of; a promoted one is named by a call.
  std::set<unsigned> Unread;
  for (size_t I = 0; I < Func.Params.size(); ++I) {
    const MedVar &P = Func.Params[I];
    if (P.Kind != MedVar::Param ||
        (I < Registers
             ? P.RegOff != Layout.Registers[I]
             : P.RegOff != kNoParamReg || P.Id != static_cast<int>(I)))
      return {};
    if (I >= Registers || P.Id < 0)
      Unread.insert(static_cast<unsigned>(I));
  }
  const uint16_t Width = TRI.FullRegWidth;
  const auto overlaps = [](uint64_t A, uint64_t ABytes, uint64_t B,
                           uint64_t BBytes) {
    return A < B + BBytes && B < A + ABytes;
  };
  const auto charge = [&](size_t Cost) {
    if (Work < Cost) {
      Work = 0;
      return false;
    }
    Work -= Cost;
    return true;
  };

  // The values the body defines.  Any other register value is the one the
  // register holds at entry.
  std::set<SourceValueKey> Defined;
  std::map<int, const MedBlock *> Blocks;
  for (const MedBlock &Block : Func.Blocks) {
    if (!charge(1 + Block.Phis.size() + Block.Ops.size()) ||
        !Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty() ||
        (&Block != &Func.Blocks.front() && Block.Preds.empty()) ||
        !Blocks.emplace(Block.Id, &Block).second)
      return {};
    // Another analysis root inside the body is an entry no caller of this
    // one passes arguments to.
    if (const auto Root = Func.ModuleAnalysisRoots.upper_bound(Block.StartAddr);
        (Root != Func.ModuleAnalysisRoots.end() && *Root < Block.EndAddr) ||
        (Func.ModuleAnalysisRoots.count(Block.StartAddr) &&
         Block.StartAddr != Func.Entry))
      return {};
    for (const PhiNode &Phi : Block.Phis) {
      if (Phi.ExceptionalEntry)
        return {};
      Defined.insert(sourceValueKey(Phi.Output));
    }
    for (const MedOp &Op : Block.Ops) {
      if (Op.Output.Size && !namesEntryValue(Op))
        Defined.insert(sourceValueKey(Op.Output));
      for (const MedVar &Aux : Op.IntrinsicOutputs)
        Defined.insert(sourceValueKey(Aux));
    }
  }
  const auto entryRegister = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg && V.Size && !Defined.count(sourceValueKey(V));
  };

  bool StackOpen = false;
  // A parameter variable can name its position by register or by id, and a
  // register's entry value names the register's position.
  const auto read = [&](const MedVar &V) {
    if (V.Kind == MedVar::Stack) {
      StackOpen = true;
    } else if (V.Kind == MedVar::Param) {
      for (size_t I = 0; I < Func.Params.size(); ++I) {
        const MedVar &P = Func.Params[I];
        if ((V.RegOff != kNoParamReg && P.RegOff == V.RegOff) ||
            (V.Id >= 0 && P.Id == V.Id) ||
            (V.Id >= 0 && static_cast<size_t>(V.Id) == I))
          Unread.erase(static_cast<unsigned>(I));
      }
    } else if (entryRegister(V)) {
      for (size_t I = 0; I < Registers && I < Func.Params.size(); ++I)
        if (overlaps(V.RegOff, V.Size, Layout.Registers[I], Width))
          Unread.erase(static_cast<unsigned>(I));
    }
  };

  // The calls whose arguments recovery lists: source lowering passes those
  // alone.  Any other call may take a register this function never set.
  std::set<std::pair<int, int>> ListedCalls;
  for (const MedCallInfo &Call : Func.CallInfos) {
    if (!charge(1 + Call.Args.size()))
      return {};
    for (const MedVar &Argument : Call.Args)
      read(Argument);
    if (!Call.IsIndirect)
      ListedCalls.emplace(Call.BlockId, Call.OpIdx);
  }
  bool RegistersOpen = false;
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &[Pred, Incoming] : Phi.Args)
        read(Incoming);
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const MedOp &Op = Block.Ops[I];
      if (namesEntryValue(Op))
        continue;
      for (uint8_t K = 0; K < Op.NumInputs; ++K)
        read(Op.Inputs[K]);
      const bool Call =
          Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL;
      if (Call ? Op.Opcode == NdOp::INDIR_CALL ||
                     !ListedCalls.count({Block.Id, static_cast<int>(I)})
               : endsCallArgumentSetup(Op, /*RegistersOnly=*/true))
        RegistersOpen = true;
      StackOpen |= Op.IsTailCall;
    }
  }
  for (const auto &[Position, Home] : Func.MutableStackParamHomes)
    if (Position >= 0)
      Unread.erase(static_cast<unsigned>(Position));

  // A result register returns the caller's value where the function never
  // writes it.  The entry block's write reaches every return.
  const bool NarrowResult = Func.MultiReturn.empty() && Func.ReturnType &&
                            Func.ReturnType->Size <= TRI.PointerSize;
  const auto writtenAtEntry = [&](uint64_t Register) {
    return llvm::any_of(Func.Blocks.front().Ops, [&](const MedOp &Op) {
      return Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL &&
             Op.Output.Kind == MedVar::Reg && Op.Output.Size &&
             !namesEntryValue(Op) && Op.Output.RegOff <= Register &&
             Register + Width <= Op.Output.RegOff + Op.Output.Size;
    });
  };
  for (size_t I = 0; I < Registers && I < Func.Params.size(); ++I) {
    const uint64_t Register = Layout.Registers[I];
    const bool Result =
        TRI.isReturnReg(Register) ||
        llvm::is_contained(TRI.IntReturnRegs, Register) ||
        llvm::any_of(Func.MultiReturn, [&](const MedReturnReg &Field) {
          return overlaps(Field.RegOff, Field.Size, Register, Width);
        });
    if (RegistersOpen || Register == TRI.IntReturnReg ||
        (Result && !NarrowResult && !writtenAtEntry(Register)))
      Unread.erase(static_cast<unsigned>(I));
  }

  // The stack positions.  Frame holds every value derived from the entry
  // stack pointer with its offset from it, or none once paths disagree or an
  // operation other than a copy or a constant displacement computes it.
  if (Func.Params.size() <= Registers)
    return Unread;
  const auto dropStack = [&] {
    Unread.erase(Unread.lower_bound(static_cast<unsigned>(Registers)),
                 Unread.end());
    return Unread;
  };
  if (StackOpen)
    return dropStack();
  std::map<SourceValueKey, std::optional<int64_t>> Frame;
  const auto frameOffset =
      [&](const MedVar &V) -> std::optional<std::optional<int64_t>> {
    if (V.isConst() || !V.Size)
      return std::nullopt;
    if (entryRegister(V))
      return TRI.isStackPointer(V.RegOff)
                 ? std::optional<std::optional<int64_t>>(
                       std::optional<int64_t>(0))
                 : std::nullopt;
    const auto At = Frame.find(sourceValueKey(V));
    return At == Frame.end()
               ? std::nullopt
               : std::optional<std::optional<int64_t>>(At->second);
  };
  const auto derive = [&](const MedVar &Output, std::optional<int64_t> Offset) {
    auto [At, Added] = Frame.emplace(sourceValueKey(Output), Offset);
    if (Added)
      return true;
    if (At->second == Offset || !At->second)
      return false;
    At->second.reset();
    return true;
  };
  const auto displaced = [&](const MedOp &Op) -> std::optional<int64_t> {
    // The offset the operation gives its output, when it moves a frame
    // address by a constant.
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
        Op.Inputs[0].Size == Op.Output.Size)
      if (const auto Base = frameOffset(Op.Inputs[0]); Base && *Base)
        return **Base;
    if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
        Op.NumInputs == 2 && Op.Output.Size == TRI.PointerSize) {
      const bool Swapped = Op.Opcode == NdOp::INT_ADD && Op.Inputs[0].isConst();
      const MedVar &Address = Op.Inputs[Swapped ? 1 : 0];
      const MedVar &Constant = Op.Inputs[Swapped ? 0 : 1];
      if (const auto Base = frameOffset(Address);
          Base && *Base && Constant.isConst() && Constant.Size &&
          Constant.Size <= sizeof(uint64_t) &&
          Address.Size == TRI.PointerSize) {
        const int64_t Value =
            llvm::SignExtend64(Constant.ConstVal, Constant.Size * 8u);
        return Op.Opcode == NdOp::INT_ADD ? **Base + Value : **Base - Value;
      }
    }
    return std::nullopt;
  };
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (const MedBlock &Block : Func.Blocks) {
      if (!charge(1 + Block.Phis.size() + Block.Ops.size()))
        return dropStack();
      for (const PhiNode &Phi : Block.Phis) {
        bool Derived = false, Agree = true;
        std::optional<int64_t> Common;
        for (const auto &[Pred, Incoming] : Phi.Args) {
          const auto Offset = frameOffset(Incoming);
          if (!Offset) {
            Agree = false;
            continue;
          }
          Agree &= *Offset && (!Derived || Common == *Offset);
          Common = *Offset;
          Derived = true;
        }
        if (Derived)
          Changed |=
              derive(Phi.Output, Agree ? Common : std::optional<int64_t>());
      }
      for (const MedOp &Op : Block.Ops) {
        // A load yields what its slot holds, and no frame address is ever
        // stored: the check below ends the proof at such a store.
        if (namesEntryValue(Op) || !Op.Output.Size || Op.Opcode == NdOp::LOAD)
          continue;
        bool Derived = false;
        for (uint8_t K = 0; K < Op.NumInputs; ++K)
          Derived |= frameOffset(Op.Inputs[K]).has_value();
        if (Derived)
          Changed |= derive(Op.Output, displaced(Op));
        for (const MedVar &Aux : Op.IntrinsicOutputs)
          if (Derived)
            Changed |= derive(Aux, std::nullopt);
      }
    }
  }
  // An argument or result register that holds a frame address may pass it
  // to a callee or to the caller.
  for (const auto &[Key, Offset] : Frame)
    if (!Offset)
      return dropStack();
  const auto mayLeave = [&](const MedVar &V) {
    if (V.Kind != MedVar::Reg || !frameOffset(V))
      return false;
    return TRI.isReturnReg(V.RegOff) ||
           llvm::any_of(Layout.Registers, [&](uint64_t Register) {
             return overlaps(V.RegOff, V.Size, Register, Width);
           });
  };
  for (const MedCallInfo &Call : Func.CallInfos)
    for (const MedVar &Argument : Call.Args)
      if (frameOffset(Argument))
        return dropStack();
  const int64_t First = Layout.EntryStackBase;
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      if (mayLeave(Phi.Output))
        return dropStack();
    for (const MedOp &Op : Block.Ops) {
      if (namesEntryValue(Op))
        continue;
      if (mayLeave(Op.Output))
        return dropStack();
      const bool Memory =
          (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Op.MemoryOrdering == NdMemoryOrdering::None;
      for (uint8_t K = 0; K < Op.NumInputs; ++K) {
        const auto Offset = frameOffset(Op.Inputs[K]);
        if (!Offset)
          continue;
        if (Memory && K == 0) {
          if (Op.Opcode == NdOp::STORE)
            continue;
          // A load reads every incoming slot it overlaps.
          const int64_t Begin = **Offset;
          const int64_t End = Begin + std::max<int64_t>(Op.Output.Size, 1);
          for (size_t I = Registers; I < Func.Params.size(); ++I) {
            const int64_t Slot =
                First + static_cast<int64_t>(I - Registers) * Layout.SlotBytes;
            if (Begin < Slot + Layout.SlotBytes && Slot < End)
              Unread.erase(static_cast<unsigned>(I));
          }
          // Past the last parameter the caller's frame begins.
          if (End >
              First + static_cast<int64_t>(Func.Params.size() - Registers) *
                          Layout.SlotBytes)
            return dropStack();
          continue;
        }
        // Moving the address keeps it; any other use lets it escape.
        if (!displaced(Op))
          return dropStack();
      }
    }
  }
  return Unread;
}

/// Whether \p V may hold a value \p Func does not determine at a call: an
/// unspecified value, what an earlier call left in a register, a register's
/// entry value where no parameter arrives in it, or a join or copy of one.
class UndeterminedCallValues {
public:
  explicit UndeterminedCallValues(const MedFunc &Func) : Func(Func) {}

  bool contains(const MedVar &V, size_t &Work) {
    Seen.clear();
    return visit(V, Work);
  }

private:
  void index() {
    if (Built)
      return;
    Built = true;
    for (const MedBlock &Block : Func.Blocks) {
      for (const PhiNode &Phi : Block.Phis)
        Joins.emplace(sourceValueKey(Phi.Output), &Phi);
      for (const MedOp &Op : Block.Ops)
        if (Op.Output.Size && !namesEntryValue(Op))
          Definitions.emplace(sourceValueKey(Op.Output), &Op);
    }
    for (const MedCallClobber &Clobber : Func.CallClobbers)
      Clobbers.insert(sourceValueKey(Clobber.Value));
  }

  bool visit(const MedVar &V, size_t &Work) {
    if (V.Kind == MedVar::Unspecified)
      return true;
    if ((V.Kind != MedVar::Reg && V.Kind != MedVar::Temp) || !V.Size)
      return false;
    index();
    // An exhausted budget or a join already on the path adds nothing: the
    // argument then keeps the value it has.
    if (!Work || !Seen.insert(sourceValueKey(V)).second)
      return false;
    --Work;
    if (Clobbers.count(sourceValueKey(V)))
      return true;
    if (const auto Join = Joins.find(sourceValueKey(V)); Join != Joins.end()) {
      if (Join->second->ExceptionalEntry &&
          visit(*Join->second->ExceptionalEntry, Work))
        return true;
      return llvm::any_of(Join->second->Args, [&](const auto &Incoming) {
        return visit(Incoming.second, Work);
      });
    }
    if (const auto Definition = Definitions.find(sourceValueKey(V));
        Definition != Definitions.end()) {
      const MedOp &Op = *Definition->second;
      return Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
             Op.Inputs[0].Size == V.Size && visit(Op.Inputs[0], Work);
    }
    // No definition: the register's entry value, which is a parameter's
    // where the function has one the body reads there.
    return V.Kind == MedVar::Reg &&
           llvm::none_of(Func.Params, [&](const MedVar &P) {
             return P.RegOff == V.RegOff && P.Id >= 0;
           });
  }

  const MedFunc &Func;
  bool Built = false;
  std::map<SourceValueKey, const PhiNode *> Joins;
  std::map<SourceValueKey, const MedOp *> Definitions;
  std::set<SourceValueKey> Clobbers;
  std::set<SourceValueKey> Seen;
};

/// The arguments of \p Func's direct calls that pass a value the function
/// does not determine in a position the callee's body never reads
/// (unreadArgumentPositions), appended to \p Unobserved.  Such a value has no
/// reader, so the source need not evaluate it only to fail: the caller of
/// this passes zero there, where an argument it determines follows: the
/// undetermined values that end a list are dropped as before.  A value the
/// function determines stays, as does every position the callee may read; the
/// callee keeps its parameters.
void collectUnobservedArguments(
    MedFunc &Func, const std::map<va_t, const MedFunc *> &Callees,
    const BinaryImage &Image, std::map<va_t, std::set<unsigned>> &Unread,
    std::vector<std::pair<MedCallInfo *, size_t>> &Unobserved) {
  const auto &TRI = getTargetRegInfo(Image.Arch);
  const size_t Registers =
      TRI.integerArgumentLayout(Image.abiFormat()).Registers.size();
  std::optional<UndeterminedCallValues> Undetermined;
  std::optional<std::map<int, const MedBlock *>> Blocks;
  size_t Work = limits::ir::kSourceEntrySpillWork;
  for (MedCallInfo &Call : Func.CallInfos) {
    if (Call.IsIndirect || Call.SourceCallHint ||
        Call.ArgumentsInCalleeRegisterOrder || Call.ArgumentsFromPrototype ||
        Call.FloatingRegisterArgCount > 0 || Call.BlockId < 0 || Call.OpIdx < 0)
      continue;
    const auto Callee = Callees.find(Call.TargetAddr);
    if (Callee == Callees.end() ||
        Callee->second->Params.size() != Call.Args.size() ||
        Call.IntegerRegisterArgCount !=
            static_cast<int>(std::min(Registers, Call.Args.size())))
      continue;
    const MedFunc &Body = *Callee->second;
    // The recovered call still names this callee, and its caller reads no
    // more result bytes than the callee returns: a register the callee left
    // as it was passed would otherwise have a reader after the call.
    std::optional<bool> Bound;
    const auto bound = [&] {
      if (!Blocks) {
        Blocks.emplace();
        for (const MedBlock &Block : Func.Blocks)
          Blocks->emplace(Block.Id, &Block);
      }
      const auto Block = Blocks->find(Call.BlockId);
      if (Block == Blocks->end() ||
          static_cast<size_t>(Call.OpIdx) >= Block->second->Ops.size())
        return false;
      const MedOp &Op = Block->second->Ops[static_cast<size_t>(Call.OpIdx)];
      return Op.Opcode == NdOp::CALL && Op.NumInputs &&
             Op.Inputs[0].isConst() &&
             Op.Inputs[0].ConstVal == Call.TargetAddr && !Op.SourceCallHint &&
             (!Body.MultiReturn.empty() ||
              Op.Output.Size <= std::max<uint16_t>(Body.ReturnType
                                                       ? Body.ReturnType->Size
                                                       : TRI.PointerSize,
                                                   TRI.PointerSize));
    };
    // Only a position an argument follows needs a value: the source drops
    // the undetermined values that end a call's list, and a zero among them
    // would print an argument the program never passed.
    std::vector<size_t> Candidates;
    std::optional<size_t> LastDetermined;
    for (size_t P = 0; P < Call.Args.size(); ++P) {
      const MedVar &Argument = Call.Args[P];
      // A constant or a parameter is determined.
      if (Argument.isConst() || Argument.Kind == MedVar::Param) {
        LastDetermined = P;
        continue;
      }
      if (!Undetermined)
        Undetermined.emplace(Func);
      if (!Undetermined->contains(Argument, Work)) {
        LastDetermined = P;
        continue;
      }
      // A register the callee's own recovery found a read of is no
      // candidate.
      if (P < Registers && Body.Params[P].Id >= 0)
        continue;
      if (!Bound)
        Bound = bound();
      if (!*Bound)
        break;
      auto [Proof, Missing] = Unread.try_emplace(Call.TargetAddr);
      if (Missing) {
        size_t ProofWork = limits::ir::kSourceEntrySpillWork;
        Proof->second = unreadArgumentPositions(Body, Image, ProofWork);
      }
      if (Proof->second.count(static_cast<unsigned>(P)))
        Candidates.push_back(P);
    }
    for (const size_t P : Candidates)
      if (LastDetermined && P < *LastDetermined)
        Unobserved.emplace_back(&Call, P);
  }
}

} // namespace

void prepareRecoveredSourceStackAccesses(const BinaryImage &Image,
                                         PipelineResult &Result,
                                         llvm::ArrayRef<LowFunc> Originals) {
  const bool HasCallerBatch =
      Image.Arch == Arch::X64 && Image.abiFormat() == BinaryFormat::COFF;
  if (!HasCallerBatch &&
      getTargetRegInfo(Image.Arch).PointerSize >= sizeof(uint64_t)) {
    std::vector<uint64_t> Weights(Result.MedFuncs.size(), 1);
    for (size_t I = 0; I < Result.MedFuncs.size(); ++I)
      for (const auto &Block : Result.MedFuncs[I].Blocks)
        Weights[I] += Block.Ops.size();
    // Other source ABIs have no call-input batch below. Each worker owns only
    // its selected body.
    parallelForEachWeighted(Weights, [&](auto Claim, size_t N) {
      for (size_t I; (I = Claim()) < N;)
        markSettledSourceStackSpills(Result.MedFuncs[I], Image.Arch,
                                     Image.abiFormat(), &Image);
    });
  }
  std::map<va_t, const MedFunc *> Callees;
  std::set<va_t> Complete;
  std::map<va_t, llvm::ArrayRef<LowBlock>> OriginalBlocks;
  for (const LowFunc &Func : Originals)
    if (Func.hasCompleteLiftCoverage() &&
        Func.UnsafeIndirectBranchAddresses.empty()) {
      Complete.insert(Func.Entry);
      if (!Func.Blocks.empty() && Func.Blocks.front().StartAddr == Func.Entry &&
          Func.Blocks.front().Preds.empty())
        OriginalBlocks.emplace(Func.Entry, Func.Blocks);
    }
  if (Result.OmittedCallees)
    for (const auto &[Entry, Blocks] :
         Result.OmittedCallees->SourceOriginalBlocks)
      if (Blocks)
        OriginalBlocks.try_emplace(Entry, *Blocks);
  for (const MedFunc &Func : Result.MedFuncs)
    if (Complete.count(Func.Entry))
      Callees.emplace(Func.Entry, &Func);
  if (Result.OmittedCallees)
    for (const auto &Func : Result.OmittedCallees->Bodies)
      Callees.try_emplace(Func->Entry, Func.get());
  for (MedFunc &Func : Result.MedFuncs) {
    if (Complete.count(Func.Entry))
      markUnobservedBorrowedFrameStores(Func, Image.Arch, Image.abiFormat(),
                                        Callees);
    markUndefinedScratchStores(Func, Image.Arch, Image.abiFormat());
  }
  // A pair-aligned wide argument leaves the lane before it unset.  Where a
  // callee's body never reads such a position, its callers pass zero there
  // instead of the value no one determined.  Every proof reads the bodies as
  // call recovery left them: the replacements follow all of them, so the
  // order of the functions changes no result.
  if (const CallArgumentConvention *Convention =
          callArgumentConvention(Image.Arch, Image.abiFormat());
      Convention && Convention->PairAlignedWideArguments &&
      Convention->ArgumentsFromCallSetup) {
    std::map<va_t, std::set<unsigned>> Unread;
    std::vector<std::pair<MedCallInfo *, size_t>> Unobserved;
    for (MedFunc &Func : Result.MedFuncs)
      collectUnobservedArguments(Func, Callees, Image, Unread, Unobserved);
    for (const auto &[Call, Position] : Unobserved)
      Call->Args[Position] = MedVar::makeConst(
          0, Call->Args[Position].Size, ConstantAddressProvenance::Scalar);
  }
  if (!HasCallerBatch)
    return;
  // What preparation reads of each callee it may follow: kept with the
  // memo's conversion of an omitted body, else read here once.
  OmittedCalleeMemo *Memo = Result.CalleeSignatureMemo.get();
  std::map<va_t, SourceStackScan> OwnScans;
  const auto ScanOf = [&](va_t Entry,
                          const MedFunc &Func) -> const SourceStackScan & {
    if (Memo && !Complete.count(Entry))
      if (const auto *Kept = Memo->conversionOf(Func);
          Kept && Kept->SourceStack)
        return *Kept->SourceStack;
    auto [At, Added] = OwnScans.try_emplace(Entry);
    if (Added)
      At->second = sourceStackScanOf(Func, Image);
    return At->second;
  };
  std::set<va_t> Referenced;
  std::vector<va_t> Pending;
  const auto Reference = [&](va_t Target) {
    if (!Result.CallPrototypeArguments.count(Target) &&
        !Result.CallUserPrototypes.count(Target) &&
        Referenced.insert(Target).second)
      Pending.push_back(Target);
  };
  for (const auto &Func : Result.MedFuncs)
    for (const auto &Call : Func.CallInfos)
      if (!Call.IsIndirect && !Call.SourceCallHint &&
          !Call.ArgumentsInCalleeRegisterOrder)
        Reference(Call.TargetAddr);
  // Follow only bodies already admitted by the bounded signature reader.
  // Reading a callee's source inputs never starts an unbounded image walk.
  for (size_t I = 0; I < Pending.size(); ++I)
    if (const auto At = Callees.find(Pending[I]); At != Callees.end())
      for (const va_t Target : ScanOf(At->first, *At->second).Calls)
        Reference(Target);
  std::vector<const MedFunc *> Sources;
  std::vector<uint64_t> Weights;
  for (const auto &[Entry, Func] : Callees) {
    if (!Referenced.count(Entry))
      continue;
    if (const SourceStackScan &Scan = ScanOf(Entry, *Func); Scan.Candidate) {
      Sources.push_back(Func);
      Weights.push_back(Scan.Weight);
    }
  }
  // Cached bodies are immutable. Prepare each candidate once; independent
  // functions and each dependency wave run in weighted parallel batches. An
  // omitted callee's preparation reads only its kept body and the image, so
  // the memo keeps it with that conversion for every later run.
  using PreparedSource = OmittedCalleeMemo::Conversion::PreparedSource;
  std::vector<OmittedCalleeMemo::Conversion *> Kept(Sources.size(), nullptr);
  std::vector<std::shared_ptr<PreparedSource>> Prepared(Sources.size());
  std::vector<const std::set<const MedOp *> *> OverwrittenSpills(Sources.size(),
                                                                 nullptr);
  std::vector<uint64_t> PrepareWeights(Sources.size(), 0);
  for (size_t I = 0; I < Sources.size(); ++I) {
    if (Memo && !Complete.count(Sources[I]->Entry))
      Kept[I] = Memo->conversionOf(*Sources[I]);
    if (Kept[I] && Kept[I]->Prepared) {
      Sources[I] = &Kept[I]->Prepared->Body;
      OverwrittenSpills[I] = &Kept[I]->Prepared->OverwrittenSpills;
    } else {
      PrepareWeights[I] = Weights[I];
    }
  }
  parallelForEachWeighted(PrepareWeights, [&](auto Claim, size_t N) {
    for (size_t I; (I = Claim()) < N;) {
      if (OverwrittenSpills[I])
        continue;
      const auto &Source = *Sources[I];
      Prepared[I] = prepareSourceStackBody(Source, Image,
                                           Complete.count(Source.Entry) != 0);
      Sources[I] = &Prepared[I]->Body;
    }
  });
  std::vector<const MedValueReads *> ValueReads(Sources.size(), nullptr);
  for (size_t I = 0; I < Sources.size(); ++I) {
    if (Prepared[I]) {
      OverwrittenSpills[I] = &Prepared[I]->OverwrittenSpills;
      ValueReads[I] = Prepared[I]->ValueReads.get();
      if (Kept[I])
        Memo->keepPrepared(*Kept[I], Prepared[I]);
    } else if (Kept[I] && Kept[I]->Prepared) {
      ValueReads[I] = Kept[I]->Prepared->ValueReads.get();
    }
  }
  std::map<va_t, std::set<size_t>> Callers;
  for (size_t I = 0; I < Sources.size(); ++I)
    for (const auto &Block : Sources[I]->Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst())
          Callers[Op.Inputs[0].ConstVal].insert(I);
  std::vector<size_t> Work;
  for (size_t I = 0; I < Sources.size(); ++I)
    Work.push_back(I);
  std::vector<size_t> Remaining(Sources.size(),
                                limits::ir::kSourceEntrySpillWork);
  SourceCalleeParameterReads Facts;
  SourceCalleeParameterOverwrites Overwrites;
  // The callees whose declarations bound their arguments, asked of each
  // callee a query meets rather than listed for the image's declarations:
  // prototypes, the integer signatures omitted callees read, and the
  // imports below, less those a variadic signature leaves open.
  const auto Declared = [&](va_t Target) {
    if (Result.CallEntryVariadicArguments.count(Target) ||
        (Result.OmittedCalleeVariadicArguments &&
         Result.OmittedCalleeVariadicArguments->count(Target)))
      return false;
    if (Result.CallPrototypeArguments.count(Target) ||
        (Result.OmittedCalleeIntegerArguments &&
         Result.OmittedCalleeIntegerArguments->count(Target)))
      return true;
    // Fixed import arity tables also own native stack bounds, without
    // needing a complete typed source prototype. An import rebound to an
    // image routine must use that routine's independently checked contract
    // instead.
    const auto Own = Result.CallArgumentSources.find(Target);
    const bool Unbound =
        Own == Result.CallArgumentSources.end() || Own->second == Target;
    const auto ImportDeclared = [&](const std::map<va_t, int> &Arguments) {
      const auto At = Arguments.find(Target);
      return At != Arguments.end() && At->second >= 0 &&
             Image.findImportAt(Target) && Unbound;
    };
    // An image's summaries list their imports (CallEntryStackArgImports),
    // ascending as CallEntryStackArgs holds them.
    if (Result.CallEntryStackArgImports) {
      if (Unbound &&
          std::binary_search(Result.CallEntryStackArgImports->begin(),
                             Result.CallEntryStackArgImports->end(), Target))
        return true;
    } else if (ImportDeclared(Result.CallEntryStackArgs)) {
      return true;
    }
    return Result.OmittedCalleeStackArgs &&
           ImportDeclared(*Result.OmittedCalleeStackArgs);
  };
  std::vector<std::optional<std::set<unsigned>>> StackReads(Sources.size());
  while (!Work.empty()) {
    std::vector<uint64_t> WaveWeights;
    for (size_t I : Work)
      WaveWeights.push_back(Weights[I]);
    std::vector<SourceRegisterParameterReads> Reads(Work.size());
    std::vector<std::set<unsigned>> Destroyed(Work.size());
    parallelForEachWeighted(WaveWeights, [&](auto Claim, size_t N) {
      for (size_t W; (W = Claim()) < N;) {
        const size_t I = Work[W];
        Reads[W] = sourceRegisterParameterReads(
            *Sources[I], Facts, Remaining[I], Overwrites, ValueReads[I]);
        if (const auto At = OriginalBlocks.find(Sources[I]->Entry);
            At != OriginalBlocks.end() && !At->second.empty()) {
          // This original-CFG fact is independent of the changing source
          // register summaries. Reuse it across this phase's dependency waves.
          if (!StackReads[I])
            StackReads[I] = unobservedSourceStackParametersDeclaredBy(
                *Sources[I], At->second, Declared, Remaining[I], &Image);
          for (unsigned P : *StackReads[I])
            Reads[W].emplace(P, 0);
          Destroyed[W] = sourceRegisterInputOverwrites(
              *Sources[I], At->second, Reads[W], Overwrites,
              *OverwrittenSpills[I], Remaining[I]);
          // A proved original destruction also rules out a source input.
          // This includes the GS case, whose register proof cannot discard
          // even a transient spill observed by its exceptional cookie check.
          for (unsigned P : Destroyed[W])
            Reads[W].emplace(P, 0);
        }
      }
    });
    std::set<size_t> Next;
    for (size_t W = 0; W < Work.size(); ++W) {
      const va_t Entry = Sources[Work[W]]->Entry;
      auto &Known = Facts[Entry];
      bool Changed = false;
      for (const auto &[P, Bytes] : Reads[W]) {
        auto [At, Added] = Known.emplace(P, Bytes);
        if (Added || Bytes < At->second) {
          At->second = Bytes;
          Changed = true;
        }
      }
      for (unsigned P : Destroyed[W])
        Changed |= Overwrites[Entry].insert(P).second;
      if (Changed)
        if (const auto At = Callers.find(Entry); At != Callers.end())
          for (size_t I : At->second)
            if (Remaining[I])
              Next.insert(I);
    }
    Work.assign(Next.begin(), Next.end());
  }
  std::vector<uint64_t> CallerWeights;
  std::set<va_t> Emitted;
  for (const auto &Func : Result.MedFuncs) {
    Emitted.insert(Func.Entry);
    uint64_t Weight = 1;
    for (const auto &Block : Func.Blocks)
      Weight += Block.Ops.size();
    CallerWeights.push_back(Weight);
  }
  parallelForEachWeighted(CallerWeights, [&](auto Claim, size_t N) {
    for (size_t I; (I = Claim()) < N;) {
      replaceKnownSourceCallArguments(Result.MedFuncs[I], Facts);
      recoverX64FillReturns(Result.MedFuncs[I], Image);
      // Call-input recovery has settled as well. Reuse this worker instead
      // of creating another large-stack coordinator for each source query.
      markSettledSourceStackSpills(Result.MedFuncs[I], Image.Arch,
                                   Image.abiFormat(), &Image);
      captureX64CallRCX(Result.MedFuncs[I], Image, Result.CallUnreadGPRs,
                        Emitted);
    }
  });
}

void omitRefusedFunctions(PipelineResult &Result,
                          const std::map<size_t, std::string> &Refused) {
  if (Refused.empty())
    return;
  std::map<va_t, size_t> Audits;
  for (size_t A = 0; A < Result.FunctionAudits.size(); ++A)
    Audits.try_emplace(Result.FunctionAudits[A].Entry, A);
  for (const auto &[I, Diagnostic] : Refused) {
    // A refused conversion leaves no MedFunc identity; LowIR keeps it.
    const LowFunc &Low = Result.LowFuncs[I];
    const auto [Found, Missing] =
        Audits.try_emplace(Low.Entry, Result.FunctionAudits.size());
    if (Missing) {
      PipelineFunctionAudit Added;
      Added.Entry = Low.Entry;
      Added.Name = Low.Name;
      Added.HasLowIR = true;
      Result.FunctionAudits.push_back(std::move(Added));
    }
    PipelineFunctionAudit &Audit = Result.FunctionAudits[Found->second];
    Audit.Disposition = PipelineFunctionDisposition::MedIRFailed;
    Audit.HasMedIR = false;
    Audit.Diagnostic = Diagnostic;
  }
  // Later stages read both lists by the same index. Each keeps its order.
  auto Omit = [&](auto &List) {
    size_t Kept = 0;
    auto Next = Refused.begin();
    for (size_t I = 0; I < List.size(); ++I) {
      if (Next != Refused.end() && Next->first == I) {
        ++Next;
        continue;
      }
      if (Kept != I)
        List[Kept] = std::move(List[I]);
      ++Kept;
    }
    List.erase(List.begin() + Kept, List.end());
  };
  Omit(Result.LowFuncs);
  Omit(Result.MedFuncs);
}

namespace {
/// The integer arguments each function of this run that \p Pending calls
/// directly can be passed: its emitted parameters, which both C routes pass
/// a defined callee exactly. They are the MedIR parameters this run settled
/// and those its debug declaration adds. A callee this proof may still
/// refuse, a variadic one, one a source type hint declares, or one whose
/// declaration places parameters off the platform convention is left out.
std::map<va_t, unsigned> settledCalleeIntegerArity(
    const BinaryImage &Image, const PipelineResult &Result,
    const std::vector<size_t> &Pending, const DebugContext *Dbg) {
  std::set<va_t> Targets;
  for (const size_t I : Pending)
    for (const MedBlock &Block : Result.MedFuncs[I].Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
            Op.Inputs[0].isConst())
          Targets.insert(Op.Inputs[0].ConstVal);
  const llvm::ArrayRef<uint64_t> Registers =
      getTargetRegInfo(Image.Arch).integerParamRegs(Image.abiFormat());
  std::map<va_t, unsigned> Arity;
  for (const MedFunc &Callee : Result.MedFuncs) {
    if (Callee.AnalysisOnly || !Targets.count(Callee.Entry) ||
        !Callee.CallReturnReads.empty() || Callee.IsVariadic ||
        Callee.SourceTypeHint)
      continue;
    unsigned Count = 0;
    for (const MedVar &Param : Callee.Params)
      for (size_t K = 0; K < Registers.size(); ++K)
        if (Param.RegOff == Registers[K])
          Count = std::max(Count, static_cast<unsigned>(K + 1));
    if (const auto Declared =
            Dbg ? Dbg->resolveFunction(Callee.Entry) : std::nullopt) {
      if (Declared->IsVariadic || !Declared->PlatformConvention ||
          !Declared->ParamEntryLocations.empty())
        continue;
      Count = std::max(Count, static_cast<unsigned>(Declared->Params.size()));
    }
    Arity.emplace(Callee.Entry, Count);
  }
  return Arity;
}
} // namespace

bool refuseObservedCallReturnReads(const BinaryImage &Image,
                                   PipelineResult &Result, bool FailsRun,
                                   const DebugContext *Dbg) {
  std::vector<size_t> Pending;
  for (size_t I = 0; I < Result.MedFuncs.size(); ++I)
    if (!Result.MedFuncs[I].CallReturnReads.empty())
      Pending.push_back(I);
  if (Pending.empty())
    return true;
  const std::map<va_t, unsigned> Arity =
      settledCalleeIntegerArity(Image, Result, Pending, Dbg);
  std::vector<std::optional<std::string>> Observations(Pending.size());
  parallelForEach(Pending.size(), [&](auto Claim, size_t Total) {
    for (size_t J; (J = Claim()) < Total;) {
      std::string Diagnostic;
      if (!validateObservedCallReturnReads(
              Result.MedFuncs[Pending[J]], Image, Diagnostic,
              limits::ir::kSourceCallReturnReadWork, &Arity))
        Observations[J] = std::move(Diagnostic);
    }
  });
  std::map<size_t, std::string> Refused;
  for (size_t J = 0; J < Pending.size(); ++J) {
    MedFunc &Func = Result.MedFuncs[Pending[J]];
    Func.CallReturnReads.clear();
    if (!Observations[J])
      continue;
    std::string Error =
        "source MedIR failed for " + Func.Name + ": " + *Observations[J];
    if (FailsRun) {
      Result.Error = std::move(Error);
      return false;
    }
    Refused.emplace(Pending[J], std::move(Error));
  }
  omitRefusedFunctions(Result, Refused);
  return true;
}

//===----------------------------------------------------------------------===//
// buildMedIR — Phase 2
//===----------------------------------------------------------------------===//

void Pipeline::buildMedIR(const BinaryImage &Img, const PipelineOptions &Opts,
                          PipelineResult &Result) {
  auto Phase2Start = std::chrono::steady_clock::now();

  const size_t Total = Result.LowFuncs.size();
  Result.MedFuncs.resize(Total);
  // Each worker owns one element; publish errors in stable function order
  // only after all workers have joined.
  std::vector<std::optional<std::string>> ConversionErrors(Total);

  // Runtime metadata is a source-rendering hint, not a rewrite ABI contract.
  // Patch/lift and safety evidence keep their existing independent semantics.
  std::map<va_t, const SourceFunctionTypeHint *> SourceHints;
  if (Result.InterpreterMachineSourceABI && Result.LowFuncs.size() == 1)
    SourceHints.emplace(Result.LowFuncs.front().Entry,
                        &*Result.InterpreterMachineSourceABI);
  if (Opts.usesSourceDeclarations())
    for (const auto &[Entry, Hint] : Opts.SourceTypeHints)
      SourceHints.emplace(Entry, &Hint);
  if (!Opts.usesMachineMedIR()) {
    for (const auto &Method : Img.ObjCMethods)
      if (objcMethodHasSourceBody(Method))
        SourceHints.emplace(Method.Implementation, &*Method.TypeHint);
  }

  std::map<va_t, SourceFunctionTypeHint> SourceEntryHints;
  for (const auto &[Entry, Hint] : SourceHints)
    SourceEntryHints.emplace(Entry, *Hint);
  auto SourceCalleeHints = SourceEntryHints;
  auto SourceSites = Opts.usesSourceDeclarations()
                         ? Opts.SourceCallSiteHints
                         : decltype(Opts.SourceCallSiteHints){};
  const auto SEHRuntimeMemory = collectLowSEHRuntimeMemory(
      Img, Result.LowFuncs, Result.SEHWritableImageRanges.get());
  std::map<va_t, const LowFunc *> SourceCalleeFunctions;
  for (const auto &F : Result.LowFuncs)
    SourceCalleeFunctions.emplace(F.Entry, &F);
  if (Opts.usesSourceDeclarations())
    for (const auto &[Entry, Hint] : Opts.SourceCalleeTypeHints)
      SourceCalleeHints.insert_or_assign(Entry, Hint);

  // Per-callee callee-cleanup pop (x86 `ret imm`, the i386 SysV sret hidden-
  // pointer pop) so each caller's CALL to such a callee gets a post-call stack-
  // pointer correction during low->med translation.  Built once (read-only) and
  // shared across the parallel converters.
  std::map<va_t, int> CalleePop;
  for (const auto &LF : Result.LowFuncs)
    if (LF.CalleePopBytes > 0)
      CalleePop[LF.Entry] = LF.CalleePopBytes;
  // A stdcall import pops its arguments too: through its stub or its slot,
  // as its prototype or its stub's decoration counts them.  A stub lifted as
  // a function keeps what its own code says.
  for (const auto &[Address, Bytes] : Result.CallEntryStackPops)
    CalleePop.try_emplace(Address, Bytes);

  // GOT/pointer-slot VAs holding a stack-probe import (____chkstk_darwin).
  // Apple clang guards a large frame with a GOT-indirect probe call in the
  // prologue; the low->med converter clears that call's spurious x0 output
  // before SSA so it does not kill the live-in argument registers (see
  // setStackProbeSlots).  Built once (read-only) and shared across the parallel
  // converters.  Empty on non-Mach-O (ImportPtrSlots is only populated there)
  // -> no-op.
  std::set<va_t> StackProbeSlots;
  for (const auto &[SlotVA, SymName] : Img.ImportPtrSlots) {
    if (isDarwinStackProbeName(SymName))
      StackProbeSlots.insert(SlotVA);
  }

  // Weight each function by its LowIR op count so the heaviest translations
  // start first and the tail stays balanced (see parallelForEachWeighted).
  std::vector<uint64_t> Weight(Total, 1);
  for (size_t I = 0; I < Total; ++I) {
    uint64_t W = 1;
    for (const auto &B : Result.LowFuncs[I].Blocks)
      W += B.Ops.size();
    Weight[I] = W;
  }

  // The runtime routines a call may name, read once for the run, or once
  // for every run that shares the analysis cache.
  std::shared_ptr<const PipelineAnalysisCache::SymbolInventory> Inventory;
  std::map<va_t, Intrinsic> OwnRuntimeRoutines;
  const std::map<va_t, Intrinsic> *RuntimeRoutines = &OwnRuntimeRoutines;
  if (PipelineAnalysisCache *Shared = sharedAnalysisCache(Img, Opts)) {
    Inventory = PipelineAnalysisCache::SymbolInventory::of(Img, *Shared);
    RuntimeRoutines = &Inventory->runtimeRoutineCalls(Img);
  } else {
    OwnRuntimeRoutines = runtimeRoutineCallsOf(Img, Img.Arch);
  }

  auto Configure = [&](LowToMedConverter &Local) {
    Local.setBinaryImage(&Img);
    Local.setRuntimeRoutineCalls(RuntimeRoutines);
    Local.setSourceCallHintsEnabled(Opts.usesSourceDeclarations());
    Local.setSourceCallRuntimeHintsEnabled(!Opts.usesMachineMedIR());
    Local.setSourceEntryTypeHints(&SourceEntryHints);
    Local.setSourceCalleeTypeHints(&SourceCalleeHints);
    Local.setSourceCalleeFunctions(&SourceCalleeFunctions);
    Local.setSEHFrameCallees(&SourceCalleeFunctions);
    Local.setSEHSourceProjectionEnabled(!Opts.usesMachineMedIR());
    Local.setSEHRuntimeMemory(&SEHRuntimeMemory);
    Local.setObjCBlockCaptureCallFields(&Opts.ObjCBlockCaptureFields);
    Local.setObjCBlockParameterReceivers(&Opts.ObjCBlockParameterReceivers);
    Local.setObjCBlockCaptureReceivers(&Opts.ObjCBlockCaptureReceivers);
    Local.setCalleePopMap(&CalleePop);
    Local.setCallMayWriteGPRs(&Result.CallMayWriteGPRs.get());
    Local.setCallExitConstants(&Result.CallExitConstants.get());
    Local.setCallEntryReadGPRs(&Result.CallEntryReadGPRs.get());
    Local.setCallUnreadGPRs(&Result.CallUnreadGPRs.get());
    Local.setCallArgumentSources(&Result.CallArgumentSources.get());
    Local.setCallEntryStackArgs(&Result.CallEntryStackArgs.get());
    Local.setCallEntryStackSlots(&Result.CallEntryStackSlots);
    Local.setCallEntryArgumentCounts(&Result.CallEntryArgumentCounts);
    Local.setCallEntryFloatingArguments(
        &Result.CallEntryFloatingArguments.get());
    Local.setCallEntryIntegerArguments(&Result.CallEntryIntegerArguments.get());
    Local.setCallEntryVariadicArguments(
        &Result.CallEntryVariadicArguments.get());
    Local.setUserPrototypeCallees(&Result.CallUserPrototypes.get());
    Local.setCallDispatchThunks(&Result.CallDispatchThunks.get());
    Local.setCallVariadicFrom(&Result.CallVariadicFrom.get());
    Local.setStackProbeIndex(Result.StackProbeIndex.get());
    Local.setFormattedCalls(&Result.FormattedCalls);
    Local.setStackProbeSlots(&StackProbeSlots);
  };
  // A projected LowIR body must remain alive until the source stack checks.
  // Each conversion owns its slot; no worker reads another worker's MedIR.
  std::vector<std::unique_ptr<LowFunc>> ProjectedFunctions(Total);
  std::vector<uint8_t> Converted(Total, 0);
  std::vector<uint8_t> PendingStackProof(Total, 0);
  auto FallbackOne = [&](size_t I) {
    Converted[I] = 0;
    ProjectedFunctions[I].reset();
    syncWarning() << "pipeline: low->med threw on " << Result.LowFuncs[I].Name
                  << "\n";
    const LowFunc &LF = Result.LowFuncs[I];
    MedFunc Fallback;
    Fallback.AnalysisOnly = LF.AnalysisOnly;
    Fallback.Name = LF.Name;
    Fallback.Entry = LF.Entry;
    Fallback.OriginalSize = LF.OriginalSize;
    Fallback.DebugName = LF.DebugName;
    Fallback.SourceFile = LF.SourceFile;
    Fallback.SourceLine = LF.SourceLine;
    Fallback.ExceptionMetadata = LF.ExceptionMetadata;
    Fallback.JumpTables = LF.JumpTables;
    for (const auto &LB : LF.Blocks) {
      MedBlock MB;
      MB.Id = LB.Id;
      MB.StartAddr = LB.StartAddr;
      MB.EndAddr = LB.EndAddr;
      MB.Succs = LB.Succs;
      MB.Preds = LB.Preds;
      Fallback.Blocks.push_back(std::move(MB));
    }
    Result.MedFuncs[I] = std::move(Fallback);
  };
  auto ConvertOne = [&](LowToMedConverter &Local, size_t I) {
    Converted[I] = 0;
    PendingStackProof[I] = 0;
    try {
      const auto Sites = SourceSites.find(Result.LowFuncs[I].Entry);
      Local.setSourceCallSiteHints(Opts.usesSourceDeclarations() &&
                                           Sites != SourceSites.end()
                                       ? &Sites->second
                                       : nullptr);
      const auto Frame = Opts.SourceStackFrames.find(Result.LowFuncs[I].Entry);
      auto &Projected = ProjectedFunctions[I];
      Projected.reset();
      if (!Opts.usesMachineMedIR() && Frame != Opts.SourceStackFrames.end()) {
        std::string Error;
        auto FrameSource = projectSourceStackFrame(Result.LowFuncs[I], Img.Arch,
                                                   Frame->second, Error);
        if (!FrameSource)
          throw std::runtime_error(Error);
        Projected = std::make_unique<LowFunc>(std::move(*FrameSource));
      }
      Result.MedFuncs[I] =
          Local.convert(Projected ? *Projected : Result.LowFuncs[I], Img.Arch,
                        Img.abiFormat());
      Result.MedFuncs[I].AnalystStackFrame = bool(Projected);
      auto &MF = Result.MedFuncs[I];
      auto &LF = Result.LowFuncs[I];
      trimFuncStorage(MF);
      MF.OriginalSize = LF.OriginalSize;
      MF.DebugName = LF.DebugName;
      MF.SourceFile = LF.SourceFile;
      MF.SourceLine = LF.SourceLine;
      auto Hint = SourceHints.find(MF.Entry);
      if (Hint != SourceHints.end())
        MF.SourceTypeHint = *Hint->second;
      Converted[I] = 1;
    } catch (const LowToMedConversionError &Error) {
      ConversionErrors[I] = Error.what();
    } catch (...) {
      FallbackOne(I);
    }
  };
  MedCallReturnRanges CallReturns;
  bool CalleeFactsReady = false;
  auto FinishOne = [&](size_t I) {
    if (!Converted[I] || ConversionErrors[I])
      return;
    bool StackPrepared = false;
    try {
      auto &MF = Result.MedFuncs[I];
      const auto &Projected = ProjectedFunctions[I];
      prepareMedStackAccesses(MF, Projected ? *Projected : Result.LowFuncs[I],
                              Img, medStackProjection(Opts),
                              CalleeFactsReady ? &CallReturns : nullptr,
                              !CalleeFactsReady,
                              Result.InterpreterMachineSourceABI.has_value() &&
                                  Result.LowFuncs.size() == 1);
      StackPrepared = true;
      if (medStackProjection(Opts) == MedStackProjection::Source)
        expandSourceRegisterCalls(MF, Img, Result.CallRegisterLeaves);
      size_t MedOps = 0;
      for (const auto &Block : MF.Blocks)
        MedOps += Block.Ops.size();
      if (MF.Blocks.size() <= limits::kMaxStructurableMedBlocks &&
          MedOps <= static_cast<size_t>(limits::kMaxSSANodes))
        inferMedTypes(MF, Img.Arch);
    } catch (const LowToMedConversionError &Error) {
      ConversionErrors[I] = Error.what();
      PendingStackProof[I] = !StackPrepared;
    } catch (...) {
      FallbackOne(I);
    }
    if (!PendingStackProof[I])
      ProjectedFunctions[I].reset();
  };
  auto CompleteStackProofs = [&] {
    std::vector<size_t> Pending;
    std::vector<uint64_t> PendingWeight;
    for (size_t I = 0; I < Total; ++I)
      if (PendingStackProof[I]) {
        Pending.push_back(I);
        PendingWeight.push_back(Weight[I]);
      }
    if (Pending.empty())
      return;
    // Ordinary local proofs already completed the other functions. Only a
    // missing stack proof asks for the complete, immutable callee table, after
    // every conversion has joined. A second refusal remains a real error.
    CallReturns = sourceCallReturnRanges(Img, Result);
    CalleeFactsReady = true;
    parallelForEachWeighted(PendingWeight, [&](auto Claim, size_t N) {
      for (size_t J; (J = Claim()) < N;) {
        const size_t I = Pending[J];
        ConversionErrors[I].reset();
        PendingStackProof[I] = 0;
        FinishOne(I);
        ProjectedFunctions[I].reset();
      }
    });
  };
  parallelForEachWeighted(Weight, [&](auto Claim, size_t N) {
    LowToMedConverter Local;
    Configure(Local);
    for (size_t I; (I = Claim()) < N;) {
      ConvertOne(Local, I);
      FinishOne(I);
    }
  });
  CompleteStackProofs();

  // Discover bounded incoming-stack leaves from this conversion, then only
  // reconvert their definitions and exact callers. Call operands must
  // be present before SSA; adding a C parameter after lowering loses the
  // caller's store and cannot restore the physical CALL return-address bias.
  if (Opts.usesSourceDeclarations() && supportsIncomingStackSources(Img) &&
      std::none_of(ConversionErrors.begin(), ConversionErrors.end(),
                   [](const auto &Error) { return Error.has_value(); })) {
    std::set<va_t> ExplicitCallTargets;
    for (const auto &[Caller, Sites] : SourceSites)
      for (const auto &[Site, Hint] : Sites)
        ExplicitCallTargets.insert(Hint.TargetAddress);
    std::set<va_t> Inferred;
    std::set<size_t> Affected;
    std::map<va_t, size_t> LocalDefinitions;
    for (size_t I = 0; I < Total; ++I) {
      const auto &Low = Result.LowFuncs[I];
      if (SourceCalleeHints.count(Low.Entry) ||
          ExplicitCallTargets.count(Low.Entry) ||
          Result.CallUserPrototypes.count(Low.Entry) ||
          Result.MedFuncs[I].AnalystStackFrame)
        continue;
      auto Hint = inferIncomingStackLeaf(Img, Low, Result.MedFuncs[I]);
      if (!Hint)
        continue;
      // A candidate must also pass the shared source-carrier binder before
      // any caller starts depending on its physical locations.
      MedFunc Bound = Result.MedFuncs[I];
      Bound.SourceTypeHint = *Hint;
      inferMedTypes(Bound, Img.Arch);
      if (!Bound.SourceParametersBound || !Bound.SourceTypeHint)
        continue;
      SourceEntryHints.emplace(Low.Entry, *Hint);
      SourceCalleeHints.emplace(Low.Entry, std::move(*Hint));
      SourceHints.emplace(Low.Entry, &SourceEntryHints.at(Low.Entry));
      Inferred.insert(Low.Entry);
      LocalDefinitions.emplace(Low.Entry, I);
      Affected.insert(I);
    }
    // A register operand can have one exact local value without being a
    // static machine operand. Keep its original occurrence as the binding key;
    // numeric agreement alone never supplies a callee signature.
    std::vector<std::map<SourceCallOccurrenceKey, va_t>> CallTargets(Total);
    for (size_t I = 0; I < Total; ++I) {
      const auto &Low = Result.LowFuncs[I];
      CallTargets[I] = constantRegisterCallTargets(Low);
      for (const auto &Block : Low.Blocks)
        for (const auto &Op : Block.Ops)
          if (const auto Site = sourceCallOccurrenceKey(Op);
              Site && Site->Opcode == NdOp::CALL && Site->StaticTarget)
            CallTargets[I].emplace(*Site, *Site->StaticTarget);
    }
    std::set<va_t> OmittedTargets;
    for (const auto &Calls : CallTargets)
      for (const auto &[Site, Target] : Calls)
        if (!SourceCalleeFunctions.count(Target) &&
            !SourceCalleeHints.count(Target) &&
            !ExplicitCallTargets.count(Target) &&
            !Result.CallUserPrototypes.count(Target))
          OmittedTargets.insert(Target);
    for (auto &[Entry, Hint] :
         readIncomingStackCallees(Img, Result, OmittedTargets)) {
      SourceCalleeHints.emplace(Entry, std::move(Hint));
      Inferred.insert(Entry);
    }
    // An automatically declared input must be supplied by every caller in
    // this request. A partial/unwritten outgoing word is not a scalar value.
    // Withdraw the candidate as a unit rather than publish incompatible
    // definition and call signatures in a whole-module source projection.
    std::set<va_t> Unavailable;
    for (size_t I = 0; I < Total; ++I)
      for (const auto &[Site, Target] : CallTargets[I]) {
        if (!Inferred.count(Target))
          continue;
        SourceCallTypeHint Hint;
        Hint.Signature = SourceCalleeHints.at(Target);
        const auto Bias =
            sourceCallReturnAddressBias(&Img, Img.Arch, Site.Instruction);
        std::string Diagnostic;
        if (!Bias ||
            !sourceCallStackArgumentsDefined(Result.LowFuncs[I], Hint, Site,
                                             Img.Arch, *Bias, Diagnostic))
          Unavailable.insert(Target);
      }
    for (va_t Entry : Unavailable) {
      Inferred.erase(Entry);
      SourceCalleeHints.erase(Entry);
      SourceHints.erase(Entry);
      SourceEntryHints.erase(Entry);
      if (auto Local = LocalDefinitions.find(Entry);
          Local != LocalDefinitions.end())
        Affected.erase(Local->second);
    }
    for (size_t I = 0; I < Total && !Inferred.empty(); ++I)
      for (const auto &[Site, Target] : CallTargets[I]) {
        if (!Inferred.count(Target))
          continue;
        SourceCallTypeHint Hint;
        Hint.CallKind = SourceCallTypeHint::Kind::Native;
        Hint.TargetAddress = Target;
        Hint.TargetName = Img.getFunctionNameAt(Target);
        Hint.Signature = SourceCalleeHints.at(Target);
        if (SourceSites[Result.LowFuncs[I].Entry]
                .try_emplace(Site, std::move(Hint))
                .second)
          Affected.insert(I);
      }
    const std::vector<size_t> Indices(Affected.begin(), Affected.end());
    std::vector<uint64_t> ReconvertWeights;
    for (size_t I : Indices)
      ReconvertWeights.push_back(Weight[I]);
    CallReturns.clear();
    parallelForEachWeighted(ReconvertWeights, [&](auto Claim, size_t N) {
      LowToMedConverter Local;
      Configure(Local);
      for (size_t J; (J = Claim()) < N;) {
        ConvertOne(Local, Indices[J]);
        FinishOne(Indices[J]);
      }
    });
    CompleteStackProofs();
  }

  // A run restricted to requested functions fails with the first one MedIR
  // refuses, as asking for that function should (refusalFailsRun). Any other
  // run names each refused function in its audit and converts the others.
  std::map<size_t, std::string> Refused;
  for (size_t I = 0; I < Total; ++I) {
    if (!ConversionErrors[I])
      continue;
    std::string Error = "LowIR to MedIR failed for " + Result.LowFuncs[I].Name +
                        ": " + *ConversionErrors[I];
    if (refusalFailsRun(Opts)) {
      Result.Error = std::move(Error);
      Result.Success = false;
      Result.MedFuncs.clear();
      return;
    }
    Refused.emplace(I, std::move(Error));
  }
  omitRefusedFunctions(Result, Refused);

  recordMedIRVerification(Result, "pipeline-med-final");

  [[maybe_unused]] auto Elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - Phase2Start)
          .count();
  LLVM_DEBUG(llvm::dbgs() << "pipeline: LowIR -> MedIR took " << Elapsed
                          << "ms (" << Result.MedFuncs.size()
                          << " functions)\n");
}

} // namespace neverd

neverd::PipelineResult neverd::Pipeline::rebuildSourceCalls(
    const BinaryImage &Img, const PipelineOptions &Opts,
    const PipelineResult &Original, DebugContext *Dbg) {
  PipelineResult Result;
  if (Opts.usesMachineMedIR() ||
      (Opts.SourceCallSiteHints.empty() && Opts.SourceStackFrames.empty())) {
    Result.Error =
        "Call-site declarations require an explicit private source variant";
    return Result;
  }
  Result.SourceImage = &Img;
  for (const auto &Function : Original.LowFuncs)
    if (Opts.OnlyFunctionEntries.count(Function.Entry))
      Result.LowFuncs.push_back(Function);
  if (Result.LowFuncs.empty()) {
    Result.Error = "The original machine operations are unavailable";
    return Result;
  }
  copyCallSignatureFacts(Original, Result);
#define COPY_SOURCE_FACT(Field) Result.Field = Original.Field
  COPY_SOURCE_FACT(CallDispatchThunks);
  COPY_SOURCE_FACT(CallEntryStackPops);
  COPY_SOURCE_FACT(CallRegisterLeaves);
  COPY_SOURCE_FACT(CallVariadicFrom);
  COPY_SOURCE_FACT(FormattedCalls);
  COPY_SOURCE_FACT(OmittedCallees);
  COPY_SOURCE_FACT(CallFloatReturns);
  COPY_SOURCE_FACT(SEHWritableImageRanges);
  COPY_SOURCE_FACT(StackProbeIndex);
#undef COPY_SOURCE_FACT
  Result.Success = true;
  buildMedIR(Img, Opts, Result);
  if (!Result.Success || Result.MedIRVerifierFailures) {
    Result.Success = false;
    return Result;
  }
  buildHighIR(Img, Opts, Result, Dbg);
  return Result;
}
