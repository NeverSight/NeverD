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
/// (MedByteDemand): not the views of the return register the lift keeps in
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
#include "neverd/ir/med/MedByteDemand.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/MedNoReturn.h"
#include "neverd/ir/med/MedStackGuard.h"
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

using ValueKey = MedValueKey;

ValueKey keyOf(const MedVar &Value) { return medValueKey(Value); }

template <typename T> using ValueMap = MedValueMap<T>;

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
  /// The values the function's stack protector check makes.
  MedStackGuard StackGuard;
  ValueMap<const MedOp *> Defs;
  /// The intrinsic each bound output of a multi-output intrinsic comes from.
  ValueMap<const MedOp *> IntrinsicOutputDefs;
  ValueMap<const PhiNode *> Phis;
  /// How this function uses what its calls to one function of this image
  /// return: a direct call's target, or the routine an import's stub or slot
  /// reaches (ownRoutineOf).
  struct ResultUse {
    /// Something the function does depends on a byte of one (MedByteDemand).
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

FunctionFacts collectFacts(const MedFunc &Func, const TargetRegInfo &TRI,
                           const BinaryImage &Img,
                           const std::map<va_t, va_t> &ArgumentSources) {
  const Arch Target = Img.Arch;
  FunctionFacts Facts;
  Facts.Func = &Func;
  Facts.StackGuard = MedStackGuard(Func, Target);
  std::vector<const MedOp *> Calls;
  MedByteDemand Demand(TRI, medLeavesOtherRegisters(Func));
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
        // classify sees through the views MedByteDemand::handBack follows.
        if (Returned)
          Demand.handBack(*Returned);
        continue;
      }
      Demand.define(Op);
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Op.Output.Size && !Op.Output.isConst())
        Calls.push_back(&Op);
    }
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
    Use.Returned |= Demand.handsBack(Call->Output);
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
  /// nothing depends on a byte of it (MedByteDemand), the operation reads zero
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
    // What a stack protector check leaves in the register shows the
    // function means no value: a compiler makes the check there only where
    // the register holds no result.
    if (F.StackGuard.holds(Value)) {
      Paths.push_back(ReturnedValue::Unwritten);
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
    // The parameter that arrived in the return register, handed back by a
    // function that never writes the register: Go passes its first argument
    // there, and a function with no result returns with it as it came.
    if (Value.Kind == MedVar::Param && Value.RegOff == TRI.IntReturnReg &&
        !F.WritesReturnRegister) {
      Paths.push_back(ReturnedValue::Incoming);
      return;
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
      // parameter arrives in it.  Only the return register itself shows the
      // function means none: another register's entry value was copied
      // there, by code whose reason this pass does not know.  A parameter
      // that arrived in the return register of a function that never writes
      // it is a value where a caller reads it.
      if (Value.Kind != MedVar::Reg || Value.SSAVer != 0) {
        Paths.push_back(ReturnedValue::Other);
        return;
      }
      const bool Parameter = llvm::any_of(F.Func->Params, [&](const MedVar &P) {
        return P.RegOff == Value.RegOff;
      });
      const bool ReturnRegister = Value.RegOff == TRI.IntReturnReg;
      Paths.push_back(Parameter ? ReturnRegister && !F.WritesReturnRegister
                                      ? ReturnedValue::Incoming
                                      : ReturnedValue::Other
                      : ReturnRegister ? ReturnedValue::Unwritten
                                       : ReturnedValue::Undefined);
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
