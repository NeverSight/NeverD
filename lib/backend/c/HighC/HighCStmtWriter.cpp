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

#include "neverd/libc/LibCNames.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <cctype>
#include <functional>

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

bool isNoReturnCallStmt(const HighCAnalysisState &State, const HighStmt &Stmt) {
  if (isCxxThrowStmt(Stmt))
    return true;
  const HighExpr *E = stmtCallExpr(Stmt);
  if (!E)
    return false;
  return isNoreturnCallExpr(State, *E);
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
  if (!Clause.TypeName.empty() && isCIdentifier(Clause.TypeName)) {
    OS << Clause.TypeName;
    if (Clause.TypeDescriptorVA)
      OS << " /* type @ 0x" << llvm::utohexstr(Clause.TypeDescriptorVA)
         << " */";
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

void HighCWriter::emitRenderedStatement(int Indent, llvm::StringRef Text) {
  while (!Text.empty()) {
    auto [Line, Rest] = Text.split('\n');
    if (!Line.empty())
      emitIndent(Indent);
    OS << Line << '\n';
    Text = Rest;
  }
}

void HighCWriter::writeStmt(const HighStmt &Stmt, int Indent) {
  if (Analysis.DeadStmts.count(&Stmt))
    return;
  const bool HideEHRuntimeMemory =
      CurrentFunc && CurrentFunc->ExceptionMetadata.has_value();
  auto IsEHRuntimeSpace = [&](NdMemoryAddressSpace Space) {
    return HideEHRuntimeMemory && Space == NdMemoryAddressSpace::X86FS;
  };
  switch (Stmt.Kind) {
  case StmtKind::Assign: {
    if (!Stmt.Dst || !Stmt.Val)
      return;
    if (isCxxThrowExpr(Stmt.Val.get())) {
      emitIndent(Indent);
      OS << "throw";
      if (!Stmt.Val->Operands.empty() && Stmt.Val->Operands[0] &&
          !isCxxRethrowObject(Stmt.Val->Operands[0].get()))
        OS << " " << exprStr(*Stmt.Val->Operands[0]);
      OS << ";\n";
      break;
    }
    if (isNoreturnCallExpr(Analysis, *Stmt.Val)) {
      emitIndent(Indent);
      OS << exprStr(*Stmt.Val) << ";\n";
      break;
    }
    if (Stmt.Val->Kind == ExprKind::Load &&
        IsEHRuntimeSpace(Stmt.Val->MemoryAddressSpace))
      return;
    if (Stmt.Val->Kind == ExprKind::Call) {
      auto Rendered = renderX86SegmentedIntrinsicStatement(
          Opts.TheArch, *Stmt.Val, Stmt.Dst.get(),
          [this](const HighExpr &E) { return exprStr(E); },
          [this](const MedVar &V) { return varName(V); },
          [this](const MedVar &V) {
            return !Analysis.DeadVars.count(varName(V));
          });
      if (!Rendered.empty()) {
        emitRenderedStatement(Indent, Rendered);
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
        emitRenderedStatement(Indent, Rendered);
        HasCIntrinsics = true;
        break;
      }
    }
    bool DeadIntrinsicResult = Stmt.Dst->Kind == ExprKind::Var &&
                               Analysis.DeadVars.count(varName(Stmt.Dst->Var));
    if (Stmt.Val->Kind == ExprKind::Call &&
        Stmt.Val->IntrinsicId != Intrinsic::None && DeadIntrinsicResult &&
        (isSideeffectIntrinsic(Stmt.Val->IntrinsicId) ||
         !intrinsicCName(Stmt.Val->IntrinsicId))) {
      emitIndent(Indent);
      OS << exprStr(*Stmt.Val) << ";\n";
      break;
    }
    if (Stmt.Dst->Kind == ExprKind::Load && !Stmt.Dst->Operands.empty()) {
      if (IsEHRuntimeSpace(Stmt.Dst->MemoryAddressSpace))
        return;
      if (Stmt.Dst->MemoryOrdering == NdMemoryOrdering::None &&
          Stmt.Dst->MemoryAddressSpace == NdMemoryAddressSpace::Default)
        if (auto Slot = namedFrameSlot(*Stmt.Dst->Operands[0])) {
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
          emitIndent(Indent);
          OS << *Slot << " = ";
          TypeRef ProjectedDestType = Stmt.Dst->Type;
          if (auto Disp = frameDisplacement(*Stmt.Dst->Operands[0])) {
            auto ProjectedSlot = FrameSlots.find(*Disp);
            if (ProjectedSlot != FrameSlots.end() && ProjectedSlot->second.Type)
              ProjectedDestType = ProjectedSlot->second.Type;
          }
          if (ProjectedDestType && ProjectedDestType->Kind == NdTypeKind::Ptr)
            OS << "(" << typeToC(ProjectedDestType) << ")(uintptr_t)("
               << exprStr(*Stmt.Val) << ")";
          else
            OS << exprStr(*Stmt.Val);
          OS << ";\n";
          break;
        }
      if (auto VA = constAddress(*Stmt.Dst->Operands[0])) {
        if (auto Name = imageObjectName(*VA)) {
          emitIndent(Indent);
          OS << *Name << " = " << exprStr(*Stmt.Val) << ";\n";
          break;
        }
      }
      emitIndent(Indent);
      std::string Value = exprStr(*Stmt.Val);
      if (Stmt.Dst->Type && Stmt.Val->Type &&
          Stmt.Dst->Type->Kind == NdTypeKind::Int &&
          Stmt.Val->Type->Kind == NdTypeKind::Ptr)
        Value = "(" + typeToC(Stmt.Dst->Type) + ")(uintptr_t)(" + Value + ")";
      OS << memoryStoreExpr(Stmt.Dst->Type, exprStr(*Stmt.Dst->Operands[0]),
                            Value, Stmt.Dst->MemoryOrdering,
                            Stmt.Dst->MemoryAddressSpace)
         << ";\n";
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
      OS << varName(Stmt.Dst->Var) << " = ";
      auto DeclaredType = declaredParamType(Stmt.Dst->Var);
      if (!DeclaredType)
        DeclaredType = Stmt.Dst->Type;
      TypeRef ProjectedValueType = Stmt.Val->Type;
      if (Stmt.Val->Kind == ExprKind::Load && !Stmt.Val->Operands.empty() &&
          Stmt.Val->Operands[0]) {
        if (auto Disp = frameDisplacement(*Stmt.Val->Operands[0])) {
          auto Slot = FrameSlots.find(*Disp);
          if (Slot != FrameSlots.end() && Slot->second.Type)
            ProjectedValueType = Slot->second.Type;
        }
      }
      if (DeclaredType && DeclaredType->Kind == NdTypeKind::Ptr)
        OS << "(" << typeToC(DeclaredType) << ")(uintptr_t)("
           << exprStr(*Stmt.Val) << ")";
      else if (DeclaredType && ProjectedValueType &&
               DeclaredType->Kind == NdTypeKind::Int &&
               ProjectedValueType->Kind == NdTypeKind::Ptr)
        OS << "(" << typeToC(DeclaredType) << ")(uintptr_t)("
           << exprStr(*Stmt.Val) << ")";
      else
        OS << exprStr(*Stmt.Val);
    } else {
      OS << exprStr(*Stmt.Dst) << " = " << exprStr(*Stmt.Val);
    }
    OS << ";\n";
    break;
  }

  case StmtKind::Store:
    if (!Stmt.StoreAddr || !Stmt.StoreVal)
      return;
    if (IsEHRuntimeSpace(Stmt.MemoryAddressSpace))
      return;
    if (isCompilerEHConstant(*Stmt.StoreVal))
      break;
    if (Stmt.MemoryOrdering == NdMemoryOrdering::None &&
        Stmt.MemoryAddressSpace == NdMemoryAddressSpace::Default)
      if (auto Slot = namedFrameSlot(*Stmt.StoreAddr)) {
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
        emitIndent(Indent);
        OS << *Slot << " = ";
        TypeRef ProjectedDestType = Stmt.StoreVal->Type;
        if (auto Disp = frameDisplacement(*Stmt.StoreAddr)) {
          auto ProjectedSlot = FrameSlots.find(*Disp);
          if (ProjectedSlot != FrameSlots.end() && ProjectedSlot->second.Type)
            ProjectedDestType = ProjectedSlot->second.Type;
        }
        if (ProjectedDestType && ProjectedDestType->Kind == NdTypeKind::Ptr)
          OS << "(" << typeToC(ProjectedDestType) << ")(uintptr_t)("
             << exprStr(*Stmt.StoreVal) << ")";
        else
          OS << exprStr(*Stmt.StoreVal);
        OS << ";\n";
        break;
      }
    if (auto VA = constAddress(*Stmt.StoreAddr)) {
      if (auto Name = imageObjectName(*VA)) {
        emitIndent(Indent);
        OS << *Name << " = " << exprStr(*Stmt.StoreVal) << ";\n";
        break;
      }
    }
    emitIndent(Indent);
    OS << memoryStoreExpr(Stmt.StoreVal->Type, exprStr(*Stmt.StoreAddr),
                          exprStr(*Stmt.StoreVal), Stmt.MemoryOrdering,
                          Stmt.MemoryAddressSpace)
       << ";\n";
    break;

  case StmtKind::Call:
    if (!Stmt.CallExpr)
      return;
    if (isCxxThrowExpr(Stmt.CallExpr.get())) {
      emitIndent(Indent);
      OS << "throw";
      if (!Stmt.CallExpr->Operands.empty() && Stmt.CallExpr->Operands[0] &&
          !isCxxRethrowObject(Stmt.CallExpr->Operands[0].get()))
        OS << " " << exprStr(*Stmt.CallExpr->Operands[0]);
      OS << ";\n";
      break;
    }
    {
      auto Rendered = renderX86SegmentedIntrinsicStatement(
          Opts.TheArch, *Stmt.CallExpr, nullptr,
          [this](const HighExpr &E) { return exprStr(E); },
          [this](const MedVar &V) { return varName(V); },
          [this](const MedVar &V) {
            return !Analysis.DeadVars.count(varName(V));
          });
      if (!Rendered.empty()) {
        emitRenderedStatement(Indent, Rendered);
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
        emitRenderedStatement(Indent, Rendered);
        HasCIntrinsics = true;
        break;
      }
    }
    emitIndent(Indent);
    OS << exprStr(*Stmt.CallExpr) << ";\n";
    break;

  case StmtKind::Return:
    emitIndent(Indent);
    if (InferredVoid || !Stmt.RetVal || Stmt.RetVal->Kind == ExprKind::Undef)
      OS << "return;\n";
    else if (Stmt.RetVal->Kind == ExprKind::Var &&
             !Analysis.AssignedVars.count(varName(Stmt.RetVal->Var)) &&
             (Stmt.RetVal->Var.Kind != MedVar::Param || InEHClauseBody))
      OS << "return;\n";
    else
      OS << "return " << formatReturnExpr(*Stmt.RetVal) << ";\n";
    break;

  case StmtKind::If:
    if (!Stmt.Cond)
      return;
    if (stmtsEffectivelyEmpty(Stmt.Body))
      return;
    emitIndent(Indent);
    OS << "if (" << exprStr(*Stmt.Cond) << ") {\n";
    writeStmts(Stmt.Body, Indent + 1);
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
      writeStmts(Stmt.ElseBody, Indent + 1);
      emitIndent(Indent);
      OS << "}\n";
      break;
    }
    emitIndent(Indent);
    OS << "if (" << exprStr(*Stmt.Cond) << ") {\n";
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    if (stmtsEffectivelyEmpty(Stmt.ElseBody)) {
      OS << "}\n";
      break;
    }
    OS << "} else {\n";
    writeStmts(Stmt.ElseBody, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::While:
    emitIndent(Indent);
    OS << "while (" << (Stmt.Cond ? exprStr(*Stmt.Cond) : "1") << ") {\n";
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::DoWhile:
    emitIndent(Indent);
    OS << "do {\n";
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "} while (" << (Stmt.Cond ? exprStr(*Stmt.Cond) : "1") << ");\n";
    break;

  case StmtKind::For:
    emitIndent(Indent);
    OS << "for (;;) {\n";
    if (Stmt.Cond) {
      emitIndent(Indent + 1);
      OS << "if (!(" << exprStr(*Stmt.Cond) << ")) break;\n";
    }
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::Switch:
    if (!Stmt.SwitchExpr)
      return;
    emitIndent(Indent);
    OS << "switch (" << exprStr(*Stmt.SwitchExpr) << ") {\n";
    for (auto &C : Stmt.Cases) {
      emitIndent(Indent);
      OS << "case " << constStr(C.Value) << ":\n";
      writeStmts(C.Body, Indent + 1);
      emitIndent(Indent + 1);
      OS << "break;\n";
    }
    if (!Stmt.DefaultBody.empty()) {
      emitIndent(Indent);
      OS << "default:\n";
      writeStmts(Stmt.DefaultBody, Indent + 1);
      emitIndent(Indent + 1);
      OS << "break;\n";
    }
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::Goto:
    emitIndent(Indent);
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
    writeStmts(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    break;

  case StmtKind::ExprStmt:
    if (Stmt.Val) {
      if (isCxxThrowExpr(Stmt.Val.get())) {
        emitIndent(Indent);
        OS << "throw";
        if (!Stmt.Val->Operands.empty() && Stmt.Val->Operands[0] &&
            !isCxxRethrowObject(Stmt.Val->Operands[0].get()))
          OS << " " << exprStr(*Stmt.Val->Operands[0]);
        OS << ";\n";
        break;
      }
      if (Stmt.Val->Kind == ExprKind::Load && !Stmt.Val->Operands.empty() &&
          namedFrameSlot(*Stmt.Val->Operands[0]))
        break;
      emitIndent(Indent);
      OS << exprStr(*Stmt.Val) << ";\n";
    }
    break;

  case StmtKind::SEHTry: {
    if (!Stmt.EHIsReducible || Stmt.EHClauses.size() != 1 ||
        Stmt.EHClauseBodies.size() != 1) {
      emitIndent(Indent);
      OS << "__try {\n";
      writeTryBody(Stmt.Body, Indent + 1);
      emitIndent(Indent);
      OS << "} __except (EXCEPTION_EXECUTE_HANDLER) {\n";
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
    writeTryBody(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    if (Clause.Kind == HighEHClauseKind::SEHFinally) {
      OS << "} __finally {\n";
      {
        const bool SavedHandler = InEHClauseBody;
        InEHClauseBody = true;
        writeStmts(Stmt.EHClauseBodies.front(), Indent + 1);
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
        if (FilterName.empty())
          FilterName =
              "nd_seh_filter_0x" + llvm::utohexstr(Clause.FilterOrActionVA);
        OS << FilterName << "(GetExceptionInformation())";
      }
      OS << ") {\n";
      {
        const bool SavedHandler = InEHClauseBody;
        InEHClauseBody = true;
        writeStmts(Stmt.EHClauseBodies.front(), Indent + 1);
        InEHClauseBody = SavedHandler;
      }
      if (Stmt.EHClauseBodies.front().empty()) {
        emitIndent(Indent + 1);
        if (CurrentFunc && CurrentFunc->ExceptionMetadata &&
            CurrentFunc->ExceptionMetadata->CodeRange.contains(
                Clause.HandlerVA))
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
    emitIndent(Indent);
    OS << "try {\n";
    writeTryBody(Stmt.Body, Indent + 1);
    emitIndent(Indent);
    OS << "}";
    for (size_t I = 0; I < Stmt.EHClauses.size(); ++I) {
      const HighEHClause &Clause = Stmt.EHClauses[I];
      if (Clause.Kind == HighEHClauseKind::CxxCleanup) {
        OS << "\n";
        emitIndent(Indent);
        OS << "/* unwind cleanup(state=" << Clause.State
           << ", kind=" << getCxxUnwindActionKindName(Clause.UnwindActionKind)
           << ", object at frame+" << Clause.UnwindObjectOffset;
        if (Clause.FilterOrActionVA)
          OS << ", dtor @ 0x" << llvm::utohexstr(Clause.FilterOrActionVA);
        OS << " */\n";
        if (I < Stmt.EHClauseBodies.size()) {
          const bool SavedHandler = InEHClauseBody;
          const bool SavedCleanup = InCxxCleanupBody;
          InEHClauseBody = true;
          InCxxCleanupBody = true;
          writeStmts(Stmt.EHClauseBodies[I], Indent);
          InEHClauseBody = SavedHandler;
          InCxxCleanupBody = SavedCleanup;
        }
        continue;
      }
      OS << " catch (";
      writeCxxCatchType(OS, Clause);
      OS << ") {\n";
      if (I < Stmt.EHClauseBodies.size()) {
        const bool SavedHandler = InEHClauseBody;
        InEHClauseBody = true;
        writeStmts(Stmt.EHClauseBodies[I], Indent + 1);
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
    emitIndent(Indent);
    OS << "/* Itanium try [0x" << llvm::utohexstr(Stmt.EHRange.Begin) << ", 0x"
       << llvm::utohexstr(Stmt.EHRange.End)
       << ") recovered from the call-site table */\n";
    emitIndent(Indent);
    OS << "{\n";
    writeStmts(Stmt.Body, Indent + 1);
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

void HighCWriter::writeStmts(const std::vector<HighStmt> &Stmts, int Indent) {
  va_t LastLabel = InvalidVA;
  bool AfterNoReturn = false;
  for (size_t I = 0; I < Stmts.size(); ++I) {
    const HighStmt &S = Stmts[I];
    if (AfterNoReturn &&
        (isDebugTrapStmt(S) || S.Kind == StmtKind::Return ||
         S.Kind == StmtKind::Nop ||
         (S.Kind == StmtKind::Block && stmtsEffectivelyEmpty(S.Body))))
      continue;
    if (S.Kind == StmtKind::IfElse && S.Cond && I + 1 < Stmts.size()) {
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
                          isNoreturnCallExpr(Analysis, *AssignCall->Val);
          ++I;
          continue;
        }
      }
    }
    bool EmittedLabel = false;
    if (S.Addr != 0 && S.Addr != InvalidVA && GotoTargets.count(S.Addr) &&
        S.Addr != LastLabel) {
      OS << "L_" + llvm::utohexstr(S.Addr) + ":\n";
      LastLabel = S.Addr;
      EmittedLabel = true;
    }
    if (EmittedLabel && Analysis.DeadStmts.count(&S)) {
      emitIndent(Indent);
      OS << ";\n";
      continue;
    }
    if (stmtHiddenFromC(S))
      continue;
    if (InCxxCleanupBody && S.Kind == StmtKind::Return)
      continue;
    if (InCxxCleanupBody && S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        S.Dst->Kind == ExprKind::Var && S.Val->Kind == ExprKind::Call) {
      const HighStmt *Ret = nullptr;
      for (size_t K = I + 1; K < Stmts.size(); ++K) {
        if (stmtHiddenFromC(Stmts[K]) || Stmts[K].Kind == StmtKind::Nop)
          continue;
        if (Stmts[K].Kind == StmtKind::Return)
          Ret = &Stmts[K];
        break;
      }
      if (Ret) {
        emitIndent(Indent);
        OS << exprStr(*S.Val) << ";\n";
        AfterNoReturn = isNoreturnCallExpr(Analysis, *S.Val);
        continue;
      }
    }
    if (InEHClauseBody && !InferredVoid) {
      if (const HighExpr *Stored = parentFrameStoredValue(S)) {
        size_t J = I + 1;
        while (J < Stmts.size() && (stmtHiddenFromC(Stmts[J]) ||
                                    Stmts[J].Kind == StmtKind::Nop))
          ++J;
        if (J < Stmts.size() && Stmts[J].Kind == StmtKind::Return) {
          if (auto Slot = namedFrameSlot(
                  S.Kind == StmtKind::Store ? *S.StoreAddr
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
    if (InEHClauseBody && S.Kind == StmtKind::Return && S.RetVal &&
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
    writeStmt(S, Indent);
    AfterNoReturn = isNoReturnCallStmt(Analysis, S);
  }
}

void HighCWriter::writeTryBody(const std::vector<HighStmt> &Stmts, int Indent) {
  size_t End = Stmts.size();
  while (End > 0) {
    const HighStmt &Last = Stmts[End - 1];
    if (Analysis.DeadStmts.count(&Last) || Last.Kind == StmtKind::Nop) {
      --End;
      continue;
    }
    if (Last.Kind == StmtKind::Goto) {
      --End;
      continue;
    }
    break;
  }
  if (End == Stmts.size()) {
    writeStmts(Stmts, Indent);
    return;
  }
  std::vector<HighStmt> Prefix(Stmts.begin(),
                               Stmts.begin() + static_cast<std::ptrdiff_t>(End));
  writeStmts(Prefix, Indent);
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
        if (Nested || SeenLoad.count(*Slot) || SlotStores[*Slot] != 1 ||
            !isParamCopy(Val) || !Val.Type ||
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
        } else if (!Analysis.DeadStmts.count(&Stmt) &&
                   (Stmt.Dst->Kind == ExprKind::Var ||
                    Stmt.Dst->Kind == ExprKind::Phi) &&
                   isCopyForwardDestination(Stmt.Dst->Var)) {
          const std::string DstName = varName(Stmt.Dst->Var);
          // Loads of a named slot keep the destination so a pointer slot
          // loaded as an integer still prints the carrier cast.
          std::optional<std::string> Src;
          if (Stmt.Val->Kind != ExprKind::Load)
            Src = copyForwardSource(*Stmt.Val);
          if (Src && Stmt.Dst->Type && Stmt.Val->Type &&
              (Stmt.Dst->Type->Kind != Stmt.Val->Type->Kind ||
               Stmt.Dst->Type->Size != Stmt.Val->Type->Size))
            Src.reset();
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
  for (const auto &[Slot, Types] : SlotLoadTypes) {
    if (!CopyForward.count(Slot))
      continue;
    bool Compatible = true;
    uint16_t Size = 0;
    for (const TypeRef &Ty : Types) {
      if (!Ty || Ty->Kind != NdTypeKind::Int) {
        Compatible = false;
        break;
      }
      if (Size == 0)
        Size = Ty->Size;
      else if (Ty->Size != Size) {
        Compatible = false;
        break;
      }
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
}

void HighCWriter::collectGotoTargets(const std::vector<HighStmt> &Stmts) {
  for (auto &S : Stmts) {
    if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
        S.GotoTarget != InvalidVA)
      GotoTargets.insert(S.GotoTarget);
    collectGotoTargets(S.Body);
    collectGotoTargets(S.ElseBody);
    for (auto &C : S.Cases)
      collectGotoTargets(C.Body);
    collectGotoTargets(S.DefaultBody);
    for (auto &ClauseBody : S.EHClauseBodies)
      collectGotoTargets(ClauseBody);
  }
}

} // namespace neverd
