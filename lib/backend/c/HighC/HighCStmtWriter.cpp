//===- HighCStmtWriter.cpp - HighIR statement rendering -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Statement rendering for the HighIR C emitter: converts HighStmt
/// structures into indented C source blocks.  Function-level orchestration
/// lives in HighCFuncWriter.cpp.
///
//===----------------------------------------------------------------------===//

#include "HighCWriter.h"

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/ir/high/HighSwiftErrorProjection.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>

namespace neverd {

namespace {

// MSVC `HandlerType` adjectives from CRT `ehdata.h`.
constexpr uint32_t kCxxCatchConst = 0x1u;
constexpr uint32_t kCxxCatchVolatile = 0x2u;
constexpr uint32_t kCxxCatchReference = 0x8u;

bool isCIdentifier(llvm::StringRef Name) {
  if (Name.empty() ||
      (!std::isalpha(static_cast<unsigned char>(Name.front())) &&
       Name.front() != '_'))
    return false;
  return llvm::all_of(Name, [](char Ch) {
    return std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_';
  });
}

bool isCxxTypeSpelling(llvm::StringRef Name) {
  while (!Name.empty()) {
    const size_t Sep = Name.find("::");
    const llvm::StringRef Part =
        Sep == llvm::StringRef::npos ? Name : Name.take_front(Sep);
    if (!isCIdentifier(Part))
      return false;
    if (Sep == llvm::StringRef::npos)
      return true;
    Name = Name.drop_front(Sep + 2);
  }
  return false;
}

bool isCxxThrowExpr(const HighExpr *E) {
  return E && E->Kind == ExprKind::Call &&
         isMsvcCxxThrowCallName(E->CallTarget);
}

bool isCxxRethrowObject(const HighExpr *Obj) {
  const HighExpr *Cur = Obj;
  for (int Depth = 0; Cur && Depth < 8; ++Depth) {
    if (Cur->Kind == ExprKind::Const)
      return Cur->ConstVal == 0;
    if ((Cur->Kind == ExprKind::Cast || Cur->Kind == ExprKind::UnaryOp) &&
        !Cur->Operands.empty()) {
      Cur = Cur->Operands[0].get();
      continue;
    }
    break;
  }
  return !Obj;
}

bool isFastFailExpr(const HighExpr *E) { return E && isX86FastFailCall(*E); }

bool isDebugTrapStmt(const HighStmt &Stmt) {
  const HighExpr *E = nullptr;
  if (Stmt.Kind == StmtKind::Call)
    E = Stmt.CallExpr.get();
  else if ((Stmt.Kind == StmtKind::Assign || Stmt.Kind == StmtKind::ExprStmt) &&
           Stmt.Val)
    E = Stmt.Val.get();
  return E && E->Kind == ExprKind::Call &&
         (E->IntrinsicId == Intrinsic::Int3 ||
          E->IntrinsicId == Intrinsic::Ud2);
}

bool isCxxThrowStmt(const HighStmt &Stmt) {
  if (Stmt.Kind == StmtKind::Call)
    return isCxxThrowExpr(Stmt.CallExpr.get());
  if ((Stmt.Kind == StmtKind::Assign || Stmt.Kind == StmtKind::ExprStmt) &&
      Stmt.Val)
    return isCxxThrowExpr(Stmt.Val.get());
  return false;
}

const HighExpr *stmtCallExpr(const HighStmt &Stmt) {
  if (Stmt.Kind == StmtKind::Call)
    return Stmt.CallExpr.get();
  if ((Stmt.Kind == StmtKind::Assign || Stmt.Kind == StmtKind::ExprStmt) &&
      Stmt.Val)
    return Stmt.Val.get();
  return nullptr;
}

bool isNoReturnCallStmt(const HighStmt &Stmt) {
  if (isCxxThrowStmt(Stmt))
    return true;
  const HighExpr *E = stmtCallExpr(Stmt);
  if (!E)
    return false;
  return isNoreturnCallExpr(*E);
}

using TryExitTest = llvm::function_ref<bool(const HighStmt &)>;

bool hasFallthroughExceptClause(const HighStmt &Stmt) {
  return Stmt.Kind == StmtKind::SEHTry && Stmt.EHIsReducible &&
         Stmt.EHClauses.size() == 1 && Stmt.EHClauseBodies.size() == 1 &&
         Stmt.EHClauses.front().Kind == HighEHClauseKind::SEHExcept;
}

/// End of the statements a try body prints.  A trailing goto that only falls
/// through to the code after the try is left out (\p LeftOut).  Goto-target
/// collection, the try writer and the exit analysis all use this, so a goto
/// that is not printed never counts as an exit and never registers a label.
size_t printedTryBodyEnd(TryExitTest Hidden, TryExitTest LeftOut,
                         const std::vector<HighStmt> &Body) {
  size_t End = Body.size();
  while (End > 0) {
    const HighStmt &Last = Body[End - 1];
    if (Hidden(Last) || LeftOut(Last)) {
      --End;
      continue;
    }
    break;
  }
  return End;
}

bool stmtsAlwaysExit(TryExitTest Hidden, TryExitTest LeftOut,
                     const std::vector<HighStmt> &Stmts, size_t End);

bool stmtAlwaysExit(TryExitTest Hidden, TryExitTest LeftOut,
                    const HighStmt &Stmt) {
  if (Hidden(Stmt))
    return false;
  switch (Stmt.Kind) {
  case StmtKind::Return:
  case StmtKind::Goto:
    return true;
  case StmtKind::IfElse:
    return stmtsAlwaysExit(Hidden, LeftOut, Stmt.Body, Stmt.Body.size()) &&
           stmtsAlwaysExit(Hidden, LeftOut, Stmt.ElseBody,
                           Stmt.ElseBody.size());
  case StmtKind::SEHTry:
  case StmtKind::CxxTry:
  case StmtKind::ItaniumTry:
    // Without its trailing goto, the printed body falls through to the next
    // statement.
    if (!stmtsAlwaysExit(Hidden, LeftOut, Stmt.Body,
                         printedTryBodyEnd(Hidden, LeftOut, Stmt.Body)))
      return false;
    if (Stmt.EHClauseBodies.empty())
      return true;
    for (const auto &ClauseBody : Stmt.EHClauseBodies) {
      if (!stmtsAlwaysExit(Hidden, LeftOut, ClauseBody,
                           hasFallthroughExceptClause(Stmt)
                               ? printedTryBodyEnd(Hidden, LeftOut, ClauseBody)
                               : ClauseBody.size()))
        return false;
    }
    return true;
  default:
    return isNoReturnCallStmt(Stmt);
  }
}

/// The call \p E returns through casts and integer views, which a function
/// printed `void` makes as a statement.
const HighExpr *returnedCall(const HighExpr &E) {
  const HighExpr *Cur = &E;
  for (unsigned Depth = 0; Cur && Depth <= limits::kMaxIntegerViewUnwrapDepth;
       ++Depth) {
    if (Cur->Kind == ExprKind::Call)
      return Cur;
    const bool View =
        Cur->Kind == ExprKind::Cast || Cur->Kind == ExprKind::BitCast ||
        (Cur->Kind == ExprKind::UnaryOp &&
         (Cur->Op == NdOp::INT_ZEXT || Cur->Op == NdOp::INT_SEXT)) ||
        (Cur->Kind == ExprKind::BinOp && Cur->Op == NdOp::SUBBYTES);
    if (!View || Cur->Operands.empty())
      return nullptr;
    Cur = Cur->Operands[0].get();
  }
  return nullptr;
}

bool stmtsAlwaysExit(TryExitTest Hidden, TryExitTest LeftOut,
                     const std::vector<HighStmt> &Stmts, size_t End) {
  for (size_t I = End; I > 0; --I) {
    const HighStmt &Stmt = Stmts[I - 1];
    if (Hidden(Stmt))
      continue;
    return stmtAlwaysExit(Hidden, LeftOut, Stmt);
  }
  return false;
}

void writeCxxCatchType(llvm::raw_ostream &OS, const HighEHClause &Clause) {
  if (Clause.TypeName.empty() && Clause.TypeDescriptorVA == 0) {
    OS << "...";
    return;
  }
  if ((Clause.Adjectives & kCxxCatchConst) != 0)
    OS << "const ";
  if ((Clause.Adjectives & kCxxCatchVolatile) != 0)
    OS << "volatile ";
  if (!Clause.TypeName.empty() && isCxxTypeSpelling(Clause.TypeName)) {
    OS << Clause.TypeName;
  } else if (!Clause.TypeName.empty()) {
    OS << "/* " << Clause.TypeName;
    if (Clause.TypeDescriptorVA)
      OS << "; type @ 0x" << llvm::utohexstr(Clause.TypeDescriptorVA);
    OS << " */";
  } else {
    OS << "/* type @ 0x" << llvm::utohexstr(Clause.TypeDescriptorVA) << " */";
  }
  if ((Clause.Adjectives & kCxxCatchReference) != 0)
    OS << " &";
}

} // namespace

void HighCWriter::emitIndent(int Indent) { emitCIndent(OS, Indent); }

// GetExceptionCode() is only valid in the __except arm, so the arm stores it
// in the name its handler's code reads.
void HighCWriter::writeSEHExceptionCodeCapture(va_t HandlerVA, int Indent) {
  auto It = SEHExceptionCodeNames.find(HandlerVA);
  if (It == SEHExceptionCodeNames.end())
    return;
  emitIndent(Indent);
  OS << It->second << " = GetExceptionCode();\n";
}

void HighCWriter::emitRenderedStatement(int Indent, llvm::StringRef Text) {
  while (!Text.empty()) {
    auto [Line, Rest] = Text.split('\n');
    if (!Line.empty())
      emitIndent(Indent);
    OS << Line << '\n';
    Text = Rest;
  }
}

// A filter's disposition prints under the name <excpt.h> gives it.
std::string HighCWriter::sehFilterValueText(const HighExpr &Value) {
  const HighExpr *Inner = unwrapIntegerView(&Value);
  if (Inner && Inner->Kind == ExprKind::Const) {
    const uint64_t Width = Inner->Type && Inner->Type->Size < 8
                               ? (uint64_t{1} << (Inner->Type->Size * 8)) - 1
                               : ~uint64_t{0};
    switch (Inner->ConstVal & Width) {
    case 1:
      return "EXCEPTION_EXECUTE_HANDLER";
    case 0:
      return "EXCEPTION_CONTINUE_SEARCH";
    default:
      if ((Inner->ConstVal & Width) == Width)
        return "EXCEPTION_CONTINUE_EXECUTION";
      break;
    }
  }
  return exprStr(Value);
}

void HighCWriter::writeCxxThrowExpr(const HighStmt &Stmt,
                                    const HighExpr &ThrowCall) {
  if (!Opts.StructuredExceptionSyntax) {
    OS << exprStr(ThrowCall);
    return;
  }
  OS << "throw";
  if (auto Printed = CxxThrowPrints.find(&Stmt);
      Printed != CxxThrowPrints.end()) {
    OS << " " << Printed->second.Type << "(";
    for (size_t I = 0; I < Printed->second.Args.size(); ++I) {
      if (I)
        OS << ", ";
      OS << exprStr(*Printed->second.Args[I]);
    }
    OS << ")";
    return;
  }
  if (!ThrowCall.Operands.empty() && ThrowCall.Operands[0] &&
      !isCxxRethrowObject(ThrowCall.Operands[0].get()))
    OS << " " << exprStr(*ThrowCall.Operands[0]);
}

void HighCWriter::writeCExceptionRegion(const HighStmt &Stmt, int Indent) {
  // The C projection shows native entry points, not an invented setjmp-based
  // replacement for the platform unwinder. A funclet keeps its own ABI,
  // return and frame access in a separate definition in this emission.
  for (size_t I = 0; I < Stmt.EHClauses.size(); ++I) {
    const HighEHClause &Clause = Stmt.EHClauses[I];
    const va_t Entry = Clause.Kind == HighEHClauseKind::CxxCleanup
                           ? Clause.FilterOrActionVA
                           : Clause.HandlerVA;
    if (I < Stmt.EHClauseBodies.size() && !Stmt.EHClauseBodies[I].empty() &&
        !isEmbeddedRegistrationCallback(Stmt, I) &&
        (!Entry || (CurrentFunc && Entry == CurrentFunc->Entry) ||
         !DefinedFunctionsByAddress.count(Entry)))
      throw std::invalid_argument(
          "plain C exception view requires the native handler function; "
          "an embedded handler cannot be assigned a new C ABI");
  }
  const std::vector<va_t> OuterLeave = std::move(LeaveTargets);
  LeaveTargets.clear();
  llvm::scope_exit Restore([&] { LeaveTargets = OuterLeave; });
  emitIndent(Indent);
  OS << "/* C++ unwind-table region [0x" << llvm::utohexstr(Stmt.EHRange.Begin)
     << ", 0x" << llvm::utohexstr(Stmt.EHRange.End)
     << "): dispatch is supplied by the image's exception runtime.\n";
  for (const HighEHClause &Clause : Stmt.EHClauses) {
    emitIndent(Indent);
    OS << " * "
       << (Clause.Kind == HighEHClauseKind::CxxCleanup ? "cleanup" : "handler")
       << " @ 0x" << llvm::utohexstr(Clause.HandlerVA) << ", action @ 0x"
       << llvm::utohexstr(Clause.FilterOrActionVA) << ", type descriptor @ 0x"
       << llvm::utohexstr(Clause.TypeDescriptorVA) << ", state=" << Clause.State
       << ", catch-object offset=" << Clause.CatchObjectOffset;
    for (va_t Continuation : Clause.ContinuationVAs)
      OS << ", continuation @ 0x" << llvm::utohexstr(Continuation);
    OS << "\n";
  }
  emitIndent(Indent);
  OS << " */\n";
  emitIndent(Indent);
  OS << "{\n";
  writeTryBody(Stmt.Body, Indent + 1);
  emitIndent(Indent);
  OS << "}\n";
  writeEmbeddedRegistrationCallbacks(Stmt, Indent);
}

void HighCWriter::writeStmt(const HighStmt &Stmt, int Indent) {
  auto Event = SourceRecorder && CurrentFunc
                   ? SourceRecorder->statement(CurrentFunc->Entry, Stmt)
                   : std::nullopt;
  if (Event)
    OS << SourceRecorder->begin(*Event);
  writeStmtImpl(Stmt, Indent);
  if (Event)
    OS << SourceRecorder->end(*Event);
}

void HighCWriter::writeStmtImpl(const HighStmt &Stmt, int Indent) {
  if (Analysis.DeadStmts.count(&Stmt) &&
      !(Stmt.Kind == StmtKind::Assign && Stmt.Dst &&
        (Stmt.Dst->Kind == ExprKind::Var || Stmt.Dst->Kind == ExprKind::Phi) &&
        AmbiguousFrameAliases.count(varName(Stmt.Dst->Var))))
    return;
  const bool HideEHRuntimeMemory = CurrentFunc &&
                                   CurrentFunc->ExceptionMetadata.has_value() &&
                                   !preservesRegistrationMemory(*CurrentFunc);
  auto IsEHRuntimeSpace = [&](NdMemoryAddressSpace Space) {
    return HideEHRuntimeMemory && Space == NdMemoryAddressSpace::X86FS;
  };
  switch (Stmt.Kind) {
  case StmtKind::Assign: {
    if (!Stmt.Dst || !Stmt.Val)
      return;
    if ((Stmt.Dst->Kind == ExprKind::Var || Stmt.Dst->Kind == ExprKind::Phi)) {
      const std::string DestRaw = varName(Stmt.Dst->Var);
      const std::string Printed = printedForwardedVar(DestRaw, 0);
      // FieldForward dests print as `t1->m_pNext` / `bins[i]`.  Hiding any
      // Printed!=raw name also dropped `recordName = Get*(...)` after the temp
      // was renamed (`t2` → `recordName`).
      if (Printed.find("->") != std::string::npos ||
          Printed.find('[') != std::string::npos)
        return;
      if (Printed.find('.') != std::string::npos &&
          Printed == exprStr(*Stmt.Val))
        return;
      if (auto Field = scalarRecordFieldDest(Printed, *Stmt.Val);
          Field && *Field == exprStr(*Stmt.Val))
        return;
    }
    if (Stmt.Dst->Kind == ExprKind::Load && !Stmt.Dst->Operands.empty() &&
        Stmt.Dst->Operands[0] &&
        Stmt.Dst->MemoryOrdering == NdMemoryOrdering::None) {
      const uint16_t AccessSize =
          Stmt.Val && Stmt.Val->Type ? Stmt.Val->Type->Size : 0;
      if (auto DestM = typedMemberAccess(*Stmt.Dst->Operands[0], AccessSize)) {
        const std::string ValS = exprStr(*Stmt.Val);
        if (ValS == *DestM || ValS.find(*DestM) != std::string::npos)
          return;
      }
    }
    if (isCxxThrowExpr(Stmt.Val.get())) {
      emitIndent(Indent);
      writeCxxThrowExpr(Stmt, *Stmt.Val);
      OS << ";\n";
      break;
    }
    if (Stmt.Val->Kind == ExprKind::Load &&
        IsEHRuntimeSpace(Stmt.Val->MemoryAddressSpace))
      return;
    if (Stmt.Val->Kind == ExprKind::Call) {
      auto Rendered = renderX86SegmentedIntrinsicStatement(
          Opts.TheArch, *Stmt.Val, Stmt.Dst.get(),
          [this](const HighExpr &E) { return intrinsicOperandStr(E); },
          [this](const MedVar &V) { return varName(V); },
          [this, &Stmt](const MedVar &V) {
            const bool OmittedPrimary =
                Analysis.OmittedCallResults.count(&Stmt) && Stmt.Dst &&
                (Stmt.Dst->Kind == ExprKind::Var ||
                 Stmt.Dst->Kind == ExprKind::Phi) &&
                Stmt.Dst->Var == V;
            return !OmittedPrimary && !Analysis.DeadVars.count(varName(V));
          },
          [this](const HighExpr &E, uint16_t Width) {
            return isSameWidthUnsigned(E, Width);
          },
          Opts.Format == BinaryFormat::COFF);
      if (!Rendered.empty()) {
        writeCIndentedSnippet(OS, Rendered, Indent);
        break;
      }
    }
    if (Stmt.Val->Kind == ExprKind::Call &&
        !Stmt.Val->IntrinsicOutputs.empty()) {
      auto Rendered = MultiOutputRender{}(
          Opts.TheArch, Stmt.Val->IntrinsicId, Stmt.Val->IntrinsicOutputs,
          Stmt.Val->Operands, [this](const HighExpr &E) { return exprStr(E); },
          [this](const MedVar &V) { return varName(V); },
          [this](const MedVar &V) {
            return !Analysis.DeadVars.count(varName(V));
          });
      if (!Rendered.empty()) {
        writeCIndentedSnippet(OS, Rendered, Indent);
        HasCIntrinsics = true;
        break;
      }
    }
    if (Stmt.Val->Kind == ExprKind::Call &&
        (Stmt.Val->IntrinsicId == Intrinsic::Svc ||
         Stmt.Val->IntrinsicId == Intrinsic::ArmSvc)) {
      // SVC is rendered as a side-effecting asm statement. The lifted X0/R0
      // result cannot be assigned from a void asm expression. A use of that
      // result needs an explicit output model, rather than an invented value.
      if (!CurrentFunc ||
          (Stmt.Dst->Kind != ExprKind::Var && Stmt.Dst->Kind != ExprKind::Phi))
        llvm::report_fatal_error("HighC cannot render an SVC result");
      bool ResultUsed = false;
      const MedVar Result = Stmt.Dst->Var;
      std::function<void(const HighExpr &)> FindUse = [&](const HighExpr &E) {
        if ((E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) &&
            E.Var == Result)
          ResultUsed = true;
        E.forEachChildExpr([&](const ExprPtr &Child) {
          if (Child)
            FindUse(*Child);
        });
      };
      walkStmts(CurrentFunc->Body, [&](const HighStmt &Use) {
        if (&Use == &Stmt || Analysis.DeadStmts.count(&Use) ||
            (InferredVoid && Use.Kind == StmtKind::Return))
          return;
        forEachRhsExpr(Use, [&](const ExprPtr &E) {
          if (E)
            FindUse(*E);
        });
      });
      if (ResultUsed)
        llvm::report_fatal_error("HighC cannot render a live SVC result");
      emitIndent(Indent);
      OS << statementCallText(*Stmt.Val) << ";\n";
      break;
    }
    // A callee declared to return nothing leaves no value in the result
    // register. Keep the call; a destination that is still read gets the
    // explicit unknown value rather than a void expression.
    if (knownVoidCall(*Stmt.Val) &&
        (Stmt.Dst->Kind == ExprKind::Var || Stmt.Dst->Kind == ExprKind::Phi)) {
      emitIndent(Indent);
      OS << statementCallText(*Stmt.Val) << ";\n";
      const std::string Dest = varName(Stmt.Dst->Var);
      const std::string Printed = printedForwardedVar(Dest, 0);
      if (!Analysis.OmittedCallResults.count(&Stmt) &&
          (isEmittedParamName(Dest) || DeclaredCNames.count(Dest) ||
           DeclaredCNames.count(Printed))) {
        emitIndent(Indent);
        OS << Printed << " = 0 /* unknown */;\n";
      }
      break;
    }
    // A call with custom statement rendering can have live auxiliary outputs
    // even when its primary result is unused. Render those effects first.
    // A self call of a function printed `void` has no value to assign.
    if (isNoreturnCallExpr(*Stmt.Val) || isVoidSelfCall(*Stmt.Val) ||
        Analysis.OmittedCallResults.count(&Stmt) ||
        (Stmt.Val->Kind == ExprKind::Call && Stmt.Dst &&
         (Stmt.Dst->Kind == ExprKind::Var || Stmt.Dst->Kind == ExprKind::Phi) &&
         !isEmittedParamName(varName(Stmt.Dst->Var)) &&
         !DeclaredCNames.count(varName(Stmt.Dst->Var)) &&
         !DeclaredCNames.count(
             printedForwardedVar(varName(Stmt.Dst->Var), 0)))) {
      emitIndent(Indent);
      OS << statementCallText(*Stmt.Val) << ";\n";
      break;
    }
    bool DeadIntrinsicResult = Stmt.Dst->Kind == ExprKind::Var &&
                               Analysis.DeadVars.count(varName(Stmt.Dst->Var));
    // A software interrupt (`int 2Dh`, the debug service) has no C spelling:
    // an asm block loads its register inputs and moves the result out.
    if (Stmt.Dst->Kind == ExprKind::Var) {
      const std::string Rendered = renderX86InterruptStatement(
          Opts.TheArch, *Stmt.Val,
          DeadIntrinsicResult ? "" : varName(Stmt.Dst->Var),
          Stmt.Dst->Type ? Stmt.Dst->Type->Size : 8,
          [this](const HighExpr &E) { return exprStr(E); });
      if (!Rendered.empty()) {
        emitRenderedStatement(Indent, Rendered);
        break;
      }
    }
    if (Stmt.Val->Kind == ExprKind::Call &&
        Stmt.Val->IntrinsicId != Intrinsic::None && DeadIntrinsicResult &&
        (isSideeffectIntrinsic(Stmt.Val->IntrinsicId) ||
         !intrinsicCName(Stmt.Val->IntrinsicId))) {
      emitIndent(Indent);
      OS << statementCallText(*Stmt.Val) << ";\n";
      break;
    }
    if (Stmt.Dst->Kind == ExprKind::Load && !Stmt.Dst->Operands.empty()) {
      if (IsEHRuntimeSpace(Stmt.Dst->MemoryAddressSpace))
        return;
      if (Stmt.Dst->MemoryOrdering == NdMemoryOrdering::None &&
          Stmt.Dst->MemoryAddressSpace == NdMemoryAddressSpace::Default)
        if (auto Slot =
                namedFrameSlot(*Stmt.Dst->Operands[0], Stmt.Dst->Type)) {
          if (isCompilerEHConstant(*Stmt.Val))
            break;
          if (isParamCopy(*Stmt.Val)) {
            if (auto Src = copyForwardSource(*Stmt.Val)) {
              auto It = CopyForward.find(*Slot);
              if (It != CopyForward.end() && It->second == *Src) {
                Analysis.DeadVars.insert(*Slot);
                break;
              }
            }
          }
          CopyForward.erase(*Slot);
          Analysis.DeadVars.erase(*Slot);
          uint64_t AndMask = 0;
          if (const HighExpr *Base = asAndWithConst(*Stmt.Val, AndMask)) {
            const HighExpr *P = peelIntegerViewOps(Base);
            if (!P)
              P = Base;
            bool Same = false;
            if (P->Kind == ExprKind::Load && !P->Operands.empty() &&
                P->Operands[0])
              if (auto Src = namedFrameSlot(*P->Operands[0]))
                Same = *Src == *Slot;
            if (!Same && (P->Kind == ExprKind::Var || P->Kind == ExprKind::Phi))
              Same = printedForwardedVar(varName(P->Var), 0) == *Slot;
            if (Same) {
              emitIndent(Indent);
              OS << formatInplaceAnd(*Slot, AndMask,
                                     Stmt.Val->Type ? Stmt.Val->Type->Size : 4)
                 << ";\n";
              break;
            }
          }
          const std::string ValueText = exprStr(*Stmt.Val);
          emitIndent(Indent);
          if (auto Field = scalarRecordFieldDest(*Slot, *Stmt.Val, ValueText))
            OS << *Field << " = ";
          else
            OS << *Slot << " = ";
          TypeRef ProjectedDestType = Stmt.Dst->Type;
          if (auto Disp = frameDisplacement(*Stmt.Dst->Operands[0])) {
            auto ProjectedSlot = FrameSlots.find(*Disp);
            if (ProjectedSlot != FrameSlots.end() &&
                ProjectedSlot->second.Type &&
                !(ProjectedDestType &&
                  ProjectedDestType->Size < ProjectedSlot->second.Type->Size))
              ProjectedDestType = ProjectedSlot->second.Type;
          }
          if (ProjectedDestType && ProjectedDestType->Kind == NdTypeKind::Ptr)
            OS << "(" << typeToC(ProjectedDestType) << ")(uintptr_t)("
               << ValueText << ")";
          else
            OS << ValueText;
          OS << ";\n";
          break;
        }
      if (Stmt.Dst->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        if (auto VA = constAddress(*Stmt.Dst->Operands[0])) {
          if (auto Name = imageObjectName(*VA)) {
            emitIndent(Indent);
            OS << *Name << " = " << exprStr(*Stmt.Val) << ";\n";
            break;
          }
        }
      }
      std::string Value =
          storedValueText(*Stmt.Val, Stmt.Dst->Type, Stmt.Dst->MemoryOrdering);
      if (Stmt.Dst->Type && Stmt.Val->Type &&
          Stmt.Dst->Type->Kind == NdTypeKind::Int &&
          Stmt.Val->Type->Kind == NdTypeKind::Ptr)
        Value = "(" + typeToC(Stmt.Dst->Type) + ")(uintptr_t)(" + Value + ")";
      bool ExactImageBytes = false;
      if (Stmt.Dst->MemoryAddressSpace == NdMemoryAddressSpace::Default)
        if (auto VA = constAddress(*Stmt.Dst->Operands[0]))
          ExactImageBytes = imageBackingAddress(*VA).has_value();
      writeMemoryStore(Stmt.Dst->Type,
                       addrStr(*Stmt.Dst->Operands[0], 0,
                               Stmt.Dst->MemoryAddressSpace ==
                                   NdMemoryAddressSpace::Default),
                       Value, Stmt.Dst->MemoryOrdering,
                       Stmt.Dst->MemoryAddressSpace, ExactImageBytes, Indent);
      break;
    }
    if (Stmt.Dst->Kind == ExprKind::Var &&
        isCopyForwardDestination(Stmt.Dst->Var)) {
      const std::string DstName = varName(Stmt.Dst->Var);
      if (auto Src = copyForwardSource(*Stmt.Val)) {
        auto It = CopyForward.find(DstName);
        if (It != CopyForward.end() && It->second == *Src) {
          Analysis.DeadVars.insert(DstName);
          break;
        }
      } else {
        CopyForward.erase(DstName);
        Analysis.DeadVars.erase(DstName);
      }
    }
    emitIndent(Indent);
    if (Stmt.Dst->Kind == ExprKind::Var || Stmt.Dst->Kind == ExprKind::Phi) {
      // Variable destinations are lvalues, unlike exprStr's machine-value
      // projection of typed pointer parameters.
      const std::string DestName =
          printedForwardedVar(varName(Stmt.Dst->Var), 0);
      if (DestName.find("->") != std::string::npos ||
          DestName.find('[') != std::string::npos)
        return;
      uint64_t AndMask = 0;
      if (const HighExpr *Base = asAndWithConst(*Stmt.Val, AndMask)) {
        const HighExpr *P = peelIntegerViewOps(Base);
        if (!P)
          P = Base;
        bool Same = (P->Kind == ExprKind::Var || P->Kind == ExprKind::Phi) &&
                    printedForwardedVar(varName(P->Var), 0) == DestName;
        if (!Same && P->Kind == ExprKind::Load && !P->Operands.empty() &&
            P->Operands[0])
          if (auto Slot = namedFrameSlot(*P->Operands[0]))
            Same = *Slot == DestName;
        if (Same) {
          OS << formatInplaceAnd(DestName, AndMask,
                                 Stmt.Val->Type ? Stmt.Val->Type->Size : 4)
             << ";\n";
          break;
        }
      }
      auto DeclaredType = declaredParamType(Stmt.Dst->Var);
      if (!DeclaredType)
        DeclaredType = Stmt.Dst->Type;
      bool PointerTypeFromCall = false;
      if (auto It = PointerArgDestTypes.find(varName(Stmt.Dst->Var));
          It != PointerArgDestTypes.end() && It->second &&
          (!DeclaredType || DeclaredType->Kind == NdTypeKind::Int ||
           DeclaredType->Kind == NdTypeKind::Unknown ||
           (DeclaredType->Kind == NdTypeKind::Ptr &&
            (!DeclaredType->Pointee ||
             DeclaredType->Pointee->Kind != NdTypeKind::Struct ||
             DeclaredType->Pointee->SourceName.empty())))) {
        // emitLocalDecls projects this machine-width carrier as the call's
        // pointer argument. Render its assignments with the same C type.
        DeclaredType = It->second;
        PointerTypeFromCall = true;
      }
      TypeRef ProjectedValueType = Stmt.Val->Type;
      if (Stmt.Val->Kind == ExprKind::Load && !Stmt.Val->Operands.empty() &&
          Stmt.Val->Operands[0]) {
        if (auto Disp = frameDisplacement(*Stmt.Val->Operands[0])) {
          auto Slot = FrameSlots.find(*Disp);
          if (Slot != FrameSlots.end() && Slot->second.Type)
            ProjectedValueType = Slot->second.Type;
        }
      }
      const std::string AddressText =
          PointerTypeFromCall ? addrStr(*Stmt.Val) : std::string();
      const bool DirectAddress =
          !AddressText.empty() && AddressText.front() == '&';
      MemoryLoadDestination LoadDestination;
      const auto DestinationType = DeclaredCTypes.find(DestName);
      if (Stmt.Val->Kind == ExprKind::Load &&
          (Stmt.Dst->Var.Kind == MedVar::Temp ||
           Stmt.Dst->Var.Kind == MedVar::Reg) &&
          DestName == varName(Stmt.Dst->Var) &&
          DestinationType != DeclaredCTypes.end() &&
          !AddressTakenNames.count(DestName) && !isAddressTakenSlot(DestName) &&
          !scalarRecordFieldDest(DestName, *Stmt.Val) && Stmt.Val->Type &&
          typeToC(DestinationType->second) == memoryTypeName(Stmt.Val->Type))
        LoadDestination.Name = DestName;
      const std::string ValueText =
          DirectAddress ? AddressText : exprStr(*Stmt.Val, 0, &LoadDestination);
      if (LoadDestination.Written) {
        OS << ValueText << ";\n";
        break;
      }
      if (auto Field = scalarRecordFieldDest(DestName, *Stmt.Val))
        OS << *Field << " = ";
      else
        OS << DestName << " = ";
      if (DeclaredType && DeclaredType->Kind == NdTypeKind::Ptr) {
        if (DirectAddress)
          OS << "(" << typeToC(DeclaredType) << ")(" << ValueText << ")";
        else
          OS << "(" << typeToC(DeclaredType) << ")(uintptr_t)(" << ValueText
             << ")";
      } else if (DeclaredType && ProjectedValueType &&
                 DeclaredType->Kind == NdTypeKind::Int &&
                 ProjectedValueType->Kind == NdTypeKind::Ptr)
        OS << "(" << typeToC(DeclaredType) << ")(uintptr_t)(" << ValueText
           << ")";
      else if (DeclaredType && DeclaredType->Kind == NdTypeKind::Int &&
               !ValueText.empty() &&
               (ValueText.front() == '&' || isStringLiteralText(ValueText)))
        OS << "(" << typeToC(DeclaredType) << ")(uintptr_t)(" << ValueText
           << ")";
      else if (isUnknownCallOperand(Stmt.Val.get()))
        OS << "0";
      else if (auto Enum = enumConstDisplay(varName(Stmt.Dst->Var), *Stmt.Val))
        OS << *Enum;
      else if (std::optional<std::string> Converted =
                   DestinationType != DeclaredCTypes.end() && !DirectAddress
                       ? implicitIntegerConversion(*Stmt.Val,
                                                   DestinationType->second)
                       : std::nullopt)
        OS << *Converted;
      else
        OS << ValueText;
    } else {
      const std::string DestS = exprStr(*Stmt.Dst);
      const std::string ValS = exprStr(*Stmt.Val);
      if (!DestS.empty() &&
          (ValS == DestS || ValS.find(DestS) != std::string::npos))
        break;
      OS << DestS << " = " << ValS;
    }
    OS << ";\n";
    break;
  }

  case StmtKind::Store: {
    if (CurrentFunc && isSwiftErrorEntrySlot(*CurrentFunc, Stmt.StoreAddr) &&
        Stmt.StoreVal && Stmt.StoreVal->Type &&
        Stmt.StoreVal->Type->Size == 8 &&
        Stmt.MemoryOrdering == NdMemoryOrdering::None &&
        Stmt.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      emitIndent(Indent);
      OS << "*" << varName(Stmt.StoreAddr->Var) << " = (void *)(uintptr_t)("
         << exprStr(*Stmt.StoreVal) << ");\n";
      break;
    }
    if (!Stmt.StoreAddr || !Stmt.StoreVal)
      return;
    {
      const uint16_t AccessSize =
          Stmt.StoreVal->Type ? Stmt.StoreVal->Type->Size : 0;
      if (auto Member = typedMemberAccess(*Stmt.StoreAddr, AccessSize)) {
        // Text containing a destination can still compute a new value (for
        // example a modular increment). Only an exact, ordinary self-load
        // proves this field assignment redundant.
        const HighExpr &Value = *Stmt.StoreVal;
        if (Stmt.MemoryOrdering == NdMemoryOrdering::None &&
            Stmt.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
            Value.Kind == ExprKind::Load &&
            Value.MemoryOrdering == NdMemoryOrdering::None &&
            Value.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
            Value.Operands.size() == 1 && Value.Operands[0] &&
            Stmt.StoreAddr->structuralEq(*Value.Operands[0]))
          break;
      }
    }
    if (IsEHRuntimeSpace(Stmt.MemoryAddressSpace))
      return;
    if (isCompilerEHConstant(*Stmt.StoreVal))
      break;
    {
      int64_t Delta = 0;
      if (isInplaceAddStore(Stmt, Delta)) {
        if (std::string Inplace =
                formatInplaceAdd(Stmt.StoreVal->Type, *Stmt.StoreAddr, Delta);
            !Inplace.empty()) {
          emitIndent(Indent);
          OS << Inplace << ";\n";
          break;
        }
      }
      uint64_t AndMask = 0;
      if (isInplaceAndStore(Stmt, AndMask)) {
        if (auto Slot = namedFrameSlot(*Stmt.StoreAddr)) {
          emitIndent(Indent);
          OS << formatInplaceAnd(*Slot, AndMask,
                                 Stmt.StoreVal->Type ? Stmt.StoreVal->Type->Size
                                                     : 4)
             << ";\n";
          break;
        }
      }
    }
    if (Stmt.MemoryOrdering == NdMemoryOrdering::None &&
        Stmt.MemoryAddressSpace == NdMemoryAddressSpace::Default)
      if (auto Member = typedMemberAccess(*Stmt.StoreAddr,
                                          Stmt.StoreVal && Stmt.StoreVal->Type
                                              ? Stmt.StoreVal->Type->Size
                                              : 0);
          Member && !namedFrameSlot(*Stmt.StoreAddr)) {
        bool Overlayed = false;
        if (auto Overlay =
                callOverlayIntegerMemberStore(*Member, *Stmt.StoreVal)) {
          Member = std::move(Overlay);
          Overlayed = true;
        }
        emitIndent(Indent);
        OS << *Member << " = ";
        if (isUnknownCallOperand(Stmt.StoreVal.get()))
          OS << "0";
        else {
          std::string Value = exprStr(*Stmt.StoreVal);
          const TypeRef StoredType = Stmt.StoreVal->Type;
          const TypeRef MemberType = typedMemberType(
              *Stmt.StoreAddr, StoredType ? StoredType->Size : 0);
          // A sign/zero-extended integer call can reuse an 8-byte pointer
          // field. Make the C conversion explicit without widening the store.
          if (!Overlayed && isIntegerOverlayStore(*Stmt.StoreVal) &&
              MemberType && MemberType->Kind == NdTypeKind::Ptr && StoredType &&
              StoredType->Kind == NdTypeKind::Int &&
              StoredType->Size == MemberType->Size) {
            const bool Signed = StoredType->IsSigned ||
                                (Stmt.StoreVal->Kind == ExprKind::UnaryOp &&
                                 Stmt.StoreVal->Op == NdOp::INT_SEXT);
            Value = "(" + typeToC(MemberType) + ")(" +
                    (Signed ? "intptr_t" : "uintptr_t") + ")(" + Value + ")";
          }
          OS << Value;
        }
        OS << ";\n";
        break;
      }
    if (Stmt.MemoryOrdering == NdMemoryOrdering::None &&
        Stmt.MemoryAddressSpace == NdMemoryAddressSpace::Default)
      if (auto Slot = namedFrameSlot(*Stmt.StoreAddr, Stmt.StoreVal->Type)) {
        if (isParamCopy(*Stmt.StoreVal)) {
          if (auto Src = copyForwardSource(*Stmt.StoreVal)) {
            auto It = CopyForward.find(*Slot);
            if (It != CopyForward.end() && It->second == *Src) {
              Analysis.DeadVars.insert(*Slot);
              break;
            }
          }
        }
        CopyForward.erase(*Slot);
        Analysis.DeadVars.erase(*Slot);
        const std::string ValueText = exprStr(*Stmt.StoreVal);
        emitIndent(Indent);
        if (auto Field =
                scalarRecordFieldDest(*Slot, *Stmt.StoreVal, ValueText))
          OS << *Field << " = ";
        else
          OS << *Slot << " = ";
        TypeRef ProjectedDestType = Stmt.StoreVal->Type;
        if (auto Disp = frameDisplacement(*Stmt.StoreAddr)) {
          auto ProjectedSlot = FrameSlots.find(*Disp);
          if (ProjectedSlot != FrameSlots.end() && ProjectedSlot->second.Type &&
              !(ProjectedDestType &&
                ProjectedDestType->Size < ProjectedSlot->second.Type->Size))
            ProjectedDestType = ProjectedSlot->second.Type;
        }
        if (isUnknownCallOperand(Stmt.StoreVal.get()))
          OS << "0";
        else if (ProjectedDestType &&
                 ProjectedDestType->Kind == NdTypeKind::Ptr)
          OS << "(" << typeToC(ProjectedDestType) << ")(uintptr_t)("
             << ValueText << ")";
        else if (ProjectedDestType &&
                 ProjectedDestType->Kind == NdTypeKind::Int &&
                 isStringLiteralText(ValueText))
          OS << "(" << typeToC(ProjectedDestType) << ")(uintptr_t)("
             << ValueText << ")";
        else
          OS << ValueText;
        OS << ";\n";
        break;
      }
    if (Stmt.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      if (auto VA = constAddress(*Stmt.StoreAddr)) {
        if (auto Name = imageObjectName(*VA)) {
          emitIndent(Indent);
          OS << *Name << " = ";
          const std::string ValueText = exprStr(*Stmt.StoreVal);
          if (isUnknownCallOperand(Stmt.StoreVal.get()))
            OS << "0";
          else if (isStringLiteralText(ValueText))
            OS << "(uintptr_t)(" << ValueText << ")";
          else
            OS << ValueText;
          OS << ";\n";
          break;
        }
      }
    }
    bool ExactImageBytes = false;
    if (Stmt.MemoryAddressSpace == NdMemoryAddressSpace::Default)
      if (auto VA = constAddress(*Stmt.StoreAddr))
        ExactImageBytes = imageBackingAddress(*VA).has_value();
    writeMemoryStore(
        Stmt.StoreVal->Type,
        addrStr(*Stmt.StoreAddr, 0,
                Stmt.MemoryAddressSpace == NdMemoryAddressSpace::Default),
        isUnknownCallOperand(Stmt.StoreVal.get())
            ? "0"
            : storedValueText(*Stmt.StoreVal, Stmt.StoreVal->Type,
                              Stmt.MemoryOrdering),
        Stmt.MemoryOrdering, Stmt.MemoryAddressSpace, ExactImageBytes, Indent);
    break;
  }

  case StmtKind::Call:
    if (!Stmt.CallExpr)
      return;
    if (isCxxThrowExpr(Stmt.CallExpr.get())) {
      emitIndent(Indent);
      writeCxxThrowExpr(Stmt, *Stmt.CallExpr);
      OS << ";\n";
      break;
    }
    // An instruction reading fixed registers loads them in an `__asm` block,
    // as a software interrupt does.
    if (x86UsesImplicitRegisterAsm(Stmt.CallExpr->IntrinsicId)) {
      const std::string Rendered = renderX86InterruptStatement(
          Opts.TheArch, *Stmt.CallExpr, "", 0,
          [this](const HighExpr &E) { return exprStr(E); });
      if (!Rendered.empty()) {
        emitRenderedStatement(Indent, Rendered);
        break;
      }
    }
    {
      auto Rendered = renderX86SegmentedIntrinsicStatement(
          Opts.TheArch, *Stmt.CallExpr, nullptr,
          [this](const HighExpr &E) { return intrinsicOperandStr(E); },
          [this](const MedVar &V) { return varName(V); },
          [this](const MedVar &V) {
            return !Analysis.DeadVars.count(varName(V));
          },
          [this](const HighExpr &E, uint16_t Width) {
            return isSameWidthUnsigned(E, Width);
          },
          Opts.Format == BinaryFormat::COFF);
      if (!Rendered.empty()) {
        writeCIndentedSnippet(OS, Rendered, Indent);
        break;
      }
    }
    if (!Stmt.CallExpr->IntrinsicOutputs.empty()) {
      auto Rendered = MultiOutputRender{}(
          Opts.TheArch, Stmt.CallExpr->IntrinsicId,
          Stmt.CallExpr->IntrinsicOutputs, Stmt.CallExpr->Operands,
          [this](const HighExpr &E) { return exprStr(E); },
          [this](const MedVar &V) { return varName(V); },
          [this](const MedVar &V) {
            return !Analysis.DeadVars.count(varName(V));
          });
      if (!Rendered.empty()) {
        writeCIndentedSnippet(OS, Rendered, Indent);
        HasCIntrinsics = true;
        break;
      }
    }
    emitIndent(Indent);
    OS << statementCallText(*Stmt.CallExpr) << ";\n";
    break;

  case StmtKind::Return:
    emitIndent(Indent);
    if (!IndirectReturnName.empty() &&
        (InferredVoid || !Stmt.RetVal || Stmt.RetVal->Kind == ExprKind::Undef ||
         (Stmt.RetVal->Kind == ExprKind::Var &&
          !Analysis.AssignedVars.count(varName(Stmt.RetVal->Var)) &&
          (Stmt.RetVal->Var.Kind != MedVar::Param || InEHClauseBody))))
      OS << "return " << IndirectReturnName << ";\n";
    else if (InferredVoid) {
      // A function inferred `void` can return what a callee left in the
      // result register, as a thunk of a `void` routine does: the call runs
      // all the same.
      if (const HighExpr *Call =
              Stmt.RetVal ? returnedCall(*Stmt.RetVal) : nullptr) {
        OS << statementCallText(*Call) << ";\n";
        emitIndent(Indent);
      }
      OS << "return;\n";
    } else if (!Stmt.RetVal || Stmt.RetVal->Kind == ExprKind::Undef ||
               (Stmt.RetVal->Kind == ExprKind::Var &&
                !Analysis.AssignedVars.count(varName(Stmt.RetVal->Var)) &&
                (Stmt.RetVal->Var.Kind != MedVar::Param || InEHClauseBody)))
      OS << "__builtin_trap(); /* unknown return value */\n";
    else
      OS << "return " << formatReturnExpr(*Stmt.RetVal) << ";\n";
    if (!IndirectReturnName.empty() && !InEHClauseBody && !InCxxCleanupBody)
      PrintedIndirectReturn = true;
    break;

  case StmtKind::If:
    if (!Stmt.Cond)
      return;
    if (stmtsEffectivelyEmpty(Stmt.Body))
      return;
    emitIndent(Indent);
    OS << "if (" << condStr(*Stmt.Cond) << ") {\n";
    writeStmtsIsolated(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::IfElse:
    if (!Stmt.Cond)
      return;
    if (stmtsEffectivelyEmpty(Stmt.Body) &&
        stmtsEffectivelyEmpty(Stmt.ElseBody))
      return;
    if (stmtsEffectivelyEmpty(Stmt.Body) &&
        !stmtsEffectivelyEmpty(Stmt.ElseBody)) {
      emitIndent(Indent);
      OS << "if (" << invertCondStr(*Stmt.Cond) << ") {\n";
      writeStmtsIsolated(Stmt.ElseBody, Indent + 1);
      emitIndent(Indent);
      OS << "}\n";
      break;
    }
    emitIndent(Indent);
    if (!stmtsEffectivelyEmpty(Stmt.ElseBody)) {
      if (auto Greater = preferGreaterIfElseCond(*Stmt.Cond)) {
        OS << "if (" << *Greater << ") {\n";
        writeStmtsIsolated(Stmt.ElseBody, Indent + 1);
        emitIndent(Indent);
        OS << "} else {\n";
        writeStmtsIsolated(Stmt.Body, Indent + 1);
        emitIndent(Indent);
        OS << "}\n";
        break;
      }
    }
    OS << "if (" << condStr(*Stmt.Cond) << ") {\n";
    writeStmtsIsolated(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    if (stmtsEffectivelyEmpty(Stmt.ElseBody)) {
      OS << "}\n";
      break;
    }
    OS << "} else {\n";
    writeStmtsIsolated(Stmt.ElseBody, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::While:
    emitIndent(Indent);
    OS << "while (" << (Stmt.Cond ? condStr(*Stmt.Cond) : "1") << ") {\n";
    writeStmtsIsolated(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::DoWhile:
    emitIndent(Indent);
    OS << "do {\n";
    writeStmtsIsolated(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "} while (" << (Stmt.Cond ? condStr(*Stmt.Cond) : "1") << ");\n";
    break;

  case StmtKind::For:
    emitIndent(Indent);
    OS << "for (;;) {\n";
    if (Stmt.Cond) {
      emitIndent(Indent + 1);
      OS << "if (!(" << exprStr(*Stmt.Cond) << ")) break;\n";
    }
    writeStmtsIsolated(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::Switch: {
    if (!Stmt.SwitchExpr)
      return;
    emitIndent(Indent);
    OS << "switch (" << exprStr(*Stmt.SwitchExpr) << ") {\n";
    // A body that already leaves the case needs no `break` after it.
    auto endsInJump = [&](const std::vector<HighStmt> &Body) {
      if (Body.empty() || Analysis.DeadStmts.count(&Body.back()))
        return false;
      const StmtKind K = Body.back().Kind;
      return K == StmtKind::Return || K == StmtKind::Goto ||
             K == StmtKind::Break || K == StmtKind::Continue;
    };
    for (auto &C : Stmt.Cases) {
      emitIndent(Indent);
      OS << "case " << constStr(C.Value, Stmt.SwitchExpr->Type) << ":\n";
      if (C.FallsThrough)
        continue;
      writeStmtsIsolated(C.Body, Indent + 1);
      if (!endsInJump(C.Body)) {
        emitIndent(Indent + 1);
        OS << "break;\n";
      }
    }
    if (!Stmt.DefaultBody.empty()) {
      emitIndent(Indent);
      OS << "default:\n";
      writeStmtsIsolated(Stmt.DefaultBody, Indent + 1);
      if (!endsInJump(Stmt.DefaultBody)) {
        emitIndent(Indent + 1);
        OS << "break;\n";
      }
    }
    emitIndent(Indent);
    OS << "}\n";
    break;
  }

  case StmtKind::Goto:
    emitIndent(Indent);
    // The fail-closed form of an indirect branch whose targets were not
    // proven. There is no label to name; the failure stays at run time.
    if (Stmt.GotoTarget == InvalidVA) {
      OS << "__builtin_trap(); /* unresolved indirect branch at 0x"
         << llvm::utohexstr(Stmt.Addr) << " */\n";
      break;
    }
    if (llvm::is_contained(LeaveTargets, Stmt.GotoTarget)) {
      OS << "__leave;\n";
      break;
    }
    OS << "goto L_" + llvm::utohexstr(Stmt.GotoTarget) + ";\n";
    break;

  case StmtKind::Break:
    emitIndent(Indent);
    OS << "break;\n";
    break;

  case StmtKind::Continue:
    emitIndent(Indent);
    OS << "continue;\n";
    break;

  case StmtKind::Block:
    if (stmtsEffectivelyEmpty(Stmt.Body))
      break;
    emitIndent(Indent);
    OS << "{\n";
    writeStmtsIsolated(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::ExprStmt:
    if (Stmt.Val) {
      if (isCxxThrowExpr(Stmt.Val.get())) {
        emitIndent(Indent);
        writeCxxThrowExpr(Stmt, *Stmt.Val);
        OS << ";\n";
        break;
      }
      if (Stmt.Val->Kind == ExprKind::Load && !Stmt.Val->Operands.empty() &&
          namedFrameSlot(*Stmt.Val->Operands[0]))
        break;
      emitIndent(Indent);
      OS << (Stmt.Val->Kind == ExprKind::Store
                 ? statementText(exprStr(*Stmt.Val))
                 : exprStr(*Stmt.Val))
         << ";\n";
    }
    break;

  case StmtKind::SEHTry: {
    // `__leave` ends the innermost __try block, so only this try's protected
    // body may use it, and only while nothing else runs on that exit: every
    // clause is an __except or a __finally printing an empty body.
    const std::vector<va_t> Leave = std::move(NextTryLeaveTargets);
    NextTryLeaveTargets.clear();
    const std::vector<va_t> OuterLeave = std::move(LeaveTargets);
    LeaveTargets.clear();
    auto RestoreLeave =
        llvm::make_scope_exit([&] { LeaveTargets = OuterLeave; });
    // C forbids a goto into a __try body, but machine code may branch into
    // the middle of a protected range.  x64 SEH protection is by address,
    // so splitting an __except try at that entry into two consecutive tries
    // is equivalent and puts the label between them.  An inline handler body
    // would be copied, so only out-of-line handlers are split.  A __finally
    // runs on normal exit too, but an out-of-line one prints an empty body
    // (its normal-exit call is already explicit code), so each piece runs
    // nothing extra.
    if (Stmt.Body.size() >= 2 && !Stmt.EHClauses.empty() &&
        std::all_of(Stmt.EHClauses.begin(), Stmt.EHClauses.end(),
                    [](const HighEHClause &Clause) {
                      return Clause.Kind == HighEHClauseKind::SEHExcept ||
                             Clause.Kind == HighEHClauseKind::SEHFinally;
                    }) &&
        std::all_of(Stmt.EHClauseBodies.begin(), Stmt.EHClauseBodies.end(),
                    [](const auto &Body) { return Body.empty(); })) {
      std::map<va_t, unsigned> Inner;
      walkStmts(Stmt.Body, [&](const HighStmt &Child) {
        if (Child.Kind == StmtKind::Goto)
          ++Inner[Child.GotoTarget];
      });
      for (size_t K = 1; K < Stmt.Body.size(); ++K) {
        const va_t Addr = Stmt.Body[K].Addr;
        auto Uses = GotoTargetUses.find(Addr);
        if (Addr == 0 || Addr == InvalidVA || Uses == GotoTargetUses.end() ||
            Uses->second <= Inner[Addr])
          continue;
        std::vector<HighStmt> Split(2, Stmt);
        Split[0].Body.assign(Stmt.Body.begin(), Stmt.Body.begin() + K);
        Split[1].Body.assign(Stmt.Body.begin() + K, Stmt.Body.end());
        Split[1].Addr = Addr;
        writeStmts(Split, Indent);
        return;
      }
    }
    if (!Stmt.EHIsReducible || Stmt.EHClauses.size() != 1 ||
        Stmt.EHClauseBodies.size() != 1) {
      emitIndent(Indent);
      OS << "__try {\n";
      writeTryBody(Stmt.Body, Indent + 1);
      emitIndent(Indent);
      OS << "} __except (EXCEPTION_EXECUTE_HANDLER) {\n";
      for (const HighEHClause &Clause : Stmt.EHClauses)
        writeSEHExceptionCodeCapture(Clause.HandlerVA, Indent + 1);
      emitIndent(Indent + 1);
      OS << "/* unstructured SEH region [0x"
         << llvm::utohexstr(Stmt.EHRange.Begin) << ", 0x"
         << llvm::utohexstr(Stmt.EHRange.End) << ") */\n";
      emitIndent(Indent);
      OS << "}\n";
      break;
    }
    const HighEHClause &Clause = Stmt.EHClauses.front();
    emitIndent(Indent);
    OS << "__try {\n";
    if (Clause.Kind == HighEHClauseKind::SEHExcept ||
        (Clause.Kind == HighEHClauseKind::SEHFinally &&
         Stmt.EHClauseBodies.front().empty()))
      LeaveTargets = Leave;
    writeTryBody(Stmt.Body, Indent + 1);
    LeaveTargets.clear();
    emitIndent(Indent);
    if (Clause.Kind == HighEHClauseKind::SEHFinally) {
      OS << "} __finally {\n";
      {
        const bool SavedHandler = InEHClauseBody;
        InEHClauseBody = true;
        writeStmtsIsolated(Stmt.EHClauseBodies.front(), Indent + 1);
        InEHClauseBody = SavedHandler;
      }
      if (Stmt.EHClauseBodies.front().empty()) {
        emitIndent(Indent + 1);
        OS << "/* finally handler @ 0x"
           << llvm::utohexstr(Clause.FilterOrActionVA) << " */\n";
      }
    } else {
      OS << "} __except (";
      if (Clause.FilterOrActionVA == 0) {
        OS << "EXCEPTION_EXECUTE_HANDLER";
      } else if (Clause.FilterValue) {
        OS << sehFilterValueText(*Clause.FilterValue);
      } else {
        std::string FilterName;
        if (auto It = DefinedFunctionsByAddress.find(Clause.FilterOrActionVA);
            It != DefinedFunctionsByAddress.end() && It->second)
          FilterName = functionIdentifier(*It->second);
        else if (Dbg) {
          if (auto Sym = Dbg->resolveFunction(Clause.FilterOrActionVA);
              Sym && !Sym->Name.empty())
            FilterName = functionIdentifier(Sym->Name);
        }
        if (FilterName.empty() && Opts.Image) {
          if (const Symbol *Sym =
                  Opts.Image->findSymbolAt(Clause.FilterOrActionVA);
              Sym && !Sym->Name.empty() &&
              llvm::StringRef(Sym->Name).find(kAutoFuncPrefix) != 0)
            FilterName = functionIdentifier(Sym->Name);
        }
        if (FilterName.empty() && Clause.FilterOrActionVA)
          FilterName = functionIdentifier(
              (kAutoFuncPrefix + llvm::utohexstr(Clause.FilterOrActionVA))
                  .str());
        OS << FilterName << "(GetExceptionInformation())";
      }
      OS << ") {\n";
      // GetExceptionCode() is only valid in the __except arm; the handler
      // code, attached here or at its label, reads the captured value.
      writeSEHExceptionCodeCapture(Clause.HandlerVA, Indent + 1);
      {
        const bool SavedHandler = InEHClauseBody;
        InEHClauseBody = true;
        writeTryBody(Stmt.EHClauseBodies.front(), Indent + 1);
        InEHClauseBody = SavedHandler;
      }
      if (Stmt.EHClauseBodies.front().empty()) {
        emitIndent(Indent + 1);
        if (CurrentFunc && CurrentFunc->ExceptionMetadata &&
            CurrentFunc->ExceptionMetadata->ownsCode(Clause.HandlerVA))
          OS << "goto L_" << llvm::utohexstr(Clause.HandlerVA) << ";\n";
        else
          OS << "/* handler @ 0x" << llvm::utohexstr(Clause.HandlerVA)
             << " */\n";
      }
    }
    emitIndent(Indent);
    OS << "}\n";
    break;
  }

  case StmtKind::CxxTry: {
    if (!Opts.StructuredExceptionSyntax) {
      writeCExceptionRegion(Stmt, Indent);
      break;
    }
    // `__leave` names the innermost __try; none of its jumps may use it here.
    const std::vector<va_t> OuterLeave = std::move(LeaveTargets);
    LeaveTargets.clear();
    NextTryLeaveTargets.clear();
    auto RestoreLeave =
        llvm::make_scope_exit([&] { LeaveTargets = OuterLeave; });
    emitIndent(Indent);
    const bool CleanupOnly =
        !Stmt.EHClauses.empty() &&
        llvm::all_of(Stmt.EHClauses, [](const HighEHClause &Clause) {
          return Clause.Kind == HighEHClauseKind::CxxCleanup;
        });
    OS << (CleanupOnly ? "__wind {\n" : "try {\n");
    writeTryBody(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}";
    for (size_t I = 0; I < Stmt.EHClauses.size(); ++I) {
      const HighEHClause &Clause = Stmt.EHClauses[I];
      if (Clause.Kind == HighEHClauseKind::CxxCleanup) {
        OS << "\n";
        emitIndent(Indent);
        OS << "__unwind {\n";
        emitIndent(Indent);
        OS << "/* unwind cleanup(state=" << Clause.State << ") */\n";
        if (I < Stmt.EHClauseBodies.size()) {
          const bool SavedHandler = InEHClauseBody;
          const bool SavedCleanup = InCxxCleanupBody;
          InEHClauseBody = true;
          InCxxCleanupBody = true;
          writeStmtsIsolated(Stmt.EHClauseBodies[I], Indent + 1);
          InEHClauseBody = SavedHandler;
          InCxxCleanupBody = SavedCleanup;
        }
        emitIndent(Indent);
        OS << "}\n";
        continue;
      }
      OS << " catch (";
      writeCxxCatchType(OS, Clause);
      if (auto Named = CxxCatchNames.find(&Clause);
          Named != CxxCatchNames.end())
        OS << Named->second;
      OS << ") {\n";
      if (I < Stmt.EHClauseBodies.size()) {
        const bool SavedHandler = InEHClauseBody;
        InEHClauseBody = true;
        const auto Named = CxxCatchNames.find(&Clause);
        if (Named != CxxCatchNames.end())
          OpenCatchObjects.push_back(Named->second);
        writeStmtsIsolated(Stmt.EHClauseBodies[I], Indent + 1);
        if (Named != CxxCatchNames.end())
          OpenCatchObjects.pop_back();
        InEHClauseBody = SavedHandler;
        if (Stmt.EHClauseBodies[I].empty() && Clause.HandlerVA) {
          emitIndent(Indent + 1);
          OS << "/* handler @ 0x" << llvm::utohexstr(Clause.HandlerVA)
             << " */\n";
        }
      }
      emitIndent(Indent);
      OS << "}";
    }
    OS << "\n";
    break;
  }

  case StmtKind::ItaniumTry: {
    // `__leave` names the innermost __try; none of its jumps may use it here.
    const std::vector<va_t> OuterLeave = std::move(LeaveTargets);
    LeaveTargets.clear();
    NextTryLeaveTargets.clear();
    auto RestoreLeave =
        llvm::make_scope_exit([&] { LeaveTargets = OuterLeave; });
    emitIndent(Indent);
    OS << "/* Itanium try [0x" << llvm::utohexstr(Stmt.EHRange.Begin) << ", 0x"
       << llvm::utohexstr(Stmt.EHRange.End)
       << ") recovered from the call-site table */\n";
    emitIndent(Indent);
    OS << "{\n";
    writeStmtsIsolated(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    // Every clause of an Itanium region enters the same landing pad, which is
    // ordinary code in this function rather than a funclet.  Naming the pad is
    // therefore the whole of what a clause adds: the pad selects between the
    // clauses itself, from the selector the personality left it.
    for (const HighEHClause &Clause : Stmt.EHClauses) {
      emitIndent(Indent);
      if (Clause.Kind == HighEHClauseKind::ItaniumSpec) {
        OS << "/* exception specification ";
        if (Clause.SpecTypeNames.empty()) {
          OS << "throw()";
        } else {
          OS << "throw(";
          for (size_t I = 0; I < Clause.SpecTypeNames.size(); ++I)
            OS << (I ? ", " : "") << Clause.SpecTypeNames[I];
          OS << ")";
        }
      } else {
        OS << "/* catch (";
        if (!Clause.TypeName.empty())
          OS << Clause.TypeName;
        else if (Clause.TypeDescriptorVA)
          OS << "typeinfo@0x" << llvm::utohexstr(Clause.TypeDescriptorVA);
        else
          OS << "...";
        OS << ")";
      }
      OS << ", filter=" << Clause.TypeFilter << ", depth=" << Clause.ChainDepth;
      if (Clause.ParseStatus != ExceptionParseStatus::Complete)
        OS << ", parse=" << getExceptionParseStatusName(Clause.ParseStatus);
      for (va_t Pad : Clause.LandingPadVAs)
        OS << " -> L_" << llvm::utohexstr(Pad);
      OS << " */\n";
    }
    break;
  }

  case StmtKind::Nop:
    // A removed PHI copy may still own a goto label, including the last label
    // in a compound statement. C11 requires a statement after that label.
    if (GotoTargets.count(Stmt.Addr)) {
      emitIndent(Indent);
      OS << ";\n";
    }
    break;
  }
}

bool HighCWriter::tryWriteCursorForLoop(const std::vector<HighStmt> &Stmts,
                                        size_t I, size_t End, int Indent,
                                        size_t &Last) {
  const HighStmt &Init = Stmts[I];
  if (Init.Kind != StmtKind::Assign || !Init.Dst || !Init.Val)
    return false;
  if (Init.Dst->Kind != ExprKind::Var && Init.Dst->Kind != ExprKind::Phi)
    return false;
  const std::string Cursor = printedForwardedVar(varName(Init.Dst->Var), 0);
  if (!isCIdentifier(Cursor))
    return false;

  auto PrintsForwardedPath = [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Assign || !S.Dst)
      return false;
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      return false;
    const std::string Printed = printedForwardedVar(varName(S.Dst->Var), 0);
    return Printed.find("->") != std::string::npos ||
           Printed.find('[') != std::string::npos;
  };
  auto IsHidden = [&](const HighStmt &S) {
    return stmtHiddenFromC(S) || PrintsForwardedPath(S) ||
           S.Kind == StmtKind::Nop || Analysis.DeadStmts.count(&S);
  };
  auto NextLive = [&](size_t From) {
    size_t K = From;
    while (K < End && IsHidden(Stmts[K]))
      ++K;
    return K;
  };
  const size_t WhileIdx = NextLive(I + 1);
  if (WhileIdx >= End)
    return false;
  const HighStmt &Loop = Stmts[WhileIdx];
  if (Loop.Kind != StmtKind::While)
    return false;
  const HighExpr *WhileCond = peelIntegerViewOps(Loop.Cond.get());
  if (Loop.Cond && !(WhileCond && WhileCond->Kind == ExprKind::Const &&
                     WhileCond->ConstVal != 0))
    return false;

  auto ExprNamesCursor = [&](const HighExpr &E) {
    const HighExpr *P = peelIntegerViewOps(&E);
    if (!P)
      P = &E;
    if (P->Kind != ExprKind::Var && P->Kind != ExprKind::Phi)
      return false;
    const std::string Raw = varName(P->Var);
    if (Raw == Cursor || copyForwardName(Raw) == Cursor)
      return true;
    return printedForwardedVar(Raw, 0) == Cursor;
  };
  auto IsZero = [&](const HighExpr &E) {
    const HighExpr *P = peelIntegerViewOps(&E);
    return P && P->Kind == ExprKind::Const && P->ConstVal == 0;
  };
  auto IsNullCheck = [&](const HighExpr &C) {
    const HighExpr *P = peelIntegerViewOps(&C);
    if (!P)
      P = &C;
    if (P->Kind == ExprKind::UnaryOp && P->Op == NdOp::BOOL_NOT &&
        !P->Operands.empty() && P->Operands[0])
      return ExprNamesCursor(*P->Operands[0]);
    if (P->Kind != ExprKind::BinOp || P->Op != NdOp::INT_EQUAL ||
        P->Operands.size() < 2 || !P->Operands[0] || !P->Operands[1])
      return false;
    return (ExprNamesCursor(*P->Operands[0]) && IsZero(*P->Operands[1])) ||
           (ExprNamesCursor(*P->Operands[1]) && IsZero(*P->Operands[0]));
  };
  auto LiveOf = [&](const std::vector<HighStmt> &Body,
                    StmtKind Kind) -> const HighStmt * {
    const HighStmt *Hit = nullptr;
    for (const HighStmt &S : Body) {
      if (IsHidden(S))
        continue;
      if (Hit)
        return nullptr;
      if (S.Kind != Kind)
        return nullptr;
      Hit = &S;
    }
    return Hit;
  };
  auto AssignsCursor = [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return false;
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      return false;
    const std::string Printed = printedForwardedVar(varName(S.Dst->Var), 0);
    return Printed == Cursor || varName(S.Dst->Var) == Cursor;
  };

  const HighStmt *MissIf = nullptr;
  const HighStmt *HitIf = nullptr;
  const HighStmt *Step = nullptr;
  for (const HighStmt &S : Loop.Body) {
    if (IsHidden(S))
      continue;
    if (!MissIf) {
      if ((S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse) && S.Cond &&
          IsNullCheck(*S.Cond) && stmtsEffectivelyEmpty(S.ElseBody)) {
        if (const HighStmt *Ret = LiveOf(S.Body, StmtKind::Return);
            Ret && Ret->RetVal) {
          MissIf = &S;
          continue;
        }
      }
      return false;
    }
    if (!HitIf) {
      if ((S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse) && S.Cond &&
          stmtsEffectivelyEmpty(S.ElseBody) &&
          LiveOf(S.Body, StmtKind::Break)) {
        HitIf = &S;
        continue;
      }
      return false;
    }
    if (!Step) {
      if (AssignsCursor(S)) {
        Step = &S;
        continue;
      }
      return false;
    }
    return false;
  }
  if (!MissIf || !HitIf || !Step)
    return false;

  const HighStmt *MissRet = nullptr;
  for (const HighStmt &S : MissIf->Body) {
    if (IsHidden(S))
      continue;
    if (S.Kind == StmtKind::Return)
      MissRet = &S;
  }
  if (!MissRet || !MissRet->RetVal)
    return false;

  auto Targeted = [&](const HighStmt &S) {
    return S.Addr && S.Addr != InvalidVA && GotoTargets.count(S.Addr);
  };
  const size_t AfterIdx = NextLive(WhileIdx + 1);
  const HighStmt *AfterRet = nullptr;
  if (AfterIdx < End && Stmts[AfterIdx].Kind == StmtKind::Return &&
      Stmts[AfterIdx].RetVal)
    AfterRet = &Stmts[AfterIdx];
  const bool Fold = AfterRet && !Targeted(*AfterRet) && !Targeted(*MissIf) &&
                    !Targeted(*MissRet);

  auto WriteLabel = [&](const HighStmt &S) {
    if (Targeted(S))
      OS << "L_" + llvm::utohexstr(S.Addr) + ":\n";
  };
  auto WriteAssignRhs = [&](const HighStmt &S) {
    auto DeclaredType = declaredParamType(S.Dst->Var);
    if (!DeclaredType)
      DeclaredType = S.Dst->Type;
    if (DeclaredType && DeclaredType->Kind == NdTypeKind::Ptr)
      OS << "(" << typeToC(DeclaredType) << ")(uintptr_t)(" << exprStr(*S.Val)
         << ")";
    else
      OS << exprStr(*S.Val);
  };

  const bool HeaderTargeted = Targeted(Loop);
  if (HeaderTargeted) {
    writeStmt(Init, Indent);
    WriteLabel(Loop);
  }
  emitIndent(Indent);
  OS << "for (" << Cursor;
  if (HeaderTargeted)
    OS << "; ";
  else {
    OS << " = ";
    WriteAssignRhs(Init);
    OS << "; ";
  }
  if (Fold)
    OS << Cursor << "; ";
  else
    OS << "; ";
  OS << Cursor << " = " << exprStr(*Step->Val) << ") {\n";
  if (Fold) {
    WriteLabel(*HitIf);
    emitIndent(Indent + 1);
    OS << "if (" << condStr(*HitIf->Cond) << ") {\n";
    emitIndent(Indent + 2);
    OS << "return " << formatReturnExpr(*AfterRet->RetVal) << ";\n";
    emitIndent(Indent + 1);
    OS << "}\n";
  } else {
    WriteLabel(*MissIf);
    writeStmt(*MissIf, Indent + 1);
    WriteLabel(*HitIf);
    writeStmt(*HitIf, Indent + 1);
  }
  emitIndent(Indent);
  OS << "}\n";
  if (Fold) {
    emitIndent(Indent);
    OS << "return " << formatReturnExpr(*MissRet->RetVal) << ";\n";
  }
  Last = Fold ? AfterIdx : WhileIdx;
  return true;
}

void HighCWriter::writeStmts(const std::vector<HighStmt> &Stmts, int Indent,
                             size_t End) {
  if (End > Stmts.size())
    End = Stmts.size();
  va_t LastLabel = InvalidVA;
  bool AfterNoReturn = false;
  auto SoleLiveGoto = [&](const std::vector<HighStmt> &Body) -> va_t {
    va_t Target = 0;
    for (const HighStmt &N : Body) {
      if (N.IsPhiCopy || N.Kind == StmtKind::Nop || stmtHiddenFromC(N))
        continue;
      if (N.Kind != StmtKind::Goto || !N.GotoTarget ||
          N.GotoTarget == InvalidVA)
        return 0;
      if (Target)
        return 0;
      Target = N.GotoTarget;
    }
    return Target;
  };
  auto TrailingLiveGoto = [&](const std::vector<HighStmt> &Body) -> va_t {
    for (auto It = Body.rbegin(); It != Body.rend(); ++It) {
      if (It->IsPhiCopy || It->Kind == StmtKind::Nop || stmtHiddenFromC(*It))
        continue;
      if (It->Kind == StmtKind::Goto && It->GotoTarget &&
          It->GotoTarget != InvalidVA)
        return It->GotoTarget;
      return 0;
    }
    return 0;
  };
  auto FindAddr = [](const std::vector<HighStmt> &Body, va_t Addr) -> size_t {
    if (!Addr || Addr == InvalidVA)
      return SIZE_MAX;
    for (size_t K = 0; K < Body.size(); ++K)
      if (Body[K].Addr == Addr)
        return K;
    return SIZE_MAX;
  };
  // A goto can still reach a statement placed after a noreturn call (the
  // `ret` that follows `int 29h`), so a labeled one is live code.
  std::function<bool(const HighStmt &)> CarriesLabel = [&](const HighStmt &S) {
    if (S.Addr != 0 && S.Addr != InvalidVA && GotoTargets.count(S.Addr))
      return true;
    for (const HighStmt &Child : S.Body)
      if (CarriesLabel(Child))
        return true;
    return false;
  };
  for (size_t I = 0; I < End; ++I) {
    const HighStmt &S = Stmts[I];
    if (Opts.StructuredExceptionSyntax && S.Kind == StmtKind::If && S.Cond &&
        I + 1 < End && Stmts[I + 1].Kind == StmtKind::CxxTry) {
      const va_t NameAt = SoleLiveGoto(S.Body);
      const HighStmt &Try = Stmts[I + 1];
      const size_t NameIdx = FindAddr(Try.Body, NameAt);
      auto WriteCleanupTry = [&](auto &&WriteBody) {
        emitIndent(Indent);
        const bool CleanupOnly =
            !Try.EHClauses.empty() &&
            llvm::all_of(Try.EHClauses, [](const HighEHClause &Clause) {
              return Clause.Kind == HighEHClauseKind::CxxCleanup;
            });
        OS << (CleanupOnly ? "__wind {\n" : "try {\n");
        WriteBody(Indent + 1);
        emitIndent(Indent);
        OS << "}";
        for (size_t C = 0; C < Try.EHClauses.size(); ++C) {
          const HighEHClause &Clause = Try.EHClauses[C];
          if (Clause.Kind == HighEHClauseKind::CxxCleanup) {
            OS << "\n";
            emitIndent(Indent);
            OS << "__unwind {\n";
            emitIndent(Indent);
            OS << "/* unwind cleanup(state=" << Clause.State << ") */\n";
            if (C < Try.EHClauseBodies.size()) {
              const bool SavedHandler = InEHClauseBody;
              const bool SavedCleanup = InCxxCleanupBody;
              InEHClauseBody = true;
              InCxxCleanupBody = true;
              writeStmtsIsolated(Try.EHClauseBodies[C], Indent + 1);
              InEHClauseBody = SavedHandler;
              InCxxCleanupBody = SavedCleanup;
            }
            emitIndent(Indent);
            OS << "}\n";
            continue;
          }
          OS << " catch (";
          writeCxxCatchType(OS, Clause);
          if (auto Named = CxxCatchNames.find(&Clause);
              Named != CxxCatchNames.end())
            OS << Named->second;
          OS << ") {\n";
          if (C < Try.EHClauseBodies.size()) {
            const bool SavedHandler = InEHClauseBody;
            InEHClauseBody = true;
            const auto Named = CxxCatchNames.find(&Clause);
            if (Named != CxxCatchNames.end())
              OpenCatchObjects.push_back(Named->second);
            writeStmtsIsolated(Try.EHClauseBodies[C], Indent + 1);
            if (Named != CxxCatchNames.end())
              OpenCatchObjects.pop_back();
            InEHClauseBody = SavedHandler;
            if (Try.EHClauseBodies[C].empty() && Clause.HandlerVA) {
              emitIndent(Indent + 1);
              OS << "/* handler @ 0x" << llvm::utohexstr(Clause.HandlerVA)
                 << " */\n";
            }
          }
          emitIndent(Indent);
          OS << "}";
        }
        OS << "\n";
      };
      if (NameAt && NameIdx == SIZE_MAX) {
        std::function<bool(const std::vector<HighStmt> &)> HasAddr =
            [&](const std::vector<HighStmt> &Body) -> bool {
          for (const HighStmt &N : Body) {
            if (N.Addr == NameAt)
              return true;
            if (HasAddr(N.Body) || HasAddr(N.ElseBody) ||
                HasAddr(N.DefaultBody))
              return true;
            for (const auto &Case : N.Cases)
              if (HasAddr(Case.Body))
                return true;
            for (const auto &Clause : N.EHClauseBodies)
              if (HasAddr(Clause))
                return true;
          }
          return false;
        };
        size_t Owner = SIZE_MAX;
        for (size_t K = 0; K < Try.Body.size(); ++K) {
          if (Try.Body[K].Kind == StmtKind::IfElse &&
              HasAddr(Try.Body[K].ElseBody)) {
            Owner = K;
            break;
          }
        }
        if (Owner != SIZE_MAX) {
          GotoTargets.erase(NameAt);
          const HighStmt &Joined = Try.Body[Owner];
          WriteCleanupTry([&](int Inner) {
            auto WriteVisible = [&](const HighStmt &N, int Ind) {
              if (stmtHiddenFromC(N))
                return;
              writeStmt(N, Ind);
            };
            emitIndent(Inner);
            OS << "if (" << invertCondStr(*S.Cond) << ") {\n";
            for (size_t K = 0; K < Owner; ++K)
              WriteVisible(Try.Body[K], Inner + 1);
            writeStmt(Joined, Inner + 1);
            emitIndent(Inner);
            OS << "} else {\n";
            for (const HighStmt &B : Joined.ElseBody)
              WriteVisible(B, Inner + 1);
            emitIndent(Inner);
            OS << "}\n";
            for (size_t K = Owner + 1; K < Try.Body.size(); ++K)
              WriteVisible(Try.Body[K], Inner);
          });
          I = I + 1;
          continue;
        }
      }
      size_t LenIdx = NameIdx;
      while (NameIdx != SIZE_MAX && LenIdx > 0 &&
             (Try.Body[LenIdx - 1].Kind == StmtKind::Nop ||
              (Try.Body[LenIdx - 1].Kind == StmtKind::Block &&
               Try.Body[LenIdx - 1].Body.empty())))
        --LenIdx;
      if (NameIdx != SIZE_MAX && LenIdx > 0)
        --LenIdx;
      const bool LenReady = NameAt && NameIdx != SIZE_MAX && LenIdx < NameIdx &&
                            Try.Body[LenIdx].Kind == StmtKind::If &&
                            Try.Body[LenIdx].Cond;
      const size_t JoinIdx = NameIdx == SIZE_MAX ? 0 : NameIdx + 1;
      const va_t JoinAt =
          LenReady && JoinIdx < Try.Body.size() ? Try.Body[JoinIdx].Addr : 0;
      if (LenReady && JoinAt &&
          TrailingLiveGoto(Try.Body[LenIdx].Body) == JoinAt) {
        const HighStmt &LenIf = Try.Body[LenIdx];
        emitIndent(Indent);
        const bool CleanupOnly =
            !Try.EHClauses.empty() &&
            llvm::all_of(Try.EHClauses, [](const HighEHClause &Clause) {
              return Clause.Kind == HighEHClauseKind::CxxCleanup;
            });
        OS << (CleanupOnly ? "__wind {\n" : "try {\n");
        const int Inner = Indent + 1;
        auto WriteVisible = [&](const HighStmt &N, int Ind) {
          if (stmtHiddenFromC(N))
            return;
          writeStmt(N, Ind);
        };
        emitIndent(Inner);
        OS << "if (" << invertCondStr(*S.Cond) << ") {\n";
        for (size_t K = 0; K < LenIdx; ++K)
          WriteVisible(Try.Body[K], Inner + 1);
        emitIndent(Inner + 1);
        OS << "if (" << condStr(*LenIf.Cond) << ") {\n";
        for (const HighStmt &B : LenIf.Body) {
          if (B.Kind == StmtKind::Goto && B.GotoTarget == JoinAt)
            continue;
          WriteVisible(B, Inner + 2);
        }
        emitIndent(Inner + 1);
        OS << "} else {\n";
        if (Try.Body[NameIdx].Kind == StmtKind::Block) {
          for (const HighStmt &B : Try.Body[NameIdx].Body)
            WriteVisible(B, Inner + 2);
        } else {
          WriteVisible(Try.Body[NameIdx], Inner + 2);
        }
        emitIndent(Inner + 1);
        OS << "}\n";
        emitIndent(Inner);
        OS << "} else {\n";
        if (Try.Body[NameIdx].Kind == StmtKind::Block) {
          for (const HighStmt &B : Try.Body[NameIdx].Body)
            WriteVisible(B, Inner + 1);
        } else {
          WriteVisible(Try.Body[NameIdx], Inner + 1);
        }
        emitIndent(Inner);
        OS << "}\n";
        for (size_t K = NameIdx + 1; K < Try.Body.size(); ++K) {
          const HighStmt &N = Try.Body[K];
          if (N.Addr && N.Addr != InvalidVA && N.Addr != NameAt &&
              N.Addr != JoinAt && GotoTargets.count(N.Addr))
            OS << "L_" + llvm::utohexstr(N.Addr) + ":\n";
          WriteVisible(N, Inner);
        }
        emitIndent(Indent);
        OS << "}";
        for (size_t C = 0; C < Try.EHClauses.size(); ++C) {
          const HighEHClause &Clause = Try.EHClauses[C];
          if (Clause.Kind == HighEHClauseKind::CxxCleanup) {
            OS << "\n";
            emitIndent(Indent);
            OS << "__unwind {\n";
            emitIndent(Indent);
            OS << "/* unwind cleanup(state=" << Clause.State << ") */\n";
            if (C < Try.EHClauseBodies.size()) {
              const bool SavedHandler = InEHClauseBody;
              const bool SavedCleanup = InCxxCleanupBody;
              InEHClauseBody = true;
              InCxxCleanupBody = true;
              writeStmtsIsolated(Try.EHClauseBodies[C], Indent + 1);
              InEHClauseBody = SavedHandler;
              InCxxCleanupBody = SavedCleanup;
            }
            emitIndent(Indent);
            OS << "}\n";
            continue;
          }
          OS << " catch (";
          writeCxxCatchType(OS, Clause);
          if (auto Named = CxxCatchNames.find(&Clause);
              Named != CxxCatchNames.end())
            OS << Named->second;
          OS << ") {\n";
          if (C < Try.EHClauseBodies.size()) {
            const bool SavedHandler = InEHClauseBody;
            InEHClauseBody = true;
            const auto Named = CxxCatchNames.find(&Clause);
            if (Named != CxxCatchNames.end())
              OpenCatchObjects.push_back(Named->second);
            writeStmtsIsolated(Try.EHClauseBodies[C], Indent + 1);
            if (Named != CxxCatchNames.end())
              OpenCatchObjects.pop_back();
            InEHClauseBody = SavedHandler;
            if (Try.EHClauseBodies[C].empty() && Clause.HandlerVA) {
              emitIndent(Indent + 1);
              OS << "/* handler @ 0x" << llvm::utohexstr(Clause.HandlerVA)
                 << " */\n";
            }
          }
          emitIndent(Indent);
          OS << "}";
        }
        OS << "\n";
        I = I + 1;
        continue;
      }
    }
    auto IsGotoTarget = [&](va_t Addr) {
      return Addr && Addr != InvalidVA && GotoTargets.count(Addr);
    };
    if (AfterNoReturn) {
      // Dead junk after `throw` / RaiseException, including leftover assigns.
      // A later addressed join, including one nested in a structured branch,
      // is live again because another edge jumps into that branch.
      bool NestedJoin = false;
      auto CheckNested = [&](const std::vector<HighStmt> &Body) {
        walkStmts(Body, [&](const HighStmt &N) {
          NestedJoin |=
              IsGotoTarget(N.Addr) ||
              (N.Kind == StmtKind::While && IsGotoTarget(N.LoopHeaderAddr));
        });
      };
      CheckNested(S.Body);
      CheckNested(S.ElseBody);
      for (const auto &Case : S.Cases)
        CheckNested(Case.Body);
      CheckNested(S.DefaultBody);
      for (const auto &ClauseBody : S.EHClauseBodies)
        CheckNested(ClauseBody);
      const bool JoinLabel =
          NestedJoin || (IsGotoTarget(S.Addr) && S.Addr != LastLabel) ||
          (S.Kind == StmtKind::While && IsGotoTarget(S.LoopHeaderAddr) &&
           S.LoopHeaderAddr != LastLabel) ||
          llvm::any_of(S.Body, CarriesLabel);
      if (!JoinLabel)
        continue;
      AfterNoReturn = false;
    }
    noteCatchReaching(S);
    if (S.Kind == StmtKind::IfElse && S.Cond && I + 1 < End &&
        stmtsEffectivelyEmpty(S.Body) && !S.ElseBody.empty() &&
        S.ElseBody.back().Kind == StmtKind::Goto &&
        S.ElseBody.back().GotoTarget &&
        S.ElseBody.back().GotoTarget != InvalidVA) {
      const va_t Target = S.ElseBody.back().GotoTarget;
      size_t PrefixEnd = I + 1;
      size_t PhiCount = 0;
      while (PrefixEnd < End) {
        const HighStmt &P = Stmts[PrefixEnd];
        if (stmtHiddenFromC(P) || P.Kind == StmtKind::Nop ||
            P.Kind == StmtKind::Block) {
          ++PrefixEnd;
          continue;
        }
        if (P.IsPhiCopy) {
          ++PhiCount;
          ++PrefixEnd;
          continue;
        }
        break;
      }
      bool TargetOk = false;
      for (size_t K = I + 1; K <= PrefixEnd && K < End; ++K)
        if (Stmts[K].Addr == Target)
          TargetOk = true;
      // The fold prints the copies but not the removed statements and
      // anchors between them, nor their labels.  Only the folded jump may
      // reach such a label.
      std::function<bool(const HighStmt &)> DropsLabel =
          [&](const HighStmt &P) {
            if (P.Addr && P.Addr != InvalidVA && GotoTargets.count(P.Addr)) {
              auto Uses = GotoTargetUses.find(P.Addr);
              if (P.Addr != Target ||
                  (Uses != GotoTargetUses.end() && Uses->second > 1))
                return true;
            }
            return llvm::any_of(P.Body, DropsLabel);
          };
      bool KeepsLabels = true;
      for (size_t K = I + 1; K < PrefixEnd && KeepsLabels; ++K) {
        const HighStmt &P = Stmts[K];
        if (stmtHiddenFromC(P) || P.Kind == StmtKind::Nop ||
            P.Kind == StmtKind::Block)
          KeepsLabels = !DropsLabel(P);
      }
      if (PhiCount > 0 && TargetOk && KeepsLabels) {
        emitIndent(Indent);
        OS << "if (" << invertCondStr(*S.Cond) << ") {\n";
        std::vector<HighStmt> Taken(S.ElseBody.begin(), S.ElseBody.end() - 1);
        writeStmtsIsolated(Taken, Indent + 1);
        emitIndent(Indent);
        OS << "} else {\n";
        std::vector<HighStmt> Phis;
        for (size_t K = I + 1; K < PrefixEnd; ++K)
          if (!stmtHiddenFromC(Stmts[K]) && Stmts[K].Kind != StmtKind::Nop &&
              Stmts[K].Kind != StmtKind::Block)
            Phis.push_back(Stmts[K]);
        writeStmtsIsolated(Phis, Indent + 1);
        emitIndent(Indent);
        OS << "}\n";
        I = PrefixEnd - 1;
        continue;
      }
    }
    if (S.Kind == StmtKind::IfElse && S.Cond && I + 1 < End) {
      const HighStmt &Ret = Stmts[I + 1];
      if (Ret.Kind == StmtKind::Return && Ret.RetVal &&
          Ret.RetVal->Kind == ExprKind::Var) {
        const std::string Join = varName(Ret.RetVal->Var);
        const std::string JoinFwd = copyForwardName(Join);
        auto JoinAssign =
            [&](const std::vector<HighStmt> &Body) -> const HighStmt * {
          const HighStmt *Hit = nullptr;
          for (const HighStmt &E : Body) {
            if (E.Kind != StmtKind::Assign || !E.Dst || !E.Val ||
                E.Dst->Kind != ExprKind::Var)
              continue;
            const std::string Dst = varName(E.Dst->Var);
            if (Dst == Join || Dst == JoinFwd)
              Hit = &E;
          }
          return Hit;
        };
        const HighStmt *ThenJoin = JoinAssign(S.Body);
        const HighStmt *ElseJoin = JoinAssign(S.ElseBody);
        const HighStmt *AssignCall = nullptr;
        const HighStmt *ThenLiveAssign = nullptr;
        size_t ElseLive = 0;
        size_t ThenLive = 0;
        // The join assignments are what this rewrite sinks into `return`, so
        // they do not count as other live work whether or not a copy
        // forward already hides them.
        size_t ThenOther = 0;
        for (const HighStmt &E : S.Body) {
          if (stmtHiddenFromC(E))
            continue;
          ++ThenLive;
          if (&E != ThenJoin)
            ++ThenOther;
          if (E.Kind == StmtKind::Assign && E.Dst && E.Val &&
              E.Dst->Kind == ExprKind::Var)
            ThenLiveAssign = &E;
        }
        for (const HighStmt &E : S.ElseBody) {
          if (stmtHiddenFromC(E))
            continue;
          if (E.Kind == StmtKind::Assign && E.Dst && E.Val &&
              E.Dst->Kind == ExprKind::Var && E.Val->Kind == ExprKind::Call)
            AssignCall = &E;
          if (&E == ElseJoin && E.Val && E.Val->Kind == ExprKind::Var)
            continue;
          ++ElseLive;
        }
        bool MatchesCall = false;
        if (AssignCall) {
          const std::string CallDst = varName(AssignCall->Dst->Var);
          if (CallDst == Join || CallDst == JoinFwd)
            MatchesCall = true;
          if (ElseJoin && ElseJoin->Val &&
              ElseJoin->Val->Kind == ExprKind::Var &&
              varName(ElseJoin->Val->Var) == CallDst)
            MatchesCall = true;
        }
        const HighExpr *ThenVal = nullptr;
        if (ThenJoin && ThenJoin->Val && ThenJoin->Val->Kind != ExprKind::Undef)
          ThenVal = ThenJoin->Val.get();
        else if (ThenLive == 1 && ThenLiveAssign && ThenLiveAssign->Val &&
                 ThenLiveAssign->Val->Kind != ExprKind::Call &&
                 ThenLiveAssign->Val->Kind != ExprKind::Undef)
          ThenVal = ThenLiveAssign->Val.get();
        const bool ThenSinks =
            ThenJoin ? ThenOther == 0
                     : (ThenLive == 0 || (ThenLive == 1 && ThenVal));
        if (ThenSinks && ElseLive == 1 && AssignCall && MatchesCall) {
          emitIndent(Indent);
          OS << "if (" << invertCondStr(*S.Cond) << ")\n";
          emitIndent(Indent + 1);
          OS << "return " << exprStr(*AssignCall->Val) << ";\n";
          emitIndent(Indent);
          if (ThenVal)
            OS << "return " << formatReturnExpr(*ThenVal) << ";\n";
          else
            OS << "return;\n";
          AfterNoReturn = isCxxThrowExpr(AssignCall->Val.get()) ||
                          isNoreturnCallExpr(*AssignCall->Val);
          ++I;
          continue;
        }
      }
    }
    // The first label wins.  A try statement shares its address with its
    // first statement; labelling the try itself keeps a goto from outside
    // (or a loop back-edge) legal, since C forbids jumping into __try and
    // x64 SEH protection is by address, not by entry.
    bool EmittedLabel = false;
    auto EmitLabel = [&](va_t Addr) {
      // C allows one definition per label, even when a tail is printed twice.
      if (!IsGotoTarget(Addr) || Addr == LastLabel ||
          !EmittedLabels.insert(Addr).second)
        return;
      OS << "L_" + llvm::utohexstr(Addr) + ":\n";
      LastLabel = Addr;
      EmittedLabel = true;
    };
    EmitLabel(S.Addr);
    if (EmittedLabel && (Analysis.DeadStmts.count(&S) || stmtHiddenFromC(S))) {
      emitIndent(Indent);
      OS << ";\n";
      continue;
    }
    if (stmtHiddenFromC(S))
      continue;
    if (InCxxCleanupBody && S.Kind == StmtKind::Return)
      continue;
    if (InCxxCleanupBody && S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi)) {
      const HighStmt *Next = nullptr;
      for (size_t K = I + 1; K < End; ++K) {
        if (stmtHiddenFromC(Stmts[K]) || Stmts[K].Kind == StmtKind::Nop)
          continue;
        Next = &Stmts[K];
        break;
      }
      const HighExpr *Call = nullptr;
      if (Next && Next->Kind == StmtKind::Call && Next->CallExpr)
        Call = Next->CallExpr.get();
      else if (Next && Next->Val && Next->Val->Kind == ExprKind::Call)
        Call = Next->Val.get();
      if (Call && !Call->Operands.empty() && Call->Operands[0] &&
          (Call->Operands[0]->Kind == ExprKind::Var ||
           Call->Operands[0]->Kind == ExprKind::Phi) &&
          varName(Call->Operands[0]->Var) == varName(S.Dst->Var)) {
        if (auto Src = copyForwardSource(*S.Val)) {
          CopyForward[varName(S.Dst->Var)] = *Src;
          Analysis.DeadVars.insert(varName(S.Dst->Var));
          continue;
        }
      }
    }
    if (InCxxCleanupBody && S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        S.Dst->Kind == ExprKind::Var && S.Val->Kind == ExprKind::Call) {
      const HighStmt *Ret = nullptr;
      for (size_t K = I + 1; K < End; ++K) {
        if (stmtHiddenFromC(Stmts[K]) || Stmts[K].Kind == StmtKind::Nop)
          continue;
        if (Stmts[K].Kind == StmtKind::Return)
          Ret = &Stmts[K];
        break;
      }
      if (Ret) {
        emitIndent(Indent);
        OS << exprStr(*S.Val) << ";\n";
        AfterNoReturn = isNoreturnCallExpr(*S.Val);
        continue;
      }
    }
    if (InEHClauseBody && !InferredVoid) {
      if (const HighExpr *Stored = parentFrameStoredValue(S)) {
        size_t J = I + 1;
        while (J < End &&
               (stmtHiddenFromC(Stmts[J]) || Stmts[J].Kind == StmtKind::Nop))
          ++J;
        if (J < End && Stmts[J].Kind == StmtKind::Return &&
            Stmts[J].CxxContinuationReturnTargets.empty()) {
          if (auto Slot = namedFrameSlot(S.Kind == StmtKind::Store
                                             ? *S.StoreAddr
                                             : *S.Dst->Operands[0]))
            Analysis.DeadVars.insert(*Slot);
          emitIndent(Indent);
          OS << "return " << formatReturnExpr(*Stored) << ";\n";
          I = J;
          AfterNoReturn = false;
          continue;
        }
      }
    }
    size_t ForLast = I;
    if (tryWriteCursorForLoop(Stmts, I, End, Indent, ForLast)) {
      AfterNoReturn = Stmts[ForLast].Kind == StmtKind::Return;
      I = ForLast;
      continue;
    }
    if (InEHClauseBody && S.Kind == StmtKind::Return &&
        S.CxxContinuationReturnTargets.empty() && S.RetVal &&
        S.RetVal->Kind == ExprKind::Var) {
      const std::string Name = varName(S.RetVal->Var);
      bool AssignedHere = false;
      for (size_t K = 0; K < I; ++K) {
        const HighStmt &Prev = Stmts[K];
        if (Prev.Kind != StmtKind::Assign || !Prev.Dst)
          continue;
        if (Prev.Dst->Kind != ExprKind::Var && Prev.Dst->Kind != ExprKind::Phi)
          continue;
        if (varName(Prev.Dst->Var) == Name)
          AssignedHere = true;
      }
      if (!AssignedHere) {
        emitIndent(Indent);
        OS << "return;\n";
        continue;
      }
    }
    NextTryLeaveTargets.clear();
    if (S.Kind == StmtKind::SEHTry)
      NextTryLeaveTargets = tryLeaveTargets(Stmts, I);
    writeStmt(S, Indent);
    AfterNoReturn =
        isNoReturnCallStmt(S) ||
        stmtAlwaysExit(
            [this](const HighStmt &Stmt) { return stmtHiddenFromC(Stmt); },
            [this](const HighStmt &Exit) { return isFallthroughTryExit(Exit); },
            S);
  }
}

void HighCWriter::writeStmtsIsolated(const std::vector<HighStmt> &Stmts,
                                     int Indent) {
  const auto SavedPtrs = ReachingCatchPtrs;
  const auto SavedFields = ReachingCatchFields;
  writeStmts(Stmts, Indent);
  ReachingCatchPtrs = SavedPtrs;
  ReachingCatchFields = SavedFields;
}

void HighCWriter::writeTryBody(const std::vector<HighStmt> &Stmts, int Indent) {
  const auto SavedPtrs = ReachingCatchPtrs;
  const auto SavedFields = ReachingCatchFields;
  writeTryBodyUnisolated(Stmts, Indent);
  ReachingCatchPtrs = SavedPtrs;
  ReachingCatchFields = SavedFields;
}

void HighCWriter::writeTryBodyUnisolated(const std::vector<HighStmt> &Stmts,
                                         int Indent) {
  // Jumping out of __try is legal, so only a goto that lands where the try
  // falls through anyway is left out.
  writeStmts(
      Stmts, Indent,
      printedTryBodyEnd(
          [this](const HighStmt &Stmt) { return stmtHiddenFromC(Stmt); },
          [this](const HighStmt &Exit) { return isFallthroughTryExit(Exit); },
          Stmts));
}

bool HighCWriter::isLabelAddress(va_t Addr) const {
  return Addr != 0 && Addr != InvalidVA &&
         (GotoTargetUses.count(Addr) || GotoTargets.count(Addr));
}

bool HighCWriter::isFallthroughTryExit(const HighStmt &Stmt) const {
  return Stmt.Kind == StmtKind::Goto && !isLabelAddress(Stmt.Addr) &&
         FallthroughTryExits.count({Stmt.Addr, Stmt.GotoTarget});
}

std::vector<va_t>
HighCWriter::fallthroughAddresses(const std::vector<HighStmt> &Stmts,
                                  size_t Index,
                                  const std::vector<va_t> &Continuation) const {
  // Empty anchors print nothing, so each of their addresses is the same point
  // as the first statement that runs.
  std::vector<va_t> Addresses;
  for (size_t J = Index + 1; J < Stmts.size(); ++J) {
    const HighStmt &Next = Stmts[J];
    if (Next.Addr != 0 && Next.Addr != InvalidVA)
      Addresses.push_back(Next.Addr);
    if (Next.Kind == StmtKind::While && Next.LoopHeaderAddr != 0)
      Addresses.push_back(Next.LoopHeaderAddr);
    if (stmtHiddenFromC(Next))
      continue;
    return Addresses;
  }
  Addresses.insert(Addresses.end(), Continuation.begin(), Continuation.end());
  return Addresses;
}

std::vector<va_t>
HighCWriter::tryLeaveTargets(const std::vector<HighStmt> &Stmts,
                             size_t Index) const {
  // A label prints at the first statement printed with its address, so one
  // printed already, or inside the try or its clauses, is not after it.
  std::set<va_t> Earlier;
  std::function<void(const HighStmt &)> Visit = [&](const HighStmt &S) {
    Earlier.insert(S.Addr);
    if (S.Kind == StmtKind::While)
      Earlier.insert(S.LoopHeaderAddr);
    for (const auto *List : {&S.Body, &S.ElseBody, &S.DefaultBody})
      for (const HighStmt &Child : *List)
        Visit(Child);
    for (const auto &Case : S.Cases)
      for (const HighStmt &Child : Case.Body)
        Visit(Child);
    for (const auto &ClauseBody : S.EHClauseBodies)
      for (const HighStmt &Child : ClauseBody)
        Visit(Child);
  };
  Visit(Stmts[Index]);
  std::vector<va_t> Targets = fallthroughAddresses(Stmts, Index, {});
  llvm::erase_if(Targets, [&](va_t Addr) {
    return Earlier.count(Addr) || EmittedLabels.count(Addr);
  });
  return Targets;
}

void HighCWriter::decideTryExits(const std::vector<HighStmt> &Stmts,
                                 const std::vector<va_t> &Continuation,
                                 std::set<std::pair<va_t, va_t>> &Kept) {
  for (size_t I = 0; I < Stmts.size(); ++I) {
    const HighStmt &S = Stmts[I];
    const std::vector<va_t> Next = fallthroughAddresses(Stmts, I, Continuation);
    const bool IsTry = S.Kind == StmtKind::SEHTry ||
                       S.Kind == StmtKind::CxxTry ||
                       S.Kind == StmtKind::ItaniumTry;
    auto RecordExits = [&](const std::vector<HighStmt> &Body) {
      // The first goto of the trailing run is the one that runs.  It can be
      // left out only when it lands where the try falls through anyway; a
      // copy of it anywhere else that cannot stays printed with its label.
      std::vector<const HighStmt *> Run;
      for (size_t K = Body.size(); K > 0; --K) {
        const HighStmt &Last = Body[K - 1];
        if (stmtHiddenFromC(Last))
          continue;
        if (Last.Kind != StmtKind::Goto || isLabelAddress(Last.Addr))
          break;
        Run.push_back(&Last);
      }
      const bool FallsThrough =
          !Run.empty() && llvm::is_contained(Next, Run.back()->GotoTarget);
      for (const HighStmt *Exit : Run)
        (FallsThrough ? FallthroughTryExits : Kept)
            .insert({Exit->Addr, Exit->GotoTarget});
    };
    if (IsTry)
      RecordExits(S.Body);
    // An except clause resumes after its try. A finally clause can also run
    // during unwinding, so its explicit transfers cannot borrow that exit.
    if (hasFallthroughExceptClause(S))
      RecordExits(S.EHClauseBodies.front());
    // A loop body ends by returning to its header; other arms end where
    // their statement does.
    std::vector<va_t> BodyEnd = Next;
    if (S.Kind == StmtKind::DoWhile || S.Kind == StmtKind::For)
      BodyEnd.clear();
    else if (S.Kind == StmtKind::While)
      BodyEnd.assign(S.LoopHeaderAddr ? 1 : 0, S.LoopHeaderAddr);
    decideTryExits(S.Body, BodyEnd, Kept);
    decideTryExits(S.ElseBody, Next, Kept);
    for (const auto &Case : S.Cases)
      decideTryExits(Case.Body, Next, Kept);
    decideTryExits(S.DefaultBody, Next, Kept);
    for (const auto &ClauseBody : S.EHClauseBodies)
      decideTryExits(ClauseBody, Next, Kept);
  }
}

bool HighCWriter::isCompilerEHConstant(const HighExpr &Val) const {
  if (Val.Kind != ExprKind::Const || !CurrentFunc ||
      !CurrentFunc->ExceptionMetadata)
    return false;
  const ExceptionFunction &EH = *CurrentFunc->ExceptionMetadata;
  const uint64_t C = Val.ConstVal;
  if (!EH.Registration)
    return false;
  const RegistrationChainInfo &Reg = *EH.Registration;
  if (Reg.HandlerVA && C == Reg.HandlerVA)
    return true;
  if (Reg.ScopeTableVA && C == Reg.ScopeTableVA)
    return true;
  if (Reg.SeededTryLevel && static_cast<int32_t>(C) == *Reg.SeededTryLevel)
    return true;
  for (const RegistrationTryLevelStore &Store : Reg.TryLevelStores)
    if (Store.Level < 0 && static_cast<int32_t>(C) == Store.Level)
      return true;
  return false;
}

void HighCWriter::collectCopyForward(const HighFunc &Func) {
  // Forwarding a copy renames every use of its destination.  That is only
  // the same value when the destination is assigned once and its source is
  // never reassigned; a loop-carried variable (`v = arg0; ... v = next;`)
  // has several definitions and must keep its own name.
  std::map<std::string, unsigned> Definitions;
  std::map<std::string, unsigned> SlotStores;
  std::function<void(const std::vector<HighStmt> &)> CountDefinitions =
      [&](const std::vector<HighStmt> &Stmts) {
        for (const HighStmt &Stmt : Stmts) {
          if (Stmt.Kind == StmtKind::Assign && Stmt.Dst &&
              (Stmt.Dst->Kind == ExprKind::Var ||
               Stmt.Dst->Kind == ExprKind::Phi))
            ++Definitions[varName(Stmt.Dst->Var)];
          // A named frame slot stored on two paths (outgoing arguments of
          // two calls) is not one copy either.
          const HighExpr *SlotAddr = nullptr;
          if (Stmt.Kind == StmtKind::Assign && Stmt.Dst &&
              Stmt.Dst->Kind == ExprKind::Load && !Stmt.Dst->Operands.empty())
            SlotAddr = Stmt.Dst->Operands[0].get();
          else if (Stmt.Kind == StmtKind::Store)
            SlotAddr = Stmt.StoreAddr.get();
          if (SlotAddr)
            if (auto Slot = namedFrameSlot(*SlotAddr))
              ++SlotStores[*Slot];
          CountDefinitions(Stmt.Body);
          CountDefinitions(Stmt.ElseBody);
          for (const auto &C : Stmt.Cases)
            CountDefinitions(C.Body);
          CountDefinitions(Stmt.DefaultBody);
          for (const auto &ClauseBody : Stmt.EHClauseBodies)
            CountDefinitions(ClauseBody);
        }
      };
  CountDefinitions(Func.Body);
  auto DefinitionCount = [&](const std::string &Name) {
    auto It = Definitions.find(Name);
    return It == Definitions.end() ? 0u : It->second;
  };

  std::set<std::string> SeenLoad;
  std::map<std::string, std::vector<TypeRef>> SlotLoadTypes;
  std::map<std::string, std::vector<TypeRef>> SlotStoreTypes;
  std::function<void(const std::vector<HighStmt> &, bool, bool)> Walk;
  Walk = [&](const std::vector<HighStmt> &Stmts, bool InHandler, bool Nested) {
    const bool Saved = InEHClauseBody;
    InEHClauseBody = InHandler;
    for (const HighStmt &Stmt : Stmts) {
      auto NoteLoads = [&](const ExprPtr &E) {
        if (!E)
          return;
        std::function<void(const HighExpr &)> Rec = [&](const HighExpr &N) {
          if (N.Kind == ExprKind::Load && !N.Operands.empty() &&
              N.Operands[0]) {
            if (auto Slot = namedFrameSlot(*N.Operands[0])) {
              SeenLoad.insert(*Slot);
              SlotLoadTypes[*Slot].push_back(N.Type);
            }
          }
          for (const ExprPtr &Op : N.Operands)
            if (Op)
              Rec(*Op);
        };
        Rec(*E);
      };
      forEachRhsExpr(Stmt, NoteLoads);

      auto ApplyNamedSlot = [&](const std::optional<std::string> &Slot,
                                const HighExpr &Val) {
        if (!Slot)
          return;
        SlotStoreTypes[*Slot].push_back(Val.Type);
        // Pointer/float homes are C objects.  Copy-forwarding them through an
        // integer load would drop a bitcast (`return arg0` for float→int).
        if (Nested || SharedFrameStorage.count(*Slot) ||
            SeenLoad.count(*Slot) || SlotStores[*Slot] != 1 ||
            isAddressTakenSlot(*Slot) || !isParamCopy(Val) || !Val.Type ||
            Val.Type->Kind != NdTypeKind::Int) {
          CopyForward.erase(*Slot);
          Analysis.DeadVars.erase(*Slot);
          return;
        }
        if (auto Src = copyForwardSource(Val)) {
          CopyForward[*Slot] = *Src;
          Analysis.DeadVars.insert(*Slot);
        } else {
          CopyForward.erase(*Slot);
          Analysis.DeadVars.erase(*Slot);
        }
      };

      if (Stmt.Kind == StmtKind::Assign && Stmt.Dst && Stmt.Val) {
        if (Stmt.Dst->Kind == ExprKind::Load && !Stmt.Dst->Operands.empty() &&
            Stmt.Dst->MemoryOrdering == NdMemoryOrdering::None &&
            Stmt.Dst->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
          ApplyNamedSlot(namedFrameSlot(*Stmt.Dst->Operands[0]), *Stmt.Val);
        } else if (!Analysis.DeadStmts.count(&Stmt) && !Stmt.IsPhiCopy &&
                   (Stmt.Dst->Kind == ExprKind::Var ||
                    Stmt.Dst->Kind == ExprKind::Phi) &&
                   isCopyForwardDestination(Stmt.Dst->Var)) {
          const std::string DstName = varName(Stmt.Dst->Var);
          // Same-type loads of a named slot (`t = arg0` via the incoming
          // home) are copies.  A pointer slot loaded as an integer still
          // keeps the destination so the carrier cast stays visible.
          std::optional<std::string> Src;
          if (Stmt.Val->Kind == ExprKind::Load) {
            if (Stmt.Dst->Type && Stmt.Val->Type &&
                Stmt.Dst->Type->Kind == Stmt.Val->Type->Kind &&
                Stmt.Dst->Type->Size == Stmt.Val->Type->Size) {
              if (auto Cand = copyForwardSource(*Stmt.Val);
                  Cand && isEmittedParamName(*Cand))
                Src = Cand;
            }
          } else {
            Src = copyForwardSource(*Stmt.Val);
            if (Src && Stmt.Dst->Type && Stmt.Val->Type &&
                (Stmt.Dst->Type->Kind != Stmt.Val->Type->Kind ||
                 Stmt.Dst->Type->Size != Stmt.Val->Type->Size))
              Src.reset();
          }
          if (Src &&
              (DefinitionCount(DstName) != 1 || DefinitionCount(*Src) > 1))
            Src.reset();
          if (Src) {
            CopyForward[DstName] = *Src;
            Analysis.DeadVars.insert(DstName);
          } else {
            CopyForward.erase(DstName);
            Analysis.DeadVars.erase(DstName);
          }
        }
      } else if (Stmt.Kind == StmtKind::Store && Stmt.StoreAddr &&
                 Stmt.StoreVal &&
                 Stmt.MemoryOrdering == NdMemoryOrdering::None &&
                 Stmt.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        ApplyNamedSlot(namedFrameSlot(*Stmt.StoreAddr), *Stmt.StoreVal);
      }

      const bool ChildNested =
          Nested || Stmt.Kind == StmtKind::If ||
          Stmt.Kind == StmtKind::IfElse || Stmt.Kind == StmtKind::While ||
          Stmt.Kind == StmtKind::DoWhile || Stmt.Kind == StmtKind::For ||
          Stmt.Kind == StmtKind::Switch || Stmt.Kind == StmtKind::SEHTry ||
          Stmt.Kind == StmtKind::CxxTry || Stmt.Kind == StmtKind::ItaniumTry;
      Walk(Stmt.Body, InHandler, ChildNested);
      Walk(Stmt.ElseBody, InHandler, ChildNested);
      for (const auto &C : Stmt.Cases)
        Walk(C.Body, InHandler, ChildNested);
      Walk(Stmt.DefaultBody, InHandler, ChildNested);
      for (const auto &ClauseBody : Stmt.EHClauseBodies)
        Walk(ClauseBody, true, true);
    }
    InEHClauseBody = Saved;
  };
  Walk(Func.Body, false, false);
  auto SlotCompatibleInteger = [](const std::vector<TypeRef> &Types,
                                  uint16_t &Size) {
    for (const TypeRef &Ty : Types) {
      if (!Ty || Ty->Kind != NdTypeKind::Int)
        return false;
      if (Size == 0)
        Size = Ty->Size;
      else if (Ty->Size != Size)
        return false;
    }
    return true;
  };
  for (const auto &[Slot, Types] : SlotLoadTypes) {
    if (!CopyForward.count(Slot))
      continue;
    uint16_t Size = 0;
    bool Compatible = SlotCompatibleInteger(Types, Size);
    if (Compatible) {
      if (auto It = SlotStoreTypes.find(Slot); It != SlotStoreTypes.end())
        Compatible = SlotCompatibleInteger(It->second, Size);
    }
    if (!Compatible) {
      CopyForward.erase(Slot);
      Analysis.DeadVars.erase(Slot);
    }
  }

  // A projected frame slot can only disappear when every remaining use reads
  // its value.  Taking the slot's address needs the actual C object and its
  // initializing store; substituting loads alone would otherwise leave an
  // uninitialized (or, before declaration recovery, undeclared) var_mXX.
  auto InvalidateName = [&](const std::string &Name) {
    CopyForward.erase(Name);
    Analysis.DeadVars.erase(Name);
  };
  std::function<void(const HighExpr &, bool)> FindEscapes;
  FindEscapes = [&](const HighExpr &Expr, bool IsMemoryAddress) {
    if (Expr.Kind == ExprKind::Addr && !Expr.Operands.empty() &&
        Expr.Operands[0]) {
      const HighExpr &Operand = *Expr.Operands[0];
      if (Operand.Kind == ExprKind::Load && !Operand.Operands.empty() &&
          Operand.Operands[0]) {
        if (auto Slot = namedFrameSlot(*Operand.Operands[0]))
          InvalidateName(*Slot);
      } else if (Operand.Kind == ExprKind::Var ||
                 Operand.Kind == ExprKind::Phi) {
        InvalidateName(varName(Operand.Var));
      }
      return;
    }
    if (auto Slot = namedFrameSlot(Expr)) {
      if (!IsMemoryAddress)
        InvalidateName(*Slot);
      return;
    }
    if (Expr.Kind == ExprKind::Load && !Expr.Operands.empty()) {
      if (Expr.Operands[0])
        FindEscapes(*Expr.Operands[0], true);
      for (size_t I = 1; I < Expr.Operands.size(); ++I)
        if (Expr.Operands[I])
          FindEscapes(*Expr.Operands[I], false);
      return;
    }
    if (Expr.Kind == ExprKind::Store && !Expr.Operands.empty()) {
      if (Expr.Operands[0])
        FindEscapes(*Expr.Operands[0], true);
      for (size_t I = 1; I < Expr.Operands.size(); ++I)
        if (Expr.Operands[I])
          FindEscapes(*Expr.Operands[I], false);
      return;
    }
    for (const ExprPtr &Operand : Expr.Operands)
      if (Operand)
        FindEscapes(*Operand, false);
  };
  walkStmts(Func.Body, [&](const HighStmt &Stmt) {
    if (Analysis.DeadStmts.count(&Stmt))
      return;
    if (Stmt.Kind == StmtKind::Assign && Stmt.Dst &&
        Stmt.Dst->Kind == ExprKind::Load && !Stmt.Dst->Operands.empty() &&
        Stmt.Dst->Operands[0])
      FindEscapes(*Stmt.Dst->Operands[0], true);
    if (Stmt.Val)
      FindEscapes(*Stmt.Val, false);
    if (Stmt.Cond)
      FindEscapes(*Stmt.Cond, false);
    if (Stmt.RetVal)
      FindEscapes(*Stmt.RetVal, false);
    if (Stmt.StoreAddr)
      FindEscapes(*Stmt.StoreAddr, true);
    if (Stmt.StoreVal)
      FindEscapes(*Stmt.StoreVal, false);
    if (Stmt.CallExpr)
      FindEscapes(*Stmt.CallExpr, false);
    if (Stmt.SwitchExpr)
      FindEscapes(*Stmt.SwitchExpr, false);
  });
  // `if (p) v = member; else v = 0` is a join, not a copy. Other
  // multi-assigns (GS return PHI, unknown-only zeros) still forward.
  std::map<std::string, unsigned> Assigns;
  std::set<std::string> HasZero;
  std::set<std::string> FieldLike;
  std::vector<std::pair<std::string, std::string>> Copies;
  walkStmts(Func.Body, [&](const HighStmt &Stmt) {
    if (Stmt.Kind != StmtKind::Assign || !Stmt.Dst || !Stmt.Val)
      return;
    if (Stmt.Dst->Kind != ExprKind::Var && Stmt.Dst->Kind != ExprKind::Phi)
      return;
    const std::string Name = varName(Stmt.Dst->Var);
    if (Name.empty())
      return;
    ++Assigns[Name];
    const HighExpr *Val = peelIntegerViewOps(Stmt.Val.get());
    if (!Val)
      return;
    if (Val->Kind == ExprKind::Const && Val->ConstVal == 0)
      HasZero.insert(Name);
    if (Val->Kind == ExprKind::Load && !Val->Operands.empty() &&
        Val->Operands[0] && typedMemberAccess(*Val->Operands[0]))
      FieldLike.insert(Name);
    if (Val->Kind == ExprKind::Var || Val->Kind == ExprKind::Phi) {
      const std::string Src = varName(Val->Var);
      if (!Src.empty())
        Copies.emplace_back(Name, Src);
    }
  });
  bool Grew = true;
  while (Grew) {
    Grew = false;
    for (const auto &[Dest, Src] : Copies) {
      if (FieldLike.count(Src) && FieldLike.insert(Dest).second)
        Grew = true;
    }
  }
  JoinPhiNames.clear();
  for (const auto &[Name, Count] : Assigns)
    if (Count > 1 && HasZero.count(Name) && FieldLike.count(Name))
      JoinPhiNames.insert(Name);
  for (auto It = CopyForward.begin(); It != CopyForward.end();) {
    if (JoinPhiNames.count(It->first)) {
      Analysis.DeadVars.erase(It->first);
      It = CopyForward.erase(It);
    } else {
      ++It;
    }
  }
}

bool HighCWriter::samePeeledAddr(const HighExpr &A, const HighExpr &B) const {
  const HighExpr *PA = peelIntegerViewOps(&A);
  const HighExpr *PB = peelIntegerViewOps(&B);
  if (!PA)
    PA = &A;
  if (!PB)
    PB = &B;
  return PA->structuralEq(*PB);
}

const HighExpr *HighCWriter::asIncrementBase(const HighExpr &Val,
                                             int64_t &Delta) const {
  const HighExpr *E = peelIntegerViewOps(&Val);
  if (!E || E->Kind != ExprKind::BinOp || E->Operands.size() != 2 ||
      (E->Op != NdOp::INT_ADD && E->Op != NdOp::INT_SUB))
    return nullptr;
  const HighExpr *Lhs = peelIntegerViewOps(E->Operands[0].get());
  const HighExpr *Rhs = peelIntegerViewOps(E->Operands[1].get());
  if (!Lhs)
    Lhs = E->Operands[0].get();
  if (!Rhs)
    Rhs = E->Operands[1].get();
  if (!Lhs || !Rhs)
    return nullptr;
  const bool Minus = E->Op == NdOp::INT_SUB;
  const HighExpr *Base = nullptr;
  const HighExpr *Off = nullptr;
  if (Rhs->Kind == ExprKind::Const) {
    Base = Lhs;
    Off = Rhs;
  } else if (!Minus && Lhs->Kind == ExprKind::Const) {
    Base = Rhs;
    Off = Lhs;
  }
  if (!Base || !Off)
    return nullptr;
  uint16_t Size = Off->Type ? Off->Type->Size : (Val.Type ? Val.Type->Size : 4);
  if (Size == 0 || Size > 8)
    Size = 4;
  const unsigned Bits = static_cast<unsigned>(Size) * 8;
  const uint64_t Mask = Bits == 64 ? ~uint64_t{0} : ((uint64_t{1} << Bits) - 1);
  uint64_t Imm = Off->ConstVal & Mask;
  if (Bits < 64 && (Imm & (uint64_t{1} << (Bits - 1))))
    Delta = static_cast<int64_t>(Imm | ~Mask);
  else
    Delta = static_cast<int64_t>(Imm);
  if (Delta == 0 || Delta == std::numeric_limits<int64_t>::min())
    return nullptr;
  if (Minus)
    Delta = -Delta;
  const uint64_t Abs =
      Delta > 0 ? static_cast<uint64_t>(Delta) : static_cast<uint64_t>(-Delta);
  if (Abs > 0xFFFFu)
    return nullptr;
  return Base;
}

bool HighCWriter::incrementBaseMatchesAddr(const HighExpr &Base,
                                           const HighExpr &Addr) const {
  const HighExpr *B = peelIntegerViewOps(&Base);
  if (!B)
    B = &Base;
  if (B->Kind == ExprKind::Load && !B->Operands.empty() && B->Operands[0])
    return B->Operands[0]->structuralEq(Addr);
  if (B->Kind != ExprKind::Var && B->Kind != ExprKind::Phi)
    return false;
  auto MatchesLoad = [&](const HighExpr *E) {
    return E && E->Kind == ExprKind::Load && E->Type && B->Type &&
           E->Type->Size == B->Type->Size && !E->Operands.empty() &&
           E->Operands[0] && E->Operands[0]->structuralEq(Addr);
  };
  const std::string Name = varName(B->Var);
  auto It = ValueForward.find(Name);
  if (It != ValueForward.end() && It->second && MatchesLoad(It->second))
    return true;
  if (!CurrentFunc)
    return false;
  // Every increment store asks this; index the function's assignments once.
  if (!AssignedValuesIndexed) {
    AssignedValuesIndexed = true;
    walkStmts(CurrentFunc->Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
        return;
      if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
        return;
      AssignedValuesByName[varName(S.Dst->Var)].push_back(S.Val.get());
    });
  }
  auto Assigned = AssignedValuesByName.find(Name);
  if (Assigned == AssignedValuesByName.end())
    return false;
  return llvm::any_of(Assigned->second, MatchesLoad);
}

const HighExpr *HighCWriter::asAndWithConst(const HighExpr &Val,
                                            uint64_t &Mask) const {
  const HighExpr *E = peelIntegerViewOps(&Val);
  if (!E || E->Kind != ExprKind::BinOp || E->Op != NdOp::INT_AND ||
      E->Operands.size() != 2)
    return nullptr;
  const HighExpr *L = peelIntegerViewOps(E->Operands[0].get());
  const HighExpr *R = peelIntegerViewOps(E->Operands[1].get());
  if (!L)
    L = E->Operands[0].get();
  if (!R)
    R = E->Operands[1].get();
  if (R && R->Kind == ExprKind::Const) {
    Mask = R->ConstVal;
    return L;
  }
  if (L && L->Kind == ExprKind::Const) {
    Mask = L->ConstVal;
    return R;
  }
  return nullptr;
}

std::string HighCWriter::formatBitAndMask(uint64_t Mask, unsigned Size) const {
  unsigned Bits = Size ? static_cast<unsigned>(Size) * 8 : 32;
  if (Bits > 64)
    Bits = 64;
  const uint64_t Width =
      Bits == 64 ? ~uint64_t(0) : ((uint64_t(1) << Bits) - 1);
  Mask &= Width;
  const uint64_t Cleared = (~Mask) & Width;
  if (Cleared && (Cleared & (Cleared - 1)) == 0 && Mask == (Width ^ Cleared))
    return "~0x" + llvm::utohexstr(Cleared);
  int64_t Signed = static_cast<int64_t>(Mask);
  if (Bits < 64 && (Mask & (uint64_t(1) << (Bits - 1))))
    Signed = static_cast<int64_t>(Mask | ~Width);
  return std::to_string(Signed);
}

std::string HighCWriter::formatInplaceAnd(const std::string &Dest,
                                          uint64_t Mask, unsigned Size) const {
  return Dest + " &= " + formatBitAndMask(Mask, Size);
}

bool HighCWriter::isInplaceAndStore(const HighStmt &S, uint64_t &Mask) const {
  if (S.Kind != StmtKind::Store || !S.StoreAddr || !S.StoreVal)
    return false;
  if (S.MemoryOrdering != NdMemoryOrdering::None ||
      S.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const HighExpr *Base = asAndWithConst(*S.StoreVal, Mask);
  if (!Base)
    return false;
  const HighExpr *P = peelIntegerViewOps(Base);
  if (!P)
    P = Base;
  if (P->Kind == ExprKind::Load && !P->Operands.empty() && P->Operands[0])
    return samePeeledAddr(*P->Operands[0], *S.StoreAddr);
  if (P->Kind == ExprKind::Var || P->Kind == ExprKind::Phi)
    if (auto Slot = namedFrameSlot(*S.StoreAddr))
      return copyForwardName(varName(P->Var)) == *Slot;
  return false;
}

bool HighCWriter::isInplaceAddStore(const HighStmt &S, int64_t &Delta) const {
  if (S.Kind != StmtKind::Store || !S.StoreAddr || !S.StoreVal)
    return false;
  if (S.MemoryOrdering != NdMemoryOrdering::None ||
      S.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const TypeRef &Ty = S.StoreVal->Type;
  // A signed compound assignment can overflow. It is safe to abbreviate a
  // signed IR update only when the actual displayed lvalue is an unsigned
  // field or named slot of the same width.
  if (!Ty || Ty->Kind != NdTypeKind::Int || Ty->IsEnum)
    return false;
  if (Ty->Size != 1 && Ty->Size != 2 && Ty->Size != 4 && Ty->Size != 8)
    return false;
  // A displayed field/slot may have a different signedness from its IR view.
  // Both load hiding and statement rendering must make the same decision.
  TypeRef DisplayType = typedMemberType(*S.StoreAddr, Ty->Size);
  if (auto Disp = frameDisplacement(*S.StoreAddr)) {
    auto Slot = FrameSlots.find(*Disp);
    if (Slot != FrameSlots.end())
      DisplayType = Slot->second.Type;
  }
  if (DisplayType &&
      (DisplayType->Kind != NdTypeKind::Int || DisplayType->IsSigned ||
       DisplayType->IsEnum || DisplayType->Size != Ty->Size))
    return false;
  if (Ty->IsSigned &&
      (!DisplayType || (!namedFrameSlot(*S.StoreAddr) &&
                        !typedMemberAccess(*S.StoreAddr, Ty->Size))))
    return false;
  // Peeling a narrowing/widening view or a narrow signed immediate changes
  // the update's value. Only abbreviate an exact same-width arithmetic step.
  const auto *Arithmetic = peelIntegerViewOps(S.StoreVal.get());
  if (Arithmetic != S.StoreVal.get() || !Arithmetic->Type ||
      Arithmetic->Type->Size != Ty->Size || Arithmetic->Operands.size() != 2)
    return false;
  for (const auto &Operand : Arithmetic->Operands)
    if (!Operand || !Operand->Type || Operand->Type->Kind != NdTypeKind::Int ||
        Operand->Type->Size != Ty->Size ||
        peelIntegerViewOps(Operand.get()) != Operand.get())
      return false;
  const HighExpr *Base = asIncrementBase(*S.StoreVal, Delta);
  return Base && incrementBaseMatchesAddr(*Base, *S.StoreAddr);
}

std::string HighCWriter::formatInplaceAdd(const TypeRef &Ty,
                                          const HighExpr &Addr, int64_t Delta) {
  if (Delta == 0 || Delta == std::numeric_limits<int64_t>::min())
    return {};
  const char *Op = Delta > 0 ? " += " : " -= ";
  const uint64_t Abs =
      Delta > 0 ? static_cast<uint64_t>(Delta) : static_cast<uint64_t>(-Delta);
  const std::string Imm = std::to_string(Abs);
  if (auto Member = typedMemberAccess(Addr, Ty ? Ty->Size : 0);
      Member && !namedFrameSlot(Addr))
    return *Member + Op + Imm;
  if (auto Slot = namedFrameSlot(Addr))
    return *Slot + Op + Imm;
  if (!Ty)
    return {};
  return "*(" + memoryTypeName(Ty) + " *)(" + addrStr(Addr) + ")" + Op + Imm;
}

void HighCWriter::hideIncrementOnlyLoads(const HighFunc &Func) {
  auto UsesName = [&](const HighExpr *E, const std::string &Name) -> bool {
    std::function<bool(const HighExpr *)> Walk = [&](const HighExpr *N) {
      if (!N)
        return false;
      if ((N->Kind == ExprKind::Var || N->Kind == ExprKind::Phi) &&
          varName(N->Var) == Name)
        return true;
      bool Hit = false;
      N->forEachChildExpr([&](const ExprPtr &C) {
        if (Walk(C.get()))
          Hit = true;
      });
      return Hit;
    };
    return Walk(E);
  };
  // Only a load whose variable is the base of some in-place add can be
  // hidden.  Find those names once rather than rescanning the body for every
  // load assignment.
  std::set<std::string> IncrementBaseNames;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (Analysis.DeadStmts.count(&S))
      return;
    int64_t Delta = 0;
    if (!isInplaceAddStore(S, Delta))
      return;
    const HighExpr *Base = asIncrementBase(*S.StoreVal, Delta);
    if (Base && (Base->Kind == ExprKind::Var || Base->Kind == ExprKind::Phi))
      IncrementBaseNames.insert(varName(Base->Var));
  });
  if (IncrementBaseNames.empty())
    return;
  walkStmts(Func.Body, [&](const HighStmt &Assign) {
    if (Analysis.DeadStmts.count(&Assign))
      return;
    if (Assign.Kind != StmtKind::Assign || !Assign.Dst || !Assign.Val)
      return;
    if (Assign.Dst->Kind != ExprKind::Var && Assign.Dst->Kind != ExprKind::Phi)
      return;
    if (Assign.Val->Kind != ExprKind::Load || Assign.Val->Operands.empty() ||
        !Assign.Val->Operands[0])
      return;
    if (Assign.Val->MemoryOrdering != NdMemoryOrdering::None)
      return;
    const std::string Name = varName(Assign.Dst->Var);
    if (Name.empty() || isEmittedParamName(Name) ||
        !IncrementBaseNames.count(Name))
      return;
    unsigned IncrementStores = 0;
    bool OtherUse = false;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (&S == &Assign || Analysis.DeadStmts.count(&S))
        return;
      int64_t Delta = 0;
      if (isInplaceAddStore(S, Delta)) {
        const HighExpr *Base = asIncrementBase(*S.StoreVal, Delta);
        if (Base &&
            (Base->Kind == ExprKind::Var || Base->Kind == ExprKind::Phi) &&
            varName(Base->Var) == Name &&
            samePeeledAddr(*Assign.Val->Operands[0], *S.StoreAddr)) {
          ++IncrementStores;
          return;
        }
      }
      bool Hit = false;
      forEachRhsExpr(S, [&](const ExprPtr &E) {
        if (UsesName(E.get(), Name))
          Hit = true;
      });
      if (S.Kind == StmtKind::Assign && S.Dst && UsesName(S.Dst.get(), Name))
        Hit = true;
      if (Hit)
        OtherUse = true;
    });
    if (!OtherUse && IncrementStores > 0) {
      Analysis.DeadStmts.insert(&Assign);
      Analysis.DeadVars.insert(Name);
    }
  });
}

void HighCWriter::hideBitClearSlotCopies(const HighFunc &Func) {
  auto UsesName = [&](const HighExpr *E, const std::string &Name) -> bool {
    std::function<bool(const HighExpr *)> Walk = [&](const HighExpr *N) {
      if (!N)
        return false;
      if ((N->Kind == ExprKind::Var || N->Kind == ExprKind::Phi) &&
          varName(N->Var) == Name)
        return true;
      bool Hit = false;
      N->forEachChildExpr([&](const ExprPtr &C) {
        if (Walk(C.get()))
          Hit = true;
      });
      return Hit;
    };
    return Walk(E);
  };
  auto SlotOfCopy = [&](const HighExpr &E) -> std::optional<std::string> {
    const HighExpr *P = peelIntegerViewOps(&E);
    if (!P)
      return std::nullopt;
    if (P->Kind == ExprKind::Load && !P->Operands.empty() && P->Operands[0] &&
        P->MemoryOrdering == NdMemoryOrdering::None)
      return namedFrameSlot(*P->Operands[0]);
    if (P->Kind != ExprKind::Var && P->Kind != ExprKind::Phi)
      return std::nullopt;
    const std::string N = varName(P->Var);
    for (const auto &[Disp, Slot] : FrameSlots)
      if (Slot.Name == N)
        return N;
    if (auto It = FrameAliases.find(N); It != FrameAliases.end())
      if (auto Slot = FrameSlots.find(It->second); Slot != FrameSlots.end())
        return Slot->second.Name;
    return std::nullopt;
  };
  auto AndBase = [&](const HighExpr &Val) -> const HighExpr * {
    const HighExpr *E = peelIntegerViewOps(&Val);
    if (!E || E->Kind != ExprKind::BinOp || E->Op != NdOp::INT_AND ||
        E->Operands.size() != 2)
      return nullptr;
    const HighExpr *L = peelIntegerViewOps(E->Operands[0].get());
    const HighExpr *R = peelIntegerViewOps(E->Operands[1].get());
    if (!L)
      L = E->Operands[0].get();
    if (!R)
      R = E->Operands[1].get();
    if (R && R->Kind == ExprKind::Const)
      return L;
    if (L && L->Kind == ExprKind::Const)
      return R;
    return nullptr;
  };
  auto StoresBitClear = [&](const HighStmt &S, const std::string &Name,
                            const std::string &Slot) {
    const HighExpr *Val = nullptr;
    std::optional<std::string> Dest;
    if (S.Kind == StmtKind::Store && S.StoreAddr && S.StoreVal) {
      Dest = namedFrameSlot(*S.StoreAddr);
      Val = S.StoreVal.get();
    } else if (S.Kind == StmtKind::Assign && S.Dst && S.Val) {
      if (S.Dst->Kind == ExprKind::Load && !S.Dst->Operands.empty() &&
          S.Dst->Operands[0])
        Dest = namedFrameSlot(*S.Dst->Operands[0]);
      else if (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi)
        Dest = varName(S.Dst->Var);
      Val = S.Val.get();
    }
    if (!Val || !Dest || *Dest != Slot)
      return false;
    const HighExpr *Base = AndBase(*Val);
    if (!Base)
      return false;
    if ((Base->Kind == ExprKind::Var || Base->Kind == ExprKind::Phi) &&
        varName(Base->Var) == Name)
      return true;
    return SlotOfCopy(*Base) == Slot;
  };

  std::function<void(const std::vector<HighStmt> &, bool)> Walk;
  Walk = [&](const std::vector<HighStmt> &Stmts, bool Handler) {
    const bool SavedHandler = InEHClauseBody;
    const bool SavedCleanup = InCxxCleanupBody;
    InEHClauseBody = Handler;
    if (Handler)
      InCxxCleanupBody = true;
    for (const HighStmt &Assign : Stmts) {
      if (!Analysis.DeadStmts.count(&Assign) &&
          Assign.Kind == StmtKind::Assign && Assign.Dst && Assign.Val &&
          (Assign.Dst->Kind == ExprKind::Var ||
           Assign.Dst->Kind == ExprKind::Phi)) {
        const auto Slot = SlotOfCopy(*Assign.Val);
        const std::string Name = varName(Assign.Dst->Var);
        if (Slot && !Name.empty() && Name != *Slot &&
            !isEmittedParamName(Name)) {
          unsigned Clears = 0;
          bool OtherUse = false;
          for (const HighStmt &S : Stmts) {
            if (&S == &Assign || Analysis.DeadStmts.count(&S) ||
                stmtHiddenFromC(S))
              continue;
            if (StoresBitClear(S, Name, *Slot)) {
              ++Clears;
              continue;
            }
            bool Hit = false;
            forEachRhsExpr(S, [&](const ExprPtr &E) {
              if (UsesName(E.get(), Name))
                Hit = true;
            });
            if (S.Kind == StmtKind::Assign && S.Dst &&
                UsesName(S.Dst.get(), Name))
              Hit = true;
            if (Hit)
              OtherUse = true;
          }
          if (!OtherUse && Clears > 0) {
            Analysis.DeadStmts.insert(&Assign);
            Analysis.DeadVars.insert(Name);
            CopyForward[Name] = *Slot;
          }
        }
      }
      Walk(Assign.Body, Handler);
      Walk(Assign.ElseBody, Handler);
      Walk(Assign.DefaultBody, Handler);
      for (const auto &Case : Assign.Cases)
        Walk(Case.Body, Handler);
      for (const auto &Clause : Assign.EHClauseBodies)
        Walk(Clause, true);
    }
    InEHClauseBody = SavedHandler;
    InCxxCleanupBody = SavedCleanup;
  };
  Walk(Func.Body, false);
}

std::optional<std::string>
HighCWriter::incomingHomeParamName(int64_t Disp) const {
  if (Opts.TheArch != Arch::X64 || !CurrentFunc)
    return std::nullopt;
  if (Disp != 8 && Disp != 16 && Disp != 24 && Disp != 32)
    return std::nullopt;
  const bool MemberThis =
      !CurrentFunc->Params.empty() && CurrentFunc->Params[0].Name == "this";
  if (!IndirectReturnName.empty()) {
    if (Disp == 16 && MemberThis)
      return IndirectReturnName;
    if (Disp == 8)
      return MemberThis ? std::string("this") : IndirectReturnName;
  }
  if (Dbg) {
    if (auto FS = Dbg->resolveFunction(CurrentFunc->Entry);
        FS && isMsvcIndirectReturn(FS->ReturnType, Opts.TheArch, Opts.Format)) {
      if (isWin64MemberIndirectReturn(*FS)) {
        if (Disp == 8)
          return std::string("this");
        if (Disp == 16)
          return std::string("result");
      } else if (Disp == 8)
        return std::string("result");
    }
  }
  return std::nullopt;
}

std::optional<std::string>
HighCWriter::namedSlotLoadDisplay(const HighExpr &E) const {
  const HighExpr *P = peelIntegerViewOps(&E);
  if (!P)
    return std::nullopt;
  if (P->Kind != ExprKind::Load || P->Operands.empty() || !P->Operands[0] ||
      P->MemoryOrdering != NdMemoryOrdering::None)
    return std::nullopt;
  auto Slot = namedFrameSlot(*P->Operands[0]);
  if (!Slot)
    return std::nullopt;
  const auto Disp = frameDisplacement(*P->Operands[0]);
  if (Disp)
    if (auto Home = incomingHomeParamName(*Disp))
      if (llvm::StringRef(*Slot).starts_with("var_") || *Slot == *Home)
        return *Home;
  unsigned Access = 0;
  if (P->Type && P->Type->Size)
    Access = P->Type->Size;
  // Synthetic `var_10` / `var_mN` homes are pointer-sized leftovers, not
  // CStringT objects. Only a source/PDB slot name may project offset-0.
  if (Access && Disp && !llvm::StringRef(*Slot).starts_with("var_")) {
    auto It = FrameSlots.find(*Disp);
    if (It != FrameSlots.end() && It->second.Type) {
      const TypeRef &Ty = It->second.Type;
      if (Ty->Kind == NdTypeKind::Struct && !Ty->IsEnum && Ty->Size >= Access)
        if (auto Field =
                Ty->displayFieldNameAt(0, static_cast<uint16_t>(Access)))
          return *Slot + "." + *Field;
    }
  }
  return copyForwardName(*Slot);
}

void HighCWriter::hideCleanupSlotCopyIntoCall(const HighFunc &Func) {
  auto UsesName = [&](const HighExpr *E, const std::string &Name) -> bool {
    std::function<bool(const HighExpr *)> Walk = [&](const HighExpr *N) {
      if (!N)
        return false;
      if ((N->Kind == ExprKind::Var || N->Kind == ExprKind::Phi) &&
          varName(N->Var) == Name)
        return true;
      bool Hit = false;
      N->forEachChildExpr([&](const ExprPtr &C) {
        if (Walk(C.get()))
          Hit = true;
      });
      return Hit;
    };
    return Walk(E);
  };
  auto CallThis = [&](const HighStmt &S) -> const HighExpr * {
    const HighExpr *Call = nullptr;
    if (S.Kind == StmtKind::Call && S.CallExpr)
      Call = S.CallExpr.get();
    else if (S.Val && S.Val->Kind == ExprKind::Call)
      Call = S.Val.get();
    if (!Call || Call->Operands.empty())
      return nullptr;
    return Call->Operands[0].get();
  };

  std::function<void(const std::vector<HighStmt> &, bool)> Walk;
  Walk = [&](const std::vector<HighStmt> &Stmts, bool Handler) {
    const bool SavedHandler = InEHClauseBody;
    const bool SavedCleanup = InCxxCleanupBody;
    InEHClauseBody = Handler;
    if (Handler)
      InCxxCleanupBody = true;
    for (const HighStmt &Assign : Stmts) {
      if (Handler && !Analysis.DeadStmts.count(&Assign) &&
          Assign.Kind == StmtKind::Assign && Assign.Dst && Assign.Val &&
          (Assign.Dst->Kind == ExprKind::Var ||
           Assign.Dst->Kind == ExprKind::Phi)) {
        const std::string Name = varName(Assign.Dst->Var);
        if (auto Src = namedSlotLoadDisplay(*Assign.Val);
            Src && !Name.empty() && Name != *Src && !isEmittedParamName(Name)) {
          unsigned CallUses = 0;
          bool OtherUse = false;
          for (const HighStmt &S : Stmts) {
            if (&S == &Assign || Analysis.DeadStmts.count(&S) ||
                stmtHiddenFromC(S))
              continue;
            if (const HighExpr *This = CallThis(S);
                This &&
                (This->Kind == ExprKind::Var || This->Kind == ExprKind::Phi) &&
                varName(This->Var) == Name) {
              ++CallUses;
              continue;
            }
            bool Hit = false;
            forEachRhsExpr(S, [&](const ExprPtr &E) {
              if (UsesName(E.get(), Name))
                Hit = true;
            });
            if (S.Kind == StmtKind::Assign && S.Dst &&
                UsesName(S.Dst.get(), Name))
              Hit = true;
            if (Hit)
              OtherUse = true;
          }
          if (!OtherUse && CallUses > 0) {
            Analysis.DeadStmts.insert(&Assign);
            Analysis.DeadVars.insert(Name);
            CopyForward[Name] = *Src;
          }
        }
      }
      Walk(Assign.Body, Handler);
      Walk(Assign.ElseBody, Handler);
      Walk(Assign.DefaultBody, Handler);
      for (const auto &Case : Assign.Cases)
        Walk(Case.Body, Handler);
      for (const auto &Clause : Assign.EHClauseBodies)
        Walk(Clause, true);
    }
    InEHClauseBody = SavedHandler;
    InCxxCleanupBody = SavedCleanup;
  };
  Walk(Func.Body, false);
}

void HighCWriter::hideUnusedFrameSlotWrites(const HighFunc &Func) {
  auto ExprHasEffect = [](const HighExpr &E) -> bool {
    std::function<bool(const HighExpr &)> Walk = [&](const HighExpr &N) {
      if (N.Kind == ExprKind::Call || N.Kind == ExprKind::Store)
        return true;
      if (N.MemoryOrdering != NdMemoryOrdering::None)
        return true;
      for (const ExprPtr &Op : N.Operands)
        if (Op && Walk(*Op))
          return true;
      return false;
    };
    return Walk(E);
  };

  std::set<std::string> Observed;
  auto NoteUse = [&](const HighExpr &E) {
    std::function<void(const HighExpr &, bool)> Rec = [&](const HighExpr &N,
                                                          bool AsAddress) {
      if (N.Kind == ExprKind::Load && !N.Operands.empty() && N.Operands[0]) {
        if (auto Slot = namedFrameSlot(*N.Operands[0]))
          Observed.insert(*Slot);
        Rec(*N.Operands[0], true);
        return;
      }
      if (N.Kind == ExprKind::Addr && !N.Operands.empty() && N.Operands[0]) {
        Rec(*N.Operands[0], false);
        return;
      }
      if (!AsAddress) {
        if (auto Slot = namedFrameSlot(N))
          Observed.insert(*Slot);
      }
      for (const ExprPtr &Op : N.Operands)
        if (Op)
          Rec(*Op, AsAddress);
      // An indirect call reads its target as a value.
      if (N.IndirectTarget)
        Rec(*N.IndirectTarget, false);
    };
    Rec(E, false);
  };

  std::function<void(const std::vector<HighStmt> &, bool)> Observe;
  Observe = [&](const std::vector<HighStmt> &Stmts, bool InHandler) {
    const bool Saved = InEHClauseBody;
    InEHClauseBody = InHandler;
    for (const HighStmt &S : Stmts) {
      if (Analysis.DeadStmts.count(&S) && S.Kind == StmtKind::Store &&
          S.StoreVal) {
        // A forwarded store is omitted, but its value may still mention a
        // later unsubstituted slot load. Keep that producer visible.
        NoteUse(*S.StoreVal);
      } else if (!Analysis.DeadStmts.count(&S) && !stmtHiddenFromC(S)) {
        if (S.Kind == StmtKind::Assign && S.Dst &&
            S.Dst->Kind == ExprKind::Load && !S.Dst->Operands.empty() &&
            S.Dst->Operands[0]) {
          if (!namedFrameSlot(*S.Dst->Operands[0]))
            NoteUse(*S.Dst->Operands[0]);
          if (S.Val)
            NoteUse(*S.Val);
        } else if (S.Kind == StmtKind::Store) {
          if (S.StoreAddr && !namedFrameSlot(*S.StoreAddr))
            NoteUse(*S.StoreAddr);
          if (S.StoreVal)
            NoteUse(*S.StoreVal);
        } else {
          if (S.Dst)
            NoteUse(*S.Dst);
          forEachRhsExpr(S, [&](const ExprPtr &E) {
            if (E)
              NoteUse(*E);
          });
        }
      }
      Observe(S.Body, InHandler);
      Observe(S.ElseBody, InHandler);
      for (const auto &C : S.Cases)
        Observe(C.Body, InHandler);
      Observe(S.DefaultBody, InHandler);
      for (const auto &ClauseBody : S.EHClauseBodies)
        Observe(ClauseBody, true);
    }
    InEHClauseBody = Saved;
  };
  Observe(Func.Body, false);

  std::function<void(const std::vector<HighStmt> &, bool)> Hide;
  Hide = [&](const std::vector<HighStmt> &Stmts, bool InHandler) {
    const bool Saved = InEHClauseBody;
    InEHClauseBody = InHandler;
    for (const HighStmt &S : Stmts) {
      // Catch/cleanup funclets store the parent result to [rdx+k]; that write
      // is the C return, not an unused home.
      if (!InHandler && !Analysis.DeadStmts.count(&S)) {
        const HighExpr *Addr = nullptr;
        const HighExpr *Val = nullptr;
        if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
            S.Dst->Kind == ExprKind::Load && !S.Dst->Operands.empty()) {
          Addr = S.Dst->Operands[0].get();
          Val = S.Val.get();
        } else if (S.Kind == StmtKind::Store && S.StoreAddr && S.StoreVal) {
          Addr = S.StoreAddr.get();
          Val = S.StoreVal.get();
        }
        if (Addr && Val && !ExprHasEffect(*Val) &&
            S.MemoryOrdering == NdMemoryOrdering::None &&
            S.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
          const auto Slot = namedFrameSlot(*Addr);
          if (Slot && !Observed.count(*Slot)) {
            bool OverlapsObserved = false;
            if (const auto Disp = frameDisplacement(*Addr)) {
              uint16_t Size = Val->Type ? Val->Type->Size : 0;
              if (auto It = FrameSlots.find(*Disp);
                  It != FrameSlots.end() && It->second.Type)
                Size = std::max(Size, It->second.Type->Size);
              if (Size == 0)
                Size = 1;
              const int64_t Begin = *Disp;
              const int64_t End = Begin + static_cast<int64_t>(Size);
              for (const auto &[ObsDisp, Obs] : FrameSlots) {
                if (!Observed.count(Obs.Name))
                  continue;
                uint16_t ObsSize = Obs.Type ? Obs.Type->Size : 1;
                if (ObsSize == 0)
                  ObsSize = 1;
                const int64_t ObsEnd = ObsDisp + static_cast<int64_t>(ObsSize);
                if (Begin < ObsEnd && ObsDisp < End) {
                  OverlapsObserved = true;
                  break;
                }
              }
              // `values_ = &slot` only names the first record.  MSVC then
              // stores the next same-size Value at +sizeof(slot).  That
              // sibling is not loaded by HighIR, so treat it as live when
              // the store is a real leftover (call dest / temp), not 0.
              if (!OverlapsObserved) {
                const HighExpr *Inner = unwrapIntegerView(Val);
                if (!Inner)
                  Inner = Val;
                const bool PackedVal = Inner->Kind == ExprKind::Var ||
                                       Inner->Kind == ExprKind::Phi ||
                                       Inner->Kind == ExprKind::Call ||
                                       Inner->Kind == ExprKind::Load;
                if (PackedVal) {
                  for (const auto &[PrevDisp, Prev] : FrameSlots) {
                    if (!Prev.AddressTaken || !Observed.count(Prev.Name))
                      continue;
                    uint16_t PrevSize =
                        Prev.Type && Prev.Type->Size ? Prev.Type->Size : 0;
                    if (PrevSize < 8)
                      continue;
                    if (PrevDisp + static_cast<int64_t>(PrevSize) == Begin) {
                      OverlapsObserved = true;
                      break;
                    }
                  }
                }
              }
            }
            if (!OverlapsObserved)
              Analysis.DeadStmts.insert(&S);
          }
        }
      }
      Hide(S.Body, InHandler);
      Hide(S.ElseBody, InHandler);
      for (const auto &C : S.Cases)
        Hide(C.Body, InHandler);
      Hide(S.DefaultBody, InHandler);
      for (const auto &ClauseBody : S.EHClauseBodies)
        Hide(ClauseBody, true);
    }
    InEHClauseBody = Saved;
  };
  Hide(Func.Body, false);
}

void HighCWriter::collectGotoTargets(const std::vector<HighStmt> &Stmts,
                                     bool TryBody) {
  // A try body's trailing goto that is not printed needs no label.
  const size_t End =
      TryBody
          ? printedTryBodyEnd(
                [this](const HighStmt &Stmt) { return stmtHiddenFromC(Stmt); },
                [this](const HighStmt &Exit) {
                  return isFallthroughTryExit(Exit);
                },
                Stmts)
          : Stmts.size();
  for (size_t I = 0; I < Stmts.size(); ++I) {
    const HighStmt &S = Stmts[I];
    if (S.Kind == StmtKind::Goto && I < End && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      GotoTargets.insert(S.GotoTarget);
    const bool IsTry = S.Kind == StmtKind::SEHTry ||
                       S.Kind == StmtKind::CxxTry ||
                       S.Kind == StmtKind::ItaniumTry;
    collectGotoTargets(S.Body, IsTry);
    collectGotoTargets(S.ElseBody, false);
    for (auto &C : S.Cases)
      collectGotoTargets(C.Body, false);
    collectGotoTargets(S.DefaultBody, false);
    for (const auto &ClauseBody : S.EHClauseBodies)
      collectGotoTargets(ClauseBody, hasFallthroughExceptClause(S));
  }
}

} // namespace neverd
