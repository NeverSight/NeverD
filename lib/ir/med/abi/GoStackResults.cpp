//===- GoStackResults.cpp - Results a Go function returns on the stack ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Returns the results a Go stack-ABI function stores in its caller's frame
/// as the function's value, and stores them there again after each call.
///
//===----------------------------------------------------------------------===//

#include "../pass/MedEntryStackOffsets.h"
#include "MedABIPassDetail.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/GoCallContract.h"
#include "neverd/ir/med/MedCallingConvDetail.h"
#include "neverd/ir/med/MedGoResults.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <tuple>
#include <utility>

namespace neverd {

namespace {

/// The operations an address threads through unchanged on its way to its
/// use, as detectStackParams follows them.
bool carriesAddress(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::SUBBYTES:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return true;
  default:
    return false;
  }
}

/// The next unused value id and SSA version of \p Func.
struct FreshValues {
  int Id = 1;
  int Version = 1;

  explicit FreshValues(const MedFunc &Func) {
    const auto Bump = [&](const MedVar &V) {
      Id = std::max(Id, V.Id + 1);
      Version = std::max(Version, V.SSAVer + 1);
    };
    for (const MedBlock &Block : Func.Blocks) {
      for (const MedOp &Op : Block.Ops) {
        Bump(Op.Output);
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          Bump(Op.Inputs[I]);
      }
      for (const PhiNode &Phi : Block.Phis) {
        Bump(Phi.Output);
        for (const auto &[Pred, Input] : Phi.Args)
          Bump(Input);
      }
    }
    // HighIR keys a value by its id and version alone: one no operation
    // names any more, a parameter or what a call left, keeps them too.
    for (const MedVar &Param : Func.Params)
      Bump(Param);
    for (const MedCallClobber &Clobber : Func.CallClobbers) {
      Bump(Clobber.Value);
      Bump(Clobber.PreservedInput);
    }
  }

  MedVar temporary(Arch A, uint16_t Size) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.TheArch = A;
    V.Id = Id++;
    V.SSAVer = 0;
    V.Size = Size;
    return V;
  }

  MedVar registerValue(Arch A, uint64_t RegOff, uint16_t Size) {
    MedVar V;
    V.Kind = MedVar::Reg;
    V.TheArch = A;
    V.RegOff = RegOff;
    V.Id = Id++;
    V.SSAVer = Version++;
    V.Size = Size;
    return V;
  }
};

/// The address \p Displacement bytes past \p Base, computed by \p Ops.
MedVar addressAt(const MedVar &Base, int64_t Displacement, va_t Addr, Arch A,
                 FreshValues &Fresh, std::vector<MedOp> &Ops) {
  if (!Displacement)
    return Base;
  MedOp Add;
  Add.Opcode = NdOp::INT_ADD;
  Add.Addr = Addr;
  Add.Output = Fresh.temporary(A, Base.Size);
  Add.addInput(Base);
  Add.addInput(
      MedVar::makeConst(static_cast<uint64_t>(Displacement), Base.Size));
  Ops.push_back(Add);
  return Ops.back().Output;
}

bool isCall(const MedOp &Op) {
  return Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL;
}

/// The words of its caller's frame a stack-ABI function takes its arguments
/// and returns its results in: \p Bytes of them past its return address.
class ArgumentWords {
public:
  ArgumentWords(const MedFunc &Func, Arch A, BinaryFormat F, uint32_t Bytes)
      : Offsets(Func, A, F), Base(goStackArgumentLayout(A).EntryStackBase),
        Bytes(Bytes) {}

  /// The byte of those words \p Address names, if any.
  std::optional<uint32_t> at(const MedVar &Address) {
    if (Address.Kind != MedVar::Temp && Address.Kind != MedVar::Reg)
      return std::nullopt;
    const std::optional<int64_t> Offset = Offsets.query(Address);
    if (!Offset || *Offset < Base || *Offset - Base >= Bytes)
      return std::nullopt;
    return static_cast<uint32_t>(*Offset - Base);
  }

  /// The byte \p Op stores at and how many it stores, if it stores into
  /// those words.
  std::optional<std::pair<uint32_t, uint32_t>> stored(const MedOp &Op) {
    if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2 ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return std::nullopt;
    const std::optional<uint32_t> At = at(Op.Inputs[0]);
    if (!At)
      return std::nullopt;
    return std::pair(
        *At, std::min<uint32_t>(std::max<uint32_t>(Op.Inputs[1].Size, 1),
                                Bytes - *At));
  }

  /// The lowest byte of those words whose address \p Op uses as a value,
  /// which a callee may write every word from, if it uses any.
  std::optional<uint32_t> handedOn(const MedOp &Op) {
    if (carriesAddress(Op.Opcode))
      return std::nullopt;
    const std::optional<med_calling_conv_detail::StackMemoryRead> Read =
        med_calling_conv_detail::stackMemoryRead(Op);
    std::optional<uint32_t> Lowest;
    for (uint8_t K = 0; K < Op.NumInputs; ++K) {
      if (((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) && K == 0) ||
          (Read && K == Read->AddressInput))
        continue;
      if (const std::optional<uint32_t> At = at(Op.Inputs[K]))
        Lowest = Lowest ? std::min(*Lowest, *At) : *At;
    }
    return Lowest;
  }

private:
  MedEntryStackOffsets Offsets;
  int64_t Base;
  uint32_t Bytes;
};

} // namespace

GoStackFrameUse goStackFrameUse(const MedFunc &Func, Arch A, BinaryFormat F,
                                std::optional<uint32_t> FrameBytes) {
  GoStackFrameUse Use;
  if (Func.Blocks.empty())
    return Use;
  const uint32_t Slot = goStackArgumentLayout(A).SlotBytes;
  // A record of no bytes says no more than none: assembly declared `$0-0`
  // returns a result all the same (`runtime.cputicks` stores `ret+0(FP)`).
  Use.Bytes = FrameBytes && *FrameBytes ? *FrameBytes
                                        : limits::ir::kGoUnrecordedFrameBytes;
  const uint32_t Bytes = Use.Bytes;
  if (!Bytes || !Slot)
    return Use;
  ArgumentWords Words(Func, A, F, Bytes);
  // Each block's uses of those words, in order: a read, a store, or the
  // address handed on, from which a callee may write every word above.
  struct Event {
    enum : uint8_t { Read, Store, Address } What;
    uint32_t Offset;
    uint32_t Size;
  };
  llvm::DenseMap<int, size_t> Index;
  for (size_t I = 0; I < Func.Blocks.size(); ++I)
    Index[Func.Blocks[I].Id] = I;
  std::vector<std::vector<Event>> Events(Func.Blocks.size());
  for (size_t I = 0; I < Func.Blocks.size(); ++I)
    for (const MedOp &Op : Func.Blocks[I].Ops) {
      const std::optional<med_calling_conv_detail::StackMemoryRead> Read =
          med_calling_conv_detail::stackMemoryRead(Op);
      if (Read)
        if (const std::optional<uint32_t> At =
                Words.at(Op.Inputs[Read->AddressInput]))
          Events[I].push_back(
              {Event::Read, *At, std::min<uint32_t>(Read->Bytes, Bytes - *At)});
      // A word detectStackParams found no write of reads as the parameter.
      for (uint8_t K = 0; K < Op.NumInputs; ++K)
        if (const MedVar &In = Op.Inputs[K];
            In.Kind == MedVar::Param && In.RegOff == kNoParamReg &&
            In.Id >= 0 && static_cast<uint64_t>(In.Id) * Slot < Bytes) {
          const uint32_t At = static_cast<uint32_t>(In.Id) * Slot;
          Events[I].push_back(
              {Event::Read, At, std::min<uint32_t>(Slot, Bytes - At)});
        }
      if (const std::optional<uint32_t> At = Words.handedOn(Op))
        Events[I].push_back({Event::Address, *At, Bytes - *At});
      if (const auto Store = Words.stored(Op)) {
        Events[I].push_back({Event::Store, Store->first, Store->second});
        Use.Stored.emplace_back(Store->first,
                                static_cast<uint16_t>(Store->second));
      }
    }
  llvm::sort(Use.Stored);
  Use.Stored.erase(std::unique(Use.Stored.begin(), Use.Stored.end()),
                   Use.Stored.end());

  // The bytes every path to a block's end has written: none at the entry,
  // and at a join those all its predecessors have.
  std::vector<llvm::BitVector> Out(Func.Blocks.size(),
                                   llvm::BitVector(Bytes, true));
  std::vector<llvm::BitVector> Gen(Func.Blocks.size(), llvm::BitVector(Bytes));
  for (size_t I = 0; I < Func.Blocks.size(); ++I)
    for (const Event &E : Events[I])
      if (E.What != Event::Read)
        Gen[I].set(E.Offset, E.Offset + E.Size);
  const auto In = [&](size_t I) {
    llvm::BitVector State(Bytes, I != 0);
    if (I != 0)
      for (const int Pred : Func.Blocks[I].Preds)
        if (const auto Found = Index.find(Pred); Found != Index.end())
          State &= Out[Found->second];
    return State;
  };
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (size_t I = 0; I < Func.Blocks.size(); ++I) {
      llvm::BitVector State = In(I);
      State |= Gen[I];
      if (State != Out[I]) {
        Out[I] = std::move(State);
        Changed = true;
      }
    }
  }
  Use.Exposed.resize(Bytes);
  for (size_t I = 0; I < Func.Blocks.size(); ++I) {
    const MedBlock &Block = Func.Blocks[I];
    // A block no other leads to is entered by the runtime, as the return a
    // recovered panic resumes at is: the function has run, and its words
    // hold what it stored (the all-written state no predecessor narrows).
    llvm::BitVector State = In(I);
    for (const Event &E : Events[I]) {
      if (E.What != Event::Read) {
        State.set(E.Offset, E.Offset + E.Size);
        continue;
      }
      for (uint32_t Byte = E.Offset; Byte < E.Offset + E.Size; ++Byte)
        if (!State.test(Byte))
          Use.Exposed.set(Byte);
    }
    if (Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::RETURN)
      continue;
    GoStackFrameUse::Return Return;
    Return.Block = I;
    if (Block.Ops.size() >= 2) {
      const MedOp &Before = Block.Ops[Block.Ops.size() - 2];
      if (isCall(Before) && Before.IsTailCall &&
          Before.Addr == Block.Ops.back().Addr) {
        Return.Tail = true;
        if (Before.Opcode == NdOp::CALL && Before.NumInputs &&
            Before.Inputs[0].isConst())
          Return.Target = Before.Inputs[0].ConstVal;
      }
    }
    Return.Written = std::move(State);
    Use.Returns.push_back(std::move(Return));
  }
  return Use;
}

std::vector<MedGoStackResult> goStackResults(
    const GoStackFrameUse &Use, Arch A,
    llvm::function_ref<bool(const GoStackFrameUse::Return &)> SharesFrame) {
  std::vector<MedGoStackResult> Results;
  const uint32_t Slot = goStackArgumentLayout(A).SlotBytes;
  if (!Use.Bytes || !Slot || Use.Stored.empty())
    return Results;
  // The bytes every return that hands back the function's own frame has
  // written.
  llvm::BitVector Written(Use.Bytes, true);
  bool Returns = false;
  for (const GoStackFrameUse::Return &Return : Use.Returns) {
    if (SharesFrame(Return))
      continue;
    Written &= Return.Written;
    Returns = true;
  }
  if (!Returns)
    return Results;
  // A word some path reads before writing is an argument's.  Of the others,
  // each store's bytes within a word are a result where every return has
  // them: the widest first, so a word zeroed whole and then set in part is
  // one result.
  struct Piece {
    uint32_t Offset;
    uint32_t Size;
  };
  std::vector<Piece> Pieces;
  for (const auto &[Offset, Size] : Use.Stored)
    for (uint32_t At = Offset; At < Offset + Size;) {
      const uint32_t End =
          std::min<uint32_t>(Offset + Size, At - At % Slot + Slot);
      Pieces.push_back({At, End - At});
      At = End;
    }
  llvm::sort(Pieces, [&](const Piece &L, const Piece &R) {
    return std::tuple(L.Offset / Slot, R.Size, L.Offset) <
           std::tuple(R.Offset / Slot, L.Size, R.Offset);
  });
  llvm::BitVector Taken(Use.Bytes);
  for (const Piece &P : Pieces) {
    if (P.Size != 1 && P.Size != 2 && P.Size != 4 && P.Size != 8)
      continue;
    const uint32_t Word = P.Offset - P.Offset % Slot;
    bool Argument = false;
    for (uint32_t Byte = Word; Byte < std::min(Word + Slot, Use.Bytes); ++Byte)
      Argument |= Use.Exposed.test(Byte);
    bool Whole = !Argument;
    for (uint32_t Byte = P.Offset; Whole && Byte < P.Offset + P.Size; ++Byte)
      Whole = Written.test(Byte) && !Taken.test(Byte);
    if (!Whole)
      continue;
    Taken.set(P.Offset, P.Offset + P.Size);
    Results.push_back({P.Offset, static_cast<uint16_t>(P.Size)});
  }
  llvm::sort(Results, [](const MedGoStackResult &L, const MedGoStackResult &R) {
    return L.Offset < R.Offset;
  });
  // Each result takes a result register of Go's internal ABI.  A function
  // with more of them than registers returns none this way.
  if (Results.size() > goIntegerArgumentRegisters(A).size())
    Results.clear();
  return Results;
}

bool returnGoStackResults(MedFunc &Func, Arch A, BinaryFormat F, uint32_t Bytes,
                          llvm::ArrayRef<size_t> Blocks) {
  const llvm::ArrayRef<uint64_t> Registers = goIntegerArgumentRegisters(A);
  const TargetRegInfo &TRI = getTargetRegInfo(A);
  const uint16_t Word = TRI.PointerSize;
  const size_t Count = Func.GoStackResults.size();
  if (!Count || Func.Blocks.empty() || Count > Registers.size())
    return false;
  // The stack pointer the function was entered with: the entry's copy of it
  // into itself.
  std::optional<MedVar> EntryStack;
  for (const MedOp &Op : Func.Blocks.front().Ops) {
    if (Op.Opcode != NdOp::COPY)
      break;
    if (med_calling_conv_detail::isEntryLiveInCopy(Op, TRI.StackPointer)) {
      EntryStack = Op.Output;
      break;
    }
  }
  if (!EntryStack)
    return false;

  // A result one store alone leaves at a return is the value stored: no
  // read of the word is needed, and the store, which nothing then reads, is
  // the dead store it always was.  Each block ends with the store that
  // reaches its end for each result: one, where every path brings the same;
  // none where paths bring different ones, another store overlaps the
  // result, or the function hands the word's address on, since a callee may
  // then write it.
  constexpr int Unknown = -2, Mixed = -1;
  std::vector<MedVar> Values;
  std::vector<std::vector<int>> Out(Func.Blocks.size(),
                                    std::vector<int>(Count, Unknown));
  {
    ArgumentWords Words(Func, A, F, Bytes);
    llvm::DenseMap<int, size_t> Index;
    for (size_t I = 0; I < Func.Blocks.size(); ++I)
      Index[Func.Blocks[I].Id] = I;
    struct Write {
      size_t Result;
      int Value;
    };
    std::vector<std::vector<Write>> Writes(Func.Blocks.size());
    std::vector<bool> HandedOn(Count);
    for (size_t I = 0; I < Func.Blocks.size(); ++I)
      for (const MedOp &Op : Func.Blocks[I].Ops) {
        if (const std::optional<uint32_t> From = Words.handedOn(Op))
          for (size_t R = 0; R < Count; ++R)
            if (*From <
                Func.GoStackResults[R].Offset + Func.GoStackResults[R].Size)
              HandedOn[R] = true;
        const auto Store = Words.stored(Op);
        if (!Store)
          continue;
        for (size_t R = 0; R < Count; ++R) {
          const MedGoStackResult &Result = Func.GoStackResults[R];
          if (Store->first >= Result.Offset + Result.Size ||
              Store->first + Store->second <= Result.Offset)
            continue;
          // The value of a vector register is a float's bits, and the
          // result registers are general ones: its word is read instead,
          // as the integer the caller's frame holds.
          const MedVar &Stored = Op.Inputs[1];
          const bool Exact =
              Store->first == Result.Offset && Store->second == Result.Size &&
              Stored.Size == Result.Size &&
              (Stored.Kind != MedVar::Reg || TRI.isGeneralReg(Stored.RegOff));
          if (Exact)
            Values.push_back(Op.Inputs[1]);
          Writes[I].push_back(
              {R, Exact ? static_cast<int>(Values.size()) - 1 : Mixed});
        }
      }
    const auto In = [&](size_t I, size_t R) {
      if (I == 0)
        return Mixed;
      int State = Unknown;
      for (const int Pred : Func.Blocks[I].Preds) {
        const auto Found = Index.find(Pred);
        if (Found == Index.end())
          return Mixed;
        const int From = Out[Found->second][R];
        if (From == Unknown)
          continue;
        if (State != Unknown && State != From)
          return Mixed;
        State = From;
      }
      return State;
    };
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (size_t I = 0; I < Func.Blocks.size(); ++I)
        for (size_t R = 0; R < Count; ++R) {
          int State = HandedOn[R] ? Mixed : In(I, R);
          if (!HandedOn[R])
            for (const Write &W : Writes[I])
              if (W.Result == R)
                State = W.Value;
          if (State != Out[I][R]) {
            Out[I][R] = State;
            Changed = true;
          }
        }
    }
  }

  const int64_t Base = goStackArgumentLayout(A).EntryStackBase;
  FreshValues Fresh(Func);
  for (const size_t Index : Blocks) {
    MedBlock &Block = Func.Blocks[Index];
    if (Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::RETURN)
      continue;
    const va_t Addr = Block.Ops.back().Addr;
    std::vector<MedOp> Ops;
    std::optional<MedVar> First;
    for (size_t I = 0; I < Count; ++I) {
      const MedGoStackResult &Result = Func.GoStackResults[I];
      MedVar Value;
      if (const int Stored = Out[Index][I]; Stored >= 0) {
        Value = Values[static_cast<size_t>(Stored)];
      } else {
        MedOp Load;
        Load.Opcode = NdOp::LOAD;
        Load.Addr = Addr;
        Load.addInput(
            addressAt(*EntryStack, Base + Result.Offset, Addr, A, Fresh, Ops));
        Load.Output = Fresh.temporary(A, Result.Size);
        Ops.push_back(Load);
        Value = Ops.back().Output;
      }
      if (Result.Size < Word) {
        MedOp Widen;
        Widen.Opcode = NdOp::INT_ZEXT;
        Widen.Addr = Addr;
        Widen.Output = Fresh.temporary(A, Word);
        Widen.addInput(Value);
        Ops.push_back(Widen);
        Value = Ops.back().Output;
      }
      MedOp Define;
      Define.Opcode = NdOp::COPY;
      Define.Addr = Addr;
      Define.Output = Fresh.registerValue(A, Registers[I], Word);
      Define.addInput(Value);
      Ops.push_back(Define);
      if (I == 0)
        First = Ops.back().Output;
    }
    MedOp &Return = Block.Ops.back();
    if (Return.NumInputs >= 1)
      Return.Inputs[0] = *First;
    else
      Return.addInput(*First);
    Block.Ops.insert(Block.Ops.end() - 1, Ops.begin(), Ops.end());
  }
  return true;
}

void storeGoStackCallResults(MedFunc &Func, Arch A,
                             const std::map<uint32_t, GoStackCall> &Sites) {
  if (Sites.empty() || Func.Blocks.empty())
    return;
  const llvm::ArrayRef<uint64_t> Registers = goIntegerArgumentRegisters(A);
  const uint16_t Word = getTargetRegInfo(A).PointerSize;
  // The stack pointer at each call, found before any operation moves.
  std::map<uint32_t, MedVar> Stacks;
  {
    size_t Budget = limits::ir::kSourceCallReturnReadWork;
    const std::map<const MedOp *, MedVar> Found =
        sourceCallStackPointers(Func, A, Budget);
    for (const auto &[Op, Stack] : Found)
      if (Op->CallSiteId && Sites.count(Op->CallSiteId))
        Stacks.emplace(Op->CallSiteId, Stack);
  }
  // A call that returns several results returns their record
  // (defineGoCallResults): the first a word, each other as wide as it is.
  // The stack ABI returns nothing in a register, so a call whose callee is
  // known to keep the first result register defines no value there: it
  // returns its first result as a value of that register now.
  FreshValues Fresh(Func);
  std::map<uint32_t, GoCallShape> Shapes;
  for (MedBlock &Block : Func.Blocks)
    for (MedOp &Op : Block.Ops) {
      if (!isCall(Op) || !Op.CallSiteId || Op.RegisterReturnFields)
        continue;
      const auto Site = Sites.find(Op.CallSiteId);
      if (Site == Sites.end() || Op.Dead || Op.DoesNotReturn)
        continue;
      if (!Op.Output.Size)
        Op.Output = Fresh.registerValue(A, Registers[0], Word);
      if (Site->second.Results.size() < 2)
        continue;
      GoCallShape Shape;
      Shape.Last = static_cast<int>(Site->second.Results.size()) - 1;
      for (const MedGoStackResult &Result : Site->second.Results)
        Shape.Widths.push_back(Shape.Widths.empty() ? Word : Result.Size);
      Shapes.emplace(Op.CallSiteId, std::move(Shape));
    }
  defineGoCallResults(Func, A, Shapes);

  const int64_t Base = goStackArgumentLayout(A).CallStackBase;
  Fresh = FreshValues(Func);
  for (MedBlock &Block : Func.Blocks) {
    if (llvm::none_of(Block.Ops, [&](const MedOp &Op) {
          return isCall(Op) && Op.CallSiteId && Sites.count(Op.CallSiteId);
        }))
      continue;
    std::vector<MedOp> Ops;
    Ops.reserve(Block.Ops.size());
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      Ops.push_back(std::move(Block.Ops[I]));
      const size_t First = Ops.size() - 1;
      const MedOp &Call = Ops.back();
      const auto Site = isCall(Call) && Call.CallSiteId
                            ? Sites.find(Call.CallSiteId)
                            : Sites.end();
      if (Site == Sites.end() || Call.Dead || Call.DoesNotReturn)
        continue;
      const llvm::ArrayRef<MedGoStackResult> Results = Site->second.Results;
      const va_t Addr = Call.Addr;
      // The values the call returns, in the order of its results: the call's
      // own for one, else the fields that follow it.
      llvm::SmallVector<MedVar, 9> Values;
      if (Results.size() == 1) {
        if (Call.Output.Kind == MedVar::Reg &&
            Call.Output.RegOff == Registers[0] && Call.Output.Size == Word &&
            !Call.RegisterReturnFields)
          Values.push_back(Call.Output);
      } else if (Call.RegisterReturnFields &&
                 Call.RegisterReturnFields->size() == Results.size()) {
        const MedVar Packed = Call.Output;
        for (size_t J = I + 1;
             J < Block.Ops.size() && Values.size() < Results.size() &&
             Block.Ops[J].Addr == Addr;
             ++J) {
          const MedOp &Field = Block.Ops[J];
          if (Field.Opcode == NdOp::SUBBYTES && Field.NumInputs == 2 &&
              Field.Inputs[0].Kind == Packed.Kind &&
              Field.Inputs[0].Id == Packed.Id &&
              Field.Inputs[0].SSAVer == Packed.SSAVer)
            Values.push_back(Field.Output);
        }
      }
      // The call's own operations keep its address: its results' stores
      // follow them.
      while (I + 1 < Block.Ops.size() && Block.Ops[I + 1].Addr == Addr &&
             !isCall(Block.Ops[I + 1]) &&
             Block.Ops[I + 1].Opcode != NdOp::RETURN)
        Ops.push_back(std::move(Block.Ops[++I]));
      const auto Stack = Stacks.find(Site->first);
      if (Site->second.SharesFrame || Values.size() != Results.size() ||
          Stack == Stacks.end())
        continue;
      // A second route over the same functions finds the stores made.
      if (llvm::any_of(llvm::ArrayRef(Ops).drop_front(First),
                       [&](const MedOp &Made) {
                         return Made.Opcode == NdOp::STORE && Made.Addr == Addr;
                       }))
        continue;
      for (size_t J = 0; J < Results.size(); ++J) {
        MedVar Value = Values[J];
        if (Value.Size > Results[J].Size) {
          MedOp Narrow;
          Narrow.Opcode = NdOp::SUBBYTES;
          Narrow.Addr = Addr;
          Narrow.Output = Fresh.temporary(A, Results[J].Size);
          Narrow.addInput(Value);
          Narrow.addInput(MedVar::makeConst(0, 4));
          Ops.push_back(Narrow);
          Value = Ops.back().Output;
        }
        MedOp Store;
        Store.Opcode = NdOp::STORE;
        Store.Addr = Addr;
        Store.addInput(addressAt(Stack->second, Base + Results[J].Offset, Addr,
                                 A, Fresh, Ops));
        Store.addInput(Value);
        Ops.push_back(Store);
      }
    }
    Block.Ops = std::move(Ops);
  }
}

bool storesGoStackCallResult(llvm::ArrayRef<MedOp> Ops, size_t Index) {
  if (Index >= Ops.size() || Ops[Index].Opcode != NdOp::STORE ||
      Ops[Index].NumInputs < 2)
    return false;
  // The call the store follows, at its address, with nothing between them
  // but the call's own operations.
  const va_t Addr = Ops[Index].Addr;
  size_t Call = Index;
  while (Call > 0 && Ops[Call - 1].Addr == Addr && !isCall(Ops[Call - 1]))
    --Call;
  if (Call == 0 || Ops[Call - 1].Addr != Addr)
    return false;
  --Call;
  // What it stores is the call's value, a field of it, or a view of one.
  using Identity = std::tuple<MedVar::VarKind, int, int>;
  const auto Of = [](const MedVar &V) {
    return Identity{V.Kind, V.Id, V.SSAVer};
  };
  if (!Ops[Call].Output.Size)
    return false;
  llvm::SmallVector<Identity, 12> Values{Of(Ops[Call].Output)};
  for (size_t I = Call + 1; I < Index; ++I) {
    const MedOp &Op = Ops[I];
    if ((Op.Opcode == NdOp::SUBBYTES || Op.Opcode == NdOp::INT_ZEXT ||
         Op.Opcode == NdOp::COPY) &&
        Op.NumInputs >= 1 && llvm::is_contained(Values, Of(Op.Inputs[0])))
      Values.push_back(Of(Op.Output));
  }
  return llvm::is_contained(Values, Of(Ops[Index].Inputs[1]));
}

bool goLeavesItsStack(const MedFunc &Func, Arch A, BinaryFormat F) {
  const uint64_t Stack = getTargetRegInfo(A).StackPointer;
  MedEntryStackOffsets Offsets(Func, A, F);
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      if (Phi.Output.Kind == MedVar::Reg && Phi.Output.Size &&
          Phi.Output.RegOff == Stack && !Offsets.query(Phi.Output))
        return true;
    for (const MedOp &Op : Block.Ops)
      if (Op.Output.Kind == MedVar::Reg && Op.Output.Size &&
          Op.Output.RegOff == Stack && !Offsets.query(Op.Output))
        return true;
  }
  return false;
}

unsigned dropGoStackResultParameters(MedFunc &Func, Arch A) {
  const uint32_t Slot = goStackArgumentLayout(A).SlotBytes;
  if (Func.GoStackResults.empty() || !Slot)
    return 0;
  const auto IsResult = [&](const MedVar &Param) {
    if (Param.Kind != MedVar::Param || Param.RegOff != kNoParamReg ||
        Param.Id < 0)
      return false;
    const uint64_t Begin = static_cast<uint64_t>(Param.Id) * Slot;
    return llvm::any_of(Func.GoStackResults, [&](const MedGoStackResult &R) {
      return R.Offset < Begin + Slot && Begin < R.Offset + R.Size;
    });
  };
  unsigned Dropped = 0;
  while (!Func.Params.empty() && IsResult(Func.Params.back())) {
    const MedVar &Param = Func.Params.back();
    // A word the function reads as it was entered is an argument, whatever
    // it stores there later; none of a result's is read so.
    const bool Read = llvm::any_of(Func.Blocks, [&](const MedBlock &Block) {
      return llvm::any_of(Block.Ops, [&](const MedOp &Op) {
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          if (Op.Inputs[I].Kind == MedVar::Param &&
              Op.Inputs[I].RegOff == kNoParamReg && Op.Inputs[I].Id == Param.Id)
            return true;
        return false;
      });
    });
    if (Read)
      break;
    Func.Params.pop_back();
    ++Dropped;
  }
  if (Dropped) {
    const int Count = static_cast<int>(Func.Params.size());
    llvm::erase_if(Func.MutableStackParamHomes,
                   [&](const std::pair<int, int64_t> &Home) {
                     return Home.first >= Count;
                   });
    if (Func.TypedParams.size() > Func.Params.size())
      Func.TypedParams.resize(Func.Params.size());
  }
  return Dropped;
}

} // namespace neverd
