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
#include <cstring>
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

/// What the proof knows at a point: the stack derivations, and the registers
/// and temporaries that may hold an address of this function's own code, or a
/// value read through or computed from a table of such addresses.  A branch to
/// such a value is a computed jump within the function, as a switch's relative
/// table makes one (`lea table(%rip), %rcx; movslq (%rcx,%rax,4), %rdx;
/// add %rcx, %rdx; jmp *%rdx`), however its index is bounded.
struct FrameFacts {
  StackValues Stack;
  std::set<Key> Local;
  bool operator==(const FrameFacts &) const = default;
};

/// The general-purpose registers both x86-64 calling conventions preserve
/// across a call.  RSI and RDI are preserved only by the Microsoft one.
constexpr uint64_t kPreserved[] = {x86reg::RBX, x86reg::RSP, x86reg::RBP,
                                   x86reg::R12, x86reg::R13, x86reg::R14,
                                   x86reg::R15};

class X64TailFrameGuard {
  const BinaryImage &Image;
  size_t Remaining;
  /// The function's own code: the span of its blocks and its positive
  /// ownership metadata.  A switch's case code that only its unresolved table
  /// reaches lies there, though no block holds it.  The entry is a call
  /// target: a branch there recurses.
  ExceptionAddressRange OwnedCode;
  va_t CodeBegin = InvalidVA;
  va_t CodeEnd = 0;
  va_t Entry = InvalidVA;

  bool localCode(uint64_t Address) const {
    return Address != Entry &&
           ((Address >= CodeBegin && Address < CodeEnd) ||
            (OwnedCode.isValid() && Address >= OwnedCode.Begin &&
             Address < OwnedCode.End));
  }

  /// Whether the read-only data at \p Address starts a table of this
  /// function's own code: a 32-bit offset from \p Address to it, as a relative
  /// switch table holds, or its absolute address.  Writable memory, which the
  /// dynamic linker or the program may fill (a GOT slot, which names its PLT
  /// stub until bound), and code are no such table, nor is a slot whose
  /// pointer the loader supplies, such as the one a Control Flow Guard
  /// dispatch jumps through.
  bool localTable(uint64_t Address) const {
    const Section *Holder = Image.getSectionFor(Address);
    if (!Holder || Holder->isWritable() || Holder->isExecutable() ||
        Image.hasRuntimeCallablePointerSlotAt(Address))
      return false;
    if (const uint8_t *Bytes = Image.readVA(Address, sizeof(int32_t))) {
      int32_t Offset = 0;
      std::memcpy(&Offset, Bytes, sizeof(Offset));
      if (localCode(Address + static_cast<int64_t>(Offset)))
        return true;
    }
    if (const uint8_t *Bytes = Image.readVA(Address, sizeof(uint64_t))) {
      uint64_t Word = 0;
      std::memcpy(&Word, Bytes, sizeof(Word));
      return localCode(Word);
    }
    return false;
  }

  /// The 8-byte constants this instruction's temporaries hold: a RIP-relative
  /// operand lifts as the next instruction's address plus its displacement,
  /// and only the sum names what the operand addresses.
  std::map<uint64_t, uint64_t> TempValues;

  std::optional<uint64_t> valueOf(const NdVar &Value) const {
    if (Value.isConst() && Value.Size == sizeof(uint64_t))
      return Value.Offset;
    if (Value.isTemp())
      if (const auto It = TempValues.find(Value.Offset); It != TempValues.end())
        return It->second;
    return std::nullopt;
  }

  /// The constant \p Op computes from constants alone, if any.
  std::optional<uint64_t> folded(const LowOp &Op) const {
    if (Op.Output.Size != sizeof(uint64_t))
      return std::nullopt;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
      return valueOf(Op.Inputs[0]);
    if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
        Op.NumInputs == 2) {
      const auto Left = valueOf(Op.Inputs[0]);
      const auto Right = valueOf(Op.Inputs[1]);
      if (Left && Right)
        return Op.Opcode == NdOp::INT_ADD ? *Left + *Right : *Left - *Right;
    }
    return std::nullopt;
  }

  /// Whether \p Value may hold what FrameFacts::Local names.  A constant does
  /// when it starts a table of this function's code; a constant that is such
  /// an address marks only the result that computes it (folded above), since
  /// every RIP-relative operand starts from one.
  bool mayBeLocal(const FrameFacts &Facts, const NdVar &Value) const {
    if (const auto Constant = valueOf(Value))
      return localTable(*Constant);
    const auto K = keyOf(Value);
    return K && Facts.Local.count(*K);
  }

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

  static void forgetTemps(FrameFacts &Facts) {
    std::erase_if(Facts.Stack, [](const auto &Entry) {
      return Entry.first.first == VnodeSpace::TEMP;
    });
    std::erase_if(Facts.Local,
                  [](const Key &K) { return K.first == VnodeSpace::TEMP; });
  }

  /// Record whether \p Out may now hold what FrameFacts::Local names.  A
  /// partial or guarded write keeps what the rest of the container held.
  static void writeLocal(FrameFacts &Facts, const NdVar &Out, bool Local,
                         bool Guarded) {
    if (Out.isReg() && Out.Size > 8) {
      std::erase_if(Facts.Local, [&](const Key &K) {
        return K.first == VnodeSpace::REG && K.second < Out.Offset + Out.Size &&
               Out.Offset < K.second + 8;
      });
      if (Local)
        for (uint64_t Byte = Out.Offset & ~uint64_t{7};
             Byte < Out.Offset + Out.Size; Byte += 8)
          Facts.Local.insert({VnodeSpace::REG, Byte});
      return;
    }
    const auto K = keyOf(Out);
    if (!K)
      return;
    if (Local)
      Facts.Local.insert(*K);
    else if (!Guarded && whole(Out))
      Facts.Local.erase(*K);
  }

  /// Run \p Block from \p Facts.  \p Candidates collects the indirect jumps
  /// that may hand on the return address: the incoming stack pointer is in
  /// place, or nothing derived from it is, so the jump runs on another stack.
  bool transfer(const LowBlock &Block, FrameFacts &Facts,
                std::set<va_t> *Candidates) {
    StackValues &State = Facts.Stack;
    for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries) {
      forgetTemps(Facts);
      TempValues.clear();
      // A guarded instruction writes its outputs only on one of its paths.
      const bool Guarded = hasLowInstructionControlFlag(
          Boundary.ControlFlags, LowInstructionControlFlag::InstructionGuard);
      for (uint64_t I = 0; I < Boundary.OpCount; ++I) {
        if (!charge(1 + State.size() + Facts.Local.size()))
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
          const bool WithinFunction =
              Op.NumInputs == 1 && mayBeLocal(Facts, Op.Inputs[0]);
          if (Candidates && (Restored || LazyResolver) && !WithinFunction)
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
          // Under either convention a callee may leave RSI and RDI changed,
          // so only the registers both keep may still hold local code.
          std::erase_if(Facts.Local, [](const Key &K) {
            return K.first != VnodeSpace::REG || !preserved(K.second);
          });
          write(State, Op.Output, {}, false);
          writeLocal(Facts, Op.Output, false, false);
          continue;
        }
        // A constant result is local code or a table of it by itself;
        // anything else is marked by what it reads.
        const auto Constant = folded(Op);
        bool Local = false;
        if (Constant)
          Local = localCode(*Constant) || localTable(*Constant);
        for (unsigned In = 0; !Constant && In < Op.NumInputs && !Local; ++In)
          Local = mayBeLocal(Facts, Op.Inputs[In]);
        if (Op.Output.isTemp()) {
          if (Constant)
            TempValues[Op.Output.Offset] = *Constant;
          else
            TempValues.erase(Op.Output.Offset);
        }
        write(State, Op.Output, resultOf(State, Op), Guarded);
        writeLocal(Facts, Op.Output, Local, Guarded);
      }
    }
    forgetTemps(Facts);
    TempValues.clear();
    return true;
  }

  static FrameFacts meet(const FrameFacts &Left, const FrameFacts &Right) {
    FrameFacts Result = Left;
    for (auto &[K, Value] : Result.Stack) {
      const auto It = Right.Stack.find(K);
      if (It == Right.Stack.end() || It->second != Value)
        Value.reset();
    }
    for (const auto &[K, Value] : Right.Stack)
      Result.Stack.try_emplace(K, std::nullopt);
    Result.Local.insert(Right.Local.begin(), Right.Local.end());
    return Result;
  }

public:
  X64TailFrameGuard(const BinaryImage &Image, ExceptionAddressRange OwnedCode,
                    size_t MaxWork)
      : Image(Image), Remaining(MaxWork), OwnedCode(OwnedCode) {}

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
      CodeBegin = std::min(CodeBegin, Block.StartAddr);
      CodeEnd = std::max(CodeEnd, Block.EndAddr);
    }
    if (Entry < 0)
      return {};
    this->Entry = Function.Entry;

    std::vector<std::optional<FrameFacts>> In(Count);
    std::vector<int> Work;
    In[Entry] = FrameFacts{{{{VnodeSpace::REG, x86reg::RSP}, 0}}, {}};
    Work.push_back(Entry);
    // A landing pad or another analysis root enters on this function's stack
    // at an offset this proof has not followed, and any preserved register
    // may hold a frame address or one of this function's own labels.
    FrameFacts Unfollowed;
    for (uint64_t Register : kPreserved) {
      Unfollowed.Stack.emplace(Key{VnodeSpace::REG, Register}, std::nullopt);
      Unfollowed.Local.insert(Key{VnodeSpace::REG, Register});
    }
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
      FrameFacts Facts = *In[Id];
      if (!transfer(Function.Blocks[Id], Facts, nullptr))
        return {};
      for (int Succ : Function.Blocks[Id].Succs) {
        if (!charge(1 + Facts.Stack.size() + Facts.Local.size()))
          return {};
        FrameFacts Next = In[Succ] ? meet(*In[Succ], Facts) : Facts;
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
      FrameFacts Facts = *In[I];
      if (!transfer(Function.Blocks[I], Facts, &Candidates))
        return {};
    }
    return Candidates;
  }
};

} // namespace

std::set<va_t> restoredX64IndirectTailFrames(const LowFunc &Function,
                                             const BinaryImage &Image,
                                             ExceptionAddressRange OwnedCode,
                                             size_t MaxWork) {
  return X64TailFrameGuard(Image, OwnedCode, MaxWork).run(Function);
}

} // namespace neverd
