//===- SEHLocalUnwindProtocol.cpp - Saved-frame local unwinds ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The saved-frame protocol over LowIR.  A bounded abstract interpretation
/// tracks which register and temporary values are exact frame addresses,
/// constants, or frame addresses at an unknown offset.  The protocol's rules
/// then read the stores, the calls and the intrinsic operands it records, in
/// the function and in each of its out-of-line handlers.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/SEHLocalUnwindProtocol.h"

#include "SEHLocalUnwindProtocolDetail.h"

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RuntimeProviders.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <deque>
#include <optional>
#include <set>
#include <utility>

namespace neverd {
namespace {

llvm::Error refuse(const llvm::Twine &Why) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "local unwind protocol: " + Why);
}

std::string hex(uint64_t Value) { return "0x" + llvm::utohexstr(Value); }

bool charge(size_t &Remaining, size_t Amount = 1) {
  if (Amount > Remaining) {
    Remaining = 0;
    return false;
  }
  Remaining -= Amount;
  return true;
}

/// The frame an address is relative to: the interpreted body's own entry
/// stack pointer, or the EstablisherFrame a handler receives.
enum class Base : uint8_t { Own, Parent, Any };

/// What the interpretation knows of one value.
struct Val {
  enum Kind : uint8_t {
    /// No frame address: a constant, an incoming value, or what memory
    /// other than a saved-frame slot holds.
    Other,
    Const,
    /// Exactly B + Off.
    Frame,
    /// What the frame slot at B + Off held when it was read.
    Load,
    /// A frame address of B at an unknown offset, or a value that may be one.
    Inexact,
    /// B + Off XORed with a value that holds no frame address, as a GS
    /// prologue stores it in the cookie slot.  Only an XOR with B + Off again
    /// takes the mask off; any other use may unmask it, so it escapes.
    Masked,
  };
  Kind K = Other;
  Base B = Base::Own;
  int64_t Off = 0;
  uint64_t C = 0;

  bool operator==(const Val &) const = default;
  bool frame() const {
    return K == Frame || K == Load || K == Inexact || K == Masked;
  }

  static Val constant(uint64_t Value) {
    Val V;
    V.K = Const;
    V.C = Value;
    return V;
  }
  static Val at(Base B, int64_t Off) {
    Val V;
    V.K = Frame;
    V.B = B;
    V.Off = Off;
    return V;
  }
  static Val loaded(Base B, int64_t Off) {
    Val V = at(B, Off);
    V.K = Load;
    return V;
  }
  static Val inexact(Base B) {
    Val V;
    V.K = Inexact;
    V.B = B;
    return V;
  }
  static Val masked(Base B, int64_t Off) {
    Val V = at(B, Off);
    V.K = Masked;
    return V;
  }
};

/// \p V of the function's own frame as a handler sees it, relative to the
/// EstablisherFrame it receives, \p Frame bytes from the entry stack
/// pointer.
Val rebase(const Val &V, int64_t Frame) {
  switch (V.K) {
  case Val::Frame:
    return Val::at(Base::Parent, V.Off - Frame);
  case Val::Masked:
    return Val::masked(Base::Parent, V.Off - Frame);
  case Val::Inexact:
  case Val::Load:
    return Val::inexact(V.B == Base::Own ? Base::Parent : V.B);
  default:
    return V;
  }
}

Val join(const Val &A, const Val &B) {
  if (A == B)
    return A;
  if (!A.frame() && !B.frame())
    return {};
  if (!A.frame())
    return Val::inexact(B.B);
  if (!B.frame())
    return Val::inexact(A.B);
  return Val::inexact(A.B == B.B ? A.B : Base::Any);
}

/// Pointer-width register values by register offset, and the constants
/// written to narrower views of a register, which a following zero
/// extension widens.
struct State {
  std::map<uint64_t, Val> Regs;
  std::map<std::pair<uint64_t, uint16_t>, uint64_t> Narrow;
  bool operator==(const State &) const = default;
};

State join(const State &A, const State &B) {
  State J;
  for (const auto &[Reg, V] : A.Regs) {
    const auto It = B.Regs.find(Reg);
    const Val Other = It == B.Regs.end() ? Val{} : It->second;
    if (const Val X = join(V, Other); X.K != Val::Other)
      J.Regs.emplace(Reg, X);
  }
  for (const auto &[Reg, V] : B.Regs)
    if (!A.Regs.count(Reg))
      if (const Val X = join(Val{}, V); X.K != Val::Other)
        J.Regs.emplace(Reg, X);
  for (const auto &Entry : A.Narrow)
    if (const auto It = B.Narrow.find(Entry.first);
        It != B.Narrow.end() && It->second == Entry.second)
      J.Narrow.insert(Entry);
  return J;
}

int64_t signedConstant(uint64_t Value, uint16_t Size) {
  if (!Size || Size >= 8)
    return static_cast<int64_t>(Value);
  const unsigned Bits = Size * 8;
  const uint64_t Mask = (uint64_t(1) << Bits) - 1;
  Value &= Mask;
  if (Value & (uint64_t(1) << (Bits - 1)))
    Value |= ~Mask;
  return static_cast<int64_t>(Value);
}

uint64_t truncate(uint64_t Value, uint16_t Size) {
  return Size && Size < 8 ? Value & ((uint64_t(1) << (Size * 8)) - 1) : Value;
}

bool overlaps(int64_t A, uint64_t ASize, int64_t B, uint64_t BSize) {
  return A < B + static_cast<int64_t>(BSize) &&
         B < A + static_cast<int64_t>(ASize);
}

struct StoreEvent {
  Val Addr;
  Val Value;
  uint16_t Size = 0;
  va_t At = InvalidVA;
};

/// A call or an intrinsic, and the values it receives.
struct UseEvent {
  va_t At = InvalidVA;
  int Seq = -1;
  bool Call = false;
  /// The direct callee, or InvalidVA.
  va_t Callee = InvalidVA;
  /// A call of the authenticated local unwind routine.
  bool Runtime = false;
  /// A call of an authenticated runtime routine the frame may reach: the
  /// local unwind, or setjmp, which keeps it in the jump buffer for
  /// longjmp's unwind.
  bool TakesFrame = false;
  std::vector<Val> Args;
  Val SP;
  va_t Return = InvalidVA;
};

/// The frame slot a body's interpretation reads as the saved frame.
struct SlotView {
  Base B = Base::Own;
  int64_t Off = 0;
  Val Holds;
};

/// Whether \p Op may raise an exception.  Register arithmetic, comparisons,
/// copies, direct branches and the return do not.
bool mayRaise(const LowOp &Op) {
  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
  case NdOp::INT_MULT:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
  case NdOp::INT_NEG2:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::BOOL_NOT:
  case NdOp::CONCAT:
  case NdOp::SUBBYTES:
  case NdOp::POPCOUNT:
  case NdOp::LZCOUNT:
  case NdOp::SELECT:
  case NdOp::INSERT:
  case NdOp::EXTRACT:
  case NdOp::BRANCH:
  case NdOp::COND_BR:
  case NdOp::RETURN:
  case NdOp::NOP:
    return false;
  default:
    return true;
  }
}

/// Whether \p Op leaves the body for code its CFG does not name: a call or a
/// branch through a register.
bool leaves(const LowOp &Op) {
  return Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
         Op.Opcode == NdOp::INDIR_BR;
}

/// The ranges \p F's scopes guard, as the runtime reads them; a range that
/// does not read exactly guards the whole function.
std::vector<ExceptionAddressRange> protectedRanges(const LowFunc &F,
                                                   Arch TargetArch) {
  std::vector<ExceptionAddressRange> Ranges;
  if (!F.ExceptionMetadata || !F.ExceptionMetadata->SEH)
    return Ranges;
  const ExceptionFunction &EH = *F.ExceptionMetadata;
  for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
    const std::optional<ExceptionAddressRange> Range =
        getSemanticSEHGuardedRange(Scope, TargetArch, EH);
    Ranges.push_back(Range ? *Range : ExceptionAddressRange{0, InvalidVA});
  }
  return Ranges;
}

/// One body's interpretation: its stores and uses, at a fixed point of its
/// ordinary CFG with every independent root entered from \p Root.
class Interpreter {
public:
  Interpreter(const LowFunc &F, const TargetRegInfo &TRI,
              const BinaryImage &Img, llvm::StringRef Routine,
              std::optional<SlotView> Slot, std::optional<SlotView> Cookie,
              va_t BodyStart, std::vector<ExceptionAddressRange> Protected,
              size_t &Work)
      : F(F), TRI(TRI), Img(Img), Routine(Routine), Slot(Slot), Cookie(Cookie),
        BodyStart(BodyStart), Protected(std::move(Protected)), Work(Work),
        Pointer(TRI.PointerSize) {}

  llvm::Error run(const State &Entry, const State &Root);

  std::vector<StoreEvent> Stores;
  std::vector<UseEvent> Uses;
  /// What the integer return register holds at each return.
  std::vector<Val> Returns;
  /// The registers the body itself puts a frame address in.
  std::set<uint64_t> FrameWrittenTo;
  /// The preserved registers every root is entered with, at the fixed point.
  State RootRegisters;
  /// Each instruction past the prologue that may leave for a root, and the
  /// stack pointer it runs on: a call, a branch through a register, and
  /// anything inside a protected range that may raise an exception, which
  /// only there reaches the function's own handlers.
  struct Exit {
    va_t At = InvalidVA;
    /// Where the next instruction starts: a call's return address.
    va_t Next = InvalidVA;
    Val SP;
  };
  std::vector<Exit> Exits;

private:
  const LowFunc &F;
  const TargetRegInfo &TRI;
  const BinaryImage &Img;
  llvm::StringRef Routine;
  std::optional<SlotView> Slot;
  /// The GS cookie slot, which holds the masked frame the GS check reads.
  std::optional<SlotView> Cookie;
  /// Where the body starts past the prologue.
  va_t BodyStart;
  /// The ranges the function's scopes guard.
  std::vector<ExceptionAddressRange> Protected;
  size_t &Work;
  uint16_t Pointer;
  std::map<std::pair<uint64_t, uint16_t>, Val> Temps;
  /// Whether this round's collection has seen any place yet.
  bool Collected = false;

  bool isRuntime(const LowOp &Op) const;
  /// Whether a call keeps the register at \p Reg, the stack pointer
  /// included, under the Windows calling convention.
  bool preserved(uint64_t Reg) const {
    return Reg == TRI.StackPointer ||
           TRI.isCallPreserved(Reg, Pointer, BinaryFormat::COFF);
  }
  Val read(const State &S, const NdVar &V) const;
  void write(State &S, const NdVar &V, const Val &X);
  /// Run \p Block from \p S.  \p Seen, when given, collects each value a
  /// preserved register holds after any operation.
  bool transfer(const LowBlock &Block, State &S, bool Record,
                State *Seen = nullptr);
};

/// The runtime routine a direct call names through an authenticated import,
/// without its leading underscores, or empty.
llvm::StringRef runtimeRoutine(const BinaryImage &Img, const LowOp &Op) {
  if (Op.Opcode != NdOp::CALL || !Op.NumInputs || !Op.Inputs[0].isConst())
    return {};
  const va_t Callee = static_cast<va_t>(Op.Inputs[0].Offset);
  const Import *Imported = Img.findImportStubAt(Callee);
  if (!Imported)
    Imported = seh_local_unwind::aarch64VeneerImport(Img, Callee);
  if (!Imported ||
      !coff_loader::isX86ContinuationRuntimeProvider(Imported->Module))
    return {};
  return stripLeadingUnderscores(Imported->Name);
}

/// Whether a direct call enters an authenticated setjmp, which stores the
/// frame it receives in the jump buffer, where only longjmp reads it to
/// unwind back to this frame.
bool takesFrame(const BinaryImage &Img, const LowOp &Op) {
  const llvm::StringRef Name = runtimeRoutine(Img, Op);
  return Name == "intrinsic_setjmp" || Name == "intrinsic_setjmpex" ||
         Name == "setjmp" || Name == "setjmpex";
}

bool Interpreter::isRuntime(const LowOp &Op) const {
  const llvm::StringRef Name = runtimeRoutine(Img, Op);
  return !Name.empty() && Name == Routine;
}

Val Interpreter::read(const State &S, const NdVar &V) const {
  if (V.isConst())
    return Val::constant(truncate(V.Offset, V.Size));
  if (V.isTemp()) {
    const auto It = Temps.find({V.Offset, V.Size});
    return It == Temps.end() ? Val{} : It->second;
  }
  if (!V.isReg())
    return {};
  const uint64_t Reg = V.Offset - V.Offset % Pointer;
  const auto It = S.Regs.find(Reg);
  if (V.Offset == Reg && V.Size == Pointer)
    return It == S.Regs.end() ? Val{} : It->second;
  // A narrower view of a frame address is no address the rules can follow.
  if (It != S.Regs.end() && It->second.frame())
    return Val::inexact(It->second.B);
  if (const auto Narrow = S.Narrow.find({V.Offset, V.Size});
      Narrow != S.Narrow.end())
    return Val::constant(Narrow->second);
  if (V.Offset == Reg && It != S.Regs.end() && It->second.K == Val::Const)
    return Val::constant(truncate(It->second.C, V.Size));
  return {};
}

void Interpreter::write(State &S, const NdVar &V, const Val &X) {
  if (V.isTemp()) {
    Temps[{V.Offset, V.Size}] = X;
    return;
  }
  if (!V.isReg() || !V.Size)
    return;
  const uint64_t Reg = V.Offset - V.Offset % Pointer;
  if (X.frame())
    FrameWrittenTo.insert(Reg);
  for (auto It = S.Narrow.begin(); It != S.Narrow.end();)
    if (It->first.first - It->first.first % Pointer == Reg)
      It = S.Narrow.erase(It);
    else
      ++It;
  if (V.Offset == Reg && V.Size == Pointer) {
    if (X.K == Val::Other)
      S.Regs.erase(Reg);
    else
      S.Regs[Reg] = X;
    return;
  }
  // Part of a register: no frame address survives it.
  if (X.frame())
    S.Regs[Reg] = Val::inexact(X.B);
  else
    S.Regs.erase(Reg);
  if (X.K == Val::Const && V.Offset == Reg)
    S.Narrow[{V.Offset, V.Size}] = truncate(X.C, V.Size);
}

bool Interpreter::transfer(const LowBlock &Block, State &S, bool Record,
                           State *Seen) {
  va_t Instruction = InvalidVA;
  size_t Boundary = 0;
  // A root is entered with the registers the body holds where it may leave
  // for a root: at the start of an instruction past the prologue that may,
  // running on the stack pointer the roots resume on.  A register absent
  // from either state holds no frame address there.
  auto Collect = [&](size_t First) {
    const va_t At = Block.Ops[First].Addr;
    if (At < BodyStart)
      return;
    bool Leaves = false, Raises = false;
    size_t K = First;
    for (; K < Block.Ops.size() && Block.Ops[K].Addr == At; ++K) {
      Leaves = Leaves || leaves(Block.Ops[K]);
      Raises = Raises || mayRaise(Block.Ops[K]);
    }
    if (!Leaves &&
        !(Raises &&
          llvm::any_of(Protected, [&](const ExceptionAddressRange &R) {
            return R.contains(At);
          })))
      return;
    if (Record)
      Exits.push_back({At,
                       K < Block.Ops.size() ? Block.Ops[K].Addr : Block.EndAddr,
                       read(S, NdVar::reg(TRI.StackPointer, Pointer))});
    if (!Seen)
      return;
    if (const auto RootSP = Seen->Regs.find(TRI.StackPointer);
        RootSP != Seen->Regs.end()) {
      const auto SP = S.Regs.find(TRI.StackPointer);
      if (SP == S.Regs.end() || SP->second != RootSP->second)
        return;
    }
    if (!Collected) {
      Collected = true;
      for (const auto &[Reg, V] : S.Regs)
        if (Reg != TRI.StackPointer && preserved(Reg))
          Seen->Regs[Reg] = V;
      return;
    }
    for (auto &[Reg, V] : Seen->Regs)
      if (Reg != TRI.StackPointer && !S.Regs.count(Reg))
        V = join(V, Val{});
    for (const auto &[Reg, V] : S.Regs)
      if (Reg != TRI.StackPointer && preserved(Reg)) {
        const auto It = Seen->Regs.find(Reg);
        Seen->Regs[Reg] = join(It == Seen->Regs.end() ? Val{} : It->second, V);
      }
  };
  for (size_t I = 0; I < Block.Ops.size(); ++I) {
    if (!charge(Work))
      return false;
    const LowOp &Op = Block.Ops[I];
    if (Op.Addr != Instruction) {
      Collect(I);
      Temps.clear();
      Instruction = Op.Addr;
    }
    while (Boundary < Block.InstructionBoundaries.size() &&
           Block.InstructionBoundaries[Boundary].FirstOp +
                   Block.InstructionBoundaries[Boundary].OpCount <=
               I)
      ++Boundary;
    auto Input = [&](size_t K) {
      return K < Op.NumInputs ? read(S, Op.Inputs[K]) : Val{};
    };
    // A constant built in two instructions (`movw`, `movt`).
    if ((Op.Opcode == NdOp::INT_AND || Op.Opcode == NdOp::INT_OR ||
         Op.Opcode == NdOp::INT_LEFT) &&
        Input(0).K == Val::Const && Input(1).K == Val::Const) {
      const uint64_t A = Input(0).C, B = Input(1).C;
      write(S, Op.Output,
            Val::constant(truncate(Op.Opcode == NdOp::INT_AND  ? A & B
                                   : Op.Opcode == NdOp::INT_OR ? A | B
                                   : B < 64                    ? A << B
                                                               : 0,
                                   Op.Output.Size)));
      continue;
    }
    switch (Op.Opcode) {
    case NdOp::COPY:
      write(S, Op.Output, Input(0));
      break;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB: {
      const Val A = Input(0), B = Input(1);
      const bool Add = Op.Opcode == NdOp::INT_ADD;
      Val X;
      if (A.K == Val::Const && B.K == Val::Const)
        X = Val::constant(
            truncate(Add ? A.C + B.C : A.C - B.C, Op.Output.Size));
      else if (A.K == Val::Frame && B.K == Val::Const)
        X = Val::at(A.B, Add ? A.Off + signedConstant(B.C, Op.Inputs[1].Size)
                             : A.Off - signedConstant(B.C, Op.Inputs[1].Size));
      else if (Add && B.K == Val::Frame && A.K == Val::Const)
        X = Val::at(B.B, B.Off + signedConstant(A.C, Op.Inputs[0].Size));
      else if (!Add && A.frame() && B.frame())
        // The distance between two frame addresses is a number.
        X = {};
      else if (A.frame() || B.frame())
        X = Val::inexact(A.frame() ? A.B : B.B);
      if (X.K == Val::Frame && Op.Output.Size != Pointer)
        X = Val::inexact(X.B);
      write(S, Op.Output, X);
      break;
    }
    case NdOp::INT_XOR: {
      // A frame address masked with a value of no frame, and unmasked by
      // the same address: the GS cookie's store and its check.
      const Val A = Input(0), B = Input(1);
      Val X;
      if (A.K == Val::Const && B.K == Val::Const)
        X = Val::constant(truncate(A.C ^ B.C, Op.Output.Size));
      else if (A.K == Val::Frame && A == B)
        X = Val::constant(0);
      else if (Op.Output.Size == Pointer && A.K == Val::Frame &&
               B.K == Val::Other)
        X = Val::masked(A.B, A.Off);
      else if (Op.Output.Size == Pointer && B.K == Val::Frame &&
               A.K == Val::Other)
        X = Val::masked(B.B, B.Off);
      else if ((A.K == Val::Masked && B.K == Val::Frame &&
                Val::at(A.B, A.Off) == B) ||
               (B.K == Val::Masked && A.K == Val::Frame &&
                Val::at(B.B, B.Off) == A))
        X = {};
      else if (A.frame() || B.frame())
        X = Val::inexact(A.frame() ? A.B : B.B);
      write(S, Op.Output, X);
      break;
    }
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT: {
      const Val A = Input(0);
      if (A.K == Val::Const)
        write(S, Op.Output,
              Val::constant(truncate(Op.Opcode == NdOp::INT_SEXT
                                         ? static_cast<uint64_t>(signedConstant(
                                               A.C, Op.Inputs[0].Size))
                                         : A.C,
                                     Op.Output.Size)));
      else
        write(S, Op.Output, A.frame() ? Val::inexact(A.B) : Val{});
      break;
    }
    case NdOp::LOAD: {
      const Val A = Input(0);
      Val X;
      if (A.K == Val::Frame) {
        const SlotView *View = nullptr;
        for (const std::optional<SlotView> *Candidate : {&Slot, &Cookie})
          if (*Candidate && A.B == (*Candidate)->B &&
              overlaps(A.Off, Op.Output.Size, (*Candidate)->Off, Pointer)) {
            View = &**Candidate;
            break;
          }
        if (View)
          X = A.Off == View->Off && Op.Output.Size == Pointer
                  ? View->Holds
                  : Val::inexact(A.B);
        else if (!Slot)
          X = Val::loaded(A.B, A.Off);
      } else if (A.frame()) {
        // An unknown frame address may read the saved frame.
        X = Val::inexact(A.B);
      } else if (A.K == Val::Const) {
        if (const auto Held =
                seh_local_unwind::armLiteralPoolValue(Img, A.C, Op.Output.Size))
          X = Val::constant(*Held);
      }
      write(S, Op.Output, X);
      break;
    }
    case NdOp::STORE:
      if (Record && Op.NumInputs == 2)
        Stores.push_back({Input(0), Input(1), Op.Inputs[1].Size, Op.Addr});
      break;
    case NdOp::CALL:
    case NdOp::INDIR_CALL:
    case NdOp::INTRINSIC: {
      if (Record) {
        UseEvent Use;
        Use.At = Op.Addr;
        Use.Seq = Op.Seq;
        Use.Call = Op.Opcode != NdOp::INTRINSIC;
        Use.Runtime = isRuntime(Op);
        if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst())
          Use.Callee = static_cast<va_t>(Op.Inputs[0].Offset);
        Use.TakesFrame = Use.Runtime || takesFrame(Img, Op);
        if (Use.Call) {
          for (uint64_t Reg : TRI.integerParamRegs(BinaryFormat::COFF))
            Use.Args.push_back(read(S, NdVar::reg(Reg, Pointer)));
          Use.SP = read(S, NdVar::reg(TRI.StackPointer, Pointer));
          if (Boundary < Block.InstructionBoundaries.size())
            Use.Return = Block.InstructionBoundaries[Boundary].Address +
                         Block.InstructionBoundaries[Boundary].Size;
        } else {
          for (size_t K = 1; K < Op.NumInputs; ++K)
            Use.Args.push_back(Input(K));
        }
        Uses.push_back(std::move(Use));
      }
      if (Op.Opcode != NdOp::INTRINSIC) {
        // The callee keeps the stack pointer and its callee-saved registers.
        for (auto It = S.Regs.begin(); It != S.Regs.end();)
          if (!preserved(It->first))
            It = S.Regs.erase(It);
          else
            ++It;
        for (auto It = S.Narrow.begin(); It != S.Narrow.end();)
          if (!preserved(It->first.first - It->first.first % Pointer))
            It = S.Narrow.erase(It);
          else
            ++It;
      }
      if (Op.Output.Size)
        write(S, Op.Output, {});
      break;
    }
    case NdOp::RETURN:
      if (Record)
        Returns.push_back(read(S, NdVar::reg(TRI.IntReturnReg, Pointer)));
      break;
    case NdOp::BRANCH:
    case NdOp::COND_BR:
    case NdOp::INDIR_BR:
    case NdOp::NOP:
      break;
    default: {
      if (!Op.Output.Size)
        break;
      std::optional<Base> From;
      for (size_t K = 0; K < Op.NumInputs; ++K)
        if (const Val A = Input(K); A.frame())
          From = From && *From != A.B ? Base::Any : A.B;
      write(S, Op.Output, From ? Val::inexact(*From) : Val{});
      break;
    }
    }
  }
  Temps.clear();
  return true;
}

llvm::Error Interpreter::run(const State &Entry, const State &RootTemplate) {
  const size_t N = F.Blocks.size();
  for (size_t B = 0; B < N; ++B)
    if (F.Blocks[B].Id != static_cast<int>(B))
      return refuse("noncanonical block identity in " + hex(F.Entry));
  std::vector<bool> IsRoot(N, false);
  for (size_t B = 1; B < N; ++B) {
    const LowBlock &Block = F.Blocks[B];
    IsRoot[B] = Block.Preds.empty() || !Block.ExceptionalPreds.empty() ||
                F.ModuleAnalysisRoots.count(Block.StartAddr) ||
                F.OrdinaryModuleAnalysisRoots.count(Block.StartAddr);
  }
  // A root is entered with the registers the function holds where it may
  // leave for one: the first round enters no root, and each later one seeds
  // the roots with every value a callee-saved register takes at such a place
  // in the previous round, until that stops growing.
  std::optional<State> Root;
  for (unsigned Round = 0; Round < 5; ++Round) {
    // Every value a preserved register takes where the body may leave for
    // a root.
    State Seen = RootTemplate;
    Collected = false;
    std::vector<std::optional<State>> In(N), Out(N);
    std::deque<size_t> Queue;
    std::vector<bool> Queued(N, false);
    auto Enqueue = [&](size_t B) {
      if (!Queued[B]) {
        Queued[B] = true;
        Queue.push_back(B);
      }
    };
    for (size_t B = 0; B < N; ++B)
      if (B == 0 || IsRoot[B])
        Enqueue(B);
    while (!Queue.empty()) {
      if (!charge(Work))
        return refuse("work budget exhausted in " + hex(F.Entry));
      const size_t B = Queue.front();
      Queue.pop_front();
      Queued[B] = false;
      std::optional<State> Input;
      if (B == 0)
        Input = Entry;
      else if (IsRoot[B] && Root)
        Input = *Root;
      for (int P : F.Blocks[B].Preds) {
        if (P < 0 || static_cast<size_t>(P) >= N)
          return refuse("invalid CFG in " + hex(F.Entry));
        if (!Out[P])
          continue;
        Input = Input ? join(*Input, *Out[P]) : *Out[P];
      }
      if (!Input)
        continue;
      State Output = *Input;
      if (!transfer(F.Blocks[B], Output, /*Record=*/false, &Seen))
        return refuse("work budget exhausted in " + hex(F.Entry));
      In[B] = std::move(Input);
      if (Out[B] && *Out[B] == Output)
        continue;
      Out[B] = std::move(Output);
      for (int S : F.Blocks[B].Succs)
        if (S >= 0 && static_cast<size_t>(S) < N)
          Enqueue(static_cast<size_t>(S));
    }
    for (auto It = Seen.Regs.begin(); It != Seen.Regs.end();)
      if (It->second.K == Val::Other)
        It = Seen.Regs.erase(It);
      else
        ++It;
    if (Root && Seen == *Root) {
      RootRegisters = *Root;
      Stores.clear();
      Uses.clear();
      Exits.clear();
      for (size_t B = 0; B < N; ++B)
        if (In[B]) {
          State Replay = *In[B];
          if (!transfer(F.Blocks[B], Replay, /*Record=*/true))
            return refuse("work budget exhausted in " + hex(F.Entry));
        }
      return llvm::Error::success();
    }
    Root = std::move(Seen);
  }
  return refuse("root registers do not settle in " + hex(F.Entry));
}

/// Whether the call \p Use passes its argument at position \p Index: a direct
/// callee whose unread-register proof covers that register never observes
/// it.
bool passesArgument(const UseEvent &Use, size_t Index,
                    llvm::ArrayRef<uint64_t> Registers,
                    const std::map<va_t, GPRFamilyMask> *Unread,
                    Arch TargetArch) {
  if (!Use.Call || !Unread || Use.Callee == InvalidVA ||
      Index >= Registers.size())
    return true;
  const auto Proof = Unread->find(Use.Callee);
  // An architecture without register families has no proof.
  const std::optional<unsigned> Family =
      gprFamilyOf(TargetArch, Registers[Index]);
  return Proof == Unread->end() || !Family ||
         !((Proof->second >> *Family) & 1u);
}

/// Check one out-of-line handler of the function against the saved-frame
/// slot, which it sees at \p SlotFromFrame bytes above its EstablisherFrame
/// and which holds the TargetFrame, \p TargetFromFrame bytes above it.
llvm::Error checkHandler(const LowFunc &Handler, const TargetRegInfo &TRI,
                         const BinaryImage &Img, llvm::StringRef Routine,
                         int64_t SlotFromFrame, int64_t TargetFromFrame,
                         size_t &Work,
                         const std::map<va_t, GPRFamilyMask> *Unread,
                         const State *ParentRegisters,
                         std::map<va_t, unsigned> &UnwindTargets) {
  const std::string Name = hex(Handler.Entry);
  if (Handler.Blocks.empty() || !Handler.hasCompleteLiftCoverage())
    return refuse("handler " + Name + " is not completely decoded");
  const llvm::ArrayRef<uint64_t> Args =
      TRI.integerParamRegs(BinaryFormat::COFF);
  if (Args.size() < 2)
    return refuse("no establisher-frame argument register");
  const uint16_t Pointer = TRI.PointerSize;
  SlotView Slot;
  Slot.B = Base::Parent;
  Slot.Off = SlotFromFrame;
  Slot.Holds = Val::at(Base::Parent, TargetFromFrame);
  Interpreter Interp(Handler, TRI, Img, Routine, Slot, std::nullopt,
                     Handler.Entry, protectedRanges(Handler, TRI.TheArch),
                     Work);
  State Entry;
  if (ParentRegisters)
    Entry = *ParentRegisters;
  Entry.Regs[TRI.StackPointer] = Val::at(Base::Own, 0);
  Entry.Regs.try_emplace(Args[1], Val::at(Base::Parent, 0));
  if (auto Error = Interp.run(Entry, State{}))
    return Error;
  // The prologue saves those registers in the handler's own frame, from
  // where only the epilogue restores them.
  const va_t PrologueEnd =
      Handler.Entry + (ParentRegisters && Handler.ExceptionMetadata
                           ? Handler.ExceptionMetadata->PrologueSize
                           : 0);
  // The register the runtime passed the frame in, which a call of the handler
  // inherits when nothing writes it, passes nothing the handler chose to.
  const Val FrameArrived = Val::at(Base::Parent, 0);
  // The frame's own base, its TargetFrame and the slot's address may leave
  // the handler only toward the runtime.
  auto Escapes = [&](const Val &V) {
    if (V.K == Val::Inexact || V.K == Val::Masked)
      return V.B != Base::Own;
    return V.K == Val::Frame && V.B == Base::Parent &&
           (V.Off == 0 || V.Off == TargetFromFrame ||
            overlaps(V.Off, 1, SlotFromFrame, Pointer));
  };
  for (const StoreEvent &Store : Interp.Stores) {
    if (!charge(Work))
      return refuse("work budget exhausted in " + Name);
    if (Store.Addr.K == Val::Inexact && Store.Addr.B != Base::Own)
      return refuse("handler " + Name +
                    " stores at an inexact address of the frame at " +
                    hex(Store.At));
    if (Store.Addr.K == Val::Frame && Store.Addr.B == Base::Parent &&
        overlaps(Store.Addr.Off, Store.Size, SlotFromFrame, Pointer))
      return refuse("handler " + Name + " writes the saved-frame slot at " +
                    hex(Store.At));
    if (Store.At < PrologueEnd && Store.Addr.K == Val::Frame &&
        Store.Addr.B == Base::Own)
      continue;
    // A handler with handlers of its own homes the frame in its own frame for
    // them, as any function homes an argument.
    const bool OwnHome = Store.Addr.K == Val::Frame &&
                         Store.Addr.B == Base::Own && Store.Addr.Off > 0 &&
                         Handler.ExceptionMetadata &&
                         Handler.ExceptionMetadata->SEH;
    if (Escapes(Store.Value) && !OwnHome)
      return refuse("handler " + Name + " stores the frame at " +
                    hex(Store.At));
  }
  for (const UseEvent &Use : Interp.Uses) {
    if (!charge(Work, Use.Args.size() + 1))
      return refuse("work budget exhausted in " + Name);
    // The handler's own unwind of this frame to an exact address.
    if (Use.Runtime && Use.Args.size() >= 2 &&
        Use.Args[0] == Val::at(Base::Parent, TargetFromFrame) &&
        Use.Args[1].K == Val::Const)
      ++UnwindTargets[static_cast<va_t>(Use.Args[1].C)];
    if (Use.TakesFrame)
      continue;
    for (size_t K = 0; K < Use.Args.size(); ++K)
      if (passesArgument(Use, K, Args, Unread, TRI.TheArch) &&
          Escapes(Use.Args[K]) &&
          !(K < Args.size() && Use.Call && Use.Args[K] == FrameArrived &&
            !Interp.FrameWrittenTo.count(Args[K])))
        return refuse("handler " + Name + " passes the frame on at " +
                      hex(Use.At));
  }
  return llvm::Error::success();
}

/// Whether something may read \p Reg where control resumes at \p Target
/// before anything writes it whole: a read, a call, which may take it as an
/// argument, or a return or any other way out of the function, which may
/// hand it back.  With \p CallsKeepIt, \p Reg is one that calls preserve and
/// the function's epilogue restores, which neither reads.  True as well when
/// the work runs out.
bool readBeforeWritten(const LowFunc &F, va_t Target, uint64_t Reg,
                       uint16_t Pointer, size_t &Work,
                       bool CallsKeepIt = false) {
  std::map<va_t, int> Starts;
  for (const LowBlock &Block : F.Blocks)
    Starts.emplace(Block.StartAddr, Block.Id);
  const auto First = Starts.find(Target);
  if (First == Starts.end())
    return true;
  auto Overlaps = [&](const NdVar &V) {
    return V.isReg() && V.Size && V.Offset < Reg + Pointer &&
           Reg < V.Offset + V.Size;
  };
  std::set<int> Seen;
  std::vector<int> Pending{First->second};
  while (!Pending.empty()) {
    const int B = Pending.back();
    Pending.pop_back();
    if (B < 0 || static_cast<size_t>(B) >= F.Blocks.size() ||
        !Seen.insert(B).second)
      continue;
    const LowBlock &Block = F.Blocks[B];
    if (!charge(Work, Block.Ops.size() + 1))
      return true;
    bool Written = false;
    for (const LowOp &Op : Block.Ops) {
      if (Op.Opcode == NdOp::INDIR_BR)
        return true;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::RETURN) {
        if (!CallsKeepIt)
          return true;
        continue;
      }
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (Overlaps(Op.Inputs[I]))
          return true;
      // A 32-bit write clears the upper half too.
      if (Op.Output.isReg() && Op.Output.Offset == Reg && Op.Output.Size >= 4) {
        Written = true;
        break;
      }
    }
    if (Written)
      continue;
    // A block that leaves the function another way, such as a tail branch,
    // may hand the register on.
    if (Block.Succs.empty() && !CallsKeepIt)
      return true;
    Pending.insert(Pending.end(), Block.Succs.begin(), Block.Succs.end());
  }
  return false;
}

} // namespace

bool handlersCallLowSEHLocalUnwind(
    const LowFunc &Function, const BinaryImage &Image,
    const std::map<va_t, const LowFunc *> &Bodies) {
  if (!Function.ExceptionMetadata || !Function.ExceptionMetadata->SEH)
    return false;
  for (const SEHScopeRecord &Scope : Function.ExceptionMetadata->SEH->Scopes)
    if (Scope.Kind == SEHScopeKind::Finally && Scope.FilterOrFinallyVA)
      if (const auto Body = Bodies.find(Scope.FilterOrFinallyVA);
          Body != Bodies.end() && Body->second &&
          callsLowSEHLocalUnwind(*Body->second, Image))
        return true;
  return false;
}

bool callsLowSEHLocalUnwind(const LowFunc &Function, const BinaryImage &Image) {
  for (const LowBlock &Block : Function.Blocks)
    for (const LowOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL) {
        if (isLowSEHLocalUnwindRoutine(runtimeRoutine(Image, Op)))
          return true;
      }
  return false;
}

namespace seh_local_unwind {

std::optional<DecodedPrologue> decodedPrologue(const LowFunc &Function,
                                               const TargetRegInfo &TRI,
                                               size_t &WorkRemaining) {
  if (Function.Blocks.empty() || !Function.ExceptionMetadata)
    return std::nullopt;
  const LowBlock &First = Function.Blocks.front();
  const va_t End = Function.Entry + Function.ExceptionMetadata->PrologueSize;
  if (First.StartAddr != Function.Entry || First.EndAddr < End ||
      !charge(WorkRemaining, First.Ops.size()))
    return std::nullopt;
  const NdVar SP = NdVar::reg(TRI.StackPointer, TRI.PointerSize);
  // Each value's distance below the entry stack pointer: pre-indexed saves
  // compute it in a temporary first.
  int64_t Below = 0;
  std::map<std::pair<uint64_t, uint16_t>, int64_t> Temps;
  auto Distance = [&](const NdVar &V) -> std::optional<int64_t> {
    if (V == SP)
      return Below;
    if (V.isTemp())
      if (const auto It = Temps.find({V.Offset, V.Size}); It != Temps.end())
        return It->second;
    return std::nullopt;
  };
  DecodedPrologue Prologue;
  bool InPrologue = true;
  unsigned Calls = 0;
  for (const LowOp &Op : First.Ops) {
    if (InPrologue && Op.Addr >= End) {
      InPrologue = false;
      Prologue.Allocation = static_cast<uint64_t>(std::max<int64_t>(Below, 0));
      Prologue.Calls = Calls;
    }
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
      ++Calls;
    std::optional<int64_t> Value;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
      Value = Distance(Op.Inputs[0]);
    } else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
               Op.NumInputs == 2 && Op.Inputs[1].isConst()) {
      if (const auto Base = Distance(Op.Inputs[0])) {
        const int64_t Step =
            signedConstant(Op.Inputs[1].Offset, Op.Inputs[1].Size);
        Value = Op.Opcode == NdOp::INT_ADD ? *Base - Step : *Base + Step;
      }
    }
    if (Op.Output.isTemp()) {
      if (Value)
        Temps[{Op.Output.Offset, Op.Output.Size}] = *Value;
      else
        Temps.erase({Op.Output.Offset, Op.Output.Size});
      continue;
    }
    if (!Op.Output.isReg() || !Op.Output.Size ||
        Op.Output.Offset >= TRI.StackPointer + TRI.PointerSize ||
        Op.Output.Offset + Op.Output.Size <= TRI.StackPointer)
      continue;
    if (Op.Output != SP || !Value) {
      if (InPrologue)
        return std::nullopt;
      break;
    }
    // An epilogue in the same block gives back what the body allocated.
    if (!InPrologue && *Value < Below)
      break;
    Below = *Value;
    if (!InPrologue)
      Prologue.Calls = Calls;
  }
  if (Below < 0)
    return std::nullopt;
  if (InPrologue) {
    Prologue.Allocation = static_cast<uint64_t>(Below);
    Prologue.Calls = Calls;
  }
  Prologue.BodyAllocation = static_cast<uint64_t>(Below);
  return Prologue;
}

std::set<va_t> surveyUnwindTargets(const LowFunc &Function,
                                   const BinaryImage &Image,
                                   const TargetRegInfo &TRI,
                                   llvm::StringRef Routine) {
  std::set<va_t> Targets;
  if (Function.Blocks.empty() || !callsLowSEHLocalUnwind(Function, Image))
    return Targets;
  size_t Work = limits::kMaxSEHEstablisherFrameWork;
  Interpreter Survey(Function, TRI, Image, Routine, std::nullopt, std::nullopt,
                     Function.Entry, {}, Work);
  State Entry;
  Entry.Regs[TRI.StackPointer] = Val::at(Base::Own, 0);
  if (auto Error = Survey.run(Entry, State{})) {
    llvm::consumeError(std::move(Error));
    return Targets;
  }
  for (const UseEvent &Use : Survey.Uses)
    if (Use.Runtime && Use.Args.size() >= 2 && Use.Args[1].K == Val::Const)
      Targets.insert(static_cast<va_t>(normalizeCodeAddress(
          Use.Args[1].C, TRI.TheArch, InstructionMode::Thumb)));
  return Targets;
}

} // namespace seh_local_unwind

std::set<va_t> collectLowSEHLocalUnwindTargets(const LowFunc &Function,
                                               const BinaryImage &Image,
                                               const TargetRegInfo &TRI) {
  if (Image.Format != BinaryFormat::COFF)
    return {};
  switch (TRI.TheArch) {
  case Arch::AArch64:
    return seh_local_unwind::aarch64UnwindTargets(Function, Image, TRI);
  case Arch::ARM:
    return seh_local_unwind::armUnwindTargets(Function, Image, TRI);
  default:
    return {};
  }
}

llvm::Expected<SEHLocalUnwindConvention>
sehLocalUnwindConvention(const LowFunc &Function, const TargetRegInfo &TRI,
                         size_t &WorkRemaining) {
  switch (TRI.TheArch) {
  case Arch::X64:
    return seh_local_unwind::x64Convention(Function, TRI, WorkRemaining);
  case Arch::AArch64:
    return seh_local_unwind::aarch64Convention(Function, TRI, WorkRemaining);
  case Arch::ARM:
    return seh_local_unwind::armConvention(Function, TRI, WorkRemaining);
  default:
    return refuse("no convention for this architecture");
  }
}

llvm::Expected<std::vector<LowSEHLocalUnwind>> certifyLowSEHLocalUnwinds(
    const LowFunc &Function, const BinaryImage &Image, const TargetRegInfo &TRI,
    const std::map<va_t, const LowFunc *> &Bodies, size_t &WorkRemaining,
    const std::map<va_t, GPRFamilyMask> *Unread,
    std::vector<LowSEHLocalUnwindRefusal> *Refused,
    std::set<va_t> *HandlerTargets) {
  if (!Function.ExceptionMetadata || !Function.ExceptionMetadata->SEH ||
      Function.Blocks.empty() || !Function.hasCompleteLiftCoverage())
    return refuse("no complete SEH body");
  const ExceptionFunction &EH = *Function.ExceptionMetadata;
  auto Convention = sehLocalUnwindConvention(Function, TRI, WorkRemaining);
  if (!Convention)
    return Convention.takeError();
  const uint16_t Pointer = TRI.PointerSize;
  const llvm::ArrayRef<uint64_t> Args =
      TRI.integerParamRegs(BinaryFormat::COFF);
  if (Args.size() < 2 || !Pointer)
    return refuse("no argument registers");
  // Windows on ARM runs only Thumb code, whose addresses carry the low bit.
  auto CodeAddress = [&](uint64_t Value) {
    return static_cast<va_t>(
        normalizeCodeAddress(Value, TRI.TheArch, InstructionMode::Thumb));
  };
  std::set<va_t> Starts;
  for (const LowBlock &Block : Function.Blocks)
    Starts.insert(Block.StartAddr);

  State Entry;
  Entry.Regs[TRI.StackPointer] = Val::at(Base::Own, 0);
  // Every root resumes on the body's stack pointer: the runtime resumes a
  // handler and a local unwind's target with the frame's own.
  State Root;
  Root.Regs[TRI.StackPointer] = Val::at(Base::Own, Convention->BodySP);

  // The GS data places the cookie slot, which holds the masked frame.
  std::optional<SlotView> Cookie;
  if (EH.GSCookie && !Convention->CookieInCallee &&
      EH.GSCookie->ParseStatus == ExceptionParseStatus::Complete &&
      !EH.GSCookie->HasAlignment) {
    Cookie.emplace();
    Cookie->B = Base::Own;
    Cookie->Off = Convention->GSCookieFrame + EH.GSCookie->CookieOffset;
    Cookie->Holds = Val::masked(Base::Own, Convention->GSCookieFrame);
  }

  // The calls of the routine and what frame each passes: the frame itself,
  // or a reload of one slot.
  const va_t BodyStart = Function.Entry + EH.PrologueSize;
  const std::vector<ExceptionAddressRange> Protected =
      protectedRanges(Function, TRI.TheArch);
  Interpreter Survey(Function, TRI, Image, Convention->Routine, std::nullopt,
                     Cookie, BodyStart, Protected, WorkRemaining);
  if (auto Error = Survey.run(Entry, Root))
    return std::move(Error);
  std::set<int64_t> Slots;
  for (const UseEvent &Use : Survey.Uses)
    if (Use.Runtime && Use.Args.front().K == Val::Load &&
        Use.Args.front().B == Base::Own)
      Slots.insert(Use.Args.front().Off);
  // The routine may be called by this frame's termination handlers alone,
  // through the same slot.
  if (Slots.empty())
    for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
      if (Scope.Kind != SEHScopeKind::Finally || !Scope.FilterOrFinallyVA)
        continue;
      const auto Body = Bodies.find(Scope.FilterOrFinallyVA);
      if (Body == Bodies.end() || !Body->second || Body->second->Blocks.empty())
        continue;
      Interpreter Handler(*Body->second, TRI, Image, Convention->Routine,
                          std::nullopt, std::nullopt, Body->second->Entry,
                          protectedRanges(*Body->second, TRI.TheArch),
                          WorkRemaining);
      State HandlerEntry;
      HandlerEntry.Regs[TRI.StackPointer] = Val::at(Base::Own, 0);
      HandlerEntry.Regs[Args[1]] = Val::at(Base::Parent, 0);
      if (auto Error = Handler.run(HandlerEntry, State{}))
        return std::move(Error);
      for (const UseEvent &Use : Handler.Uses)
        if (Use.Runtime && Use.Args.front().K == Val::Load &&
            Use.Args.front().B == Base::Parent)
          Slots.insert(Use.Args.front().Off + Convention->HandlerFrame);
    }
  if (Slots.size() > 1)
    return refuse("the frame reaches the routine from several slots in " +
                  hex(Function.Entry));

  std::optional<SlotView> Slot;
  if (!Slots.empty()) {
    Slot.emplace();
    Slot->B = Base::Own;
    Slot->Off = *Slots.begin();
    Slot->Holds = Val::at(Base::Own, Convention->TargetFrame);
  }
  // What each out-of-line handler's own local unwinds of this frame name.
  std::map<va_t, std::map<va_t, unsigned>> HandlerUnwinds;
  Interpreter Interp(Function, TRI, Image, Convention->Routine, Slot, Cookie,
                     BodyStart, Protected, WorkRemaining);
  if (auto Error = Interp.run(Entry, Root))
    return std::move(Error);

  // The registers a handler starts with, relative to its EstablisherFrame: the
  // function's callee-saved registers as its roots see them, and the frame
  // pointer where the runtime passes that as the frame.
  std::optional<State> HandlerRegisters;
  if (Convention->HandlersSeeParentRegisters) {
    const bool ByFramePointer = Convention->FramePointerIsHandlerFrame;
    if (ByFramePointer && Slot) {
      const auto FP = Interp.RootRegisters.Regs.find(TRI.FramePointer);
      if (!TRI.FramePointer || FP == Interp.RootRegisters.Regs.end() ||
          FP->second != Val::at(Base::Own, Convention->HandlerFrame))
        return refuse(
            "the frame pointer the handlers receive is not fixed in " +
            hex(Function.Entry));
    }
    HandlerRegisters.emplace();
    for (const auto &[Reg, V] : Interp.RootRegisters.Regs)
      if (Reg != TRI.StackPointer && Reg != TRI.LinkRegister &&
          (!ByFramePointer || Reg != TRI.FramePointer))
        HandlerRegisters->Regs[Reg] = rebase(V, Convention->HandlerFrame);
    // The handler's own frame pointer is the runtime's.
    if (ByFramePointer)
      HandlerRegisters->Regs[TRI.FramePointer] = Val::inexact(Base::Parent);
    // The funclet model reads the frame pointer as the establisher frame only
    // where it is the body's stack pointer; elsewhere the frame the handler
    // receives may be any address of the parent's frame.
    HandlerRegisters->Regs[Args[1]] =
        !ByFramePointer || Convention->HandlerFrame == Convention->BodySP
            ? Val::at(Base::Parent, 0)
            : Val::inexact(Base::Parent);
  }

  if (Slot) {
    const int64_t S = Slot->Off;
    // The function's own frame values reach memory and callees only as
    // pointers to its other objects, which the body's stack pointer is where
    // the frame lies elsewhere (it addresses the lowest local there).
    auto Escapes = [&](const Val &V) {
      if (V.K == Val::Inexact || V.K == Val::Masked)
        return true;
      return V.K == Val::Frame && (V.Off == Convention->TargetFrame ||
                                   V.Off == Convention->HandlerFrame ||
                                   overlaps(V.Off, 1, S, Pointer));
    };
    bool Saved = false;
    for (const StoreEvent &Store : Interp.Stores) {
      if (!charge(WorkRemaining))
        return refuse("work budget exhausted in " + hex(Function.Entry));
      if (Store.Addr.K == Val::Inexact)
        return refuse("a store at an inexact frame address at " +
                      hex(Store.At));
      if (Store.Addr.K == Val::Frame &&
          overlaps(Store.Addr.Off, Store.Size, S, Pointer)) {
        if (Store.Addr.Off != S || Store.Size != Pointer ||
            Store.Value != Slot->Holds)
          return refuse("the saved-frame slot is written another way at " +
                        hex(Store.At));
        Saved = true;
        continue;
      }
      if (Cookie && Store.Addr.K == Val::Frame && Store.Addr.B == Cookie->B &&
          overlaps(Store.Addr.Off, Store.Size, Cookie->Off, Pointer)) {
        if (Store.Addr.Off != Cookie->Off || Store.Size != Pointer ||
            Store.Value != Cookie->Holds)
          return refuse("the GS cookie slot is written another way at " +
                        hex(Store.At));
        continue;
      }
      if (Escapes(Store.Value))
        return refuse("the frame is stored at " + hex(Store.At));
    }
    if (!Saved)
      return refuse("nothing saves the frame in its slot in " +
                    hex(Function.Entry));
    for (const UseEvent &Use : Interp.Uses) {
      if (!charge(WorkRemaining, Use.Args.size() + 1))
        return refuse("work budget exhausted in " + hex(Function.Entry));
      if (Use.Call) {
        if (Use.SP.K != Val::Frame || Use.SP.B != Base::Own)
          return refuse("a call at " + hex(Use.At) +
                        " runs on an unknown stack pointer");
        if (Convention->CalleeHomeBytes &&
            overlaps(Use.SP.Off, Convention->CalleeHomeBytes, S, Pointer))
          return refuse("a callee's home area holds the saved frame at " +
                        hex(Use.At));
      }
      if (Use.TakesFrame)
        continue;
      for (size_t K = 0; K < Use.Args.size(); ++K)
        if (passesArgument(Use, K, Args, Unread, TRI.TheArch) &&
            Escapes(Use.Args[K]))
          return refuse("the frame is passed on at " + hex(Use.At));
    }
    // The runtime enters the function's roots and its handlers with the
    // registers of where it left the body inside a protected range, which
    // the roots' registers cover only where the body runs on their stack
    // pointer.
    for (const auto &Exit : Interp.Exits) {
      if (!charge(WorkRemaining, Protected.size() + 1))
        return refuse("work budget exhausted in " + hex(Function.Entry));
      for (const ExceptionAddressRange &Range : Protected)
        if ((Range.contains(Exit.At) || Range.contains(Exit.Next)) &&
            Exit.SP != Val::at(Base::Own, Convention->BodySP))
          return refuse("the protected code at " + hex(Exit.At) +
                        " runs on another stack pointer");
    }
    // Every handler this frame passes its EstablisherFrame to.
    std::set<va_t> Handlers;
    for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
      if (Scope.Kind != SEHScopeKind::Filter &&
          Scope.Kind != SEHScopeKind::Finally)
        continue;
      if (!Scope.FilterOrFinallyVA || EH.ownsCode(Scope.FilterOrFinallyVA))
        return refuse("a handler of " + hex(Function.Entry) +
                      " is not out of line");
      Handlers.insert(Scope.FilterOrFinallyVA);
    }
    for (va_t Handler : Handlers) {
      const auto Body = Bodies.find(Handler);
      if (Body == Bodies.end() || !Body->second)
        return refuse("handler " + hex(Handler) + " is not available");
      std::map<va_t, unsigned> Targets;
      if (auto Error = checkHandler(
              *Body->second, TRI, Image, Convention->Routine,
              S - Convention->HandlerFrame,
              Convention->TargetFrame - Convention->HandlerFrame, WorkRemaining,
              Unread, HandlerRegisters ? &*HandlerRegisters : nullptr, Targets))
        return std::move(Error);
      for (const auto &[Target, Calls] : Targets)
        if (EH.ownsCode(Target) && Starts.count(Target))
          HandlerUnwinds[Handler][Target] += Calls;
    }
  }

  // Without a saved slot the frame reaches a handler's call as its own
  // EstablisherFrame, or at the frame's distance from it.
  if (!Slot)
    for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
      if (Scope.Kind != SEHScopeKind::Finally || !Scope.FilterOrFinallyVA)
        continue;
      const auto Body = Bodies.find(Scope.FilterOrFinallyVA);
      if (Body == Bodies.end() || !Body->second || Body->second->Blocks.empty())
        continue;
      Interpreter Handler(*Body->second, TRI, Image, Convention->Routine,
                          std::nullopt, std::nullopt, Body->second->Entry,
                          protectedRanges(*Body->second, TRI.TheArch),
                          WorkRemaining);
      State HandlerEntry;
      if (HandlerRegisters)
        HandlerEntry = *HandlerRegisters;
      HandlerEntry.Regs[TRI.StackPointer] = Val::at(Base::Own, 0);
      HandlerEntry.Regs[Args[1]] = Val::at(Base::Parent, 0);
      if (auto Error = Handler.run(HandlerEntry, State{}))
        return std::move(Error);
      for (const UseEvent &Use : Handler.Uses)
        if (Use.Runtime && Use.Args.size() >= 2 &&
            Use.Args[0] ==
                Val::at(Base::Parent,
                        Convention->TargetFrame - Convention->HandlerFrame) &&
            Use.Args[1].K == Val::Const &&
            EH.ownsCode(CodeAddress(Use.Args[1].C)) &&
            Starts.count(CodeAddress(Use.Args[1].C)))
          ++HandlerUnwinds[Scope.FilterOrFinallyVA][CodeAddress(Use.Args[1].C)];
    }

  // A handler nested in a handler, or one whose `__try` the compiler left with
  // no range and so no scope, names a target of this function the same way: a
  // local unwind resumes only the frame of the code it names.
  std::set<va_t> OtherHandlers;
  if (!Slots.empty() || Convention->TargetFrame == Convention->HandlerFrame) {
    if (!charge(WorkRemaining, Bodies.size()))
      return refuse("work budget exhausted in " + hex(Function.Entry));
    for (const auto &[Entry, Body] : Bodies) {
      if (!Body || Body == &Function || HandlerUnwinds.count(Entry) ||
          Body->Blocks.empty() || Starts.count(Entry))
        continue;
      if (llvm::none_of(Body->CodeRefTargets, [&](va_t Target) {
            return Starts.count(CodeAddress(Target)) &&
                   EH.ownsCode(CodeAddress(Target));
          }))
        continue;
      Interpreter Other(*Body, TRI, Image, Convention->Routine, std::nullopt,
                        std::nullopt, Entry,
                        protectedRanges(*Body, TRI.TheArch), WorkRemaining);
      State OtherEntry;
      OtherEntry.Regs[TRI.StackPointer] = Val::at(Base::Own, 0);
      if (auto Error = Other.run(OtherEntry, State{}))
        return std::move(Error);
      for (const UseEvent &Use : Other.Uses)
        if (Use.Runtime && Use.Args.size() >= 2 &&
            Use.Args[1].K == Val::Const &&
            EH.ownsCode(CodeAddress(Use.Args[1].C)) &&
            Starts.count(CodeAddress(Use.Args[1].C))) {
          ++HandlerUnwinds[Entry][CodeAddress(Use.Args[1].C)];
          OtherHandlers.insert(Entry);
        }
    }
  }

  std::vector<LowSEHLocalUnwind> Sites;
  for (const UseEvent &Use : Interp.Uses) {
    if (!Use.Runtime || Use.Return == InvalidVA)
      continue;
    const Val &Frame = Use.Args[0];
    const Val &Target = Use.Args[1];
    const va_t TargetVA = CodeAddress(Target.C);
    const char *Why = nullptr;
    if (Frame != Val::at(Base::Own, Convention->TargetFrame))
      Why = "the frame it passes is not the function's own";
    else if (Use.SP != Val::at(Base::Own, Convention->BodySP))
      Why = "it runs on another stack pointer than the body's";
    else if (Target.K != Val::Const || !EH.ownsCode(TargetVA) ||
             TargetVA == Function.Entry)
      Why = "its target is no exact address in the function's body";
    else if (!Starts.count(TargetVA))
      Why = "its target starts no block";
    else if (!Convention->ResumeReturnValue &&
             readBeforeWritten(Function, TargetVA, TRI.IntReturnReg, Pointer,
                               WorkRemaining))
      // The routine fixes no resume value: the target must not read the one
      // the call left.
      Why = "the target reads the result register, which the routine leaves "
            "unknown";
    if (Why) {
      if (Refused)
        Refused->push_back({Use.At, Why});
      continue;
    }
    LowSEHLocalUnwind Site;
    Site.CallAddr = Use.At;
    Site.CallSeq = Use.Seq;
    Site.ControlPc = Convention->ControlPcIsReturnAddress ? Use.Return : Use.At;
    Site.Target = TargetVA;
    Site.HandlerFrameFromSP = Convention->HandlerFrame - Convention->BodySP;
    // A frame pointer the body's stack pointer equals is that stack pointer.
    if (Convention->FramePointerIsHandlerFrame &&
        Convention->HandlerFrame != Convention->BodySP)
      Site.HandlerFrameRegister = TRI.FramePointer;
    Site.ResumeReturnValue = Convention->ResumeReturnValue;
    Site.TargetFrame = Convention->TargetFrame;
    Sites.push_back(Site);
  }

  // The targets only these calls name.  A branch to one is ordinary flow; a
  // value is a name, which another call or the runtime may enter it by.
  std::map<va_t, bool> Alone;
  for (const LowSEHLocalUnwind &Site : Sites)
    Alone.emplace(Site.Target, true);
  for (const auto &[Handler, Targets] : HandlerUnwinds)
    for (const auto &[Target, Calls] : Targets)
      Alone.emplace(Target, true);
  auto Name = [&](uint64_t Value) {
    if (const auto It = Alone.find(CodeAddress(Value)); It != Alone.end())
      It->second = false;
  };
  auto NameAll = [&] {
    for (auto &Entry : Alone)
      Entry.second = false;
  };
  // Every constant operand of a handler \p F but a branch or call target,
  // and every code address it takes PC-relatively.
  auto NameIn = [&](const LowFunc &F) {
    // A handler's own unwind of this frame runs as its `__finally` clause's
    // `goto`, so it names its target for no entry of its own: each mention
    // that is such a call's argument goes uncounted.
    std::map<va_t, unsigned> Calls;
    if (const auto It = HandlerUnwinds.find(F.Entry);
        It != HandlerUnwinds.end())
      Calls = It->second;
    std::map<va_t, unsigned> Mentions;
    for (const LowBlock &Block : F.Blocks) {
      if (!charge(WorkRemaining, Block.Ops.size() + 1))
        return false;
      for (const LowOp &Op : Block.Ops) {
        const bool Transfer = Op.Opcode == NdOp::BRANCH ||
                              Op.Opcode == NdOp::COND_BR ||
                              Op.Opcode == NdOp::CALL;
        for (unsigned I = Transfer ? 1 : 0; I < Op.NumInputs; ++I)
          if (Op.Inputs[I].isConst())
            ++Mentions[CodeAddress(Op.Inputs[I].Offset)];
      }
    }
    for (const auto &[Value, Count] : Mentions) {
      const auto Call = Calls.find(Value);
      if (Call == Calls.end() || Count > Call->second)
        Name(Value);
    }
    for (va_t Target : F.CodeRefTargets)
      if (!Calls.count(CodeAddress(Target)))
        Name(Target);
    return true;
  };
  if (!Alone.empty()) {
    std::set<std::pair<va_t, int>> Certified;
    for (const LowSEHLocalUnwind &Site : Sites)
      Certified.insert({Site.CallAddr, Site.CallSeq});
    // In the function itself, what its values reach: memory, a call or an
    // intrinsic other than the certified ones, or its caller.
    for (const StoreEvent &Store : Interp.Stores)
      if (Store.Value.K == Val::Const)
        Name(Store.Value.C);
    for (const UseEvent &Use : Interp.Uses)
      if (!Certified.count({Use.At, Use.Seq}))
        for (const Val &Arg : Use.Args)
          if (Arg.K == Val::Const)
            Name(Arg.C);
    for (const Val &Returned : Interp.Returns)
      if (Returned.K == Val::Const)
        Name(Returned.C);
    // Every out-of-line filter and termination handler, and those nested in
    // them: a finally that local-unwinds to a target names it too.
    std::set<va_t> Seen;
    std::vector<va_t> Pending;
    // A filter expression is not a function of its own unless it has a frame:
    // an unframed leaf the pipeline holds no body of calls nothing, and C gives
    // an expression no way to name a label.
    std::set<va_t> Filters;
    auto AddHandlers = [&](const ExceptionFunction &E) {
      if (!E.SEH)
        return;
      for (const SEHScopeRecord &Scope : E.SEH->Scopes)
        if ((Scope.Kind == SEHScopeKind::Filter ||
             Scope.Kind == SEHScopeKind::Finally) &&
            Scope.FilterOrFinallyVA && !E.ownsCode(Scope.FilterOrFinallyVA)) {
          Pending.push_back(Scope.FilterOrFinallyVA);
          if (Scope.Kind == SEHScopeKind::Filter)
            Filters.insert(Scope.FilterOrFinallyVA);
        }
    };
    AddHandlers(EH);
    Pending.insert(Pending.end(), OtherHandlers.begin(), OtherHandlers.end());
    while (!Pending.empty()) {
      const va_t Handler = Pending.back();
      Pending.pop_back();
      if (!Seen.insert(Handler).second)
        continue;
      const auto Body = Bodies.find(Handler);
      if (Filters.count(Handler) && (Body == Bodies.end() || !Body->second))
        continue;
      if (Body == Bodies.end() || !Body->second || !NameIn(*Body->second)) {
        NameAll();
        break;
      }
      if (Body->second->ExceptionMetadata)
        AddHandlers(*Body->second->ExceptionMetadata);
    }
  }
  // A handler resumes the function with the callee-saved registers it held
  // where the runtime called the handler: the code at the target cannot read
  // one the ranges the handler ends write, whose value there it cannot know.
  for (const auto &[Handler, Targets] : HandlerUnwinds) {
    std::set<uint64_t> Written;
    for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
      if (Scope.Kind != SEHScopeKind::Finally ||
          Scope.FilterOrFinallyVA != Handler)
        continue;
      const std::optional<ExceptionAddressRange> Range =
          getSemanticSEHGuardedRange(Scope, TRI.TheArch, EH);
      if (!Range) {
        for (const auto &[Target, Calls] : Targets)
          Alone[Target] = false;
        break;
      }
      for (const LowBlock &Block : Function.Blocks)
        if (Block.StartAddr < Range->End && Block.EndAddr > Range->Begin) {
          if (!charge(WorkRemaining, Block.Ops.size() + 1))
            return refuse("work budget exhausted in " + hex(Function.Entry));
          for (const LowOp &Op : Block.Ops)
            if (Op.Addr >= Range->Begin && Op.Addr < Range->End &&
                Op.Output.isReg() && Op.Output.Size &&
                Op.Output.Offset != TRI.StackPointer &&
                TRI.isCallPreserved(Op.Output.Offset -
                                        Op.Output.Offset % Pointer,
                                    Pointer, BinaryFormat::COFF))
              Written.insert(Op.Output.Offset - Op.Output.Offset % Pointer);
        }
    }
    for (const auto &[Target, Calls] : Targets)
      for (uint64_t Reg : Written)
        if (Alone[Target] &&
            readBeforeWritten(Function, Target, Reg, Pointer, WorkRemaining,
                              /*CallsKeepIt=*/true))
          Alone[Target] = false;
  }
  for (LowSEHLocalUnwind &Site : Sites)
    Site.OnlyExitsName = Alone[Site.Target];
  if (HandlerTargets)
    for (const auto &[Handler, Targets] : HandlerUnwinds)
      for (const auto &[Target, Calls] : Targets)
        if (Alone[Target])
          HandlerTargets->insert(Target);
  return Sites;
}

} // namespace neverd
