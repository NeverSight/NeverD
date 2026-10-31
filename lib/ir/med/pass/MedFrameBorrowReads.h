//===- MedFrameBorrowReads.h - Bounded borrowed-frame reads -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Complete use proofs for removing unobserved private-frame stores.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_PASS_MEDFRAMEBORROWREADS_H
#define NEVERD_IR_MED_PASS_MEDFRAMEBORROWREADS_H

#include "../abi/MedABIPassDetail.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/med/MedStackAlignment.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SaveAndRestore.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::detail {

/// A complete use proof, rather than a type or an observed access list. A
/// borrowed pointer can move through scalar SSA values and direct synchronous
/// calls; storing it, returning it, or losing its offset cancels the proof.
/// Reads are a union over every path, so an unvisited CFG arm cannot supply a
/// noncapture claim. Distinct offsets at a join and recursive borrows remain
/// conservative. No result outlives the current operation/argument sequence.
class FrameBorrowReads {
public:
  using Range = std::pair<int64_t, int64_t>;
  using Ranges = std::vector<Range>;

  FrameBorrowReads(Arch Architecture, BinaryFormat Format,
                   const std::map<va_t, const MedFunc *> &Callees)
      : TRI(getTargetRegInfo(Architecture)), Format(Format), Callees(Callees) {}

  std::optional<Ranges> frame(const MedFunc &Func) {
    if (Func.Blocks.empty() || !Func.Blocks.front().Preds.empty())
      return std::nullopt;
    // An opaque zero-argument call is harmless only before this invocation
    // has formed any address into its new private frame. After that point,
    // even an omitted argument register could still carry such an address.
    const auto &Entry = Func.Blocks.front();
    std::vector<MedVar> Values;
    std::vector<size_t> Positions;
    for (size_t I = 0; I < Entry.Ops.size(); ++I) {
      const MedOp &Op = Entry.Ops[I];
      if (Op.Output.Kind == MedVar::Reg && Op.Output.Size &&
          Op.Output.RegOff != TRI.StackPointer) {
        Values.push_back(Op.Output);
        Positions.push_back(I);
      }
    }
    const auto Offsets =
        entryStackOffsets(Func, Values, Arch::X64, Format, Func.EntryKind);
    size_t FirstAddress = Entry.Ops.size();
    for (size_t I = 0; I < Offsets.size(); ++I)
      if (Offsets[I])
        FirstAddress = std::min(FirstAddress, Positions[I]);
    for (size_t I = 0; I < Entry.Ops.size(); ++I)
      if (Entry.Ops[I].Opcode == NdOp::STORE && !Entry.Ops[I].PrologueSave)
        FirstAddress = std::min(FirstAddress, I);
    for (size_t I = 0; I < FirstAddress; ++I)
      EmptyPrefixCalls.insert(&Entry.Ops[I]);
    const auto StackPointers = sourceCallStackPointers(Func, Arch::X64, Budget);
    Values.clear();
    for (const auto &[Call, Stack] : StackPointers)
      Values.push_back(Stack);
    const auto CallOffsets =
        entryStackOffsets(Func, Values, Arch::X64, Format, Func.EntryKind);
    size_t I = 0;
    for (const auto &[Call, Stack] : StackPointers) {
      const auto Offset = CallOffsets[I++];
      int64_t End;
      const auto Extent = outgoingExtent(*Call);
      if (!Offset || !Extent || llvm::AddOverflow(*Offset, *Extent, End))
        return std::nullopt;
      // A call owns its home area and any storage this invocation already
      // released below SP. Neither requires an explicit pointer parameter.
      ImplicitFrameReads.emplace_back(-Func.FrameSize, End);
      ReadSites[Call].emplace_back(-Func.FrameSize, End);
      LocatedCalls.insert(Call);
    }
    return through(Func, roots(Func, TRI.StackPointer, std::nullopt), true,
                   true);
  }

  /// The complete frame-use proof above supplies every possible reader. A
  /// partial overwrite kills only its bytes on that path; neither a callee
  /// write nor a missing operation is assumed to initialize anything.
  bool unreadAfter(
      const MedFunc &Func, const MedOp &Save, int64_t Offset, uint16_t Bytes,
      const std::map<const MedOp *, std::pair<int64_t, uint16_t>> &Stores) {
    if (!Bytes || Bytes > sizeof(uint64_t))
      return false;
    std::map<int, size_t> Blocks;
    std::optional<std::pair<size_t, size_t>> Start;
    for (size_t B = 0; B < Func.Blocks.size(); ++B) {
      const auto &Block = Func.Blocks[B];
      if (Block.Ops.size() >= Budget)
        return false;
      Budget -= Block.Ops.size() + 1;
      if (!Blocks.emplace(Block.Id, B).second)
        return false;
      for (size_t I = 0; I < Block.Ops.size(); ++I)
        if (&Block.Ops[I] == &Save)
          Start = {B, I + 1};
    }
    if (!Start)
      return false;
    auto overlap = [&](int64_t Begin, int64_t End) {
      uint64_t Mask = 0;
      for (unsigned I = 0; I < Bytes; ++I)
        if (Offset + I >= Begin && Offset + I < End)
          Mask |= uint64_t{1} << I;
      return Mask;
    };
    using Visit = std::tuple<size_t, size_t, uint64_t>;
    std::vector<Visit> Work{
        {Start->first, Start->second, (uint64_t{1} << Bytes) - 1}};
    std::map<std::pair<size_t, size_t>, uint64_t> Seen;
    while (!Work.empty()) {
      auto [B, First, Pending] = Work.back();
      Work.pop_back();
      auto &Visited = Seen[{B, First}];
      Pending &= ~Visited;
      if (!Pending)
        continue;
      Visited |= Pending;
      const auto &Block = Func.Blocks[B];
      bool Returned = false;
      for (size_t I = First; Pending && I < Block.Ops.size(); ++I) {
        if (!Budget)
          return false;
        --Budget;
        const auto &Op = Block.Ops[I];
        if (Op.Dead)
          continue;
        if (const auto At = ReadSites.find(&Op); At != ReadSites.end())
          for (const auto &[Begin, End] : At->second) {
            if (!Budget)
              return false;
            --Budget;
            if (Pending & overlap(Begin, End))
              return false;
          }
        if (!Op.PrologueSave)
          if (const auto At = Stores.find(&Op); At != Stores.end()) {
            int64_t End;
            if (llvm::AddOverflow(At->second.first, int64_t(At->second.second),
                                  End))
              return false;
            Pending &= ~overlap(At->second.first, End);
          }
        if (Op.Opcode == NdOp::RETURN || Op.DoesNotReturn) {
          Returned = true;
          break;
        }
      }
      if (!Pending || Returned)
        continue;
      if (Block.Succs.empty())
        return false;
      for (int Successor : Block.Succs) {
        if (!Budget)
          return false;
        --Budget;
        const auto Next = Blocks.find(Successor);
        if (Next == Blocks.end())
          return false;
        Work.emplace_back(Next->second, 0, Pending);
      }
    }
    return true;
  }

private:
  using ValueKey = std::tuple<MedVar::VarKind, int, int>;

  static ValueKey keyOf(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }

  std::optional<unsigned> argumentCount(const MedOp &Call) const {
    const auto Layout = TRI.integerArgumentLayout(Format);
    if (Call.CalleeRegisterArgs < 0 || Call.CalleeStackArgs < 0 ||
        unsigned(Call.CalleeRegisterArgs) > Layout.Registers.size())
      return std::nullopt;
    const unsigned Count =
        std::max(Call.CalleeRegisterArgs, Call.CalleeStackArgs);
    if (Count > limits::ir::kMaxSourceABIParameters)
      return std::nullopt;
    return Count;
  }

  std::optional<int64_t> outgoingExtent(const MedOp &Call) const {
    const auto Count = argumentCount(Call);
    if (!Count)
      return std::nullopt;
    const auto Layout = TRI.integerArgumentLayout(Format);
    return Layout.CallStackBase +
           (*Count > Layout.Registers.size()
                ? (*Count - Layout.Registers.size()) * Layout.SlotBytes
                : 0);
  }

  struct Use {
    const MedOp *Op = nullptr;
    const PhiNode *Phi = nullptr;
    const MedCallInfo *Call = nullptr;
    unsigned Input = 0;
  };
  const TargetRegInfo &TRI;
  BinaryFormat Format;
  const std::map<va_t, const MedFunc *> &Callees;
  size_t Budget = limits::ir::kSourceFrameBorrowWork;
  unsigned CallDepth = 0;
  std::set<const MedOp *> EmptyPrefixCalls;
  std::set<const MedOp *> LocatedCalls;
  Ranges ImplicitFrameReads;
  std::map<const MedOp *, Ranges> ReadSites;
  std::map<std::pair<va_t, uint64_t>, std::optional<Ranges>> Known;

  struct FunctionUses {
    std::map<ValueKey, std::vector<Use>> Uses;
    std::map<std::pair<int, int>, MedCallInfo> NativeCalls;
    Ranges StackReads;
    bool Ready = false;
  };
  std::map<va_t, FunctionUses> IndexedUses;
  std::map<va_t, std::vector<MedVar>> IncomingRoots;

  std::vector<MedVar> roots(const MedFunc &Func, uint64_t Register,
                            std::optional<unsigned> Parameter) {
    if (Func.Blocks.empty())
      return {};
    auto [Cached, New] = IncomingRoots.try_emplace(Func.Entry);
    if (New) {
      std::map<ValueKey, MedVar> Values;
      std::set<ValueKey> Defined, EntrySeeds;
      for (const MedOp &Op : Func.Blocks.front().Ops) {
        if (!Budget)
          return {};
        --Budget;
        if (Op.Opcode != NdOp::COPY)
          break;
        if (Op.Output.Kind == MedVar::Reg && Op.NumInputs == 1 &&
            Op.Inputs[0].Kind == MedVar::Reg &&
            Op.Inputs[0].Id == Op.Output.Id &&
            Op.Inputs[0].RegOff == Op.Output.RegOff &&
            Op.Inputs[0].SSAVer == 0) {
          EntrySeeds.insert(keyOf(Op.Output));
          EntrySeeds.insert(keyOf(Op.Inputs[0]));
        }
      }
      for (const MedBlock &Block : Func.Blocks) {
        for (const PhiNode &Phi : Block.Phis) {
          if (!Budget)
            return {};
          --Budget;
          Defined.insert(keyOf(Phi.Output));
        }
        for (const MedOp &Op : Block.Ops) {
          if (!Budget)
            return {};
          --Budget;
          if (Op.Output.Size && !Op.Output.isConst())
            Defined.insert(keyOf(Op.Output));
        }
      }
      const auto Note = [&](const MedVar &V) {
        if (!Budget)
          return;
        --Budget;
        // A first definition can have version zero. Only an unbound value
        // or an entry seed carries the incoming register.
        if ((!Defined.count(keyOf(V)) || EntrySeeds.count(keyOf(V))) &&
            ((V.Kind == MedVar::Reg && V.SSAVer == 0) ||
             V.Kind == MedVar::Param))
          Values.emplace(keyOf(V), V);
      };
      for (const MedBlock &Block : Func.Blocks) {
        for (const PhiNode &Phi : Block.Phis)
          for (const auto &[Pred, Value] : Phi.Args)
            Note(Value);
        for (const MedOp &Op : Block.Ops)
          for (unsigned I = 0; I < Op.NumInputs; ++I)
            Note(Op.Inputs[I]);
      }
      for (const MedCallInfo &Call : Func.CallInfos)
        for (const MedVar &Arg : Call.Args)
          Note(Arg);
      for (const auto &[Key, Value] : Values)
        Cached->second.push_back(Value);
    }
    std::vector<MedVar> Result;
    for (const auto &Value : Cached->second) {
      if (!Budget)
        return {};
      --Budget;
      if ((Register != kNoParamReg && Value.Kind == MedVar::Reg &&
           Value.RegOff / TRI.PointerSize == Register / TRI.PointerSize) ||
          (Parameter && Value.Kind == MedVar::Param && Value.Id == *Parameter))
        Result.push_back(Value);
    }
    return Result;
  }

  std::optional<Ranges> parameter(const MedCallInfo &Call, unsigned Index) {
    const auto Found = Callees.find(Call.TargetAddr);
    if (Call.IsIndirect || Found == Callees.end())
      return std::nullopt;
    const MedFunc &Func = *Found->second;
    if (Func.SourceParametersBound || Func.IsVariadic ||
        Func.CC != CallingConv::Win64 || !Func.FPParamScalarBytes.empty())
      return std::nullopt;
    const auto Layout = TRI.integerArgumentLayout(Format);
    const llvm::ArrayRef<uint64_t> Registers =
        Call.ArgumentsInCalleeRegisterOrder
            ? llvm::ArrayRef<uint64_t>(Func.IntegerArgumentRegisters)
            : Layout.Registers;
    const uint64_t Register =
        Index < Registers.size() ? Registers[Index] : kNoParamReg;
    // Incoming stack storage has its own alias/overwrite semantics. A
    // missing Param alone does not prove that its bytes are never read.
    if (Register == kNoParamReg) {
      return std::nullopt;
    }
    return inputRegister(Func, Register);
  }

  std::optional<Ranges> inputRegister(const MedFunc &Func, uint64_t Register) {
    const auto Key = std::pair{Func.Entry, Register};
    if (const auto It = Known.find(Key); It != Known.end())
      return It->second;
    if (CallDepth >= limits::ir::kSourceFrameBorrowCallDepth)
      return std::nullopt;
    llvm::SaveAndRestore<unsigned> Depth(CallDepth, CallDepth + 1);
    const int Index = TRI.integerArgumentLayout(Format).registerIndex(Register);
    auto Result = through(
        Func,
        roots(Func, Register,
              Index < 0 ? std::nullopt : std::optional<unsigned>(Index)),
        false, Register == TRI.StackPointer);
    Known.emplace(Key, Result);
    return Result;
  }

  std::optional<Ranges> through(const MedFunc &Func,
                                const std::vector<MedVar> &Roots, bool IsFrame,
                                bool IsStack = false) {
    if (!Budget || Func.SkippedSSA || Func.AnalystStackFrame ||
        Func.Blocks.empty() || Func.SourceParametersBound || Func.IsVariadic ||
        Func.CC != CallingConv::Win64 || !Func.FPParamScalarBytes.empty() ||
        !Func.IntegerArgumentRegisters.empty() ||
        Func.EntryKind != StackEntryKind::Call ||
        (Func.ExceptionMetadata &&
         (Func.ExceptionMetadata->Personality != ExceptionPersonality::None ||
          Func.ExceptionMetadata->ParseStatus !=
              ExceptionParseStatus::Complete)) ||
        llvm::any_of(Func.ModuleAnalysisRoots,
                     [&](va_t Root) { return Root != Func.Entry; }))
      return std::nullopt;
    auto [Indexed, New] = IndexedUses.try_emplace(Func.Entry);
    auto &Graph = Indexed->second;
    if (!New && !Graph.Ready)
      return std::nullopt;
    auto &Uses = Graph.Uses;
    if (New) {
      // Index each immutable body once, independently of the queried register.
      // A recursive call reaches an unfinished index and supplies no fact.
      std::vector<const MedOp *> OpaquePrefixCalls;
      std::map<std::pair<int, int>, const MedCallInfo *> Calls;
      for (const MedCallInfo &Call : Func.CallInfos) {
        if (!Budget)
          return std::nullopt;
        --Budget;
        if (!Calls.emplace(std::pair{Call.BlockId, Call.OpIdx}, &Call).second)
          return std::nullopt;
      }
      std::vector<std::pair<const MedOp *, const MedCallInfo *>> Invocations;
      std::set<const MedOp *> LocalCalls;
      std::map<const MedOp *, int64_t> CallOffsets;
      if (IsFrame) {
        LocalCalls = LocatedCalls;
        Graph.StackReads = ImplicitFrameReads;
      } else {
        const auto StackPointers =
            sourceCallStackPointers(Func, Arch::X64, Budget);
        std::vector<MedVar> Values;
        for (const auto &[Call, Stack] : StackPointers)
          Values.push_back(Stack);
        const auto Offsets =
            entryStackOffsets(Func, Values, Arch::X64, Format, Func.EntryKind);
        size_t I = 0;
        for (const auto &[Call, Stack] : StackPointers) {
          const auto Offset = Offsets[I++];
          int64_t End;
          const auto Extent = outgoingExtent(*Call);
          if (!Offset || *Offset > 0 || !Extent ||
              llvm::AddOverflow(*Offset, *Extent, End))
            return std::nullopt;
          LocalCalls.insert(Call);
          CallOffsets.emplace(Call, *Offset);
          Graph.StackReads.emplace_back(-Func.FrameSize, End);
        }
      }
      // Omitted bodies have native register operands but no recovered source
      // CallInfos. Recover their required stack operands with the ABI owner's
      // reaching-store proof. Batch address tracing once for this body, and
      // reuse its block index across all calls under the enclosing work limit.
      std::optional<MedBlockIdIndex> NativeBlocks;
      std::map<ValueKey, std::optional<int64_t>> NativeAddresses;
      const auto Layout = TRI.integerArgumentLayout(Format);
      if (!IsFrame && Func.CallInfos.empty() &&
          llvm::any_of(LocalCalls, [&](const MedOp *Call) {
            return Call->CalleeStackArgs > int(Layout.Registers.size());
          })) {
        NativeBlocks.emplace(Func);
        std::vector<MedVar> Addresses;
        for (const auto &Block : Func.Blocks)
          for (const auto &Op : Block.Ops) {
            if (!Budget)
              return std::nullopt;
            --Budget;
            if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2 &&
                Op.Inputs[0].Size == TRI.PointerSize)
              Addresses.push_back(Op.Inputs[0]);
          }
        const auto Offsets = entryStackOffsets(Func, Addresses, Arch::X64,
                                               Format, Func.EntryKind);
        for (size_t I = 0; I < Addresses.size(); ++I) {
          auto [At, Added] =
              NativeAddresses.emplace(keyOf(Addresses[I]), Offsets[I]);
          if (!Added && At->second != Offsets[I])
            At->second.reset();
        }
      }
      // Recovery may already have replaced an incoming stack load with Param.
      // Its original storage is still a reader when proving the SP footprint.
      // Account for the physical slot even when no LOAD remains in this body.
      const auto NoteStackParameter = [&](const MedVar &Value) {
        const auto Layout = TRI.integerArgumentLayout(Format);
        if (Value.Kind != MedVar::Param || Value.Id < 0 ||
            unsigned(Value.Id) < Layout.Registers.size())
          return true;
        if (unsigned(Value.Id) >= Func.Params.size() ||
            unsigned(Value.Id) >= limits::ir::kMaxSourceABIParameters ||
            Value.RegOff != kNoParamReg || !Value.Size ||
            Value.Size > Layout.SlotBytes)
          return false;
        const int64_t Offset =
            Layout.EntryStackBase +
            (Value.Id - Layout.Registers.size()) * Layout.SlotBytes;
        Graph.StackReads.emplace_back(Offset, Offset + Value.Size);
        return true;
      };
      for (const MedBlock &Block : Func.Blocks) {
        if (!Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty())
          return std::nullopt;
        for (const PhiNode &Phi : Block.Phis) {
          if (!Budget || Phi.ExceptionalEntry)
            return std::nullopt;
          --Budget;
          for (const auto &[Pred, Value] : Phi.Args) {
            if (!Budget || !NoteStackParameter(Value))
              return std::nullopt;
            --Budget;
            Uses[keyOf(Value)].push_back({nullptr, &Phi});
          }
        }
        for (size_t I = 0; I < Block.Ops.size(); ++I) {
          if (!Budget)
            return std::nullopt;
          --Budget;
          const MedOp &Op = Block.Ops[I];
          if (Op.Dead)
            continue;
          const auto Position = std::pair{Block.Id, static_cast<int>(I)};
          auto At = Calls.find(Position);
          const MedCallInfo *Call = At == Calls.end() ? nullptr : At->second;
          const bool IsCall =
              Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL;
          if (Call && !IsCall)
            return std::nullopt;
          if (IsCall) {
            const auto Count = argumentCount(Op);
            if (!LocalCalls.count(&Op) || Op.SourceCallHint || !Count ||
                Op.CalleeVectorArgs != 0 || Op.CalleeVectorSlots ||
                Op.IsTailCall || Op.DoesNotReturn)
              return std::nullopt;
            // Native operands own the register prefix. Missing positional
            // registers stay unspecified and must pass the implicit-register
            // reader checks below; never recover them from stale GPR values.
            if (!Call && !IsFrame && Func.CallInfos.empty() &&
                Op.Opcode == NdOp::CALL && Op.NumInputs &&
                Op.Inputs[0].isConst() &&
                Op.NumInputs == unsigned(Op.CalleeRegisterArgs) + 1) {
              auto &Native = Graph.NativeCalls[Position];
              Native.BlockId = Block.Id;
              Native.OpIdx = static_cast<int>(I);
              Native.TargetAddr = Op.Inputs[0].ConstVal;
              Native.Args.assign(Op.Inputs.begin() + 1,
                                 Op.Inputs.begin() + Op.NumInputs);
              Native.Args.resize(
                  std::min<size_t>(*Count, Layout.Registers.size()),
                  MedVar::makeUnspecified(TRI.PointerSize, Arch::X64));
              if (*Count > Layout.Registers.size()) {
                const auto Stack = CallOffsets.find(&Op);
                if (!NativeBlocks || Stack == CallOffsets.end())
                  return std::nullopt;
                const auto Relative =
                    [&](const MedVar &Address) -> std::optional<int64_t> {
                  const auto At = NativeAddresses.find(keyOf(Address));
                  int64_t Offset;
                  if (Address.Size != TRI.PointerSize ||
                      At == NativeAddresses.end() || !At->second ||
                      llvm::SubOverflow(*At->second, Stack->second, Offset))
                    return std::nullopt;
                  return Offset;
                };
                std::vector<bool> FoundMask, FromStackScan;
                AbiCallContext Context{
                    Func,
                    Block,
                    static_cast<int>(I),
                    Native,
                    TRI,
                    Arch::X64,
                    Layout,
                    Native.Args,
                    FoundMask,
                    FromStackScan,
                    static_cast<int>(*Count - Layout.Registers.size()),
                    Relative,
                    &*NativeBlocks};
                const auto Stores =
                    callSetupStackStores(Context, nullptr, &Budget);
                while (Native.Args.size() < *Count) {
                  const auto At = Stores.find(
                      Layout.CallStackBase +
                      (Native.Args.size() - Layout.Registers.size()) *
                          Layout.SlotBytes);
                  if (At == Stores.end())
                    return std::nullopt;
                  Native.Args.push_back(At->second);
                }
              }
              Call = &Native;
            }
            if (!Call || Call->SourceCallHint ||
                Call->ArgumentsInCalleeRegisterOrder ||
                Call->Args.size() != *Count || !Op.NumInputs ||
                (Op.Opcode == NdOp::CALL &&
                 (Call->IsIndirect || !Op.Inputs[0].isConst() ||
                  Call->TargetAddr != Op.Inputs[0].ConstVal)))
              return std::nullopt;
            Invocations.emplace_back(&Op, Call);
          }
          if (!IsFrame)
            for (unsigned K = 0; K < Op.NumInputs; ++K) {
              const MedVar &Input = Op.Inputs[K];
              if (Input.Kind == MedVar::Reg && Input.SSAVer == 0 &&
                  !TRI.isGeneralReg(Input.RegOff) &&
                  !TRI.isFlag(Input.RegOff, Input.Size))
                return std::nullopt;
            }
          for (unsigned K = 0; K < Op.NumInputs; ++K) {
            if (!Budget || !NoteStackParameter(Op.Inputs[K]))
              return std::nullopt;
            --Budget;
            if (K && Call && K <= Call->Args.size() &&
                Op.Inputs[K] == Call->Args[K - 1])
              continue;
            Uses[keyOf(Op.Inputs[K])].push_back({&Op, nullptr, nullptr, K});
          }
          if (Call)
            for (unsigned K = 0; K < Call->Args.size(); ++K) {
              if (!Budget || !NoteStackParameter(Call->Args[K]))
                return std::nullopt;
              --Budget;
              Uses[keyOf(Call->Args[K])].push_back({&Op, nullptr, Call, K});
            }
        }
      }
      for (const MedCallClobber &Clobber : Func.CallClobbers) {
        if (!Budget)
          return std::nullopt;
        --Budget;
        Uses[keyOf(Clobber.PreservedInput)].push_back({});
      }
      if (Invocations.size() != LocalCalls.size() ||
          (!Func.CallInfos.empty() &&
           Invocations.size() != Func.CallInfos.size()))
        return std::nullopt;
      for (const auto &[Op, Call] : Invocations) {
        const auto Callee = Callees.find(Call->TargetAddr);
        if (Op->Opcode == NdOp::INDIR_CALL || Call->IsIndirect ||
            Callee == Callees.end()) {
          if (!IsFrame || !EmptyPrefixCalls.count(Op) || !Call->Args.empty())
            return std::nullopt;
          OpaquePrefixCalls.push_back(Op);
          continue;
        }
        const auto &Body = *Callee->second;
        const auto Layout = TRI.integerArgumentLayout(Format);
        if (Body.Entry != Call->TargetAddr ||
            Body.Params.size() > Call->Args.size())
          return std::nullopt;
        const auto Extent = outgoingExtent(*Op);
        if (!Extent)
          return std::nullopt;
        const int64_t EntryExtent =
            *Extent + Layout.EntryStackBase - Layout.CallStackBase;
        const auto StackReads = inputRegister(Body, TRI.StackPointer);
        if (!StackReads || llvm::any_of(*StackReads, [&](const Range &Read) {
              return Read.second > EntryExtent;
            }))
          return std::nullopt;
        // Every implicit family must ignore a possible borrowed pointer.
        // An inferred argument count or a may-write mask cannot prove that.
        for (unsigned Family = 0; Family < kX64VectorFamilyBase; ++Family) {
          const uint64_t Register = Family * TRI.PointerSize;
          const int Index = Layout.registerIndex(Register);
          if (Register == TRI.StackPointer ||
              (Index >= 0 && Index < Op->CalleeRegisterArgs))
            continue;
          const auto Implicit = inputRegister(Body, Register);
          if (!Implicit || !Implicit->empty())
            return std::nullopt;
        }
      }
      for (const MedOp *Call : OpaquePrefixCalls)
        if (Call->Output.Size && !Uses[keyOf(Call->Output)].empty())
          return std::nullopt;
      Graph.Ready = true;
    }

    std::vector<std::pair<MedVar, int64_t>> Work;
    for (const MedVar &Root : Roots)
      Work.emplace_back(Root, 0);
    std::map<ValueKey, int64_t> Seen;
    Ranges Reads = IsStack ? Graph.StackReads : Ranges{};
    while (!Work.empty()) {
      const auto [Value, Offset] = Work.back();
      Work.pop_back();
      if (!Budget || Value.Size != TRI.PointerSize)
        return std::nullopt;
      --Budget;
      const auto [At, New] = Seen.emplace(keyOf(Value), Offset);
      if (!New) {
        if (At->second != Offset)
          return std::nullopt;
        continue;
      }
      for (const Use &By : Uses[keyOf(Value)]) {
        if (!Budget)
          return std::nullopt;
        --Budget;
        if (By.Phi) {
          Work.emplace_back(By.Phi->Output, Offset);
          continue;
        }
        if (!By.Op)
          return std::nullopt;
        const MedOp &Op = *By.Op;
        if (By.Call) {
          const auto Borrowed = parameter(*By.Call, By.Input);
          if (!Borrowed || Borrowed->size() > Budget)
            return std::nullopt;
          Budget -= Borrowed->size();
          for (const auto &[Begin, End] : *Borrowed) {
            int64_t First, Last;
            if (llvm::AddOverflow(Offset, Begin, First) ||
                llvm::AddOverflow(Offset, End, Last))
              return std::nullopt;
            Reads.emplace_back(First, Last);
            if (IsFrame)
              ReadSites[By.Op].emplace_back(First, Last);
          }
          continue;
        }
        if (Op.MemoryOrdering != NdMemoryOrdering::None ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          return std::nullopt;
        if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
            By.Input == 0) {
          if (Op.Opcode == NdOp::LOAD) {
            int64_t End;
            if (!Op.Output.Size ||
                llvm::AddOverflow(Offset, int64_t(Op.Output.Size), End))
              return std::nullopt;
            Reads.emplace_back(Offset, End);
            if (IsFrame)
              ReadSites[By.Op].emplace_back(Offset, End);
          }
          continue;
        }
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
          Work.emplace_back(Op.Output, Offset);
          continue;
        }
        if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
            Op.NumInputs == 2 && Op.Inputs[1 - By.Input].isConst() &&
            (Op.Opcode == NdOp::INT_ADD || By.Input == 0)) {
          const int64_t Delta =
              static_cast<int64_t>(Op.Inputs[1 - By.Input].ConstVal);
          int64_t Next;
          const bool Overflow = Op.Opcode == NdOp::INT_ADD
                                    ? llvm::AddOverflow(Offset, Delta, Next)
                                    : llvm::SubOverflow(Offset, Delta, Next);
          if (Overflow)
            return std::nullopt;
          Work.emplace_back(Op.Output, Next);
          continue;
        }
        // Other integer transforms, including pointer comparisons and lossy
        // slices, do not supply a bounded noncapture proof here.
        return std::nullopt;
      }
    }
    // Queries below use disjoint, ordered intervals. Charging the walk alone
    // would still permit a quadratic store-by-read overlap scan afterwards.
    llvm::sort(Reads);
    Ranges Merged;
    for (const auto &Read : Reads) {
      if (Read.first >= Read.second)
        continue;
      if (!Merged.empty() && Merged.back().second >= Read.first)
        Merged.back().second = std::max(Merged.back().second, Read.second);
      else
        Merged.push_back(Read);
    }
    return Merged;
  }
};

} // namespace neverd::detail

#endif // NEVERD_IR_MED_PASS_MEDFRAMEBORROWREADS_H
