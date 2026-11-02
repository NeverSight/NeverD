//===- IndirectTailFrame.cpp - AArch64 tail-transfer frame guard ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove restoration of the native stack and link before an indirect tail call.
//===----------------------------------------------------------------------===//

#include "IndirectTailFrame.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIntrinsicEffects.h"
#include "neverd/loader/ExceptionForwarding.h"

#include <algorithm>
#include <bit>
#include <deque>
#include <iterator>
#include <map>
#include <optional>

namespace neverd {
namespace {

struct FrameWord {
  enum Kind { Unknown, Stack, Link, SignedLink } K = Unknown;
  int64_t Offset = 0;
  // Keep derived-frame taint when arithmetic or a join loses the exact offset.
  // Losing an exact address must not make a later escape appear harmless.
  bool MayBeStack = false;
  // Derived from an address of this function's own code (`adr x1, label` of a
  // switch's case table): a branch there is a computed jump within the
  // function, not a tail call.
  bool MayBeLocalCode = false;
  // For SignedLink, Offset is the signing modifier's entry-SP offset.
  unsigned AuthenticationKey = 0;
  bool ZeroModifier = false;
  std::optional<uint64_t> SecondModifier;
  bool ImplicitPrefix = false;
  std::optional<uint64_t> Constant;
  bool operator==(const FrameWord &) const = default;
};

using WordKey = std::pair<VnodeSpace, uint64_t>;
struct FrameState {
  std::map<WordKey, FrameWord> Values;
  std::map<int64_t, FrameWord> Spills;
  bool Escaped = false;
  bool operator==(const FrameState &) const = default;
};

class TailFrameGuard {
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::AArch64);
  const std::vector<TargetRegisterRange> Preserved;
  size_t Remaining;
  const LowFunc *Owner = nullptr;

  bool functionTemporary(uint64_t Offset) const {
    const auto &Ranges = Owner->FunctionTemporaries;
    const auto It =
        std::upper_bound(Ranges.begin(), Ranges.end(), Offset,
                         [](uint64_t Byte, const LowFunctionTemporary &Range) {
                           return Byte < Range.Offset;
                         });
    if (It == Ranges.begin())
      return false;
    const auto &Range = *std::prev(It);
    return Offset - Range.Offset <= Range.Bytes &&
           sizeof(uint64_t) <= Range.Bytes - (Offset - Range.Offset);
  }

  /// The function's own code: the span from its lowest block to the end of
  /// its highest, and the range its frame description covers.  A switch's
  /// case code that only its unresolved table reaches lies there, though no
  /// block holds it.  The entry itself is a call target: a branch there
  /// recurses (`volatile f self = f; return self(...)`).
  va_t CodeBegin = InvalidVA;
  va_t CodeEnd = 0;
  ExceptionAddressRange Described;
  va_t Entry = InvalidVA;
  /// The function's frame description; a branch out of the frame must leave
  /// an unwind from it nothing of the frame to run.
  const ExceptionFunction *Unwind = nullptr;

  bool localCode(uint64_t Address) const {
    return Address != Entry &&
           ((Address >= CodeBegin && Address < CodeEnd) ||
            (Described.isValid() && Address >= Described.Begin &&
             Address < Described.End));
  }

  bool charge(size_t Work) {
    if (Work > Remaining)
      return false;
    Remaining -= Work;
    return true;
  }

  static bool bounded(int64_t Offset) {
    return Offset >= -limits::kMaxFrameSize && Offset <= limits::kMaxFrameSize;
  }

  static bool validRange(const NdVar &Value) {
    return Value.Size && Value.Offset <= UINT64_MAX - Value.Size;
  }

  bool callPreserves(uint64_t Offset) const {
    for (const auto &Range : Preserved)
      if (Offset >= Range.Offset && Offset - Range.Offset <= Range.Bytes &&
          8 <= Range.Bytes - (Offset - Range.Offset))
        return true;
    return false;
  }

  static FrameWord lookup(const FrameState &State, const NdVar &Value) {
    if ((!Value.isReg() && !Value.isTemp()) || Value.Size != 8)
      return {};
    const auto Found = State.Values.find({Value.Space, Value.Offset});
    return Found == State.Values.end() ? FrameWord{} : Found->second;
  }

  bool read(const FrameState &State, const NdVar &Value, FrameWord &Word) {
    Word = lookup(State, Value);
    if (Value.isConst() && Value.Size == sizeof(uint64_t))
      Word.Constant = Value.Offset;
    if ((!Value.isReg() && !Value.isTemp()) || Value.Size == 8)
      return true;
    if (!validRange(Value) || !charge(State.Values.size()))
      return false;
    for (const auto &[Key, Fact] : State.Values)
      if (Key.first == Value.Space && Key.second < Value.Offset + Value.Size &&
          Value.Offset < Key.second + 8) {
        Word.MayBeStack |= Fact.MayBeStack;
        Word.MayBeLocalCode |= Fact.MayBeLocalCode;
      }
    return true;
  }

  bool write(FrameState &State, const NdVar &Output, FrameWord Word) {
    if (!Output.isReg() && !Output.isTemp())
      return true;
    if (!validRange(Output) || !charge(State.Values.size()))
      return false;
    for (auto It = State.Values.begin(); It != State.Values.end();) {
      const auto &[Key, Fact] = *It;
      if (Key.first != Output.Space ||
          Key.second >= Output.Offset + Output.Size ||
          Output.Offset >= Key.second + 8) {
        ++It;
        continue;
      }
      // A partial write loses the whole identity, including an SP/LR alias.
      // Conservatively retain any frame taint in the remaining bytes.
      if (Output.Size != 8) {
        Word.MayBeStack |= Fact.MayBeStack;
        Word.MayBeLocalCode |= Fact.MayBeLocalCode;
      }
      if (Output.Size != 8 || Output.Offset != Key.second)
        State.Escaped |= Fact.MayBeStack;
      It = State.Values.erase(It);
    }
    if (Output.Size != 8) {
      Word.K = FrameWord::Unknown;
      Word.Constant.reset();
    }
    if (Word.K != FrameWord::Unknown || Word.MayBeStack ||
        Word.MayBeLocalCode || Word.Constant)
      State.Values[{Output.Space, Output.Offset}] = Word;
    return true;
  }

  /// Whether \p Size bytes at \p Offset lie in the frame this function has
  /// allocated: below its entry stack pointer and at or above the current one.
  static bool allocatedFrame(const FrameWord &SP, int64_t Offset,
                             uint64_t Size) {
    return SP.K == FrameWord::Stack && Offset >= SP.Offset &&
           Offset + static_cast<int64_t>(Size) <= 0;
  }

  /// A store whose target the spills do not track exactly.  Through an
  /// address derived from the frame, or once a frame address has escaped, it
  /// may hit any spill: all are forgotten.  Any other address cannot reach the
  /// frame this function allocated, which nothing outside it can name under
  /// the native ABI, so only those spills survive.  Every stack-derived word
  /// it drops or writes escapes.
  bool storeUntracked(FrameState &State, const FrameWord &Address,
                      const FrameWord &Value) {
    if (!charge(State.Spills.size()))
      return false;
    const auto SP = lookup(State, NdVar::reg(TRI.StackPointer, 8));
    const bool Foreign = !Address.MayBeStack && !State.Escaped;
    std::erase_if(State.Spills, [&](const auto &Item) {
      if (Foreign && allocatedFrame(SP, Item.first, 8))
        return false;
      State.Escaped |= Item.second.MayBeStack;
      return true;
    });
    State.Escaped |= Value.MayBeStack;
    return true;
  }

  /// The taint of a value loaded through a frame address that no exact spill
  /// answers: it may be any spilled word.
  bool spilledTaint(const FrameState &State, FrameWord &Result) {
    if (!charge(State.Spills.size()))
      return false;
    for (const auto &[Offset, Spill] : State.Spills) {
      Result.MayBeStack |= Spill.MayBeStack;
      Result.MayBeLocalCode |= Spill.MayBeLocalCode;
    }
    return true;
  }

  bool forgetTemps(FrameState &State) {
    if (!charge(State.Values.size()))
      return false;
    std::erase_if(State.Values, [&](const auto &Item) {
      return Item.first.first == VnodeSpace::TEMP &&
             !functionTemporary(Item.first.second);
    });
    return true;
  }

  bool transfer(const LowBlock &Block, FrameState &State,
                std::set<va_t> *Candidates = nullptr) {
    va_t Instruction = InvalidVA;
    for (const LowOp &Op : Block.Ops) {
      if (!charge(1) || Op.NumInputs > std::size(Op.Inputs))
        return false;
      if (Op.Addr != Instruction) {
        if (!forgetTemps(State))
          return false;
        Instruction = Op.Addr;
      }
      FrameWord Inputs[kLowOperationInputs];
      FrameWord Result;
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        if (!read(State, Op.Inputs[I], Inputs[I]))
          return false;
        if (Op.Inputs[I].isConst() && localCode(Op.Inputs[I].Offset))
          Inputs[I].MayBeLocalCode = true;
        Result.MayBeStack |= Inputs[I].MayBeStack;
        Result.MayBeLocalCode |= Inputs[I].MayBeLocalCode;
      }

      if (Op.Opcode == NdOp::INDIR_BR) {
        const auto SP = lookup(State, NdVar::reg(TRI.StackPointer, 8));
        const auto LR = lookup(State, NdVar::reg(TRI.LinkRegister, 8));
        if (Candidates && Op.NumInputs == 1 && Op.Inputs[0].Size == 8 &&
            SP.K == FrameWord::Stack && SP.Offset == 0 &&
            LR.K == FrameWord::Link && !Inputs[0].MayBeStack &&
            !Inputs[0].MayBeLocalCode && Inputs[0].K != FrameWord::Link &&
            Inputs[0].K != FrameWord::SignedLink &&
            (!Unwind || exceptionForwardsUnwindAt(*Unwind, Op.Addr)))
          Candidates->insert(Op.Addr);
      }

      switch (Op.Opcode) {
      case NdOp::COPY:
        if (Op.NumInputs == 1 && Op.Output.Size == 8 && Op.Inputs[0].Size == 8)
          Result = Inputs[0];
        break;
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
        if (Op.NumInputs == 2 && Op.Output.Size == 8) {
          if (Inputs[0].Constant && Inputs[1].Constant &&
              Op.Inputs[0].Size == sizeof(uint64_t) &&
              Op.Inputs[1].Size == sizeof(uint64_t))
            Result.Constant = Op.Opcode == NdOp::INT_ADD
                                  ? *Inputs[0].Constant + *Inputs[1].Constant
                                  : *Inputs[0].Constant - *Inputs[1].Constant;
          for (unsigned Base = 0; Base < 2; ++Base) {
            const unsigned Scalar = 1 - Base;
            const NdVar &Delta = Op.Inputs[Scalar];
            if (Inputs[Base].K != FrameWord::Stack ||
                Op.Inputs[Base].Size != 8 || !Delta.isConst() || !Delta.Size ||
                Delta.Size > 8 || isAddressProvenance(Delta.Provenance) ||
                (Op.Opcode == NdOp::INT_SUB && Base != 0))
              continue;
            // A narrow arithmetic immediate is zero-extended to the full
            // pointer width; negative displacements use all eight bytes.
            const uint64_t Bits =
                Delta.Size == 8
                    ? Delta.Offset
                    : Delta.Offset & ((uint64_t{1} << (Delta.Size * 8)) - 1);
            int64_t Step = std::bit_cast<int64_t>(Bits);
            if (!bounded(Step))
              continue;
            if (Op.Opcode == NdOp::INT_SUB)
              Step = -Step;
            const int64_t Offset = Inputs[Base].Offset + Step;
            if (bounded(Offset))
              Result = {FrameWord::Stack, Offset, true, Result.MayBeLocalCode};
          }
        }
        break;
      case NdOp::LOAD:
      case NdOp::STORE: {
        const auto Memory = lowMemoryOperands(Op);
        FrameWord Address, Value;
        if (!Memory.Complete || !Memory.Address ||
            !read(State, *Memory.Address, Address) ||
            (Memory.StoredValue && !read(State, *Memory.StoredValue, Value)))
          return false;
        const bool Exact =
            Address.K == FrameWord::Stack && bounded(Address.Offset) &&
            bounded(Address.Offset + Memory.AccessSize) &&
            Op.MemoryOrdering == NdMemoryOrdering::None &&
            Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
        // An address is not a loaded value. Its taint is transported only by
        // an exact known spill; untracked loads provide no restoration fact.
        Result = {};
        if (Op.Opcode == NdOp::STORE) {
          if (!Exact) {
            if (!storeUntracked(State, Address, Value))
              return false;
          } else {
            if (!charge(State.Spills.size()))
              return false;
            std::erase_if(State.Spills, [&](const auto &Item) {
              const bool Overlap =
                  Item.first < Address.Offset + Memory.AccessSize &&
                  Address.Offset < Item.first + 8;
              if (Overlap &&
                  (Memory.AccessSize != 8 || Address.Offset != Item.first))
                State.Escaped |= Item.second.MayBeStack;
              return Overlap;
            });
            const bool Recorded =
                Memory.AccessSize == 8 &&
                (Value.K != FrameWord::Unknown || Value.MayBeStack ||
                 Value.MayBeLocalCode || Value.Constant);
            if (Recorded)
              State.Spills[Address.Offset] = Value;
            // A frame address no spill records, or stored outside the frame
            // this function allocated, can reach code that writes through it.
            const auto SP = lookup(State, NdVar::reg(TRI.StackPointer, 8));
            if (!Recorded ||
                !allocatedFrame(SP, Address.Offset, Memory.AccessSize))
              State.Escaped |= Value.MayBeStack;
          }
        } else {
          bool Answered = false;
          if (Exact && Memory.AccessSize == 8) {
            const auto Found = State.Spills.find(Address.Offset);
            if (Found != State.Spills.end()) {
              Result = Found->second;
              Answered = true;
            }
          }
          if (!Answered && Address.MayBeStack && !spilledTaint(State, Result))
            return false;
        }
        break;
      }
      case NdOp::CALL:
      case NdOp::INDIR_CALL: {
        if (!charge(State.Values.size() + State.Spills.size()) ||
            !charge(State.Values.size() * Preserved.size()))
          return false;
        for (uint64_t Register : TRI.IntParamRegs)
          State.Escaped |= lookup(State, NdVar::reg(Register, 8)).MayBeStack;
        if (TRI.indirectResultReg())
          State.Escaped |=
              lookup(State, NdVar::reg(TRI.indirectResultReg(), 8)).MayBeStack;
        for (unsigned I = 1; I < Op.NumInputs; ++I)
          State.Escaped |= Inputs[I].MayBeStack;
        // A frame pointer spilled to the outgoing area can be an implicit
        // stack argument even when the LowIR call lists only its target.
        for (const auto &[Offset, Spill] : State.Spills)
          State.Escaped |= Spill.MayBeStack;
        const auto SP = lookup(State, NdVar::reg(TRI.StackPointer, 8));
        // Native calls retain the caller's unexposed allocated frame under
        // the existing ABI. Incoming/outgoing or freed slots grant no fact.
        std::erase_if(State.Spills, [&](const auto &Item) {
          return State.Escaped || SP.K != FrameWord::Stack ||
                 Item.first < SP.Offset || Item.first + 8 > 0;
        });
        std::erase_if(State.Values, [&](const auto &Item) {
          return Item.first.first == VnodeSpace::TEMP
                     ? !functionTemporary(Item.first.second)
                     : !callPreserves(Item.first.second);
        });
        Result = {};
        break;
      }
      case NdOp::ATOMIC_XCHG:
      case NdOp::ATOMIC_ADD:
      case NdOp::ATOMIC_CMPXCHG:
      case NdOp::INTRINSIC: {
        // Checked arithmetic writes only its explicit result. Retain all
        // operand taint without assuming checking controls or an exact value.
        if (lowAArch64CPAShapeIsValid(Op) || lowAArch64SVECPAShapeIsValid(Op) ||
            lowAArch64SVEPrefixShapeIsValid(Op))
          break;
        if (lowAArch64PAuthShapeIsValid(Op)) {
          // A modifier's stack address is not the authenticated target.
          // A saved link is restored only by its matching key and modifier.
          Result = Inputs[1];
          Result.Constant.reset();
          const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
          if (const auto Auth = aarch64PAuthOperation(Id)) {
            Result.K = FrameWord::Unknown;
            const bool Zero = Op.NumInputs == 3 && Op.Inputs[2].isConst() &&
                              Op.Inputs[2].Offset == 0;
            const bool KnownModifier = Zero || Inputs[2].K == FrameWord::Stack;
            const int64_t Modifier = Zero ? 0 : Inputs[2].Offset;
            if (!Auth->Strip && !Auth->Authenticate && KnownModifier &&
                Inputs[1].K == FrameWord::Link) {
              Result.K = FrameWord::SignedLink;
              Result.Offset = Modifier;
              Result.AuthenticationKey = Auth->Key;
              Result.ZeroModifier = Zero;
            } else if (Auth->Authenticate && KnownModifier &&
                       Inputs[1].K == FrameWord::SignedLink &&
                       !Inputs[1].SecondModifier &&
                       Inputs[1].AuthenticationKey == Auth->Key &&
                       Inputs[1].ZeroModifier == Zero &&
                       Inputs[1].Offset == Modifier) {
              Result = {FrameWord::Link};
            }
          } else if (const auto Auth = aarch64PAuthExtendedOperation(Id)) {
            Result.K = FrameWord::Unknown;
            const bool Zero = Inputs[2].Constant == 0;
            const bool KnownModifier = Zero || Inputs[2].K == FrameWord::Stack;
            const int64_t Modifier = Zero ? 0 : Inputs[2].Offset;
            const bool KnownPrefix =
                !Auth->ConditionalPrefix ||
                Inputs[kAArch64PACMOperandCount].Constant == 1;
            if (KnownModifier && Inputs[3].Constant && KnownPrefix) {
              if (!Auth->Authenticate && Inputs[1].K == FrameWord::Link) {
                Result.K = FrameWord::SignedLink;
                Result.Offset = Modifier;
                Result.AuthenticationKey = Auth->Key;
                Result.ZeroModifier = Zero;
                Result.SecondModifier = Inputs[3].Constant;
                Result.ImplicitPrefix = Auth->ConditionalPrefix;
              } else if (Auth->Authenticate &&
                         Inputs[1].K == FrameWord::SignedLink &&
                         Inputs[1].AuthenticationKey == Auth->Key &&
                         Inputs[1].ZeroModifier == Zero &&
                         Inputs[1].Offset == Modifier &&
                         Inputs[1].SecondModifier == Inputs[3].Constant &&
                         Inputs[1].ImplicitPrefix == Auth->ConditionalPrefix) {
                Result = {FrameWord::Link};
              }
            }
          }
          break;
        }
        // A value-only intrinsic is an ordinary value, and a trap ends its
        // path, so nothing after it is reached.
        if (lowIntrinsicHasOnlyValueEffects(Op) ||
            lowIntrinsicIsUnconditionalTrap(Op))
          break;
        // A read of state no analysis tracks writes only its output.
        if (lowIntrinsicWritesOnlyItsOutput(Op)) {
          Result = {};
          break;
        }
        // An atomic or exclusive access writes only its output and the memory
        // at its address.  Its ordering, and an exclusive store's possible
        // failure, keep that memory out of the spill facts, and the value it
        // returns is untracked, though through a frame address it may be any
        // spilled word.  No implicit memory/register effect contract is
        // invented for any other intrinsic.
        const auto Memory = Op.Opcode == NdOp::INTRINSIC
                                ? lowIntrinsicMemoryOperands(Op)
                                : lowMemoryOperands(Op);
        FrameWord Address, Value;
        if (!Memory.Complete || !Memory.Address ||
            !read(State, *Memory.Address, Address))
          return false;
        Result = {};
        if ((Address.MayBeStack && !spilledTaint(State, Result)) ||
            (Memory.StoredValue && (!read(State, *Memory.StoredValue, Value) ||
                                    !storeUntracked(State, Address, Value))))
          return false;
        break;
      }
      default:
        break;
      }
      if (!write(State, Op.Output, Result))
        return false;
    }
    return forgetTemps(State);
  }

  template <typename Key>
  bool meetWords(std::map<Key, FrameWord> &Into,
                 const std::map<Key, FrameWord> &Other) {
    if (!charge(Into.size() + Other.size()))
      return false;
    const auto Merge = [](FrameWord A, FrameWord B) {
      const bool May = A.MayBeStack || B.MayBeStack;
      const bool LocalCode = A.MayBeLocalCode || B.MayBeLocalCode;
      if (A.K != B.K || A.Offset != B.Offset ||
          A.AuthenticationKey != B.AuthenticationKey ||
          A.ZeroModifier != B.ZeroModifier ||
          A.SecondModifier != B.SecondModifier ||
          A.ImplicitPrefix != B.ImplicitPrefix)
        A = {};
      if (A.Constant != B.Constant)
        A.Constant.reset();
      A.MayBeStack = May;
      A.MayBeLocalCode = LocalCode;
      return A;
    };
    for (auto It = Into.begin(); It != Into.end();) {
      const auto Found = Other.find(It->first);
      It->second =
          Merge(It->second, Found == Other.end() ? FrameWord{} : Found->second);
      if (It->second.K == FrameWord::Unknown && !It->second.MayBeStack &&
          !It->second.MayBeLocalCode && !It->second.Constant)
        It = Into.erase(It);
      else
        ++It;
    }
    for (const auto &[KeyValue, Word] : Other)
      if (!Into.count(KeyValue) && (Word.MayBeStack || Word.MayBeLocalCode))
        Into[KeyValue] = {FrameWord::Unknown, 0, Word.MayBeStack,
                          Word.MayBeLocalCode};
    return true;
  }

public:
  TailFrameGuard(BinaryFormat Format, size_t MaxWork)
      : Preserved(TRI.callPreservedRanges(Format)),
        Remaining(std::min(MaxWork, limits::kMaxIndirectTailFrameWork)) {}

  std::set<va_t> run(const LowFunc &Function) {
    // A frame description that only unwinds the frame (a plain FDE, which
    // every function of an ordinary shared library has) runs no handler, so
    // a branch out of the frame leaves nothing behind.  A C or C++ table may
    // still let an unwind from one branch pass through; each candidate is
    // checked against it, and the blocks below still refuse exceptional
    // edges.
    Unwind =
        Function.ExceptionMetadata ? &*Function.ExceptionMetadata : nullptr;
    Owner = &Function;
    if (auto Error = validateLowInstructionBoundaries(Function)) {
      llvm::consumeError(std::move(Error));
      return {};
    }
    if (Function.Blocks.empty() || !Function.hasCompleteLiftCoverage() ||
        (Unwind && !Unwind->describesUnwindOnly() && !Unwind->Itanium))
      return {};
    const size_t Count = Function.Blocks.size();
    if (!charge(Count))
      return {};
    CodeBegin = InvalidVA;
    CodeEnd = 0;
    for (const LowBlock &Block : Function.Blocks) {
      CodeBegin = std::min(CodeBegin, Block.StartAddr);
      CodeEnd = std::max(CodeEnd, Block.EndAddr);
    }
    Described = Function.ExceptionMetadata
                    ? Function.ExceptionMetadata->CodeRange
                    : ExceptionAddressRange{};
    Entry = Function.Entry;
    std::vector<std::optional<FrameState>> Seeds(Count), In(Count), Out(Count);
    int Entry = -1;
    std::set<va_t> Starts;
    for (size_t I = 0; I < Count; ++I) {
      const auto &Block = Function.Blocks[I];
      if (Block.Id != static_cast<int>(I) ||
          !Starts.insert(Block.StartAddr).second ||
          !Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty() ||
          !charge(Block.Preds.size() + Block.Succs.size() +
                  Block.InstructionBoundaries.size()))
        return {};
      uint64_t NextOp = 0;
      for (const auto &Boundary : Block.InstructionBoundaries) {
        if (Boundary.FirstOp != NextOp ||
            Boundary.OpCount > Block.Ops.size() - NextOp ||
            hasLowInstructionControlFlag(
                Boundary.ControlFlags,
                LowInstructionControlFlag::InstructionGuard))
          return {};
        if (!charge(Boundary.OpCount))
          return {};
        for (uint64_t J = 0; J < Boundary.OpCount; ++J)
          if (Block.Ops[NextOp + J].Addr != Boundary.Address)
            return {};
        NextOp += Boundary.OpCount;
      }
      if (NextOp != Block.Ops.size())
        return {};
      if (Block.StartAddr == Function.Entry)
        Entry = Block.Id;
      if (Function.ModuleAnalysisRoots.count(Block.StartAddr) &&
          Block.StartAddr != Function.Entry)
        Seeds[I] = FrameState{};
      for (int Succ : Block.Succs)
        if (Succ < 0 || static_cast<size_t>(Succ) >= Count ||
            !charge(Function.Blocks[Succ].Preds.size()) ||
            std::find(Function.Blocks[Succ].Preds.begin(),
                      Function.Blocks[Succ].Preds.end(),
                      Block.Id) == Function.Blocks[Succ].Preds.end())
          return {};
      for (int Pred : Block.Preds)
        if (Pred < 0 || static_cast<size_t>(Pred) >= Count ||
            !charge(Function.Blocks[Pred].Succs.size()) ||
            !Function.Blocks[Pred].hasSucc(Block.Id))
          return {};
    }
    if (Entry < 0)
      return {};
    if (!charge(Function.ModuleAnalysisRoots.size()))
      return {};
    for (va_t Root : Function.ModuleAnalysisRoots)
      if (!Starts.count(Root))
        return {};
    FrameState Initial;
    Initial.Values[{VnodeSpace::REG, TRI.StackPointer}] = {FrameWord::Stack, 0,
                                                           true};
    Initial.Values[{VnodeSpace::REG, TRI.LinkRegister}] = {FrameWord::Link, 0,
                                                           false};
    Seeds[Entry] = std::move(Initial);

    std::deque<int> Worklist;
    std::vector<bool> Queued(Count, false);
    for (size_t I = 0; I < Count; ++I)
      if (Seeds[I]) {
        Worklist.push_back(static_cast<int>(I));
        Queued[I] = true;
      }
    while (!Worklist.empty()) {
      const int Id = Worklist.front();
      Worklist.pop_front();
      Queued[Id] = false;
      if (!charge(1))
        return {};
      auto Next = Seeds[Id];
      for (int Pred : Function.Blocks[Id].Preds) {
        if (!Out[Pred])
          continue;
        if (!charge(Out[Pred]->Values.size() + Out[Pred]->Spills.size()))
          return {};
        if (!Next)
          Next = Out[Pred];
        else {
          Next->Escaped |= Out[Pred]->Escaped;
          if (!meetWords(Next->Values, Out[Pred]->Values) ||
              !meetWords(Next->Spills, Out[Pred]->Spills))
            return {};
        }
      }
      if (!Next || (In[Id] && *In[Id] == *Next))
        continue;
      In[Id] = Next;
      if (!transfer(Function.Blocks[Id], *Next))
        return {};
      if (Out[Id] && *Out[Id] == *Next)
        continue;
      Out[Id] = std::move(Next);
      for (int Succ : Function.Blocks[Id].Succs)
        if (!Queued[Succ]) {
          Worklist.push_back(Succ);
          Queued[Succ] = true;
        }
    }
    // Publish only after every predecessor and backedge has converged.
    std::set<va_t> Candidates;
    for (size_t I = 0; I < Count; ++I) {
      if (!In[I] || !charge(In[I]->Values.size() + In[I]->Spills.size()))
        return {};
      auto State = *In[I];
      if (!transfer(Function.Blocks[I], State, &Candidates))
        return {};
    }
    return Candidates;
  }
};

} // namespace

std::set<va_t> restoredAArch64IndirectTailFrames(const LowFunc &Function,
                                                 BinaryFormat Format,
                                                 size_t MaxWork) {
  return TailFrameGuard(Format, MaxWork).run(Function);
}

} // namespace neverd
