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
///  - the parameter that arrived in the return register of a function that
///    never writes it (AArch64 x0 of a writer through its pointer argument):
///    a value only when a caller reads it;
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
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/SymbolDecoration.h"
#include "neverd/loader/SymbolSpelling.h"
#include "neverd/pipeline/Limits.h"
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
  /// Any other value.
  Other,
};

/// One function's SSA definitions and the values it reads.
struct FunctionFacts {
  const MedFunc *Func = nullptr;
  std::map<ValueKey, const MedOp *> Defs;
  /// The intrinsic each bound output of a multi-output intrinsic comes from.
  std::map<ValueKey, const MedOp *> IntrinsicOutputDefs;
  std::map<ValueKey, const PhiNode *> Phis;
  /// Values an operation other than a return reads, directly or through the
  /// PHIs that merge them.
  std::set<ValueKey> Read;
  /// Values a return hands back, directly or through the PHIs that merge
  /// them.
  std::set<ValueKey> Returned;
  /// The results of the calls to each function of this image: a direct
  /// call's target, or the routine an import's stub or slot reaches
  /// (ownRoutineOf).
  std::map<va_t, std::vector<ValueKey>> CallResults;
  /// What the return register holds at each return; none for the value it
  /// entered the function with.
  std::vector<std::optional<MedVar>> Returns;
  /// Whether any operation or PHI writes the return register.  A return
  /// whose value no backward walk finds holds the entry value only when
  /// none does; otherwise a join can hide a value written before it.
  bool WritesReturnRegister = false;
};

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

/// The constant addresses \p Call reaches its callee by: a direct call's
/// target, or the slot an indirect call reads it from (`call *slot`, or a load
/// of the slot it calls through), given each value's definition \p Defs.
struct CallAddress {
  std::optional<va_t> Target, Slot;
};
CallAddress callAddressOf(const MedFunc &Func, const MedOp &Call,
                          const std::map<ValueKey, const MedOp *> &Defs) {
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
  std::vector<const MedOp *> Calls;
  // Each PHI's arguments, by its output: they are read where it is.
  std::map<ValueKey, std::vector<ValueKey>> PhiArgs;
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis) {
      Facts.WritesReturnRegister |= Phi.Output.Kind == MedVar::Reg &&
                                    Phi.Output.RegOff == TRI.IntReturnReg;
      Facts.Phis.emplace(keyOf(Phi.Output), &Phi);
      for (const auto &[Predecessor, Value] : Phi.Args) {
        (void)Predecessor;
        if (!Value.isConst())
          PhiArgs[keyOf(Phi.Output)].push_back(keyOf(Value));
      }
    }
    for (const MedOp &Op : Block.Ops) {
      // An entry copy of the register into itself writes nothing.
      const auto WritesReturn = [&](const MedVar &Value) {
        return Value.Kind == MedVar::Reg && Value.RegOff == TRI.IntReturnReg &&
               !(Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                 keyOf(Op.Inputs[0]) == keyOf(Value));
      };
      Facts.WritesReturnRegister |=
          WritesReturn(Op.Output) ||
          llvm::any_of(Op.IntrinsicOutputs, WritesReturn);
      if (Op.Output.Size && !Op.Output.isConst())
        Facts.Defs.emplace(keyOf(Op.Output), &Op);
      for (const MedVar &Output : Op.IntrinsicOutputs)
        if (!Output.isConst())
          Facts.IntrinsicOutputDefs.emplace(keyOf(Output), &Op);
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
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Op.Output.Size && !Op.Output.isConst())
        Calls.push_back(&Op);
    }
  }
  // A merge nothing reads, such as the return register's after a call made
  // on one path only, reads none of its arguments.
  const auto ThroughPhis = [&](std::set<ValueKey> &Values) {
    std::vector<ValueKey> Pending(Values.begin(), Values.end());
    while (!Pending.empty()) {
      const ValueKey Merged = Pending.back();
      Pending.pop_back();
      if (const auto Args = PhiArgs.find(Merged); Args != PhiArgs.end())
        for (const ValueKey &Arg : Args->second)
          if (Values.insert(Arg).second)
            Pending.push_back(Arg);
    }
  };
  ThroughPhis(Facts.Read);
  ThroughPhis(Facts.Returned);
  // A call's callee is known once every definition is: an indirect call may
  // load its slot in a later block.
  for (const MedOp *Call : Calls) {
    const CallAddress Address = callAddressOf(Func, *Call, Facts.Defs);
    std::optional<va_t> Callee = Address.Target;
    if (const std::optional<va_t> Own =
            ownRoutineOf(Img, ArgumentSources, Address))
      Callee = Own;
    if (Callee)
      Facts.CallResults[*Callee].push_back(keyOf(Call->Output));
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
      Facts.emplace(Func.Entry,
                    collectFacts(Func, TRI, Img, Result.CallArgumentSources));
      if (const std::optional<bool> None = declaredNoValue(Func))
        Declared.emplace(Func.Entry, *None);
      if (!Func.Name.empty())
        Names.emplace(Func.Entry, Func.Name);
    }
  }

  void settle() {
    for (unsigned Round = 0;
         Round < ::neverd::limits::pipeline::kPipelineReturnContractsMaxRounds;
         ++Round) {
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
  /// The numbers the i386 PC-rooted proof gives each function's values
  /// (MedToHighConverter::i386PcRootedNumbers), computed on first use.
  mutable std::map<const MedFunc *, std::map<VarKey, uint32_t>> PcRootedNumbers;
  /// What each declared function's declaration says: true for none.
  std::map<va_t, bool> Declared;
  std::map<va_t, std::string> Names;

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

  /// Whether the call \p Call returns no value: it does not return, or its
  /// callee returns none.
  bool callReturnsNoValue(const FunctionFacts &F, const MedOp &Call) const {
    if (Call.DoesNotReturn)
      return true;
    if (Call.NumInputs < 1)
      return false;
    const CallAddress Address = callAddressOf(*F.Func, Call, F.Defs);
    std::optional<va_t> Target = Address.Target, Slot = Address.Slot;
    // A call through the stub or slot of an import the loader starts at
    // this image's own routine calls that routine, as the C prints it.
    if (const std::optional<va_t> Own =
            ownRoutineOf(Img, Result.CallArgumentSources, Address)) {
      Target = Own;
      Slot.reset();
    }
    // A callee's declaration decides, then what its name says, then what
    // this pass settled for it; the program's prototype of a definition is
    // its declaration's.  One this pass does not decompile is declared as the
    // C writers read it, by the debug information or a prototype of its
    // name: the call they print returns nothing then.
    if (Target) {
      if (const auto Declaration = Declared.find(*Target);
          Declaration != Declared.end())
        return Declaration->second;
      if (Dbg && !Facts.count(*Target))
        if (const auto Debug = Dbg->resolveFunction(*Target);
            Debug && Debug->Addr == *Target && Debug->ReturnType)
          return Debug->ReturnType->Kind == NdTypeKind::Void;
      if (const auto Name = Names.find(*Target);
          Name != Names.end() && !Img.functionHasBytePatch(*Target))
        if (const std::optional<bool> None =
                namedNoValue(Name->second, /*ByProgramPrototype=*/false))
          return *None;
      if (const auto Known = NoValue.find(*Target); Known != NoValue.end())
        return Known->second;
    }
    if (const Import *Imp =
            importAt(Target ? *Target : Slot.value_or(InvalidVA)))
      return namedNoValue(Imp->Name).value_or(false);
    // A loaded GOT slot names its import through the canonical storage view
    // (`mov rax, [slot]; jmp rax`).
    if (Slot) {
      const ImportStorageSlotCollection Storage =
          Img.collectImportStorageSlot(*Slot);
      if (const auto It = Storage.Slots.find(*Slot);
          It != Storage.Slots.end() && !Storage.Conflicts.count(*Slot) &&
          It->second.Addend == 0)
        return namedNoValue(It->second.Name).value_or(false);
    }
    return false;
  }

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
    auto [Numbers, Computed] = PcRootedNumbers.try_emplace(F.Func);
    if (Computed)
      Numbers->second =
          MedToHighConverter::i386PcRootedNumbers(*F.Func, Img.Arch, &Img);
    const auto Number = Numbers->second.find(varKey(Value));
    return Number != Numbers->second.end() && Number->second == uint32_t(*GOT)
               ? GOTAddressKind::Reloaded
               : GOTAddressKind::None;
  }

  /// What \p Value hands back, each path a PHI joins counted on its own.
  void classify(const FunctionFacts &F, const MedVar &Value,
                std::vector<ReturnedValue> &Paths, std::set<ValueKey> &Seen,
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
        Paths.push_back(!entersReturnRegister(Func) ? ReturnedValue::Undefined
                        : F.WritesReturnRegister    ? ReturnedValue::Other
                                                    : ReturnedValue::Incoming);
        continue;
      }
      std::set<ValueKey> Seen;
      classify(F, *Returned, Paths, Seen, 0);
    }
    // No ordinary RETURN is not a proof of no return: an external catch
    // funclet can resume its parent and supply a value after attachment. Use
    // the shared proof, which includes exceptional edges and missing handlers.
    if (Paths.empty())
      return Func.DoesNotReturn || hasProvenNoReturnExit(Func, Img.Arch);
    const bool Read = callerReadsResult(Func);
    for (ReturnedValue &Path : Paths)
      if (Path == ReturnedValue::Incoming ||
          Path == ReturnedValue::SystemCallResult ||
          Path == ReturnedValue::ReloadedGOTAddress)
        Path = Read ? ReturnedValue::Other : ReturnedValue::Undefined;
    const auto Count = [&](ReturnedValue Kind) {
      return llvm::count(Paths, Kind);
    };
    if (Count(ReturnedValue::Undefined) == static_cast<long>(Paths.size()))
      return true;
    return Count(ReturnedValue::Undefined) &&
           !Count(ReturnedValue::Deliberate) && !Read;
  }
};

} // namespace

void settleReturnContracts(const BinaryImage &Img, PipelineResult &Result,
                           const DebugContext *Dbg) {
  ReturnContracts(Img, Result, Dbg).settle();
}

} // namespace neverd
