//===- X64SEHFinallyBodies.cpp - __finally termination bodies ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../pass/HighFrameAddress.h"
#include "../SEHFinallyBodiesDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighSourceFlow.h"

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

/// The incoming parameter a function homes at entry-SP displacement \p Offset,
/// as a Win64 prologue spills `[rsp + k] = arg`.  Only the statements before
/// the first structured region are consulted, and a displacement written more
/// than once, or by a non-parameter, yields nothing there.
std::map<int64_t, MedVar> homedParameters(const HighFunc &Func,
                                          Arch TargetArch) {
  std::map<int64_t, MedVar> Homed;
  std::set<int64_t> Ambiguous;
  size_t Budget = limits::kMaxCxxObjectAddressWork;
  for (const HighStmt &S : Func.Body) {
    if (S.Kind == StmtKind::SEHTry || S.Kind == StmtKind::CxxTry ||
        S.Kind == StmtKind::ItaniumTry)
      break;
    if (S.Kind != StmtKind::Store || !S.StoreAddr || !S.StoreVal)
      continue;
    const auto Offset =
        high_detail::frameAddressOffset(S.StoreAddr, Func, TargetArch, Budget);
    if (!Offset)
      continue;
    if (!Ambiguous.insert(*Offset).second) {
      Homed.erase(*Offset);
      continue;
    }
    const ExprPtr &V = S.StoreVal;
    if (V->Kind == ExprKind::Var && V->Var.Kind == MedVar::Param &&
        S.StoreAddr->Type &&
        S.StoreAddr->Type->Size == getTargetRegInfo(TargetArch).PointerSize)
      Homed.emplace(*Offset, V->Var);
  }
  // A home nothing in the function reads leaves no store behind in HighIR; the
  // prologue's own says what it holds.
  for (const auto &[Offset, Position] : Func.SEHHomedArguments) {
    if (Homed.count(Offset) || Ambiguous.count(Offset) ||
        Position >= Func.Params.size())
      continue;
    MedVar Param;
    Param.Kind = MedVar::Param;
    Param.TheArch = TargetArch;
    Param.Id = static_cast<int>(Position);
    Param.SSAVer = 0;
    Param.RenameTag = -1;
    Param.RegOff = Func.Params[Position].RegOff;
    Param.Size = getTargetRegInfo(TargetArch).PointerSize;
    Homed.emplace(Offset, Param);
  }
  // The home keeps the incoming value; a parameter the function assigns or
  // merges under another name no longer spells it.
  auto Reassigned = [&](const MedVar &V) {
    return V.Kind == MedVar::Param && (V.SSAVer != 0 || V.RenameTag >= 0);
  };
  std::set<int> Moved;
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (E->Kind == ExprKind::Var && Reassigned(E->Var))
      Moved.insert(E->Var.Id);
    for (const MedVar &Out : E->IntrinsicOutputs)
      if (Out.Kind == MedVar::Param)
        Moved.insert(Out.Id);
    E->forEachChildExpr(Visit);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Dst && S.Dst->Kind == ExprKind::Var &&
        S.Dst->Var.Kind == MedVar::Param)
      Moved.insert(S.Dst->Var.Id);
    forEachExpr(S, Visit);
  });
  for (auto It = Homed.begin(); It != Homed.end();)
    if (Moved.count(It->second.Id))
      It = Homed.erase(It);
    else
      ++It;
  return Homed;
}

/// The funclet body projected into the parent frame: the establisher-frame
/// parameter rebased onto the parent's entry stack pointer, each home-slot load
/// turned back into the parameter it holds, and the first parameter replaced by
/// \p First (a termination handler's abnormal flag: a call to
/// `AbnormalTermination()`, or a folded constant for the dedup check; a
/// filter's exception pointers), with the funclet's names moved past
/// \p Names.  Nullopt when they cannot move.
std::optional<std::vector<HighStmt>>
projectFuncletBody(const std::vector<HighStmt> &FuncletBody,
                   const HighFunc &Parent, Arch TargetArch, uint64_t FrameBytes,
                   const ExprPtr &First, const std::map<int64_t, MedVar> &Homed,
                   const MedVar &EntrySP, FuncletNames &Names) {
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  std::vector<HighStmt> Body = cloneStmts(FuncletBody);
  // Rename first: the parent values substituted below keep their names.
  if (!renameFuncletLocals(Body, Names))
    return std::nullopt;

  // arg0 -> abnormal flag or exception pointers, arg1 -> entry SP - FrameBytes.
  mapExprs(Body, [&](ExprPtr E) -> ExprPtr {
    if (!E || E->Kind != ExprKind::Var)
      return E;
    if (E->Var.Kind == MedVar::Param && E->Var.Id == 0 && First) {
      ExprPtr Value = cloneExpr(First);
      if (E->Type)
        Value->Type = E->Type;
      return Value;
    }
    if (E->Var.Kind == MedVar::Param && E->Var.Id == 1) {
      ExprPtr Base = HighExpr::makeVar(EntrySP, NdType::makeInt(8, false));
      ExprPtr Rebased = HighExpr::makeBinop(
          NdOp::INT_SUB, Base,
          HighExpr::makeConst(FrameBytes, 8,
                              ConstantAddressProvenance::Scalar));
      Rebased->Type = E->Type ? E->Type : NdType::makeInt(8, false);
      return Rebased;
    }
    return E;
  });

  // A pointer-width load of a homed slot is the homed parameter.
  if (!Homed.empty()) {
    size_t Budget = limits::kMaxCxxObjectAddressWork;
    mapExprs(Body, [&](ExprPtr E) -> ExprPtr {
      if (!E || E->Kind != ExprKind::Load || E->Operands.size() != 1 ||
          !E->Type || E->Type->Size != TRI.PointerSize ||
          E->MemoryOrdering != NdMemoryOrdering::None ||
          E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return E;
      const auto Offset = high_detail::frameAddressOffset(
          E->Operands[0], Parent, TargetArch, Budget);
      if (!Offset)
        return E;
      auto It = Homed.find(*Offset);
      if (It == Homed.end())
        return E;
      return HighExpr::makeVar(It->second, E->Type);
    });
  }

  simplifyProjectedBody(Body);
  return Body;
}

/// Whether any expression of \p Stmts reads the funclet's EstablisherFrame.
bool readsEstablisherFrame(const std::vector<HighStmt> &Stmts) {
  bool Found = false;
  walkStmts(Stmts, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
        if (!E || Found)
          return;
        if (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Param &&
            E->Var.Id == 1)
          Found = true;
        E->forEachChildExpr(Visit);
      };
      Visit(Root);
    });
  });
  return Found;
}

/// Whether \p Stmts name \p Parent's entry stack pointer.
bool namesFrame(const std::vector<HighStmt> &Stmts, const HighFunc &Parent) {
  bool Used = false;
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E || Used)
      return;
    if (E->Kind == ExprKind::Var &&
        isSyntheticEntryStackPointer(E->Var, Parent, Arch::X64))
      Used = true;
    E->forEachChildExpr(Visit);
  };
  walkStmts(Stmts, [&](const HighStmt &S) { forEachExpr(S, Visit); });
  return Used;
}

/// A finally callback takes only (flag, frame) and a filter only (exception
/// pointers, frame), each in its register, and neither reads a register of the
/// parent: the dispatcher calls them with its own.  The frame
/// rebases onto the parent's entry stack pointer by the bytes its prologue
/// allocates, and a home slot it reads is the argument the parent saved there.
class X64Projection final : public HandlerProjection {
public:
  explicit X64Projection(HighFunc &Parent) : Parent(Parent) {}

  void prepare(FuncletNames &Names) override {
    Homed = homedParameters(Parent, Arch::X64);
    EntrySP = entryStackPointer(Parent, Arch::X64, Names);
  }

  std::optional<std::vector<HighStmt>>
  project(const HighFunc &Handler, const std::set<va_t> &UnwindTargets,
          const ExprPtr &Flag, FuncletNames &Names) override {
    return projectBody(Handler, &UnwindTargets, Flag, Names);
  }

  std::optional<std::vector<HighStmt>>
  projectFilter(const HighFunc &Filter, const ExprPtr &Pointers,
                FuncletNames &Names) override {
    return projectBody(Filter, nullptr, Pointers, Names);
  }

private:
  HighFunc &Parent;
  std::map<int64_t, MedVar> Homed;
  MedVar EntrySP;

  /// The callback's body spelled in the parent, its first argument replaced by
  /// \p First.  A termination handler's (\p UnwindTargets given) makes its
  /// local unwinds jumps and its returns the end of the clause; a filter keeps
  /// its returns, which say what it decides.  The dispatcher passes both the
  /// same EstablisherFrame.
  std::optional<std::vector<HighStmt>>
  projectBody(const HighFunc &Handler, const std::set<va_t> *UnwindTargets,
              const ExprPtr &First, FuncletNames &Names) {
    const llvm::ArrayRef<uint64_t> Arguments =
        getTargetRegInfo(Arch::X64).integerParamRegs(BinaryFormat::COFF);
    for (size_t I = 0; I < Handler.Params.size(); ++I)
      if (I >= Arguments.size() || Handler.Params[I].RegOff != Arguments[I])
        return std::nullopt;
    std::vector<HighStmt> Body = cloneStmts(Handler.Body);
    if (UnwindTargets) {
      jumpLocalUnwinds(Body, *UnwindTargets, Arch::X64);
      if (!settleHandlerReturns(Body))
        return std::nullopt;
      dropUnreadValues(Body);
    }
    if (!localizeHandlerFrame(Body, Arch::X64))
      return std::nullopt;
    if (!funcletVariablesMovable(Body, Arch::X64, /*MaxParam=*/1,
                                 [](const MedVar &) { return false; }))
      return std::nullopt;
    uint64_t FrameBytes = 0;
    if (readsEstablisherFrame(Body)) {
      const auto Frame = Parent.SEHTerminationFrameBytes.find(Handler.Entry);
      if (Frame == Parent.SEHTerminationFrameBytes.end())
        return std::nullopt;
      FrameBytes = Frame->second;
    }
    // A parent without frame slots of its own may keep what the handler reads
    // in its argument homes, where the projection finds the arguments.
    const int64_t OwnFrameSize = Parent.FrameSize;
    if (FrameBytes && OwnFrameSize <= 0)
      Parent.FrameSize = static_cast<int64_t>(FrameBytes);
    std::optional<std::vector<HighStmt>> Projected = projectFuncletBody(
        Body, Parent, Arch::X64, FrameBytes, First, Homed, EntrySP, Names);
    // Other slots of a frame the parent holds nothing in: whatever its
    // prologue may have saved there left no statement that says what they
    // hold.
    const bool Unknowable =
        Projected && OwnFrameSize <= 0 && namesFrame(*Projected, Parent);
    Parent.FrameSize = OwnFrameSize;
    if (!Projected)
      return std::nullopt;
    if (Unknowable)
      return std::nullopt;
    return Projected;
  }
};

} // namespace

std::unique_ptr<HandlerProjection> makeX64Projection(HighFunc &Parent) {
  return std::make_unique<X64Projection>(Parent);
}

} // namespace seh_finally
} // namespace neverd
