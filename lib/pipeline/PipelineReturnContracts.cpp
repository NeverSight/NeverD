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
///  - nothing defined: the return register as the function entered it (when
///    no parameter arrives in it), an unspecified value, the result of a
///    callee that itself returns no value, or of an instruction run for its
///    effect;
///  - a deliberate value: a constant or a comparison, as `return 0` or a
///    `setcc` byte merged into the register;
///  - some other value.
/// A function returns no value when every path hands back nothing defined,
/// or when one does and none hands back a deliberate value, unless a caller
/// reads what a call to it returns.  What a declaration says decides first:
/// a bound source signature, debug information, or the MSVC rule that a
/// destructor returns nothing; a routine known by name returns what its
/// prototype declares.  Code generation keeps the register.
///
//===----------------------------------------------------------------------===//

#include "PipelineReturnModelingDetail.h"

#include "neverd/Common.h"
#include "neverd/backend/c/MsvcCallee.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/X86FPState.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/SymbolDecoration.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/STLExtras.h"

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

enum class ReturnedValue : uint8_t {
  /// Nothing the function defines.
  Undefined,
  /// A constant or a comparison: a value the function means.
  Deliberate,
  /// Any other value.
  Other,
};

/// One function's SSA definitions and the values it reads.
struct FunctionFacts {
  const MedFunc *Func = nullptr;
  std::map<ValueKey, const MedOp *> Defs;
  std::map<ValueKey, const PhiNode *> Phis;
  /// Values an operation other than a return reads.
  std::set<ValueKey> Read;
  /// Values a return hands back.
  std::set<ValueKey> Returned;
  /// The results of the calls to each direct callee.
  std::map<va_t, std::vector<ValueKey>> CallResults;
  /// What the return register holds at each return; none for the value it
  /// entered the function with.
  std::vector<std::optional<MedVar>> Returns;
};

/// What the return register holds at the RETURN \p Index of \p Block: the
/// RETURN's operand where that is the register (an x86 RETURN can read a
/// temporary copy propagation left it), else the last full write to it on
/// the way there.  None for the value the function entered with.  On ARM
/// and AArch64 the operand is the return address.
std::optional<MedVar> returnRegisterAt(const MedFunc &Func,
                                       const MedBlock &Block, size_t Index,
                                       const TargetRegInfo &TRI, Arch Target) {
  const MedOp &Return = Block.Ops[Index];
  if (Return.NumInputs >= 1) {
    const MedVar &Operand = Return.Inputs[0];
    if (Operand.Kind == MedVar::Reg && Operand.RegOff == TRI.IntReturnReg)
      return Operand;
    if ((Target == Arch::X86 || Target == Arch::X64) &&
        (Operand.isConst() || Operand.Kind == MedVar::Temp))
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
    for (size_t I = End; I-- > 0;)
      if (Writes(Current->Ops[I].Output))
        return Current->Ops[I].Output;
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

FunctionFacts collectFacts(const MedFunc &Func, const TargetRegInfo &TRI,
                           Arch Target) {
  FunctionFacts Facts;
  Facts.Func = &Func;
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis) {
      Facts.Phis.emplace(keyOf(Phi.Output), &Phi);
      for (const auto &[Predecessor, Value] : Phi.Args) {
        (void)Predecessor;
        if (!Value.isConst())
          Facts.Read.insert(keyOf(Value));
      }
    }
    for (const MedOp &Op : Block.Ops) {
      if (Op.Output.Size && !Op.Output.isConst())
        Facts.Defs.emplace(keyOf(Op.Output), &Op);
      if (Op.Opcode == NdOp::RETURN) {
        const auto Returned = returnRegisterAt(
            Func, Block, static_cast<size_t>(&Op - Block.Ops.data()), TRI,
            Target);
        Facts.Returns.push_back(Returned);
        if (Returned && !Returned->isConst())
          Facts.Returned.insert(keyOf(*Returned));
        continue;
      }
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (!Op.Inputs[I].isConst())
          Facts.Read.insert(keyOf(Op.Inputs[I]));
      if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
          Op.Inputs[0].isConst() && Op.Output.Size && !Op.Output.isConst())
        Facts.CallResults[static_cast<va_t>(Op.Inputs[0].ConstVal)].push_back(
            keyOf(Op.Output));
    }
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
    for (const MedFunc &Func : Result.MedFuncs) {
      Facts.emplace(Func.Entry, collectFacts(Func, TRI, Img.Arch));
      if (const std::optional<bool> None = declaredNoValue(Func))
        Declared.emplace(Func.Entry, *None);
      if (!Func.Name.empty())
        Names.emplace(Func.Entry, Func.Name);
    }
  }

  void settle() {
    // Each function's answer reads its callees' and its callers': repeat
    // until no answer changes, within a bound for call-graph cycles.
    constexpr unsigned MaxRounds = 8;
    for (unsigned Round = 0; Round < MaxRounds; ++Round) {
      bool Changed = false;
      for (const MedFunc &Func : Result.MedFuncs) {
        const auto Declaration = Declared.find(Func.Entry);
        const bool None = Declaration != Declared.end() ? Declaration->second
                                                        : returnsNoValue(Func);
        bool &Known = NoValue[Func.Entry];
        Changed |= Known != None;
        Known = None;
      }
      if (!Changed)
        break;
    }
    for (MedFunc &Func : Result.MedFuncs)
      Func.ReturnsNoValue = NoValue[Func.Entry];
  }

private:
  const BinaryImage &Img;
  PipelineResult &Result;
  const DebugContext *Dbg;
  const TargetRegInfo &TRI;
  std::map<va_t, FunctionFacts> Facts;
  std::map<va_t, bool> NoValue;
  /// What each declared function's declaration says: true for none.
  std::map<va_t, bool> Declared;
  std::map<va_t, std::string> Names;

  /// Whether \p Func's declaration says it returns nothing, as the C
  /// writers read it: a bound source signature, else debug information, else
  /// the MSVC rule that a destructor returns nothing.  None when nothing
  /// declares it.
  std::optional<bool> declaredNoValue(const MedFunc &Func) const {
    if (Func.SourceTypeHint && Func.SourceTypeHint->ReturnType)
      return Func.SourceTypeHint->ReturnType->Kind == NdTypeKind::Void;
    if (Dbg)
      if (const auto Debug = Dbg->resolveFunction(Func.Entry);
          Debug && Debug->ReturnType)
        return Debug->ReturnType->Kind == NdTypeKind::Void;
    if (isMsvcDestructorName(Func.Name))
      return true;
    return std::nullopt;
  }

  /// Whether a routine called \p Name returns nothing by its prototype, or
  /// as an MSVC destructor.  None when neither knows it.
  std::optional<bool> namedNoValue(llvm::StringRef Name) const {
    if (Name.empty())
      return std::nullopt;
    if (isMsvcDestructorName(Name))
      return true;
    const std::string CName =
        importNamesAreCNames(Img.Format)
            ? Name.str()
            : cNameOfSymbol(Name, Img.Format, Img.Arch).str();
    if (const std::optional<bool> Returns =
            libc::libcReturnsValue(CName, Img.abiFormat()))
      return !*Returns;
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

  /// Whether the call \p Call returns no value: it does not return, or its
  /// callee returns none.
  bool callReturnsNoValue(const FunctionFacts &F, const MedOp &Call) const {
    if (Call.DoesNotReturn)
      return true;
    if (Call.NumInputs < 1)
      return false;
    std::optional<va_t> Target, Slot;
    const MedVar &Callee = Call.Inputs[0];
    if (Call.Opcode == NdOp::CALL && Callee.isConst())
      Target = static_cast<va_t>(Callee.ConstVal);
    else if (Call.Opcode == NdOp::INDIR_CALL && Callee.isConst())
      Slot = static_cast<va_t>(Callee.ConstVal);
    else if (Call.Opcode == NdOp::INDIR_CALL)
      if (const auto Def = F.Defs.find(keyOf(Callee)); Def != F.Defs.end())
        if (const MedOp &Load = *Def->second; Load.Opcode == NdOp::LOAD &&
                                              Load.NumInputs >= 1 &&
                                              Load.Inputs[0].isConst())
          Slot = static_cast<va_t>(Load.Inputs[0].ConstVal);
    // A callee's declaration decides, then a prototype of its name, then
    // what this pass settled for it.
    if (Target) {
      if (const auto Declaration = Declared.find(*Target);
          Declaration != Declared.end())
        return Declaration->second;
      if (const auto Name = Names.find(*Target); Name != Names.end())
        if (const std::optional<bool> None = namedNoValue(Name->second))
          return *None;
      if (const auto Known = NoValue.find(*Target); Known != NoValue.end())
        return Known->second;
    }
    const Import *Imp = importAt(Target ? *Target : Slot.value_or(InvalidVA));
    return Imp && namedNoValue(Imp->Name).value_or(false);
  }

  /// What \p Value hands back, each path a PHI joins counted on its own.
  void classify(const FunctionFacts &F, const MedVar &Value,
                std::vector<ReturnedValue> &Paths, std::set<ValueKey> &Seen,
                unsigned Depth) const {
    constexpr unsigned MaxDepth = 32;
    if (Value.isConst()) {
      Paths.push_back(ReturnedValue::Deliberate);
      return;
    }
    if (Value.Kind == MedVar::Unspecified) {
      Paths.push_back(ReturnedValue::Undefined);
      return;
    }
    if (Value.Kind == MedVar::Param || Depth > MaxDepth ||
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
                          ? ReturnedValue::Undefined
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
      std::vector<ReturnedValue> Low;
      classify(F, Op.Inputs[1], Low, Seen, Depth + 1);
      Paths.push_back(llvm::all_of(Low,
                                   [](ReturnedValue V) {
                                     return V == ReturnedValue::Deliberate;
                                   })
                          ? ReturnedValue::Deliberate
                          : ReturnedValue::Other);
      return;
    }
    case NdOp::SELECT: {
      if (Op.NumInputs != 3)
        break;
      std::vector<ReturnedValue> Arms;
      classify(F, Op.Inputs[1], Arms, Seen, Depth + 1);
      classify(F, Op.Inputs[2], Arms, Seen, Depth + 1);
      Paths.push_back(llvm::all_of(Arms,
                                   [](ReturnedValue V) {
                                     return V == ReturnedValue::Deliberate;
                                   })
                          ? ReturnedValue::Deliberate
                          : ReturnedValue::Other);
      return;
    }
    case NdOp::CALL:
    case NdOp::INDIR_CALL:
      Paths.push_back(callReturnsNoValue(F, Op) ? ReturnedValue::Undefined
                                                : ReturnedValue::Other);
      return;
    case NdOp::INTRINSIC: {
      // An instruction run for its effect (a fence, a breakpoint, a system
      // call) or one C has no name for leaves no value the function means;
      // the x64 debug service returns its status, and an x87 state read
      // (fnstsw, fnstcw) the state.
      const Intrinsic Id = Op.NumInputs >= 1 && Op.Inputs[0].isConst()
                               ? static_cast<Intrinsic>(Op.Inputs[0].ConstVal)
                               : Intrinsic::None;
      Paths.push_back(Id != Intrinsic::None && Id != Intrinsic::DebugService &&
                              Id != Intrinsic::CetRdSsp &&
                              !x86FPStateReturnsValue(Id) &&
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

  /// Whether a caller reads what a call to \p Func returns.
  bool callerReadsResult(const MedFunc &Func) const {
    for (const auto &[Entry, Caller] : Facts)
      if (const auto Calls = Caller.CallResults.find(Func.Entry);
          Calls != Caller.CallResults.end())
        for (const ValueKey &Result : Calls->second) {
          if (Caller.Read.count(Result))
            return true;
          // A caller that hands it back on reads it when it returns a value.
          if (Caller.Returned.count(Result))
            if (const auto Known = NoValue.find(Entry);
                Known == NoValue.end() || !Known->second)
              return true;
        }
    return false;
  }

  /// Whether a parameter arrives in the return register (AArch64 x0, ARM r0).
  bool entersReturnRegister(const MedFunc &Func) const {
    return llvm::any_of(Func.Params, [&](const MedVar &Param) {
      return Param.RegOff == TRI.IntReturnReg;
    });
  }

  bool returnsNoValue(const MedFunc &Func) const {
    if (!settles(Func))
      return false;
    const FunctionFacts &F = Facts.at(Func.Entry);
    std::vector<ReturnedValue> Paths;
    for (const std::optional<MedVar> &Returned : F.Returns) {
      if (!Returned) {
        Paths.push_back(entersReturnRegister(Func) ? ReturnedValue::Other
                                                   : ReturnedValue::Undefined);
        continue;
      }
      std::set<ValueKey> Seen;
      classify(F, *Returned, Paths, Seen, 0);
    }
    // A function that never returns hands nothing back.
    if (Paths.empty())
      return true;
    const auto Count = [&](ReturnedValue Kind) {
      return llvm::count(Paths, Kind);
    };
    if (Count(ReturnedValue::Undefined) == static_cast<long>(Paths.size()))
      return true;
    return Count(ReturnedValue::Undefined) &&
           !Count(ReturnedValue::Deliberate) && !callerReadsResult(Func);
  }
};

} // namespace

void settleReturnContracts(const BinaryImage &Img, PipelineResult &Result,
                           const DebugContext *Dbg) {
  ReturnContracts(Img, Result, Dbg).settle();
}

} // namespace neverd
