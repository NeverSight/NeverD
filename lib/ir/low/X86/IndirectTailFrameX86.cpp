//===- IndirectTailFrameX86.cpp - x86-64 tail-transfer frame guard --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove that an x86-64 indirect jump leaves the stack pointer where the
/// caller's CALL left it, so the jump hands that return address on.
//===----------------------------------------------------------------------===//

#include "../IndirectTailFrame.h"

#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {
namespace {

using Key = std::pair<VnodeSpace, uint64_t>;

/// The registers and temporaries whose value derives from the incoming stack
/// pointer: that pointer plus a known offset, or plus one this proof lost
/// (nullopt).  A register is keyed by its 8-byte container.  A key that is
/// absent holds nothing derived from the incoming stack pointer: a constant,
/// an argument, a call's result, or a value read from memory.
using StackValues = std::map<Key, std::optional<int64_t>>;

/// What a value derives from the incoming stack pointer.
struct Derivation {
  bool Derived = false;
  std::optional<int64_t> Offset;
};

/// The general-purpose registers both x86-64 calling conventions preserve
/// across a call.  RSI and RDI are preserved only by the Microsoft one.
constexpr uint64_t kPreserved[] = {x86reg::RBX, x86reg::RSP, x86reg::RBP,
                                   x86reg::R12, x86reg::R13, x86reg::R14,
                                   x86reg::R15};

class X64TailFrameGuard {
  const BinaryImage &Image;
  size_t Remaining;

  bool charge(size_t Work) {
    if (Work > Remaining)
      return false;
    Remaining -= Work;
    return true;
  }

  static bool preserved(uint64_t Register) {
    return std::find(std::begin(kPreserved), std::end(kPreserved), Register) !=
           std::end(kPreserved);
  }

  static std::optional<Key> keyOf(const NdVar &Value) {
    if (Value.isTemp())
      return Key{VnodeSpace::TEMP, Value.Offset};
    if (Value.isReg() && Value.Size && Value.Size <= 8)
      return Key{VnodeSpace::REG, Value.Offset & ~uint64_t{7}};
    return std::nullopt;
  }

  static bool whole(const NdVar &Value) {
    return Value.Size == 8 && (Value.isTemp() || Value.Offset % 8 == 0);
  }

  static Derivation derive(const StackValues &State, const NdVar &Value) {
    const auto K = keyOf(Value);
    if (!K)
      return {};
    const auto It = State.find(*K);
    if (It == State.end())
      return {};
    return {true, whole(Value) ? It->second : std::nullopt};
  }

  static std::optional<int64_t> constantOf(const NdVar &Value) {
    if (!Value.isConst() || Value.Size != 8)
      return std::nullopt;
    return static_cast<int64_t>(Value.Offset);
  }

  /// What \p Op's result derives from the incoming stack pointer.  A value
  /// read from memory is no derivation this proof follows.
  static Derivation resultOf(const StackValues &State, const LowOp &Op) {
    if (Op.Opcode == NdOp::LOAD)
      return {};
    Derivation Inputs[kLowOperationInputs];
    bool Derived = false;
    for (unsigned I = 0; I < Op.NumInputs && I < kLowOperationInputs; ++I) {
      Inputs[I] = derive(State, Op.Inputs[I]);
      Derived |= Inputs[I].Derived;
    }
    if (!Derived)
      return {};
    int64_t Sum = 0;
    if (Op.Output.Size == 8 && Op.NumInputs == 1 && Op.Opcode == NdOp::COPY)
      return Inputs[0];
    if (Op.Output.Size == 8 && Op.NumInputs == 2 && Op.Opcode == NdOp::INT_ADD)
      for (unsigned I = 0; I < 2; ++I) {
        const auto Amount = constantOf(Op.Inputs[1 - I]);
        if (Inputs[I].Offset && Amount &&
            !__builtin_add_overflow(*Inputs[I].Offset, *Amount, &Sum))
          return {true, Sum};
      }
    if (Op.Output.Size == 8 && Op.NumInputs == 2 &&
        Op.Opcode == NdOp::INT_SUB) {
      const auto Amount = constantOf(Op.Inputs[1]);
      if (Inputs[0].Offset && Amount &&
          !__builtin_sub_overflow(*Inputs[0].Offset, *Amount, &Sum))
        return {true, Sum};
    }
    return {true, std::nullopt};
  }

  /// Record that \p Out now holds \p Result, or, for a guarded write, either
  /// \p Result or what it held.  A partial write of a register keeps part of
  /// its container, so derivation on either side loses the offset.
  static void write(StackValues &State, const NdVar &Out, Derivation Result,
                    bool Guarded) {
    if (Out.isReg() && Out.Size > 8) {
      std::erase_if(State, [&](const auto &Entry) {
        return Entry.first.first == VnodeSpace::REG &&
               Entry.first.second < Out.Offset + Out.Size &&
               Out.Offset < Entry.first.second + 8;
      });
      return;
    }
    const auto K = keyOf(Out);
    if (!K)
      return;
    const auto It = State.find(*K);
    const bool Held = It != State.end();
    if (Result.Offset && (*Result.Offset < -limits::kMaxFrameSize ||
                          *Result.Offset > limits::kMaxFrameSize))
      Result.Offset.reset();
    if ((Guarded || !whole(Out)) && Held) {
      if (!Result.Derived || It->second != Result.Offset || !whole(Out))
        It->second.reset();
      return;
    }
    if (Guarded || !whole(Out)) {
      if (Result.Derived)
        State[*K] = std::nullopt;
      return;
    }
    if (Result.Derived)
      State[*K] = Result.Offset;
    else if (Held)
      State.erase(It);
  }

  static void forgetTemps(StackValues &State) {
    std::erase_if(State, [](const auto &Entry) {
      return Entry.first.first == VnodeSpace::TEMP;
    });
  }

  /// Run \p Block from \p State.  \p Candidates collects the indirect jumps
  /// that may hand on the return address: the incoming stack pointer is in
  /// place, or nothing derived from it is, so the jump runs on another stack.
  bool transfer(const LowBlock &Block, StackValues &State,
                std::set<va_t> *Candidates) {
    for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries) {
      forgetTemps(State);
      // A guarded instruction writes its outputs only on one of its paths.
      const bool Guarded = hasLowInstructionControlFlag(
          Boundary.ControlFlags, LowInstructionControlFlag::InstructionGuard);
      for (uint64_t I = 0; I < Boundary.OpCount; ++I) {
        if (!charge(1 + State.size()))
          return false;
        const LowOp &Op = Block.Ops[Boundary.FirstOp + I];
        if (Op.Opcode == NdOp::INDIR_BR) {
          // A routine that resumes saved state, as gogo<>, longjmp and a
          // fiber switch do, first takes RSP from memory or an argument.
          // PLT0 pushes the lazy-binding resolver's argument and jumps
          // through the slot the dynamic linker fills with that resolver;
          // `jmp *slot(%rip)` lifts as a jump through that constant slot.
          const auto Stack = State.find({VnodeSpace::REG, x86reg::RSP});
          const bool Restored = Stack == State.end() || Stack->second == 0;
          const bool LazyResolver =
              Op.NumInputs == 1 && Op.Inputs[0].isConst() &&
              Image.hasRuntimeCallablePointerSlotAt(
                  Op.Inputs[0].Offset,
                  RuntimeCallablePointerSlotKind::ELFLazyResolver);
          if (Candidates && (Restored || LazyResolver))
            Candidates->insert(Op.Addr);
          continue;
        }
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
          // A call returns with the stack pointer it was made with and keeps
          // the registers both conventions preserve. RSI and RDI keep what
          // they derived from only under the Microsoft convention; the rest
          // hold what the call left, which derives from nothing here.
          for (auto It = State.begin(); It != State.end();) {
            const auto &[Space, Register] = It->first;
            if (Space == VnodeSpace::REG && preserved(Register)) {
              ++It;
            } else if (Space == VnodeSpace::REG &&
                       (Register == x86reg::RSI || Register == x86reg::RDI)) {
              It->second.reset();
              ++It;
            } else {
              It = State.erase(It);
            }
          }
          write(State, Op.Output, {}, false);
          continue;
        }
        write(State, Op.Output, resultOf(State, Op), Guarded);
      }
    }
    forgetTemps(State);
    return true;
  }

  static StackValues meet(const StackValues &Left, const StackValues &Right) {
    StackValues Result = Left;
    for (auto &[K, Value] : Result) {
      const auto It = Right.find(K);
      if (It == Right.end() || It->second != Value)
        Value.reset();
    }
    for (const auto &[K, Value] : Right)
      Result.try_emplace(K, std::nullopt);
    return Result;
  }

public:
  X64TailFrameGuard(const BinaryImage &Image, size_t MaxWork)
      : Image(Image), Remaining(MaxWork) {}

  std::set<va_t> run(const LowFunc &Function) {
    if (auto Error = validateLowInstructionBoundaries(Function)) {
      llvm::consumeError(std::move(Error));
      return {};
    }
    // The caller admits the proof: an unsupported instruction's effects are
    // unknown, while a decode failure only ends its path.
    if (Function.Blocks.empty())
      return {};
    const size_t Count = Function.Blocks.size();
    if (!charge(Count))
      return {};
    int Entry = -1;
    for (size_t I = 0; I < Count; ++I) {
      const LowBlock &Block = Function.Blocks[I];
      if (Block.Id != static_cast<int>(I) ||
          !charge(Block.Succs.size() + Block.InstructionBoundaries.size()))
        return {};
      uint64_t NextOp = 0;
      for (const LowInstructionBoundary &Boundary :
           Block.InstructionBoundaries) {
        if (Boundary.FirstOp != NextOp ||
            Boundary.OpCount > Block.Ops.size() - NextOp)
          return {};
        NextOp += Boundary.OpCount;
      }
      if (NextOp != Block.Ops.size())
        return {};
      for (int Succ : Block.Succs)
        if (Succ < 0 || static_cast<size_t>(Succ) >= Count)
          return {};
      if (Block.StartAddr == Function.Entry)
        Entry = Block.Id;
    }
    if (Entry < 0)
      return {};

    std::vector<std::optional<StackValues>> In(Count);
    std::vector<int> Work;
    In[Entry] = StackValues{{{VnodeSpace::REG, x86reg::RSP}, 0}};
    Work.push_back(Entry);
    // A landing pad or another analysis root enters on this function's stack
    // at an offset this proof has not followed, and any preserved register
    // may hold a frame address.
    StackValues Unfollowed;
    for (uint64_t Register : kPreserved)
      Unfollowed.emplace(Key{VnodeSpace::REG, Register}, std::nullopt);
    for (size_t I = 0; I < Count; ++I) {
      const LowBlock &Block = Function.Blocks[I];
      if (static_cast<int>(I) == Entry ||
          (Block.ExceptionalPreds.empty() &&
           !Function.ModuleAnalysisRoots.count(Block.StartAddr)))
        continue;
      In[I] = Unfollowed;
      Work.push_back(static_cast<int>(I));
    }
    while (!Work.empty()) {
      const int Id = Work.back();
      Work.pop_back();
      StackValues State = *In[Id];
      if (!transfer(Function.Blocks[Id], State, nullptr))
        return {};
      for (int Succ : Function.Blocks[Id].Succs) {
        if (!charge(1 + State.size()))
          return {};
        StackValues Next = In[Succ] ? meet(*In[Succ], State) : State;
        if (In[Succ] && *In[Succ] == Next)
          continue;
        In[Succ] = std::move(Next);
        Work.push_back(Succ);
      }
    }

    std::set<va_t> Candidates;
    for (size_t I = 0; I < Count; ++I) {
      if (!In[I])
        continue;
      StackValues State = *In[I];
      if (!transfer(Function.Blocks[I], State, &Candidates))
        return {};
    }
    return Candidates;
  }
};

} // namespace

std::set<va_t> restoredX64IndirectTailFrames(const LowFunc &Function,
                                             const BinaryImage &Image,
                                             size_t MaxWork) {
  return X64TailFrameGuard(Image, MaxWork).run(Function);
}

} // namespace neverd
