//===- PipelineGoResults.cpp - Go register ABI direct call results --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Returns, from every direct call of a Go function its callers read past
/// RAX, the same results, and gives the function that record.
///
//===----------------------------------------------------------------------===//

#include "PipelineReturnModelingDetail.h"

#include "neverd/ir/CallRegisterContract.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/GoCallContract.h"
#include "neverd/ir/med/MedGoResults.h"
#include "neverd/ir/med/MedReturnValue.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

namespace {

using ValueKey = std::tuple<MedVar::VarKind, int, int>;

ValueKey identity(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }

/// The copy of a register's entry value into itself that conversion puts at
/// the entry.
bool entryCopy(const MedOp &Op) {
  return Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
         identity(Op.Inputs[0]) == identity(Op.Output);
}

} // namespace

void materializeGoCallResults(const BinaryImage &Img, PipelineResult &Result) {
  // Go's internal ABI returns a call's integer results in RAX, RBX, RCX,
  // RDI, RSI and R8-R11 and keeps none of those registers but its results
  // (goCallEntersInternalABI): a caller's read of one after the call is a
  // result, and so is every register before it.  A function returns the same
  // record to each of its calls, so every direct call of it returns the most
  // any caller reads, and the function returns that record too.  A function
  // whose return hands back what a call it makes returns (`CALL g; RET`)
  // reads those results of that call.
  const auto &Go = Img.ExceptionMetadata.GoModule;
  if ((Img.Arch != Arch::X64 && Img.Arch != Arch::AArch64) || !Go ||
      !Go->RegisterArguments)
    return;
  const Arch A = Img.Arch;
  const llvm::ArrayRef<uint64_t> Results = goIntegerArgumentRegisters(A);
  const std::map<va_t, GPRReadWidths> &Reads = Result.CallEntryReadGPRs.get();
  std::vector<MedFunc> &Funcs = Result.MedFuncs;
  const size_t N = Funcs.size();
  std::vector<uint64_t> Weights(N);
  for (size_t I = 0; I < N; ++I)
    for (const MedBlock &Block : Funcs[I].Blocks)
      Weights[I] += Block.Ops.size() + Block.Phis.size() + 1;
  // The direct calls of internal-ABI Go functions each function makes, and
  // its indirect calls through a function value, by call site, and the last
  // result each reads.  Each function is its own.
  std::vector<std::map<uint32_t, va_t>> Calls(N);
  std::vector<std::map<uint32_t, GoCallShape>> FirstReads(N);
  parallelForEachWeighted(Weights, [&](auto Claim, size_t Count) {
    for (size_t I; (I = Claim()) < Count;) {
      const MedFunc &Func = Funcs[I];
      const bool Compiled = goCallEntersInternalABI(Img, Func.Entry, InvalidVA,
                                                    /*CalleeReads=*/nullptr);
      std::set<uint32_t> Sites;
      for (const MedBlock &Block : Func.Blocks)
        for (const MedOp &Op : Block.Ops) {
          if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
              Op.SourceCallHint || Op.DoesNotReturn ||
              (Op.RegisterContract != CallRegisterContract::GoCode &&
               Op.RegisterContract != CallRegisterContract::GoInternal))
            continue;
          const bool Direct =
              Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst();
          if (!Direct) {
            if (Compiled && Op.Opcode == NdOp::INDIR_CALL) {
              Calls[I].emplace(Op.CallSiteId, InvalidVA);
              Sites.insert(Op.CallSiteId);
            }
            continue;
          }
          const va_t Callee = Op.Inputs[0].ConstVal;
          const auto Summary = Reads.find(Callee);
          if (!goCallEntersInternalABI(
                  Img, Func.Entry, Callee,
                  Summary == Reads.end() ? nullptr : &Summary->second))
            continue;
          // A call another rule gave other result registers keeps them.
          const bool ModelledAlone = Op.Output.Kind == MedVar::Reg &&
                                     Op.Output.RegOff == Results[0] &&
                                     !Op.RegisterReturnFields;
          const bool GoFields =
              Op.RegisterReturnFields &&
              llvm::all_of(
                  *Op.RegisterReturnFields, [&](const MedReturnReg &F) {
                    return !F.IsFP && goResultPosition(A, F.RegOff) >= 0;
                  });
          if (ModelledAlone || GoFields) {
            Calls[I].emplace(Op.CallSiteId, Callee);
            Sites.insert(Op.CallSiteId);
          }
        }
      if (!Sites.empty())
        FirstReads[I] = goCallResultsRead(Func, A, Sites);
    }
  });
  if (llvm::all_of(Calls, [](const auto &Sites) { return Sites.empty(); }))
    return;
  std::map<va_t, size_t> ByEntry;
  for (size_t I = 0; I < N; ++I)
    ByEntry.emplace(Funcs[I].Entry, I);

  // The results each function's callers read, and each indirect call's: the
  // last, and how much of each.  A function returns as much of each result
  // as any caller reads.
  const uint16_t Word = getTargetRegInfo(A).PointerSize;
  std::map<va_t, GoCallShape> Callee;
  std::map<std::pair<size_t, uint32_t>, GoCallShape> Indirect;
  const auto Merge = [](GoCallShape &Known, const GoCallShape &Read) {
    if (Known.Widths.size() < Read.Widths.size())
      Known.Widths.resize(Read.Widths.size(), 0);
    for (size_t Position = 0; Position < Read.Widths.size(); ++Position)
      Known.Widths[Position] =
          std::max(Known.Widths[Position], Read.Widths[Position]);
    if (Read.Last <= Known.Last)
      return false;
    Known.Last = Read.Last;
    return true;
  };
  const auto Raise = [&](size_t I,
                         const std::map<uint32_t, GoCallShape> &Read) {
    bool Grew = false;
    for (const auto &[Site, Shape] : Read) {
      const va_t Target = Calls[I].at(Site);
      Grew |= Merge(Target == InvalidVA ? Indirect[{I, Site}] : Callee[Target],
                    Shape);
    }
    return Grew;
  };
  for (size_t I = 0; I < N; ++I)
    Raise(I, FirstReads[I]);
  FirstReads.clear();
  // A function returning results reads what its returns hand back.
  std::vector<std::vector<MedVar>> Returned(N);
  std::vector<llvm::DenseMap<int, const MedBlock *>> Blocks(N);
  const auto BlocksOf =
      [&](size_t I) -> const llvm::DenseMap<int, const MedBlock *> & {
    if (Blocks[I].empty())
      for (const MedBlock &Block : Funcs[I].Blocks)
        Blocks[I][Block.Id] = &Block;
    return Blocks[I];
  };
  const auto Gather = [&](size_t I) {
    std::set<uint32_t> Sites;
    for (const auto &[Site, Target] : Calls[I])
      Sites.insert(Site);
    return Raise(I, goCallResultsRead(Funcs[I], A, Sites, Returned[I]));
  };
  for (bool Grew = true; Grew;) {
    Grew = false;
    for (const auto &[Entry, Shape] : std::map<va_t, GoCallShape>(Callee)) {
      const int Last = Shape.Last;
      const auto Found = ByEntry.find(Entry);
      if (Found == ByEntry.end() || Last <= 0)
        continue;
      const size_t I = Found->second;
      const auto &ById = BlocksOf(I);
      std::vector<MedVar> &Values = Returned[I];
      const size_t Before = Values.size();
      for (const MedBlock &Block : Funcs[I].Blocks) {
        if (Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::RETURN)
          continue;
        for (int Position = 1; Position <= Last; ++Position) {
          const GoReturnedValue Value =
              goReturnedValue(ById, Block, Results[Position], A);
          if (Value.What == GoReturnedValue::Phi) {
            if (llvm::none_of(Values, [&](const MedVar &V) {
                  return identity(V) == identity(*Value.Value);
                }))
              Values.push_back(*Value.Value);
            continue;
          }
          if (Value.What != GoReturnedValue::Call)
            continue;
          const auto Site = Calls[I].find(Value.Caller->CallSiteId);
          if (Site == Calls[I].end())
            continue;
          // The call's result is the function's, as much of it as any of
          // the function's callers may read: all of it.
          GoCallShape Forwarded;
          Forwarded.Last = Position;
          Forwarded.Widths.assign(Position + 1, 0);
          Forwarded.Widths[Position] = Word;
          Grew |= Merge(Site->second == InvalidVA ? Indirect[{I, Site->first}]
                                                  : Callee[Site->second],
                        Forwarded);
        }
      }
      if (Values.size() != Before)
        Grew |= Gather(I);
    }
  }

  // A function returns no more results than each of its returns supplies:
  // the register's newest write, a parameter the function leaves unchanged,
  // or what a call it makes returns there, directly or through PHIs.  A read
  // its callers make past that reads a register no return defines on some
  // path, which the caller's own read then shows; a result the function
  // left undefined would fail every call of it instead.  Results are a
  // prefix from RAX, so one RAX misses leaves none.  Shrinking one function
  // can shrink a function that returns its call's results, so the counts
  // settle from above.
  const auto ShapeOf = [&](size_t I, uint32_t Site) -> const GoCallShape * {
    const auto Found = Calls[I].find(Site);
    if (Found == Calls[I].end())
      return nullptr;
    if (Found->second == InvalidVA) {
      const auto Shape = Indirect.find({I, Site});
      return Shape == Indirect.end() ? nullptr : &Shape->second;
    }
    const auto Shape = Callee.find(Found->second);
    return Shape == Callee.end() ? nullptr : &Shape->second;
  };
  const auto CallResults = [&](size_t I, uint32_t Site) {
    const GoCallShape *Shape = ShapeOf(I, Site);
    return Shape ? std::max(Shape->Last, 0) : 0;
  };
  struct Definitions {
    llvm::DenseMap<ValueKey, const MedOp *> Ops;
    llvm::DenseMap<ValueKey, const PhiNode *> Phis;
    llvm::DenseMap<ValueKey, uint32_t> Clobbered;
  };
  std::map<size_t, Definitions> Defs;
  const auto DefinitionsOf = [&](size_t I) -> const Definitions & {
    auto [It, New] = Defs.try_emplace(I);
    if (New) {
      for (const MedBlock &Block : Funcs[I].Blocks) {
        for (const PhiNode &Phi : Block.Phis)
          It->second.Phis.try_emplace(identity(Phi.Output), &Phi);
        for (const MedOp &Op : Block.Ops)
          if (Op.Output.Size)
            It->second.Ops.try_emplace(identity(Op.Output), &Op);
      }
      for (const MedCallClobber &Clobber : Funcs[I].CallClobbers)
        It->second.Clobbered.try_emplace(identity(Clobber.Value),
                                         Clobber.CallSiteId);
    }
    return It->second;
  };
  const auto Supplied = [&](size_t I, int Position) {
    const MedFunc &Func = Funcs[I];
    const uint64_t Register = Results[Position];
    const bool Parameter = llvm::any_of(Func.Params, [&](const MedVar &P) {
      return P.Kind == MedVar::Param && P.RegOff == Register;
    });
    const Definitions &Def = DefinitionsOf(I);
    std::set<ValueKey> Seen;
    std::function<bool(const MedVar &)> Value = [&](const MedVar &V) {
      const ValueKey Key = identity(V);
      if (!Seen.insert(Key).second)
        return true;
      if (const auto Site = Def.Clobbered.find(Key);
          Site != Def.Clobbered.end())
        return CallResults(I, Site->second) >= Position;
      if (const auto Phi = Def.Phis.find(Key); Phi != Def.Phis.end())
        return llvm::all_of(Phi->second->Args,
                            [&](const auto &Arg) { return Value(Arg.second); });
      if (const auto Op = Def.Ops.find(Key); Op != Def.Ops.end())
        return !entryCopy(*Op->second) || Parameter;
      return V.Kind != MedVar::Reg || Parameter;
    };
    const auto &ById = BlocksOf(I);
    for (const MedBlock &Block : Func.Blocks) {
      if (Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::RETURN)
        continue;
      const GoReturnedValue Returned =
          goReturnedValue(ById, Block, Register, A);
      bool Ok = false;
      switch (Returned.What) {
      case GoReturnedValue::Defined:
      case GoReturnedValue::Phi:
        Ok = Value(*Returned.Value);
        break;
      case GoReturnedValue::Call:
        Ok = CallResults(I, Returned.Caller->CallSiteId) >= Position;
        break;
      case GoReturnedValue::Entry:
        Ok = Parameter;
        break;
      case GoReturnedValue::Unknown:
        break;
      }
      if (!Ok)
        return false;
    }
    return true;
  };
  for (bool Shrank = true; Shrank;) {
    Shrank = false;
    for (auto &[Entry, Shape] : Callee) {
      const auto Found = ByEntry.find(Entry);
      if (Found == ByEntry.end() || Shape.Last <= 0)
        continue;
      int Supply = -1;
      while (Supply < Shape.Last && Supplied(Found->second, Supply + 1))
        ++Supply;
      if (Supply < Shape.Last) {
        Shape.Last = std::max(Supply, 0);
        Shrank = true;
      }
    }
  }
  Defs.clear();
  Blocks.clear();

  // The shape a call returns: its results through the last, RAX whole, each
  // other as wide as a reader takes it, or a word where none does.
  const auto Settled = [&](const GoCallShape &Shape) {
    GoCallShape Result;
    Result.Last = Shape.Last;
    for (int Position = 0; Position <= Shape.Last; ++Position) {
      const uint16_t Width =
          Position && static_cast<size_t>(Position) < Shape.Widths.size()
              ? Shape.Widths[Position]
              : 0;
      Result.Widths.push_back(Width ? Width : Word);
    }
    return Result;
  };
  // Each function's calls return what their callees do, rewritten in the
  // function alone.
  parallelForEachWeighted(Weights, [&](auto Claim, size_t Count) {
    for (size_t I; (I = Claim()) < Count;) {
      std::map<uint32_t, GoCallShape> Shapes;
      for (const auto &[Site, Target] : Calls[I])
        if (const GoCallShape *Shape = ShapeOf(I, Site);
            Shape && Shape->Last > 0)
          Shapes.emplace(Site, Settled(*Shape));
      if (!Shapes.empty())
        defineGoCallResults(Funcs[I], A, Shapes);
    }
  });
  for (const auto &[Entry, Shape] : Callee) {
    const auto Found = ByEntry.find(Entry);
    if (Found == ByEntry.end() || Shape.Last <= 0)
      continue;
    const GoCallShape Fields = Settled(Shape);
    std::vector<MedReturnReg> Return;
    for (int Position = 0; Position <= Fields.Last; ++Position)
      Return.push_back({Results[Position], Fields.Widths[Position], false});
    Funcs[Found->second].MultiReturn = std::move(Return);
  }
}

} // namespace neverd
