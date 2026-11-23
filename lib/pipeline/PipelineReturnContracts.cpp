//===- PipelineReturnContracts.cpp - Functions that return no value ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Settles once, for both C backends, which functions return no value their
/// callers could rely on.  The machine always leaves something in the return
/// register; C shows a function as void when what it leaves there is no
/// value it means.  The C writers cannot tell that apart on their own IR:
/// optimized LLVM IR folds the undefined incoming register a function hands
/// back to the same constant as a deliberate `return 0`.
///
/// A return hands back, on each path to it, one of
///  - nothing defined: an unspecified value, what a call that does not
///    return or an instruction run for its effect left, or the result of a
///    callee that itself returns no value;
///  - nothing defined, by a path that shows the function means no value:
///    the return register as the function entered it, when no parameter
///    arrives in it (no instruction on the way wrote it and no call was
///    made), or the result of a callee that returned and returns nothing, a
///    routine declared void or a function such a path of its own shows.
///    Compiled code that means to return a value has no such path;
///  - the parameter that arrived in the return register of a function that
///    never writes it (AArch64 x0 of a writer through its pointer argument):
///    a value only when a caller reads it;
///  - a deliberate value: a constant or a comparison, as `return 0` or a
///    `setcc` byte merged into the register;
///  - some other value.
/// A function returns no value when every path hands back nothing defined.
/// Unless a caller reads what a call to it returns, it also returns none
/// when a path shows it means none, whatever the others hold (a constant
/// beside such a path is what the code happened to hold, as `xor %eax,
/// %eax` to store a zero), or when a path hands back nothing defined and
/// none a deliberate value.
///
/// A caller reads a result where something it does depends on a byte of it
/// (ByteDemand): not the views of the return register the lift keeps in
/// step with it, nor the bytes a `setcc` merges a byte over where only that
/// byte goes on.  A caller that only hands the result back reads it where
/// it returns a value in the integer register itself, which is the other
/// half of the same question (ReturnContracts::settle).
///
/// What a declaration says decides first: a bound source signature, debug
/// information, or the MSVC rule that a destructor returns nothing; a
/// routine known by name returns what its prototype declares.  Code
/// generation keeps the register.
///
//===----------------------------------------------------------------------===//

#include "PipelineReturnModelingDetail.h"

#include "neverd/Common.h"
#include "neverd/backend/c/MsvcCallee.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/X86/X86FPState.h"
#include "neverd/ir/X86/X87Context.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/intrinsics/AArch64/AArch64CPA.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/intrinsics/IntrinsicValueEffects.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/med/IndirectCallSlot.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/MedNoReturn.h"
#include "neverd/ir/med/X86/I386PicAddress.h"
#include "neverd/ir/med/X86/X86RegistrationFrame.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/SymbolDecoration.h"
#include "neverd/loader/SymbolSpelling.h"
#include "neverd/pipeline/Limits.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"

#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {

namespace {

using ValueKey = std::tuple<MedVar::VarKind, int, int>;

ValueKey keyOf(const MedVar &Value) {
  return {Value.Kind, Value.Id, Value.SSAVer};
}

/// Hashes a value's key for the maps below.
struct ValueKeyInfo {
  static ValueKey getEmptyKey() {
    return {static_cast<MedVar::VarKind>(0xFE), -1, -1};
  }
  static ValueKey getTombstoneKey() {
    return {static_cast<MedVar::VarKind>(0xFF), -1, -1};
  }
  static unsigned getHashValue(const ValueKey &Key) {
    return static_cast<unsigned>(
        llvm::hash_combine(static_cast<unsigned>(std::get<0>(Key)),
                           std::get<1>(Key), std::get<2>(Key)));
  }
  static bool isEqual(const ValueKey &A, const ValueKey &B) { return A == B; }
};
template <typename T>
using ValueMap = llvm::DenseMap<ValueKey, T, ValueKeyInfo>;

enum class ReturnedValue : uint8_t {
  /// Nothing the function defines.
  Undefined,
  /// The return register as the function entered it, where no parameter
  /// arrives in it: no instruction on the way wrote it and no call was made.
  /// Nothing defined, and more: compiled code that means to return a value
  /// has no such path, so the path shows the function means none.
  Unwritten,
  /// What a call to a routine declared to return nothing left, handed back
  /// after the routine returned: it shows the same.
  Nothing,
  /// The parameter in a return register the function never writes.
  Incoming,
  /// A system call's result, which the call writes to the return register
  /// whatever the function means: a value where a caller reads it.
  SystemCallResult,
  /// The GOT address reloaded from its spill: no value the function means,
  /// unless a caller reads what a call to it returns.  A read the stack model
  /// places on the spill slot by mistake looks the same: after a
  /// struct-returning call that pops its hidden pointer, `vmsg` reads the
  /// result's first field four bytes from the slot.
  ReloadedGOTAddress,
  /// A constant or a comparison: a value the function means.
  Deliberate,
  /// What a call to a function this pass settles returns: nothing defined
  /// where that function returns no value, else some other value.
  CalleeResult,
  /// Any other value.
  Other,
};

/// What one path to a return hands back.
struct ReturnPath {
  ReturnedValue Kind = ReturnedValue::Other;
  /// The function whose result a CalleeResult path hands back, by its
  /// position among the functions this pass settles.
  unsigned Callee = ~0u;
  ReturnPath(ReturnedValue Kind, unsigned Callee = ~0u)
      : Kind(Kind), Callee(Callee) {}
};

/// One function's SSA definitions and what it does with its calls' results.
struct FunctionFacts {
  const MedFunc *Func = nullptr;
  ValueMap<const MedOp *> Defs;
  /// The intrinsic each bound output of a multi-output intrinsic comes from.
  ValueMap<const MedOp *> IntrinsicOutputDefs;
  ValueMap<const PhiNode *> Phis;
  /// How this function uses what its calls to one function of this image
  /// return: a direct call's target, or the routine an import's stub or slot
  /// reaches (ownRoutineOf).
  struct ResultUse {
    /// Something the function does depends on a byte of one (ByteDemand).
    bool Read = false;
    /// A return hands one back: directly, through the PHIs that merge it,
    /// or as the narrower or wider view of it a return reads.
    bool Returned = false;
  };
  std::map<va_t, ResultUse> CallResults;
  /// What the return register holds at each return; none for the value it
  /// entered the function with.
  std::vector<std::optional<MedVar>> Returns;
  /// Whether any operation or PHI writes the return register.  A return
  /// whose value no backward walk finds holds the entry value only when
  /// none does; otherwise a join can hide a value written before it.
  bool WritesReturnRegister = false;
  /// The calls whose result an operation reads though nothing depends on a
  /// byte of it, such as the rest of a register a `setcc` wrote a byte of.
  std::vector<const MedOp *> UnreadCalls;
  /// Those results with what their calls hand back
  /// (ReturnContracts::summarize).
  std::vector<std::pair<MedVar, ReturnPath>> UnreadResults;
  /// What each path to a return hands back (ReturnContracts::summarize).
  std::vector<ReturnPath> Paths;
  /// Where no path returns: whether that is proven (hasProvenNoReturnExit).
  bool NeverReturns = false;
};

/// The bytes of a value something depends on: bit N for byte N, up to the
/// 64 of the widest vector register.  A wider value is depended on whole.
using ByteMask = uint64_t;
constexpr unsigned kMaskBytes = 64;

ByteMask bytesOf(const MedVar &Value) {
  return !Value.Size || Value.Size >= kMaskBytes
             ? ~ByteMask(0)
             : (ByteMask(1) << Value.Size) - 1;
}

/// \p Bytes and every byte below its highest: what a carry chain reads.
ByteMask throughHighest(ByteMask Bytes) {
  return Bytes ? llvm::maskTrailingOnes<ByteMask>(llvm::Log2_64(Bytes) + 1) : 0;
}

/// Whether \p Op is run for more than the value it computes, or leaves that
/// value where no operation shows who reads it: a store, a call, a branch,
/// a load, a division, an intrinsic, and a write of anything but a register
/// or a temporary.  Such an operation reads its inputs whole whatever reads
/// its output.
bool readsInputsWhole(const MedOp &Op) {
  switch (Op.Opcode) {
  case NdOp::LOAD:
  case NdOp::STORE:
  case NdOp::ATOMIC_XCHG:
  case NdOp::ATOMIC_ADD:
  case NdOp::ATOMIC_CMPXCHG:
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
  case NdOp::BRANCH:
  case NdOp::COND_BR:
  case NdOp::INDIR_BR:
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
  case NdOp::RETURN:
  case NdOp::NOP:
  case NdOp::INTRINSIC:
    return true;
  default:
    break;
  }
  if (Op.MemoryOrdering != NdMemoryOrdering::None || !Op.Output.Size ||
      !Op.IntrinsicOutputs.empty())
    return true;
  return Op.Output.Kind != MedVar::Temp && Op.Output.Kind != MedVar::Reg;
}

/// The bytes of each input of \p Op that \p Bytes of its output depend on.
/// A copy, a view, a byte merge, a bitwise operation, a carry chain and a
/// left shift pass bytes on in place or upwards; any other operation reads
/// its inputs whole.
template <typename NeedFn, typename ZeroFn>
void inputDemand(const MedOp &Op, ByteMask Bytes, NeedFn &&Need,
                 ZeroFn &&ZeroBytes) {
  // A value the mask covers, and one a machine word holds.
  const auto Covered = [](const MedVar &Value) {
    return Value.Size && Value.Size <= kMaskBytes;
  };
  const auto Word = [](const MedVar &Value) {
    return Value.Size && Value.Size <= 8;
  };
  if (Covered(Op.Output))
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_NOT:
      if (Op.NumInputs == 1 && Op.Inputs[0].Size == Op.Output.Size) {
        Need(Op.Inputs[0], Bytes);
        return;
      }
      break;
    case NdOp::INT_ZEXT:
      if (Op.NumInputs == 1 && Covered(Op.Inputs[0])) {
        Need(Op.Inputs[0], Bytes & bytesOf(Op.Inputs[0]));
        return;
      }
      break;
    case NdOp::INT_SEXT:
      if (Op.NumInputs == 1 && Covered(Op.Inputs[0])) {
        // The bytes above the source repeat its sign.
        const ByteMask Low = bytesOf(Op.Inputs[0]);
        Need(Op.Inputs[0],
             (Bytes & Low) |
                 (Bytes & ~Low ? ByteMask(1) << (Op.Inputs[0].Size - 1) : 0));
        return;
      }
      break;
    case NdOp::SUBBYTES:
      if (Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
          Covered(Op.Inputs[0]) && Op.Inputs[1].ConstVal < kMaskBytes) {
        Need(Op.Inputs[0], Bytes << Op.Inputs[1].ConstVal);
        return;
      }
      break;
    case NdOp::CONCAT:
      // The second operand is the low part.
      if (Op.NumInputs == 2 && Covered(Op.Inputs[0]) && Covered(Op.Inputs[1]) &&
          Op.Inputs[1].Size < kMaskBytes) {
        Need(Op.Inputs[1], Bytes & bytesOf(Op.Inputs[1]));
        Need(Op.Inputs[0], Bytes >> Op.Inputs[1].Size);
        return;
      }
      break;
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
      if (Op.NumInputs == 2 && Word(Op.Output)) {
        for (unsigned I = 0; I < 2; ++I) {
          const MedVar &Other = Op.Inputs[1 - I];
          ByteMask Kept = Bytes;
          // A byte ANDed with zero or ORed with ones shows nothing of the
          // other operand: `and %eax, %ebx` reads only the low byte of an
          // EAX a `setcc` wrote where EBX holds a zero-extended byte.
          if (Other.isConst() && Op.Opcode == NdOp::INT_OR) {
            for (unsigned Byte = 0; Byte < 8; ++Byte)
              if (((Other.ConstVal >> (8 * Byte)) & 0xFF) == 0xFFu)
                Kept &= ~(ByteMask(1) << Byte);
          } else if (Op.Opcode == NdOp::INT_AND) {
            Kept &= ~ZeroBytes(Other);
          }
          Need(Op.Inputs[I], Kept);
        }
        return;
      }
      break;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_MULT:
      if (Op.NumInputs == 2 && Word(Op.Output)) {
        Need(Op.Inputs[0], throughHighest(Bytes));
        Need(Op.Inputs[1], throughHighest(Bytes));
        return;
      }
      break;
    case NdOp::INT_LEFT:
      // A left shift moves bits up only, whatever the amount.
      if (Op.NumInputs == 2 && Word(Op.Output)) {
        Need(Op.Inputs[0], throughHighest(Bytes));
        Need(Op.Inputs[1], bytesOf(Op.Inputs[1]));
        return;
      }
      break;
    default:
      break;
    }
  for (uint8_t I = 0; I < Op.NumInputs; ++I)
    Need(Op.Inputs[I], bytesOf(Op.Inputs[I]));
}

/// Which bytes of each value something the function does depends on.
///
/// An operation run for more than its value reads its inputs whole
/// (readsInputsWhole).  One that only computes a value reads what the
/// readers of that value need of it (inputDemand), and a PHI what its
/// readers need: the views of the return register a call leaves (`eax`,
/// `al`) and the PHIs that merge them read nothing where no operation reads
/// them, and the bytes a `setcc` merges a byte over read nothing where only
/// that byte is read.
///
/// No operation shows every reader.  A caller may take what a function
/// leaves in a register other than the integer return register
/// (MedFunc::MultiReturn, a floating-point result), and an exception
/// handler lifted apart from its function reads the registers the function
/// held when it threw.  So a value in such a register that nothing reads is
/// read whole, unless the function is known to return an integer and has no
/// exceptional edge (leavesOtherRegisters).  An unread value of the return
/// register or of a temporary reads nothing: what a return hands back is
/// known (returnRegisterAt), and a temporary goes nowhere else.
class ByteDemand {
public:
  ByteDemand(const TargetRegInfo &TRI, bool OtherRegistersRead)
      : TRI(TRI), OtherRegistersRead(OtherRegistersRead) {}

  void define(const PhiNode &Phi) {
    const unsigned Output = nodeOf(Phi.Output);
    Nodes[Output].Phi = &Phi;
    for (const auto &[Predecessor, Value] : Phi.Args) {
      (void)Predecessor;
      use(Value);
    }
    if (Phi.ExceptionalEntry)
      use(*Phi.ExceptionalEntry);
  }

  void define(const MedOp &Op) {
    if (Op.Output.Size && !Op.Output.isConst()) {
      const unsigned Output = nodeOf(Op.Output);
      if (!Nodes[Output].Op && !Nodes[Output].Phi)
        Nodes[Output].Op = &Op;
    }
    const bool Whole = readsInputsWhole(Op);
    for (uint8_t I = 0; I < Op.NumInputs; ++I) {
      use(Op.Inputs[I]);
      if (Whole)
        need(Op.Inputs[I], bytesOf(Op.Inputs[I]));
    }
  }

  /// Notes that an operation, a PHI or a return reads \p Value.
  void use(const MedVar &Value) {
    if (!Value.isConst())
      Nodes[nodeOf(Value)].Used = true;
  }

  void need(const MedVar &Value, ByteMask Bytes) {
    if (Value.isConst())
      return;
    Bytes = Value.Size > kMaskBytes && Bytes ? ~ByteMask(0)
                                             : Bytes & bytesOf(Value);
    const unsigned Index = nodeOf(Value);
    Node &N = Nodes[Index];
    if ((N.Bytes | Bytes) == N.Bytes)
      return;
    N.Bytes |= Bytes;
    if (!N.Pending) {
      N.Pending = true;
      Pending.push_back(Index);
    }
  }

  /// Reads whole each value nothing reads that something unseen may, then
  /// carries every need back to the values it rests on.
  void settle() {
    const auto Seen = [&](const MedVar &Value) {
      return Value.Kind == MedVar::Temp ||
             (Value.Kind == MedVar::Reg &&
              (Value.RegOff == TRI.IntReturnReg || !OtherRegistersRead));
    };
    for (size_t I = 0; I < Nodes.size(); ++I) {
      if (Nodes[I].Used)
        continue;
      if (const MedOp *Op = Nodes[I].Op) {
        if (!readsInputsWhole(*Op) && !Seen(Op->Output))
          need(Op->Output, bytesOf(Op->Output));
      } else if (const PhiNode *Phi = Nodes[I].Phi) {
        if (!Seen(Phi->Output))
          need(Phi->Output, bytesOf(Phi->Output));
      }
    }
    while (!Pending.empty()) {
      const unsigned Index = Pending.back();
      Pending.pop_back();
      Nodes[Index].Pending = false;
      const ByteMask Bytes = Nodes[Index].Bytes;
      if (const PhiNode *Phi = Nodes[Index].Phi) {
        for (const auto &[Predecessor, Value] : Phi->Args) {
          (void)Predecessor;
          need(Value, Bytes);
        }
        if (Phi->ExceptionalEntry)
          need(*Phi->ExceptionalEntry, Bytes);
        continue;
      }
      const MedOp *Op = Nodes[Index].Op;
      if (Op && !readsInputsWhole(*Op))
        inputDemand(
            *Op, Bytes,
            [&](const MedVar &Input, ByteMask Needed) { need(Input, Needed); },
            [&](const MedVar &Input) { return zeroBytes(Input); });
    }
  }

  bool reads(const MedVar &Value) const {
    const auto Found = Index.find(keyOf(Value));
    return Found != Index.end() && Nodes[Found->second].Bytes;
  }

  /// Whether an operation, a PHI or a return reads \p Value.
  bool used(const MedVar &Value) const {
    const auto Found = Index.find(keyOf(Value));
    return Found != Index.end() && Nodes[Found->second].Used;
  }

  /// The bytes of \p Value that are zero whatever the function runs on:
  /// those of a constant, those a zero extension adds, and what a copy, a
  /// view, a byte merge, a bitwise operation and a PHI make of them.  The
  /// values \p Value rests on are worked out together, each PHI starting as
  /// zero wherever its arms may be, until no answer changes: a flag ANDed
  /// around a loop stays the byte it started as.  Nothing is known past the
  /// bound.
  ByteMask zeroBytes(const MedVar &Value) {
    if (Value.isConst())
      return zeroBytesOf(Value);
    const auto Root = Index.find(keyOf(Value));
    if (Root == Index.end())
      return 0;
    if (const auto Known = Zero.find(Root->second); Known != Zero.end())
      return Known->second;
    std::vector<unsigned> Slice, Work{Root->second};
    llvm::DenseMap<unsigned, ByteMask> Answer;
    while (!Work.empty()) {
      const unsigned Each = Work.back();
      Work.pop_back();
      if (Answer.count(Each) || Zero.count(Each))
        continue;
      const Node &N = Nodes[Each];
      if (!N.Phi && !(N.Op && passesZeroBytes(*N.Op))) {
        Zero[Each] = 0;
        continue;
      }
      if (Slice.size() >=
          ::neverd::limits::pipeline::kPipelineReturnContractsZeroByteValues) {
        // Past the bound nothing in the slice is known.
        for (unsigned Seen : Slice)
          Zero[Seen] = 0;
        Zero[Each] = 0;
        return Zero[Root->second] = 0;
      }
      Slice.push_back(Each);
      Answer[Each] = N.Phi ? bytesOf(N.Phi->Output) : bytesOf(N.Op->Output);
      const auto Push = [&](const MedVar &Input) {
        if (Input.isConst())
          return;
        if (const auto Found = Index.find(keyOf(Input)); Found != Index.end())
          Work.push_back(Found->second);
      };
      if (N.Phi) {
        for (const auto &[Predecessor, Arm] : N.Phi->Args) {
          (void)Predecessor;
          Push(Arm);
        }
        if (N.Phi->ExceptionalEntry)
          Push(*N.Phi->ExceptionalEntry);
      } else {
        for (uint8_t I = 0; I < N.Op->NumInputs; ++I)
          Push(N.Op->Inputs[I]);
      }
    }
    const auto Of = [&](const MedVar &Input) -> ByteMask {
      if (Input.isConst())
        return zeroBytesOf(Input);
      const auto Found = Index.find(keyOf(Input));
      if (Found == Index.end())
        return 0;
      if (const auto Now = Answer.find(Found->second); Now != Answer.end())
        return Now->second;
      const auto Known = Zero.find(Found->second);
      return Known == Zero.end() ? ByteMask(0) : Known->second;
    };
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (unsigned Each : Slice) {
        const Node &N = Nodes[Each];
        ByteMask Now;
        if (N.Phi) {
          Now = bytesOf(N.Phi->Output);
          for (const auto &[Predecessor, Arm] : N.Phi->Args) {
            (void)Predecessor;
            Now &= Of(Arm);
          }
          if (N.Phi->ExceptionalEntry)
            Now &= Of(*N.Phi->ExceptionalEntry);
        } else {
          Now = zeroBytesThrough(*N.Op, Of);
        }
        Now &= Answer[Each];
        Changed |= Now != Answer[Each];
        Answer[Each] = Now;
      }
    }
    for (unsigned Each : Slice)
      Zero[Each] = Answer[Each];
    return Zero[Root->second];
  }

private:
  struct Node {
    const MedOp *Op = nullptr;
    const PhiNode *Phi = nullptr;
    ByteMask Bytes = 0;
    /// An operation, a PHI or a return reads it.
    bool Used = false;
    bool Pending = false;
  };

  /// The zero bytes of the constant \p Value, among the bytes it has.
  static ByteMask zeroBytesOf(const MedVar &Value) {
    ByteMask Zero = 0;
    for (unsigned Byte = 0; Byte < 8 && (!Value.Size || Byte < Value.Size);
         ++Byte)
      if (!((Value.ConstVal >> (8 * Byte)) & 0xFF))
        Zero |= ByteMask(1) << Byte;
    return Zero;
  }

  /// Whether zeroBytesThrough knows what \p Op makes of zero bytes.
  static bool passesZeroBytes(const MedOp &Op) {
    if (!Op.Output.Size || Op.Output.Size > 8 ||
        Op.MemoryOrdering != NdMemoryOrdering::None)
      return false;
    const auto Narrow = [](const MedVar &Value) {
      return Value.Size && Value.Size <= 8;
    };
    switch (Op.Opcode) {
    case NdOp::COPY:
      return Op.NumInputs == 1 && Op.Inputs[0].Size == Op.Output.Size;
    case NdOp::INT_ZEXT:
      return Op.NumInputs == 1 && Narrow(Op.Inputs[0]);
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
      return Op.NumInputs == 2;
    case NdOp::SUBBYTES:
      return Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
             Narrow(Op.Inputs[0]) && Op.Inputs[1].ConstVal < 8;
    case NdOp::CONCAT:
      return Op.NumInputs == 2 && Narrow(Op.Inputs[0]) &&
             Narrow(Op.Inputs[1]) && Op.Inputs[1].Size < 8;
    default:
      return false;
    }
  }

  /// The zero bytes of what \p Op computes, given its inputs' by \p Of.
  template <typename OfFn>
  static ByteMask zeroBytesThrough(const MedOp &Op, OfFn &&Of) {
    const ByteMask Output = bytesOf(Op.Output);
    switch (Op.Opcode) {
    case NdOp::COPY:
      return Of(Op.Inputs[0]) & Output;
    case NdOp::INT_ZEXT: {
      const ByteMask Low = bytesOf(Op.Inputs[0]);
      return ((Of(Op.Inputs[0]) & Low) | ~Low) & Output;
    }
    case NdOp::INT_AND:
      return (Of(Op.Inputs[0]) | Of(Op.Inputs[1])) & Output;
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
      return Of(Op.Inputs[0]) & Of(Op.Inputs[1]) & Output;
    case NdOp::SUBBYTES:
      return (Of(Op.Inputs[0]) >> Op.Inputs[1].ConstVal) & Output;
    case NdOp::CONCAT:
      return ((Of(Op.Inputs[0]) << Op.Inputs[1].Size) |
              (Of(Op.Inputs[1]) & bytesOf(Op.Inputs[1]))) &
             Output;
    default:
      return 0;
    }
  }

  unsigned nodeOf(const MedVar &Value) {
    const auto [Found, New] =
        Index.try_emplace(keyOf(Value), static_cast<unsigned>(Nodes.size()));
    if (New)
      Nodes.emplace_back();
    return Found->second;
  }

  const TargetRegInfo &TRI;
  /// Whether something no operation shows may read a register other than
  /// the integer return register (leavesOtherRegisters).
  bool OtherRegistersRead;
  ValueMap<unsigned> Index;
  std::vector<Node> Nodes;
  std::vector<unsigned> Pending;
  /// The zero bytes of the values asked about, by node (zeroBytes).
  llvm::DenseMap<unsigned, ByteMask> Zero;
};

/// A live-in self-copy names the value the register already holds.
bool writesReturnRegister(const MedOp &Op, const MedVar &Value,
                          const TargetRegInfo &TRI) {
  return Value.Kind == MedVar::Reg && Value.RegOff == TRI.IntReturnReg &&
         !(Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
           Op.Inputs[0] == Value);
}

/// What the return register holds at the RETURN \p Index of \p Block: the
/// RETURN's operand where that is the register, and any x86 RETURN's operand,
/// which copy propagation may have left reading the register's source (a
/// temporary, a constant, or another register: `mov %rdi, %rax; ret` reads
/// RDI); else the last full write to it on the way there.  None for the
/// value the function entered with.  On ARM and AArch64 the operand is the
/// return address.
std::optional<MedVar> returnRegisterAt(const MedFunc &Func,
                                       const MedBlock &Block, size_t Index,
                                       const TargetRegInfo &TRI, Arch Target) {
  const MedOp &Return = Block.Ops[Index];
  if (Return.NumInputs >= 1) {
    const MedVar &Operand = Return.Inputs[0];
    if (Operand.Kind == MedVar::Reg && Operand.RegOff == TRI.IntReturnReg)
      return Operand;
    if (Target == Arch::X86 || Target == Arch::X64)
      return Operand;
  }
  auto Writes = [&](const MedVar &Value) {
    return Value.Kind == MedVar::Reg && Value.RegOff == TRI.IntReturnReg &&
           Value.Size == TRI.FullRegWidth;
  };
  std::map<int, const MedBlock *> ById;
  for (const MedBlock &Each : Func.Blocks)
    ById.emplace(Each.Id, &Each);
  std::set<int> Visited;
  const MedBlock *Current = &Block;
  size_t End = Index;
  while (Current && Visited.insert(Current->Id).second) {
    for (size_t I = End; I-- > 0;) {
      const MedOp &Op = Current->Ops[I];
      if (Op.Output.Size == TRI.FullRegWidth &&
          writesReturnRegister(Op, Op.Output, TRI))
        return Op.Output;
    }
    for (const PhiNode &Phi : Current->Phis)
      if (Writes(Phi.Output))
        return Phi.Output;
    // Without a PHI, every path in carries one value: the one before the
    // only predecessor's end, or the entry value.
    if (Current->Preds.size() != 1)
      break;
    const auto Predecessor = ById.find(Current->Preds.front());
    Current = Predecessor == ById.end() ? nullptr : Predecessor->second;
    End = Current ? Current->Ops.size() : 0;
  }
  return std::nullopt;
}

/// The constant addresses \p Call reaches its callee by: a direct call's
/// target, or the slot an indirect call reads it from (`call *slot`, or a load
/// of the slot it calls through), given each value's definition \p Defs.
struct CallAddress {
  std::optional<va_t> Target, Slot;
};
CallAddress callAddressOf(const MedFunc &Func, const MedOp &Call,
                          const ValueMap<const MedOp *> &Defs) {
  CallAddress Address;
  if (Call.NumInputs < 1)
    return Address;
  const MedVar &Callee = Call.Inputs[0];
  if (Call.Opcode == NdOp::CALL && Callee.isConst())
    Address.Target = static_cast<va_t>(Callee.ConstVal);
  else if (Call.Opcode == NdOp::INDIR_CALL)
    Address.Slot = indirectCallSlot(Func, Call, [&](const MedVar &Value) {
      const auto Definition = Defs.find(keyOf(Value));
      return Definition == Defs.end() ? nullptr : Definition->second;
    });
  return Address;
}

/// The routine of this image a call by \p Address reaches through the stub
/// or slot of an import the loader starts at it (importOwnRoutine): the C
/// calls that routine, so its contract is the call's.
std::optional<va_t> ownRoutineOf(const BinaryImage &Img,
                                 const std::map<va_t, va_t> &ArgumentSources,
                                 const CallAddress &Address) {
  if (!Address.Target && !Address.Slot)
    return std::nullopt;
  return importOwnRoutine(Img, &ArgumentSources,
                          Address.Target ? *Address.Target : *Address.Slot);
}

/// Whether something no operation of \p Func shows may read what it holds
/// in a register other than the integer return register: a caller, where
/// the function returns a record of registers or a floating-point or
/// vector value, or nothing yet says it returns an integer; and an
/// exception handler, where the function has an exceptional edge.
bool leavesOtherRegisters(const MedFunc &Func) {
  if (!Func.MultiReturn.empty() || Func.FPReturnViaX87 ||
      Func.ExplicitX87ReturnValue || Func.ReturnsVectorRegister)
    return true;
  TypeRef Type = Func.ReturnType;
  if (Func.SourceTypeHint && Func.SourceTypeHint->ReturnType)
    Type = Func.SourceTypeHint->ReturnType;
  if (!Type ||
      (Type->Kind != NdTypeKind::Int && Type->Kind != NdTypeKind::Ptr &&
       Type->Kind != NdTypeKind::Void))
    return true;
  return llvm::any_of(Func.Blocks, [](const MedBlock &Block) {
    return !Block.ExceptionalSuccs.empty() || !Block.ExceptionalPreds.empty();
  });
}

FunctionFacts collectFacts(const MedFunc &Func, const TargetRegInfo &TRI,
                           const BinaryImage &Img,
                           const std::map<va_t, va_t> &ArgumentSources) {
  const Arch Target = Img.Arch;
  FunctionFacts Facts;
  Facts.Func = &Func;
  std::vector<const MedOp *> Calls;
  ByteDemand Demand(TRI, leavesOtherRegisters(Func));
  std::vector<MedVar> HandedBack;
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis) {
      Facts.WritesReturnRegister |= Phi.Output.Kind == MedVar::Reg &&
                                    Phi.Output.RegOff == TRI.IntReturnReg;
      Facts.Phis.try_emplace(keyOf(Phi.Output), &Phi);
      Demand.define(Phi);
    }
    for (const MedOp &Op : Block.Ops) {
      Facts.WritesReturnRegister |=
          writesReturnRegister(Op, Op.Output, TRI) ||
          llvm::any_of(Op.IntrinsicOutputs, [&](const MedVar &Output) {
            return writesReturnRegister(Op, Output, TRI);
          });
      if (Op.Output.Size && !Op.Output.isConst())
        Facts.Defs.try_emplace(keyOf(Op.Output), &Op);
      for (const MedVar &Output : Op.IntrinsicOutputs)
        if (!Output.isConst())
          Facts.IntrinsicOutputDefs.try_emplace(keyOf(Output), &Op);
      if (Op.Opcode == NdOp::RETURN) {
        if (isRegistrationDisplayRuntimeReturn(Func, Op.Addr))
          continue;
        const auto Returned = returnRegisterAt(
            Func, Block, static_cast<size_t>(&Op - Block.Ops.data()), TRI,
            Target);
        Facts.Returns.push_back(Returned);
        if (Returned && !Returned->isConst()) {
          HandedBack.push_back(*Returned);
          Demand.use(*Returned);
        }
        continue;
      }
      Demand.define(Op);
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Op.Output.Size && !Op.Output.isConst())
        Calls.push_back(&Op);
    }
  }
  // What a return hands back: a call's result as it comes, through the PHIs
  // that merge it and the views of it a return reads (classify sees through
  // the same); any other value is one the function computes, read whole.
  ValueMap<bool> Returned;
  while (!HandedBack.empty()) {
    const MedVar Value = HandedBack.back();
    HandedBack.pop_back();
    if (Value.isConst() || !Returned.try_emplace(keyOf(Value), true).second)
      continue;
    if (const auto Phi = Facts.Phis.find(keyOf(Value));
        Phi != Facts.Phis.end()) {
      for (const auto &[Predecessor, Arm] : Phi->second->Args) {
        (void)Predecessor;
        HandedBack.push_back(Arm);
      }
      continue;
    }
    const auto Def = Facts.Defs.find(keyOf(Value));
    if (Def == Facts.Defs.end())
      continue;
    const MedOp &Op = *Def->second;
    const bool View =
        ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
          Op.Opcode == NdOp::INT_SEXT) &&
         Op.NumInputs == 1) ||
        (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
         Op.Inputs[1].isConst() && Op.Inputs[1].ConstVal == 0);
    if (View && !readsInputsWhole(Op))
      HandedBack.push_back(Op.Inputs[0]);
    else if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
      Demand.need(Value, bytesOf(Value));
  }
  Demand.settle();
  // A call's callee is known once every definition is: an indirect call may
  // load its slot in a later block.
  for (const MedOp *Call : Calls) {
    // Whether a function returns a value says nothing of what it leaves in
    // another register, which a call may be given as its result.
    if (Call->Output.Kind == MedVar::Reg &&
        Call->Output.RegOff == TRI.IntReturnReg &&
        !Demand.reads(Call->Output) && Demand.used(Call->Output))
      Facts.UnreadCalls.push_back(Call);
    const CallAddress Address = callAddressOf(Func, *Call, Facts.Defs);
    std::optional<va_t> Callee = Address.Target;
    if (const std::optional<va_t> Own =
            ownRoutineOf(Img, ArgumentSources, Address))
      Callee = Own;
    if (!Callee)
      continue;
    FunctionFacts::ResultUse &Use = Facts.CallResults[*Callee];
    Use.Read |= Demand.reads(Call->Output);
    Use.Returned |= Returned.count(keyOf(Call->Output)) != 0;
  }
  return Facts;
}

bool isComparison(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::BOOL_NOT:
  case NdOp::FLOAT_EQUAL:
  case NdOp::FLOAT_NOTEQUAL:
  case NdOp::FLOAT_LESS:
  case NdOp::FLOAT_LESSEQUAL:
  case NdOp::FLOAT_ISNAN:
    return true;
  default:
    return false;
  }
}

class ReturnContracts {
public:
  ReturnContracts(const BinaryImage &Img, PipelineResult &Result,
                  const DebugContext *Dbg)
      : Img(Img), Result(Result), Dbg(Dbg), TRI(getTargetRegInfo(Img.Arch)) {
    // One answer for each entry: the first function at it.
    for (size_t I = 0; I < Result.MedFuncs.size(); ++I) {
      const MedFunc &Func = Result.MedFuncs[I];
      if (!IndexOf.try_emplace(Func.Entry, States.size()).second)
        continue;
      State &S = States.emplace_back();
      S.Func = &Func;
      S.Function = I;
      S.Declared = declaredNoValue(Func);
      S.Settles = !S.Declared && settles(Func);
      S.IntegerResult = returnsInIntegerRegister(Func);
      if (S.Declared)
        Declared.emplace(Func.Entry, *S.Declared);
      if (!Func.Name.empty())
        Names.emplace(Func.Entry, Func.Name);
    }
    // Each function's facts read that function alone.
    const std::map<va_t, va_t> &ArgumentSources = Result.callArgumentSources();
    parallelForEach(States.size(), [&](auto Claim, size_t Total) {
      for (size_t I; (I = Claim()) < Total;)
        States[I].Facts =
            collectFacts(*States[I].Func, TRI, Img, ArgumentSources);
    });
    for (size_t I = 0; I < States.size(); ++I) {
      summarize(States[I]);
      for (const auto &[Callee, Use] : States[I].Facts.CallResults)
        if (const auto Found = IndexOf.find(Callee); Found != IndexOf.end()) {
          States[Found->second].Callers.push_back(
              {static_cast<unsigned>(I), Use});
          if (Use.Returned)
            States[I].HandsBack.push_back(static_cast<unsigned>(Found->second));
        }
      for (const ReturnPath &Path : States[I].Facts.Paths)
        if (Path.Kind == ReturnedValue::CalleeResult)
          States[Path.Callee].PathCallers.push_back(static_cast<unsigned>(I));
    }
  }

  /// A function that hands a callee's result back returns a value only if
  /// the callee does, and the callee's result is read only if that function
  /// returns one: each answer rests on the other.  Three rules keep them
  /// apart.
  ///
  /// That a path hands back nothing defined is shown from the paths up: a
  /// callee counts as returning none only once it is settled so, so a
  /// function that returns what its own call returns shows nothing about
  /// itself (ground).
  ///
  /// That a caller reads nothing of a result it hands back is assumed only
  /// of a function one of whose paths shows it means no value: a return
  /// that never wrote the register, or the result of a callee that returns
  /// nothing after returning.  Every function this pass settles starts as
  /// one that may return none; the answers that supports are worked out,
  /// the functions it does not support leave it, and the rounds end when
  /// what is left is supported.  The assumption only shrinks.  Of any other
  /// function, which may be handing back what its caller passed it, a
  /// caller that hands the result back reads it until it is itself settled
  /// to return none (answer).
  ///
  /// A read reaches down: where something reads a function's result, the
  /// results that function hands back are read too (passReadsDown).
  void settle() {
    std::vector<unsigned> Candidates;
    for (size_t I = 0; I < States.size(); ++I) {
      State &S = States[I];
      if (S.Declared)
        S.NoValue = *S.Declared;
      else if (S.Settles)
        Candidates.push_back(static_cast<unsigned>(I));
      S.Assumed = S.Settles || handsBackNothing(S);
    }
    bool Supported = false;
    unsigned Round = 0;
    for (; Round <
               ::neverd::limits::pipeline::kPipelineReturnContractsMaxRounds &&
           !Supported;
         ++Round) {
      passReadsDown();
      ground(Candidates);
      Supported = true;
      for (unsigned I : Candidates)
        if (States[I].Assumed && !States[I].NoValue) {
          States[I].Assumed = false;
          Supported = false;
        }
    }
    // An image the bound stops gets the answers no assumption supports.
    if (!Supported) {
      for (unsigned I : Candidates)
        States[I].Assumed = false;
      passReadsDown();
      ground(Candidates);
    }
    for (MedFunc &Func : Result.MedFuncs)
      Func.ReturnsNoValue = States[IndexOf.find(Func.Entry)->second].NoValue;
    zeroUnreadResults();
  }

private:
  /// A caller's use of what its calls to a function return.
  struct Caller {
    unsigned Index;
    FunctionFacts::ResultUse Use;
  };
  /// One function, by its entry.
  struct State {
    const MedFunc *Func = nullptr;
    /// Its place in the pipeline result.
    size_t Function = 0;
    FunctionFacts Facts;
    /// What its declaration says: true for none.
    std::optional<bool> Declared;
    /// Whether this pass settles it: nothing declares it and its type is an
    /// integer the lift inferred (settles).
    bool Settles = false;
    /// Whether what it returns may be what the integer return register
    /// holds (returnsInIntegerRegister).
    bool IntegerResult = true;
    /// The callers of this image, in address order.
    std::vector<Caller> Callers;
    /// The functions a path of which hands back what this one returns.
    std::vector<unsigned> PathCallers;
    /// The functions whose result a return of this one hands back.
    std::vector<unsigned> HandsBack;
    /// The answer: it returns no value.
    bool NoValue = false;
    /// Whether a path of its own shows that: one that never wrote the
    /// register, or the result of a callee a path of which shows it.
    bool Shown = false;
    /// Whether it is taken to read nothing of a result it hands back while
    /// the answers are worked out: it returns no value, or returns one in
    /// another register.
    bool Assumed = false;
    /// Whether something reads its result under that assumption
    /// (passReadsDown).
    bool ReadDown = false;
  };

  const BinaryImage &Img;
  PipelineResult &Result;
  const DebugContext *Dbg;
  const TargetRegInfo &TRI;
  std::vector<State> States;
  std::map<va_t, size_t> IndexOf;
  /// What each declared function's declaration says: true for none.
  std::map<va_t, bool> Declared;
  std::map<va_t, std::string> Names;

  /// Works out what each path to a return of \p S hands back, once: only a
  /// callee's answer changes it afterwards (ReturnedValue::CalleeResult).
  void summarize(State &S) {
    FunctionFacts &F = S.Facts;
    if (S.Settles) {
      for (const std::optional<MedVar> &Returned : F.Returns) {
        if (!Returned) {
          F.Paths.push_back(!entersReturnRegister(*S.Func)
                                ? ReturnedValue::Unwritten
                            : F.WritesReturnRegister ? ReturnedValue::Other
                                                     : ReturnedValue::Incoming);
          continue;
        }
        std::set<ValueKey> Seen;
        classify(F, *Returned, F.Paths, Seen, 0);
      }
      // No ordinary RETURN is not a proof of no return: an external catch
      // funclet can resume its parent and supply a value after attachment.
      // Use the shared proof, which includes exceptional edges and missing
      // handlers.
      if (F.Paths.empty())
        F.NeverReturns =
            S.Func->DoesNotReturn || hasProvenNoReturnExit(*S.Func, Img.Arch);
    }
    for (const MedOp *Call : F.UnreadCalls) {
      const ReturnPath Path = callPath(F, *Call);
      if (Path.Kind == ReturnedValue::Undefined ||
          Path.Kind == ReturnedValue::Nothing ||
          Path.Kind == ReturnedValue::CalleeResult)
        F.UnreadResults.emplace_back(Call->Output, Path);
    }
    F.UnreadCalls = {};
    F.Defs = ValueMap<const MedOp *>();
    F.IntrinsicOutputDefs = ValueMap<const MedOp *>();
    F.Phis = ValueMap<const PhiNode *>();
    F.Returns = {};
  }

  /// A call that returns no value leaves no result to read, and the C shows
  /// a read of one as unknown.  Where an operation reads such a result but
  /// nothing depends on a byte of it (ByteDemand), the operation reads zero
  /// instead: the bytes it took from the result reach nothing.
  void zeroUnreadResults() {
    for (State &S : States) {
      ValueMap<bool> Unread;
      for (const auto &[Value, Path] : S.Facts.UnreadResults)
        if (Path.Kind != ReturnedValue::CalleeResult ||
            States[Path.Callee].NoValue)
          Unread.try_emplace(keyOf(Value), true);
      if (Unread.empty())
        continue;
      for (MedBlock &Block : Result.MedFuncs[S.Function].Blocks)
        for (MedOp &Op : Block.Ops) {
          if (Op.Opcode == NdOp::RETURN)
            continue;
          for (uint8_t I = 0; I < Op.NumInputs; ++I)
            if (!Op.Inputs[I].isConst() && Unread.count(keyOf(Op.Inputs[I])))
              Op.Inputs[I] = MedVar::makeConst(0, Op.Inputs[I].Size);
        }
    }
  }

  /// Whether \p S hands back nothing a caller could read whatever its
  /// returns hold: it returns no value, or returns one in another register.
  static bool handsBackNothing(const State &S) {
    return S.NoValue || !S.IntegerResult;
  }

  /// Marks each function whose result something reads while the assumption
  /// stands: a caller reads it, or hands it back and is taken to return a
  /// value, or hands it back and has its own result read.
  void passReadsDown() {
    std::vector<unsigned> Work;
    for (size_t I = 0; I < States.size(); ++I) {
      State &S = States[I];
      S.ReadDown = llvm::any_of(S.Callers, [&](const Caller &C) {
        return C.Use.Read || (C.Use.Returned && !States[C.Index].Assumed);
      });
      if (S.ReadDown)
        Work.push_back(static_cast<unsigned>(I));
    }
    while (!Work.empty()) {
      const unsigned Reader = Work.back();
      Work.pop_back();
      // What is read of a function that returns in another register is not
      // the integer register it hands back.
      if (!States[Reader].IntegerResult)
        continue;
      for (unsigned Callee : States[Reader].HandsBack)
        if (!States[Callee].ReadDown) {
          States[Callee].ReadDown = true;
          Work.push_back(Callee);
        }
    }
  }

  /// Settles which of \p Candidates return no value, from the paths up: a
  /// function joins once its own paths, the callees already settled and the
  /// callers that read it show it.  An answer changes what the callers that
  /// hand the function's result back return and what the callees whose
  /// result it hands back are read by.
  void ground(const std::vector<unsigned> &Candidates) {
    for (unsigned I : Candidates)
      States[I].NoValue = States[I].Shown = false;
    std::vector<unsigned> Work(Candidates.rbegin(), Candidates.rend());
    while (!Work.empty()) {
      State &S = States[Work.back()];
      Work.pop_back();
      if (!S.Settles)
        continue;
      const Answer Now = answer(S);
      if (Now.NoValue == S.NoValue && Now.Shown == S.Shown)
        continue;
      S.NoValue = Now.NoValue;
      S.Shown = Now.Shown;
      for (unsigned Reader : S.PathCallers)
        Work.push_back(Reader);
      for (unsigned Callee : S.HandsBack)
        Work.push_back(Callee);
    }
  }

  /// Whether what \p Func returns may be what the integer return register
  /// holds.  Not where its result is a floating-point or vector value on a
  /// target that returns those in a register of their own (x86-64 XMM0,
  /// AArch64 V0): a return of such a function hands the integer register
  /// back as the last call left it (`sqlite3_column_double` after
  /// `sqlite3_mutex_leave`), which reads nothing.
  bool returnsInIntegerRegister(const MedFunc &Func) const {
    if (Img.Arch != Arch::X64 && Img.Arch != Arch::AArch64)
      return true;
    TypeRef Type = Func.ReturnType;
    if (Func.SourceTypeHint && Func.SourceTypeHint->ReturnType)
      Type = Func.SourceTypeHint->ReturnType;
    else if (Dbg)
      if (const auto Debug = Dbg->resolveFunction(Func.Entry);
          Debug && Debug->ReturnType)
        Type = Debug->ReturnType;
    return !Type || (Type->Kind != NdTypeKind::Float &&
                     Type->Kind != NdTypeKind::Vector);
  }

  /// Whether \p Func's declaration says it returns nothing, as the C
  /// writers read it: a bound source signature, else debug information, else
  /// the target ABI/runtime prototype of an unpatched definition. None when
  /// nothing declares it.
  std::optional<bool> declaredNoValue(const MedFunc &Func) const {
    if (Func.SourceTypeHint && Func.SourceTypeHint->ReturnType)
      return Func.SourceTypeHint->ReturnType->Kind == NdTypeKind::Void;
    if (Dbg)
      if (const auto Debug = Dbg->resolveFunction(Func.Entry);
          Debug && Debug->ReturnType)
        return Debug->ReturnType->Kind == NdTypeKind::Void;
    if (isMsvcDestructorName(Func.Name))
      return true;
    // The program's prototype of a definition is resolveFunction's, which
    // refuses one an alias the linker folded with it contradicts (KeClearEvent
    // and KeResetEvent): the name alone does not choose between them.
    if (!Img.functionHasBytePatch(Func.Entry))
      return namedNoValue(Func.Name, /*ByProgramPrototype=*/false);
    return std::nullopt;
  }

  /// Whether a routine called \p Name returns nothing by its prototype, the
  /// libc tables' or, with \p ByProgramPrototype, one the program's types
  /// declare, or as an MSVC destructor.  None when none knows it.
  std::optional<bool> namedNoValue(llvm::StringRef Name,
                                   bool ByProgramPrototype = true) const {
    if (Name.empty())
      return std::nullopt;
    if (isMsvcDestructorName(Name))
      return true;
    // The generic and AArch64 ELF C++ ABIs declare these as void. ARM32
    // and Apple ARM64 instead have a machine `this` result; leave those to
    // their own contracts. A substring such as D1 in a normal name is not
    // a destructor: require a complete mangling parse.
    if (Img.Format == BinaryFormat::ELF &&
        (Img.Arch == Arch::AArch64 || Img.Arch == Arch::X64 ||
         Img.Arch == Arch::X86) &&
        isItaniumConstructorOrDestructor(Name))
      return true;
    const std::string CName =
        cNameOfStatedName(Name, Img.Format, Img.Arch).str();
    if (const std::optional<bool> Returns =
            libc::libcReturnsValue(CName, libc::libcVariantOf(Img)))
      return !*Returns;
    if (Dbg && ByProgramPrototype)
      if (const std::optional<FunctionSym> Declared =
              Dbg->resolvePrototype(CName);
          Declared && Declared->ReturnType)
        return Declared->ReturnType->Kind == NdTypeKind::Void;
    return std::nullopt;
  }

  /// Whether this settles the function's return type: an integer one the lift
  /// inferred, not a declaration's, a floating-point or an aggregate one.
  static bool settles(const MedFunc &Func) {
    return !Func.SourceTypeHint && !Func.SourceParametersBound &&
           Func.MultiReturn.empty() && !Func.FPReturnViaX87 &&
           (!Func.ReturnType || Func.ReturnType->Kind == NdTypeKind::Int);
  }

  /// The import a call reaches through \p Slot or the stub at it, if any.
  const Import *importAt(va_t Address) const {
    if (const Import *Imp = Img.findImportAt(Address))
      return Imp;
    return Img.findImportStubAt(Address);
  }

  /// What the call \p Call hands back: nothing defined where it does not
  /// return or its callee is declared to return none, a value where the
  /// callee is declared to return one, and the callee's own answer where
  /// this pass settles it.
  ReturnPath callPath(const FunctionFacts &F, const MedOp &Call) const {
    const auto Known = [](bool NoValue) {
      return ReturnPath(NoValue ? ReturnedValue::Nothing
                                : ReturnedValue::Other);
    };
    // A call that does not return leaves nothing, and shows nothing of the
    // function: no return follows it.
    if (Call.DoesNotReturn)
      return ReturnPath(ReturnedValue::Undefined);
    if (Call.NumInputs < 1)
      return Known(false);
    const CallAddress Address = callAddressOf(*F.Func, Call, F.Defs);
    std::optional<va_t> Target = Address.Target, Slot = Address.Slot;
    // A call through the stub or slot of an import the loader starts at
    // this image's own routine calls that routine, as the C prints it.
    if (const std::optional<va_t> Own =
            ownRoutineOf(Img, Result.callArgumentSources(), Address)) {
      Target = Own;
      Slot.reset();
    }
    // A callee's declaration decides, then what its name says, then what
    // this pass settled for it; the program's prototype of a definition is
    // its declaration's.  One this pass does not decompile is declared as the
    // C writers read it, by the debug information or a prototype of its
    // name: the call they print returns nothing then.
    if (Target) {
      const auto Function = IndexOf.find(*Target);
      if (const auto Declaration = Declared.find(*Target);
          Declaration != Declared.end())
        return Known(Declaration->second);
      if (Dbg && Function == IndexOf.end())
        if (const auto Debug = Dbg->resolveFunction(*Target);
            Debug && Debug->Addr == *Target && Debug->ReturnType)
          return Known(Debug->ReturnType->Kind == NdTypeKind::Void);
      if (const auto Name = Names.find(*Target);
          Name != Names.end() && !Img.functionHasBytePatch(*Target))
        if (const std::optional<bool> None =
                namedNoValue(Name->second, /*ByProgramPrototype=*/false))
          return Known(*None);
      // A function whose type is not this pass's to settle returns a value.
      if (Function != IndexOf.end())
        return States[Function->second].Settles
                   ? ReturnPath(ReturnedValue::CalleeResult,
                                static_cast<unsigned>(Function->second))
                   : Known(false);
    }
    if (const Import *Imp =
            importAt(Target ? *Target : Slot.value_or(InvalidVA)))
      return Known(namedNoValue(Imp->Name).value_or(false));
    // A loaded GOT slot names its import through the canonical storage view
    // (`mov rax, [slot]; jmp rax`).
    if (Slot) {
      const ImportStorageSlotCollection Storage =
          Img.collectImportStorageSlot(*Slot);
      if (const auto It = Storage.Slots.find(*Slot);
          It != Storage.Slots.end() && !Storage.Conflicts.count(*Slot) &&
          It->second.Addend == 0)
        return Known(namedNoValue(It->second.Name).value_or(false));
    }
    return Known(false);
  }

  /// The numbers the i386 PC-rooted proof gives the values of the function
  /// last asked about (MedToHighConverter::i386PcRootedNumbers).
  mutable const MedFunc *PcRootedFunc = nullptr;
  mutable std::map<VarKey, uint32_t> PcRootedNumbers;

  /// How \p Value is the linked GOT's address that an authenticated i386
  /// get-PC sequence computes (`call 1f; 1: pop %eax; add $GOT-1b, %eax`),
  /// the base the function addresses its data from: computed from the PC
  /// alone, or reloaded from a frame slot every write of which stores it.
  enum class GOTAddressKind : uint8_t { None, Computed, Reloaded };
  GOTAddressKind i386GOTAddress(const FunctionFacts &F,
                                const MedVar &Value) const {
    if (Img.Arch != Arch::X86 || !F.Func || F.Func->I386GetPcModels.empty())
      return GOTAddressKind::None;
    const std::optional<va_t> GOT = Img.linkedI386GlobalOffsetTable();
    if (!GOT)
      return GOTAddressKind::None;
    const std::optional<uint64_t> Folded = foldI386PicAddress(
        *F.Func, Value, [&](const MedVar &Each) -> const MedOp * {
          const auto Def = F.Defs.find(keyOf(Each));
          return Def == F.Defs.end() ? nullptr : Def->second;
        });
    if (Folded)
      return uint32_t(*Folded) == uint32_t(*GOT) ? GOTAddressKind::Computed
                                                 : GOTAddressKind::None;
    // clang -O0 spills the GOT address and reloads it: the reload is the
    // address by the proof the HighIR route reads it with.
    if (PcRootedFunc != F.Func) {
      PcRootedFunc = F.Func;
      PcRootedNumbers =
          MedToHighConverter::i386PcRootedNumbers(*F.Func, Img.Arch, &Img);
    }
    const auto Number = PcRootedNumbers.find(varKey(Value));
    return Number != PcRootedNumbers.end() && Number->second == uint32_t(*GOT)
               ? GOTAddressKind::Reloaded
               : GOTAddressKind::None;
  }

  /// What \p Value hands back, each path a PHI joins counted on its own.
  void classify(const FunctionFacts &F, const MedVar &Value,
                std::vector<ReturnPath> &Paths, std::set<ValueKey> &Seen,
                unsigned Depth) const {
    if (Value.isConst()) {
      Paths.push_back(ReturnedValue::Deliberate);
      return;
    }
    if (Value.Kind == MedVar::Unspecified) {
      Paths.push_back(ReturnedValue::Undefined);
      return;
    }
    // The GOT address computed from the PC is nothing the function means.
    switch (i386GOTAddress(F, Value)) {
    case GOTAddressKind::Computed:
      Paths.push_back(ReturnedValue::Undefined);
      return;
    case GOTAddressKind::Reloaded:
      Paths.push_back(ReturnedValue::ReloadedGOTAddress);
      return;
    case GOTAddressKind::None:
      break;
    }
    if (Value.Kind == MedVar::Param ||
        Depth > ::neverd::limits::pipeline::
                    kPipelineReturnContractsClassifyMaxDepth ||
        !Seen.insert(keyOf(Value)).second) {
      Paths.push_back(ReturnedValue::Other);
      return;
    }
    if (const auto Phi = F.Phis.find(keyOf(Value)); Phi != F.Phis.end()) {
      for (const auto &[Predecessor, Arm] : Phi->second->Args) {
        (void)Predecessor;
        classify(F, Arm, Paths, Seen, Depth + 1);
      }
      return;
    }
    // An SMC or HVC's result under the SMC Calling Convention, as a system
    // call's, is a value where a caller reads it.
    if (const auto Bound = F.IntrinsicOutputDefs.find(keyOf(Value));
        Bound != F.IntrinsicOutputDefs.end() && Bound->second->NumInputs >= 1 &&
        Bound->second->Inputs[0].isConst() &&
        intrinsicReturnsSmcccResults(
            static_cast<Intrinsic>(Bound->second->Inputs[0].ConstVal))) {
      Paths.push_back(ReturnedValue::SystemCallResult);
      return;
    }
    const auto Def = F.Defs.find(keyOf(Value));
    if (Def == F.Defs.end() ||
        (Def->second->Opcode == NdOp::COPY && Def->second->NumInputs == 1 &&
         keyOf(Def->second->Inputs[0]) == keyOf(Value))) {
      // A register as the function entered it: no value, unless a
      // parameter arrives in it.
      const bool Parameter = Value.Kind == MedVar::Reg &&
                             llvm::any_of(F.Func->Params, [&](const MedVar &P) {
                               return P.RegOff == Value.RegOff;
                             });
      Paths.push_back(Value.Kind == MedVar::Reg && Value.SSAVer == 0 &&
                              !Parameter
                          ? ReturnedValue::Unwritten
                          : ReturnedValue::Other);
      return;
    }
    const MedOp &Op = *Def->second;
    if (isComparison(Op.Opcode)) {
      Paths.push_back(ReturnedValue::Deliberate);
      return;
    }
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      if (Op.NumInputs >= 1) {
        classify(F, Op.Inputs[0], Paths, Seen, Depth + 1);
        return;
      }
      break;
    case NdOp::SUBBYTES:
      // The low bytes of what a path hands back.
      if (Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
          Op.Inputs[1].ConstVal == 0) {
        classify(F, Op.Inputs[0], Paths, Seen, Depth + 1);
        return;
      }
      break;
    case NdOp::CONCAT: {
      // A narrow value merged into the register (a setcc byte over the rest
      // of RAX) is what a narrow return reads.
      if (Op.NumInputs != 2)
        break;
      std::vector<ReturnPath> Low;
      classify(F, Op.Inputs[1], Low, Seen, Depth + 1);
      Paths.push_back(llvm::all_of(Low,
                                   [](const ReturnPath &V) {
                                     return V.Kind == ReturnedValue::Deliberate;
                                   })
                          ? ReturnedValue::Deliberate
                          : ReturnedValue::Other);
      return;
    }
    case NdOp::SELECT: {
      if (Op.NumInputs != 3)
        break;
      std::vector<ReturnPath> Arms;
      classify(F, Op.Inputs[1], Arms, Seen, Depth + 1);
      classify(F, Op.Inputs[2], Arms, Seen, Depth + 1);
      Paths.push_back(llvm::all_of(Arms,
                                   [](const ReturnPath &V) {
                                     return V.Kind == ReturnedValue::Deliberate;
                                   })
                          ? ReturnedValue::Deliberate
                          : ReturnedValue::Other);
      return;
    }
    case NdOp::CALL:
    case NdOp::INDIR_CALL:
      // A local unwind or a `__finally` body's `call` leaves whatever it
      // leaves: no value the function means unless a caller reads it.
      if (F.Func->RegistrationStates && F.Func->RegistrationStates->Display &&
          F.Func->RegistrationStates->Display->NoValueCalls.count(Op.Addr)) {
        Paths.push_back(ReturnedValue::Incoming);
        return;
      }
      Paths.push_back(callPath(F, Op));
      return;
    case NdOp::INTRINSIC: {
      // An instruction run for its effect (a fence, a breakpoint) or one C
      // has no name for leaves no value the function means; the x64 debug
      // service returns its status, an x87 state read (fnstsw, fnstcw) the
      // state, and `str`, `sldt` or `smsw` and a system register's read the
      // register it reads.  A system call writes its result whatever the
      // function means.
      const Intrinsic Id = Op.NumInputs >= 1 && Op.Inputs[0].isConst()
                               ? static_cast<Intrinsic>(Op.Inputs[0].ConstVal)
                               : Intrinsic::None;
      if (intrinsicReturnsSystemCallResult(Id)) {
        Paths.push_back(ReturnedValue::SystemCallResult);
        return;
      }
      // A merge read both observes memory and defines a scalar result. An
      // unpredictable result must also remain observable so its backend can
      // reject it, rather than silently changing the function to void.
      if (Op.Output.Size && mipsScalarMedShapeIsValid(Op, Img.Arch)) {
        Paths.push_back(ReturnedValue::Other);
        return;
      }
      if (isAArch64PAuthIntrinsic(Id) || aarch64CPAOperation(Id) ||
          isAArch64PAuthLoadIntrinsic(Id) ||
          isAArch64WritebackMemoryIntrinsic(Id) ||
          isAArch64AddressMemoryIntrinsic(Id)) {
        Paths.push_back(Img.Arch == Arch::AArch64 &&
                                (aarch64PAuthMedShapeIsValid(Op) ||
                                 aarch64CPAMedShapeIsValid(Op) ||
                                 aarch64PAuthLoadMedShapeIsValid(Op) ||
                                 aarch64WritebackMemoryMedShapeIsValid(Op) ||
                                 aarch64AddressLoadMedShapeIsValid(Op))
                            ? ReturnedValue::Other
                            : ReturnedValue::Undefined);
        return;
      }
      if (isX86DescriptorStateIntrinsic(Id)) {
        Paths.push_back(Op.Output.Size &&
                                x86DescriptorStateMedShapeIsValid(Op, Img.Arch)
                            ? ReturnedValue::Other
                            : ReturnedValue::Undefined);
        return;
      }
      // A real system-instruction completion remains a value even when the
      // instruction also owns memory or processor effects. Use the same
      // shape contracts as lowering; an invented output on a void effect
      // must not acquire a return value here.
      if (Op.Output.Size &&
          ((Id == Intrinsic::X86AMXState &&
            x86AmxShapeIsValid(x86AmxMedShape(Op, Img.Arch))) ||
           x86SgxShapeIsValid(Id, x86SgxMedShape(Op, Img.Arch)) ||
           (isX86BoundedLeafIntrinsic(Id) &&
            x86BoundedLeafShapeIsValid(Id,
                                       x86BoundedLeafMedShape(Op, Img.Arch))) ||
           x86SystemStateShapeIsValid(Id,
                                      x86SystemStateMedShape(Op, Img.Arch)) ||
           x86VirtualizationShapeIsValid(
               Id, x86VirtualizationMedShape(Op, Img.Arch)) ||
           (Img.Arch == Arch::X64 &&
            intrinsicX86MsrAccessShapeIsValid(Id, x86MsrAccessMedShape(Op))))) {
        Paths.push_back(ReturnedValue::Other);
        return;
      }
      Paths.push_back(Id != Intrinsic::None && Id != Intrinsic::DebugService &&
                              Id != Intrinsic::CetRdSsp &&
                              !x86FPStateReturnsValue(Id) &&
                              !x87ContextReturnsValue(Id) &&
                              !isX86SystemRegisterStore(Id) &&
                              !intrinsicReadsSystemRegister(Id) &&
                              (isSideeffectIntrinsic(Id) || !intrinsicCName(Id))
                          ? ReturnedValue::Undefined
                          : ReturnedValue::Other);
      return;
    }
    default:
      break;
    }
    Paths.push_back(ReturnedValue::Other);
  }

  /// Whether a caller reads what a call to \p S returns.  Where a path of
  /// \p S shows it means no value (\p Shown), that is what the assumption
  /// leaves read.  Otherwise \p S may be handing back what its caller
  /// passed it (the register a parameter arrives in, as a routine that keeps
  /// its registers does), so a caller that hands its result back reads it
  /// until that caller is settled to return none.
  bool callerReadsResult(const State &S, bool Shown) const {
    if (S.ReadDown)
      return true;
    return !Shown && llvm::any_of(S.Callers, [&](const Caller &C) {
      return C.Use.Returned && !handsBackNothing(States[C.Index]);
    });
  }

  /// Whether a parameter arrives in the return register (AArch64 x0, ARM r0).
  bool entersReturnRegister(const MedFunc &Func) const {
    return llvm::any_of(Func.Params, [&](const MedVar &Param) {
      return Param.RegOff == TRI.IntReturnReg;
    });
  }

  /// Whether a function returns no value, and whether a path of its own
  /// shows it.
  struct Answer {
    bool NoValue = false;
    bool Shown = false;
  };

  Answer answer(const State &S) const {
    const std::vector<ReturnPath> &Paths = S.Facts.Paths;
    if (Paths.empty())
      return {S.Facts.NeverReturns, false};
    size_t Undefined = 0, Deliberate = 0, UnlessRead = 0;
    bool Shown = false;
    for (const ReturnPath &Path : Paths)
      switch (Path.Kind) {
      case ReturnedValue::CalleeResult:
        if (States[Path.Callee].NoValue) {
          ++Undefined;
          Shown |= States[Path.Callee].Shown;
        }
        break;
      case ReturnedValue::Unwritten:
      case ReturnedValue::Nothing:
        Shown = true;
        ++Undefined;
        break;
      case ReturnedValue::Undefined:
        ++Undefined;
        break;
      case ReturnedValue::Incoming:
      case ReturnedValue::SystemCallResult:
      case ReturnedValue::ReloadedGOTAddress:
        ++UnlessRead;
        break;
      case ReturnedValue::Deliberate:
        ++Deliberate;
        break;
      case ReturnedValue::Other:
        break;
      }
    const bool Read = callerReadsResult(S, Shown);
    if (!Read)
      Undefined += UnlessRead;
    if (Undefined == Paths.size())
      return {true, Shown};
    if (Read)
      return {};
    // A constant left beside a path that shows the function means no value
    // is what the code happened to hold there (`xor %eax, %eax` to store a
    // zero).  Beside a path that only leaves nothing, such as the code after
    // a call that does not return, it is the function's value.
    if (Shown || (Undefined && !Deliberate))
      return {true, Shown};
    return {};
  }
};

} // namespace

void settleReturnContracts(const BinaryImage &Img, PipelineResult &Result,
                           const DebugContext *Dbg) {
  ReturnContracts(Img, Result, Dbg).settle();
}

} // namespace neverd
