//===- ARMSEHFinallyBodies.cpp - ARM __finally termination bodies --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM and AArch64 call a `__finally` termination handler out of line with the
/// abnormal-termination flag as its only argument.  The unwinder restores the
/// parent's callee-saved registers to their values where the exception struck
/// first, so the handler reads the parent's state through them.  This puts
/// the handler's unwind work into the parent's empty `__finally` clause under
/// `if (AbnormalTermination())`, with each register it reads replaced by the
/// value buildSsa proved the parent holds in it throughout every range the
/// handler ends.  The compiler's normal-path code stays where it is, so the
/// finally still runs once on either exit.
///
//===----------------------------------------------------------------------===//

#include "../SEHFinallyBodiesDetail.h"

#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/STLExtras.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {
namespace seh_finally {
namespace {

using RegisterValues = std::map<uint64_t, SEHFuncletRegisterValue>;

/// What a handler's projection needs of one parent, computed on first use.
class ParentView {
public:
  ParentView(const HighFunc &Parent, Arch TargetArch)
      : Parent(Parent), TargetArch(TargetArch),
        TRI(getTargetRegInfo(TargetArch)) {}

  /// The parent's value of a register, as \p Read reads it.  Null when the
  /// value has no form the parent can spell.
  ExprPtr value(const SEHFuncletRegisterValue &Value, const ExprPtr &Read) {
    const uint16_t Width = TRI.PointerSize;
    ExprPtr Full;
    switch (Value.ValueKind) {
    case SEHFuncletRegisterValue::Kind::EntryStackOffset: {
      // Without a frame the writer does not spell the entry stack pointer as
      // `frame_base`.
      if (Parent.FrameSize <= 0 && Parent.FrameHeadroom <= 0)
        return nullptr;
      Full = HighExpr::makeVar(entrySP(), NdType::makeInt(Width, false));
      if (Value.Offset) {
        const bool Below = Value.Offset < 0;
        const uint64_t Magnitude = Below
                                       ? 0 - static_cast<uint64_t>(Value.Offset)
                                       : static_cast<uint64_t>(Value.Offset);
        Full = HighExpr::makeBinop(
            Below ? NdOp::INT_SUB : NdOp::INT_ADD, Full,
            HighExpr::makeConst(Magnitude, Width,
                                ConstantAddressProvenance::Scalar));
      }
      break;
    }
    case SEHFuncletRegisterValue::Kind::EntryArgument: {
      const std::optional<MedVar> Param = parameter(Value.RegOff);
      if (!Param)
        return nullptr;
      Full = HighExpr::makeVar(*Param, NdType::makeInt(Width, false));
      break;
    }
    case SEHFuncletRegisterValue::Kind::Constant:
      Full = HighExpr::makeConst(Value.Value, Width, Value.Provenance,
                                 Value.AddressOwnerVA);
      break;
    }
    const uint16_t Size = Read->Var.Size;
    if (Size == Width) {
      Full->Type = Read->Type ? Read->Type : NdType::makeInt(Width, false);
      return Full;
    }
    // A narrower view of the register is its low bytes.
    if (!Size || Size > Width)
      return nullptr;
    ExprPtr Low =
        HighExpr::makeBinop(NdOp::SUBBYTES, Full, HighExpr::makeConst(0, 4));
    Low->Type = Read->Type ? Read->Type : NdType::makeInt(Size, false);
    return Low;
  }

  /// The names the next attached handler body starts from.
  FuncletNames &names() {
    if (!Names)
      Names = firstFreeNames(Parent.Body);
    return *Names;
  }

  /// The parent's entry stack pointer.  A body resolves it before it takes
  /// fresh names, which a fresh entry stack pointer takes from too.
  const MedVar &entrySP() {
    if (!SP)
      SP = entryStackPointer(Parent, TargetArch, names());
    return *SP;
  }

private:
  const HighFunc &Parent;
  Arch TargetArch;
  const TargetRegInfo &TRI;
  std::optional<FuncletNames> Names;
  std::optional<MedVar> SP;
  std::map<uint64_t, std::optional<MedVar>> Parameters;

  /// The parameter that arrives in \p RegOff, when the parent reads it under
  /// its own name everywhere: no out-of-SSA name merges it with a later
  /// value, and nothing assigns it.
  std::optional<MedVar> parameter(uint64_t RegOff) {
    auto [It, Inserted] = Parameters.try_emplace(RegOff);
    if (!Inserted)
      return It->second;
    if (Parent.SourceTypeHint)
      return std::nullopt;
    std::optional<size_t> Index;
    for (size_t I = 0; I < Parent.Params.size(); ++I)
      if (Parent.Params[I].RegOff == RegOff) {
        if (Index)
          return std::nullopt;
        Index = I;
      }
    if (!Index || (Parent.Params[*Index].Type &&
                   Parent.Params[*Index].Type->Kind == NdTypeKind::Float))
      return std::nullopt;
    const int Id = static_cast<int>(*Index);
    bool Plain = true;
    auto IsIt = [&](const MedVar &V) {
      return V.Kind == MedVar::Param && V.Id == Id;
    };
    std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
      if (!E || !Plain)
        return;
      if (E->Kind == ExprKind::Var && IsIt(E->Var) && E->Var.RenameTag >= 0)
        Plain = false;
      for (const MedVar &Out : E->IntrinsicOutputs)
        if (IsIt(Out))
          Plain = false;
      E->forEachChildExpr(Visit);
    };
    walkStmts(Parent.Body, [&](const HighStmt &S) {
      if (S.Dst && S.Dst->Kind == ExprKind::Var && IsIt(S.Dst->Var))
        Plain = false;
      forEachExpr(S, Visit);
    });
    if (!Plain)
      return std::nullopt;
    MedVar Param;
    Param.Kind = MedVar::Param;
    Param.TheArch = TargetArch;
    Param.Id = Id;
    Param.SSAVer = 0;
    Param.RenameTag = -1;
    Param.RegOff = RegOff;
    Param.Size = TRI.PointerSize;
    It->second = Param;
    return Param;
  }
};

/// Whether \p Stmts are statements a projected handler may hold: straight-line
/// work and `if`s over it, with no loop, switch or region of their own.  A
/// `return` or a jump may stand in a branch the unwind path's folding drops;
/// settleExits refuses what survives it.
bool supportedShape(const std::vector<HighStmt> &Stmts) {
  for (const HighStmt &S : Stmts) {
    if (!S.Cases.empty() || !S.DefaultBody.empty() || !S.EHClauses.empty() ||
        !S.EHClauseBodies.empty())
      return false;
    switch (S.Kind) {
    case StmtKind::Assign:
    case StmtKind::Store:
    case StmtKind::ExprStmt:
    case StmtKind::Call:
    case StmtKind::Nop:
    case StmtKind::Return:
    case StmtKind::Goto:
      if (!S.Body.empty() || !S.ElseBody.empty())
        return false;
      break;
    case StmtKind::Block:
    case StmtKind::If:
    case StmtKind::IfElse:
      if (S.Kind != StmtKind::Block && !S.Cond)
        return false;
      if (!supportedShape(S.Body) || !supportedShape(S.ElseBody))
        return false;
      break;
    default:
      return false;
    }
  }
  return true;
}

/// Cut \p Body at its first top-level `return`, after which nothing runs.
/// False when a `return` is left inside a branch the folding kept, which the
/// clause could only take by leaving the `__finally`, or when any jump is
/// left, whose target the clause does not hold.
bool settleExits(std::vector<HighStmt> &Body) {
  for (size_t I = 0; I < Body.size(); ++I)
    if (Body[I].Kind == StmtKind::Return) {
      std::optional<HighStmt> Work = returnedWork(Body[I]);
      Body.resize(I);
      if (Work)
        Body.push_back(std::move(*Work));
      break;
    }
  bool Leaves = false;
  walkStmts(Body, [&](const HighStmt &S) {
    Leaves |= S.Kind == StmtKind::Return || S.Kind == StmtKind::Goto;
  });
  return !Leaves;
}

/// The handler's unwind work in the parent: \p Funclet's body with its flag
/// set, its entry registers replaced by \p Registers' parent values, and its
/// own names moved past the parent's.  Nullopt keeps the address comment.
std::optional<std::vector<HighStmt>>
projectUnwindWork(const HighFunc &Funclet, const RegisterValues &Registers,
                  ParentView &View, Arch TargetArch) {
  if (Funclet.Body.empty() ||
      (Funclet.ExceptionMetadata && Funclet.ExceptionMetadata->SEH &&
       !Funclet.ExceptionMetadata->SEH->Scopes.empty()) ||
      (Funclet.ExceptionMetadata &&
       (Funclet.ExceptionMetadata->Cxx || Funclet.ExceptionMetadata->Itanium ||
        Funclet.ExceptionMetadata->Registration)))
    return std::nullopt;
  if (!supportedShape(Funclet.Body) || callsLocalUnwind(Funclet.Body))
    return std::nullopt;
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  auto Known = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg && !TRI.isStackPointer(V.RegOff) &&
           Registers.count(V.RegOff);
  };
  // The flag arrives in the first argument register; an AArch64 handler also
  // takes the establisher frame in the second, which the parent's map holds
  // under the stack pointer when the protected ranges run on it.
  const auto Frame = TargetArch == Arch::AArch64
                         ? Registers.find(TRI.StackPointer)
                         : Registers.end();
  const int MaxParam = Frame != Registers.end() ? 1 : 0;
  const IntegerArgumentLayout Arguments =
      TRI.integerArgumentLayout(BinaryFormat::COFF);
  if (Funclet.SourceTypeHint ||
      Arguments.Registers.size() <= static_cast<size_t>(MaxParam) ||
      Funclet.Params.size() > Arguments.Registers.size())
    return std::nullopt;
  for (size_t I = 0; I < Funclet.Params.size(); ++I)
    if (Funclet.Params[I].RegOff != Arguments.Registers[I])
      return std::nullopt;
  if (!funcletVariablesMovable(Funclet.Body, TargetArch, MaxParam, Known))
    return std::nullopt;

  // A `__finally` yields no value; settleExits keeps what its `return` does.
  std::vector<HighStmt> Body = cloneStmts(Funclet.Body);
  if (llvm::any_of(Registers, [](const auto &Register) {
        return Register.second.ValueKind ==
               SEHFuncletRegisterValue::Kind::EntryStackOffset;
      }))
    View.entrySP();
  FuncletNames Names = View.names();
  if (!renameFuncletLocals(Body, Names))
    return std::nullopt;
  const std::set<VarKey> Defined = definedVariables(Body);
  bool Spelled = true;
  mapExprs(Body, [&](ExprPtr E) -> ExprPtr {
    if (!E || E->Kind != ExprKind::Var)
      return E;
    if (E->Var.Kind == MedVar::Param && E->Var.Id == 1) {
      ExprPtr Value = View.value(Frame->second, E);
      if (!Value)
        Spelled = false;
      return Value ? Value : E;
    }
    if (E->Var.Kind == MedVar::Param) {
      // The clause runs this work only while unwinding.
      ExprPtr Flag = HighExpr::makeConst(1, E->Var.Size ? E->Var.Size : 4,
                                         ConstantAddressProvenance::Scalar);
      if (E->Type)
        Flag->Type = E->Type;
      return Flag;
    }
    if (E->Var.Kind == MedVar::Reg && isEntryValue(E->Var, Defined)) {
      ExprPtr Value = View.value(Registers.at(E->Var.RegOff), E);
      if (!Value) {
        Spelled = false;
        return E;
      }
      return Value;
    }
    return E;
  });
  if (!Spelled)
    return std::nullopt;
  simplifyProjectedBody(Body);
  if (!settleExits(Body) || Body.empty())
    return std::nullopt;
  // A later attach takes fresh names past the ones this one takes.
  View.names() = Names;
  return Body;
}

} // namespace

void attachARM(std::vector<HighFunc> &Funcs, Arch TargetArch,
               std::set<va_t> *Merged) {
  // Index only the handlers some parent's scope table names, so a unit
  // without a termination handler pays nothing.
  std::map<va_t, const HighFunc *> ByEntry;
  auto OutOfLineFinally = [](const HighFunc &Func, const SEHScopeRecord &S) {
    return S.Kind == SEHScopeKind::Finally && S.FilterOrFinallyVA &&
           !Func.ExceptionMetadata->ownsCode(S.FilterOrFinallyVA);
  };
  for (const HighFunc &Func : Funcs)
    if (Func.ExceptionMetadata && Func.ExceptionMetadata->SEH)
      for (const SEHScopeRecord &Scope : Func.ExceptionMetadata->SEH->Scopes)
        if (OutOfLineFinally(Func, Scope))
          ByEntry.emplace(Scope.FilterOrFinallyVA, nullptr);
  if (ByEntry.empty())
    return;
  for (const HighFunc &Func : Funcs)
    if (auto It = ByEntry.find(Func.Entry); It != ByEntry.end() && !It->second)
      It->second = &Func;

  static const RegisterValues NoRegisters;
  for (HighFunc &Parent : Funcs) {
    if (!Parent.ExceptionMetadata || !Parent.ExceptionMetadata->SEH ||
        llvm::none_of(Parent.ExceptionMetadata->SEH->Scopes,
                      [&](const SEHScopeRecord &Scope) {
                        return OutOfLineFinally(Parent, Scope);
                      }))
      continue;
    // A region nested in another keeps its place: unlike x64's clean form,
    // the guarded clause removes nothing around it.
    std::vector<HighStmt *> Regions;
    const UnwindExits Exits(Parent);
    walkStmts(Parent.Body, [&](HighStmt &S) {
      if (isEmptyFinallyRegion(S) && protectedBodyQualifies(S, Exits))
        Regions.push_back(&S);
    });
    if (Regions.empty())
      continue;
    ParentView View(Parent, TargetArch);
    for (HighStmt *Stmt : Regions) {
      const va_t FuncletVA = Stmt->EHClauses.front().FilterOrActionVA;
      const auto Funclet = ByEntry.find(FuncletVA);
      if (Funclet == ByEntry.end() || !Funclet->second ||
          Funclet->second == &Parent)
        continue;
      const auto Registers = Parent.SEHFuncletRegisters.find(FuncletVA);
      std::optional<std::vector<HighStmt>> Work = projectUnwindWork(
          *Funclet->second,
          Registers == Parent.SEHFuncletRegisters.end() ? NoRegisters
                                                        : Registers->second,
          View, TargetArch);
      if (!Work)
        continue;
      Stmt->EHClauseBodies.front().push_back(
          guardAbnormalWork(std::move(*Work)));
      if (Merged)
        Merged->insert(FuncletVA);
    }
  }
}

} // namespace seh_finally
} // namespace neverd
