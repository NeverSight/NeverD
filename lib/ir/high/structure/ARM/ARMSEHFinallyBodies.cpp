//===- ARMSEHFinallyBodies.cpp - ARM __finally termination bodies --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM and AArch64 call a `__finally` termination handler out of line with the
/// abnormal-termination flag as its first argument.  The unwinder restores the
/// parent's callee-saved registers to their values where the exception struck
/// first, so the handler reads the parent's state through them.  This spells
/// the handler's body in the parent, with each register it reads replaced by
/// the value buildSsa proved the parent holds in it throughout every range the
/// handler ends.
///
//===----------------------------------------------------------------------===//

#include "../SEHFinallyBodiesDetail.h"

#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/STLExtras.h"

#include <functional>
#include <map>
#include <memory>
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
      Full = HighExpr::makeVar(SP, NdType::makeInt(Width, false));
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
    case SEHFuncletRegisterValue::Kind::EntryRegister:
      // C has no name for what a callee-saved register held on entry: the
      // parent's own reads of it print as the unknown register they are, and
      // the handler's read stays one too.  A nested handler does this, whose
      // parent runs under the unwinder, with or without that parent's parent
      // in the view.
      return Read;
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

  /// Resolve the parent's entry stack pointer before a body takes fresh names,
  /// which a fresh entry stack pointer takes from too.
  void prepare(FuncletNames &Names) {
    SP = entryStackPointer(Parent, TargetArch, Names);
  }

private:
  const HighFunc &Parent;
  Arch TargetArch;
  const TargetRegInfo &TRI;
  MedVar SP;
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

/// A handler's call that forwards an argument register past \p MaxParam, the
/// last whose value the parent can spell, as a helper with variadic arguments
/// or a callee of unknown signature reads them all.  What the dispatcher left
/// there, or a frame the parent has no form for, is nothing the handler knows,
/// and the source spells it as any other unknown value.
void unknownScratchArguments(std::vector<HighStmt> &Body, int MaxParam) {
  mapExprs(Body, [&](ExprPtr E) -> ExprPtr {
    if (!E || E->Kind != ExprKind::Call)
      return E;
    for (ExprPtr &Argument : E->Operands) {
      const HighExpr *Read = Argument.get();
      while (Read &&
             (Read->Kind == ExprKind::Cast || Read->Kind == ExprKind::BitCast))
        Read = Read->Operands.empty() ? nullptr : Read->Operands[0].get();
      if (Read && Read->Kind == ExprKind::Var &&
          Read->Var.Kind == MedVar::Param && Read->Var.Id > MaxParam) {
        TypeRef Type = Argument->Type;
        Argument = HighExpr::makeUndef(Read->Var.Size);
        if (Type)
          Argument->Type = std::move(Type);
      }
    }
    return E;
  });
}

/// The unwinder restores the parent's callee-saved registers to their values
/// where the exception struck first, and the handler reads the parent's state
/// through them: each it reads is the value buildSsa proved the parent holds in
/// it throughout every range the handler ends.
class ARMProjection final : public HandlerProjection {
public:
  ARMProjection(HighFunc &Parent, Arch TargetArch)
      : Parent(Parent), TargetArch(TargetArch), View(Parent, TargetArch) {}

  void prepare(FuncletNames &Names) override { View.prepare(Names); }

  std::optional<std::vector<HighStmt>>
  project(const HighFunc &Handler, const std::set<va_t> &UnwindTargets,
          const ExprPtr &Flag, FuncletNames &Names) override {
    static const RegisterValues NoRegisters;
    const auto Found = Parent.SEHFuncletRegisters.find(Handler.Entry);
    const RegisterValues &Registers =
        Found == Parent.SEHFuncletRegisters.end() ? NoRegisters : Found->second;
    const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
    auto Known = [&](const MedVar &V) {
      return V.Kind == MedVar::Reg && !TRI.isStackPointer(V.RegOff) &&
             Registers.count(V.RegOff);
    };
    // The flag arrives in the first argument register; an AArch64 handler also
    // takes the parent's frame pointer in the second, as the C-specific handler
    // passes it, whose value the parent's map holds where the protected ranges
    // leave it alone.  ARM's takes the EstablisherFrame there, the stack
    // pointer the parent was entered with.
    std::optional<SEHFuncletRegisterValue> Frame;
    if (TargetArch == Arch::AArch64) {
      auto Held = Registers.find(TRI.FramePointer);
      if (Held == Registers.end())
        Held = Registers.find(TRI.StackPointer);
      if (Held != Registers.end())
        Frame = Held->second;
    } else {
      Frame.emplace();
      Frame->ValueKind = SEHFuncletRegisterValue::Kind::EntryStackOffset;
      Frame->Offset = 0;
    }
    const int MaxParam = Frame ? 1 : 0;
    const IntegerArgumentLayout Arguments =
        TRI.integerArgumentLayout(BinaryFormat::COFF);
    if (Handler.SourceTypeHint ||
        Arguments.Registers.size() <= static_cast<size_t>(MaxParam) ||
        Handler.Params.size() > Arguments.Registers.size())
      return std::nullopt;
    for (size_t I = 0; I < Handler.Params.size(); ++I)
      if (Handler.Params[I].RegOff != Arguments.Registers[I])
        return std::nullopt;
    std::vector<HighStmt> Body = cloneStmts(Handler.Body);
    jumpLocalUnwinds(Body, UnwindTargets, TargetArch);
    if (!settleHandlerReturns(Body))
      return std::nullopt;
    dropUnreadValues(Body);
    unknownScratchArguments(Body, MaxParam);
    if (!localizeHandlerFrame(Body, TargetArch))
      return std::nullopt;
    if (!funcletVariablesMovable(Body, TargetArch, MaxParam, Known))
      return std::nullopt;
    if (!renameFuncletLocals(Body, Names))
      return std::nullopt;
    const std::set<VarKey> Defined = definedVariables(Body);
    bool Spelled = true;
    mapExprs(Body, [&](ExprPtr E) -> ExprPtr {
      if (!E || E->Kind != ExprKind::Var)
        return E;
      if (E->Var.Kind == MedVar::Param && E->Var.Id == 1) {
        ExprPtr Value = View.value(*Frame, E);
        if (!Value)
          Spelled = false;
        return Value ? Value : E;
      }
      if (E->Var.Kind == MedVar::Param) {
        ExprPtr Value = cloneExpr(Flag);
        if (E->Type)
          Value->Type = E->Type;
        return Value;
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
    return Body;
  }

private:
  HighFunc &Parent;
  Arch TargetArch;
  ParentView View;
};

} // namespace

std::unique_ptr<HandlerProjection> makeARMProjection(HighFunc &Parent,
                                                     Arch TargetArch) {
  return std::make_unique<ARMProjection>(Parent, TargetArch);
}

} // namespace seh_finally
} // namespace neverd
