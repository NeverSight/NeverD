//===- HighCFuncWriter.cpp - HighIR function-level rendering --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Function-level orchestration for the HighIR C emitter: analysis pass
/// scheduling, local variable declaration emission, and function signature
/// rendering.  Statement-level rendering lives in HighCStmtWriter.cpp.
///
//===----------------------------------------------------------------------===//

#include "HighCWriter.h"

#include "neverd/ArchSupport.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"

#include <limits>

namespace neverd {

namespace {

uint64_t checkedStackAdd(uint64_t Left, uint64_t Right) {
  if (Left > std::numeric_limits<uint64_t>::max() - Right)
    llvm::report_fatal_error("HighC synthetic stack size overflow");
  return Left + Right;
}

uint64_t checkedStackAlign(uint64_t Size) {
  constexpr uint64_t Mask = kSyntheticStackAlignment - 1;
  return checkedStackAdd(Size, Mask) & ~Mask;
}

} // anonymous namespace

void HighCWriter::runAnalysisPasses(const HighFunc &Func) {
  GotoTargets.clear();
  collectGotoTargets(Func.Body);
  walkStmts(Func.Body, [&](const HighStmt &Stmt) {
    for (const HighEHClause &Clause : Stmt.EHClauses) {
      auto Reachable = [&](va_t Address) {
        return Address && Func.ExceptionMetadata &&
               Func.ExceptionMetadata->CodeRange.contains(Address);
      };
      if (Clause.Kind == HighEHClauseKind::SEHExcept &&
          Reachable(Clause.HandlerVA))
        GotoTargets.insert(Clause.HandlerVA);
      // An Itanium landing pad is a block of this function, so the clause
      // comment can point at a real label instead of a bare address.
      for (va_t Pad : Clause.LandingPadVAs)
        if (Reachable(Pad))
          GotoTargets.insert(Pad);
    }
  });

  auto VarFn = [this](const MedVar &V) { return varName(V); };
  auto ExprFn = [this](const HighExpr &E) { return exprStr(E); };
  analyzeDeadStores(Analysis, Func, VarFn, ExprFn);
  analyzeStoreForwarding(Analysis, Func, VarFn, ExprFn);
  InferredVoid = analyzeVoidReturn(Analysis, Func, VarFn, ExprFn);

  HiLoPairs.clear();
  auto RegisterHiLo = [this](const HighStmt &S, const HighExpr &CE) {
    if (CE.IntrinsicOutputs.size() < 2)
      return;
    const char *Collapse = hiloCollapseExpr(CE.IntrinsicId);
    if (!Collapse)
      return;
    std::string Lo = varName(CE.IntrinsicOutputs[0]);
    std::string Hi = varName(CE.IntrinsicOutputs[1]);
    if (!Analysis.DeadVars.count(Lo) && !Analysis.DeadVars.count(Hi))
      HiLoPairs.push_back({Lo, Hi, Collapse, &S});
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (Analysis.DeadStmts.count(&S))
      return;
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        S.Dst->Kind == ExprKind::Var && S.Val->Kind == ExprKind::Call &&
        !S.Val->IntrinsicOutputs.empty()) {
      auto Rendered = MultiOutputRender{}(
          Opts.TheArch, S.Val->IntrinsicId, S.Val->IntrinsicOutputs,
          S.Val->Operands, [this](const HighExpr &E) { return exprStr(E); },
          [this](const MedVar &V) { return varName(V); },
          [this](const MedVar &V) {
            return !Analysis.DeadVars.count(varName(V));
          });
      if (!Rendered.empty())
        Analysis.DeadVars.insert(varName(S.Dst->Var));
      RegisterHiLo(S, *S.Val);
    }
    if (S.Kind == StmtKind::Call && S.CallExpr &&
        !S.CallExpr->IntrinsicOutputs.empty())
      RegisterHiLo(S, *S.CallExpr);
  });

  if (InferredVoid)
    analyzeVoidDeadChain(Analysis, Func, VarFn);

  if (!HiLoPairs.empty() && !InferredVoid) {
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Return || !S.RetVal)
        return;
      auto Collapsed = collapseHiLo(*S.RetVal);
      (void)Collapsed;
    });
  }

  FuncReturnType = InferredVoid ? nullptr : Func.ReturnType;
}

void HighCWriter::emitLocalDecls(const HighFunc &Func,
                                 const std::set<std::string> &ParamNames) {
  auto VarFn = [this](const MedVar &V) { return varName(V); };

  std::map<std::string, TypeRef> UsedVars;
  std::map<std::string, std::string> ExplicitTypes;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (Analysis.DeadStmts.count(&S))
      return;
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        S.Dst->Kind == ExprKind::Var) {
      std::string Name = varName(S.Dst->Var);
      if (S.Val->Kind == ExprKind::BinOp &&
          (S.Val->Op == NdOp::ATOMIC_ADD ||
           S.Val->Op == NdOp::ATOMIC_CMPXCHG) &&
          S.Val->Type) {
        ExplicitTypes[Name] = typeToC(S.Val->Type);
      } else if (S.Val->Kind == ExprKind::Call &&
                 S.Val->IntrinsicId == Intrinsic::A64_SvePtrue) {
        ExplicitTypes[Name] = "svbool_t";
      } else if (S.Val->Kind == ExprKind::Call &&
                 (S.Val->IntrinsicId == Intrinsic::A64_SveDup ||
                  S.Val->IntrinsicId == Intrinsic::A64_SveIndex)) {
        uint64_t ElemBytes = 1;
        size_t ElemIndex =
            S.Val->IntrinsicId == Intrinsic::A64_SveIndex ? 2 : 1;
        if (S.Val->Operands.size() > ElemIndex &&
            S.Val->Operands[ElemIndex]->Kind == ExprKind::Const)
          ElemBytes = S.Val->Operands[ElemIndex]->ConstVal;
        ExplicitTypes[Name] = ElemBytes == 2   ? "svuint16_t"
                              : ElemBytes == 4 ? "svuint32_t"
                              : ElemBytes == 8 ? "svuint64_t"
                                               : "svuint8_t";
      }
    }
    forEachExpr(S, [&](const ExprPtr &E) {
      if (E)
        collectUsedVarsExpr(*E, UsedVars, VarFn);
    });
  });

  if (!Analysis.StoreFwd.empty()) {
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Store || !S.StoreAddr || !S.StoreVal)
        return;
      std::string Addr = exprStr(*S.StoreAddr);
      if (Analysis.StoreFwd.count(Addr))
        collectUsedVarsExpr(*S.StoreVal, UsedVars, VarFn);
    });
  }

  std::set<std::string> DeclaredNames(ParamNames);
  for (auto &Local : Func.Locals) {
    if (DeclaredNames.count(Local.Name))
      continue;
    if (UsedVars.find(Local.Name) == UsedVars.end())
      continue;
    if (Analysis.DeadVars.count(Local.Name))
      continue;
    DeclaredNames.insert(Local.Name);
    emitIndent(1);
    auto ExplicitTy = ExplicitTypes.find(Local.Name);
    OS << (ExplicitTy == ExplicitTypes.end() ? typeToC(Local.Type)
                                             : ExplicitTy->second)
       << " " << Local.Name << ";\n";
  }

  for (auto &[Name, Ty] : UsedVars) {
    if (DeclaredNames.count(Name))
      continue;
    if (Analysis.DeadVars.count(Name))
      continue;
    DeclaredNames.insert(Name);
    emitIndent(1);
    auto ExplicitTy = ExplicitTypes.find(Name);
    OS << (ExplicitTy == ExplicitTypes.end() ? typeToC(Ty) : ExplicitTy->second)
       << " " << Name << ";\n";
  }
  if (!DeclaredNames.empty())
    OS << "\n";
}

void HighCWriter::writeExceptionAnnotation(const HighFunc &Func) {
  if (!Func.ExceptionMetadata)
    return;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  OS << "/* neverd.exception: encoding="
     << getExceptionEncodingName(EH.Encoding)
     << ", status=" << getExceptionParseStatusName(EH.ParseStatus)
     << ", personality=" << getExceptionPersonalityName(EH.Personality) << "\n";
  OS << " * code=[0x" << llvm::utohexstr(EH.CodeRange.Begin) << ", 0x"
     << llvm::utohexstr(EH.CodeRange.End) << ")";
  if (EH.UnwindInfoVA)
    OS << ", unwind=0x" << llvm::utohexstr(EH.UnwindInfoVA);
  OS << "\n";
  OS << " * highir.structured_regions=" << Func.StructuredExceptionRegions
     << ", fallback_regions=" << Func.UnstructuredExceptionRegions << "\n";

  if (EH.SEH) {
    for (size_t I = 0; I < EH.SEH->Scopes.size(); ++I) {
      const SEHScopeRecord &Scope = EH.SEH->Scopes[I];
      const char *Kind = Scope.Kind == SEHScopeKind::Finally    ? "finally"
                         : Scope.Kind == SEHScopeKind::CatchAll ? "except-all"
                                                                : "filter";
      OS << " * seh.scope[" << I << "]: " << Kind << " [0x"
         << llvm::utohexstr(Scope.GuardedRange.Begin) << ", 0x"
         << llvm::utohexstr(Scope.GuardedRange.End) << ")";
      if (Scope.FilterOrFinallyVA)
        OS << " filter_or_finally=0x"
           << llvm::utohexstr(Scope.FilterOrFinallyVA);
      if (Scope.HandlerVA)
        OS << " handler=0x" << llvm::utohexstr(Scope.HandlerVA);
      OS << "\n";
    }
  }
  if (EH.Cxx) {
    OS << " * cxx.format="
       << (EH.Cxx->NativeEncoding == CxxExceptionInfo::Encoding::FH4 ? "fh4"
                                                                     : "fh3")
       << ", states=" << EH.Cxx->MaxState
       << ", try_blocks=" << EH.Cxx->TryBlocks.size()
       << ", ip_states=" << EH.Cxx->IPMap.size() << "\n";
    for (size_t I = 0; I < EH.Cxx->UnwindMap.size(); ++I) {
      const CxxUnwindAction &Action = EH.Cxx->UnwindMap[I];
      OS << " * cxx.unwind[" << I << "]: to_state=" << Action.ToState
         << ", kind=" << getCxxUnwindActionKindName(Action.Kind);
      if (Action.ActionVA)
        OS << ", action=0x" << llvm::utohexstr(Action.ActionVA);
      if (Action.ObjectOffset)
        OS << ", object_offset=" << Action.ObjectOffset;
      OS << "\n";
    }
    for (size_t I = 0; I < EH.Cxx->TryBlocks.size(); ++I) {
      const CxxTryBlock &Try = EH.Cxx->TryBlocks[I];
      OS << " * cxx.try[" << I << "]: states=" << Try.TryLow << ".."
         << Try.TryHigh << ", catch_high=" << Try.CatchHigh
         << ", handlers=" << Try.Handlers.size() << "\n";
      for (size_t J = 0; J < Try.Handlers.size(); ++J) {
        const CxxCatchHandler &Catch = Try.Handlers[J];
        OS << " *   catch[" << J << "]: type=0x"
           << llvm::utohexstr(Catch.TypeDescriptorVA) << ", handler=0x"
           << llvm::utohexstr(Catch.HandlerVA) << ", adjectives=0x"
           << llvm::utohexstr(Catch.Adjectives)
           << ", object_offset=" << Catch.CatchObjectOffset
           << ", parent_frame_offset=" << Catch.ParentFrameOffset;
        if (!Catch.ContinuationVAs.empty()) {
          OS << ", continuations=";
          for (size_t K = 0; K < Catch.ContinuationVAs.size(); ++K) {
            if (K)
              OS << ",";
            OS << "0x" << llvm::utohexstr(Catch.ContinuationVAs[K]);
          }
        }
        OS << "\n";
      }
    }
    for (size_t I = 0; I < EH.Cxx->IPMap.size(); ++I)
      OS << " * cxx.ip_state[" << I << "]: ip=0x"
         << llvm::utohexstr(EH.Cxx->IPMap[I].IP)
         << ", state=" << EH.Cxx->IPMap[I].State << "\n";
  }
  if (EH.Itanium) {
    const ItaniumEHInfo &LSDA = *EH.Itanium;
    OS << " * itanium.lsda=0x" << llvm::utohexstr(LSDA.LSDAVA)
       << ", form=" << (LSDA.IsCallSiteAddressForm ? "call-site" : "sjlj")
       << ", call_sites=" << LSDA.CallSites.size()
       << ", actions=" << LSDA.Actions.size()
       << ", types=" << LSDA.TypeTable.size()
       << ", specs=" << LSDA.ExceptionSpecs.size() << "\n";
    for (size_t I = 0; I < LSDA.CallSites.size(); ++I) {
      const ItaniumCallSite &Site = LSDA.CallSites[I];
      OS << " * itanium.call_site[" << I << "]: ";
      if (LSDA.IsCallSiteAddressForm) {
        OS << "[0x" << llvm::utohexstr(Site.GuardedRange.Begin) << ", 0x"
           << llvm::utohexstr(Site.GuardedRange.End) << ")";
        if (Site.LandingPadVA)
          OS << " pad=0x" << llvm::utohexstr(Site.LandingPadVA);
        else
          OS << " pad=none";
      } else {
        // An SJLJ entry names neither a range nor a pad address.  What it has
        // is the number the frame stores to select it and the selector its
        // dispatch switch runs on, so those are what get reported rather than
        // a pair of zeroes dressed up as a range.
        OS << "index=" << Site.CallSiteIndex
           << " select=" << Site.NativeLandingPad;
      }
      if (Site.FirstActionOffset)
        OS << " action=+" << *Site.FirstActionOffset;
      OS << "\n";
    }
    for (const ItaniumAction &Action : LSDA.Actions) {
      OS << " * itanium.action[+" << Action.TableOffset
         << "]: filter=" << Action.TypeFilter;
      if (Action.NextActionOffset)
        OS << ", next=+" << *Action.NextActionOffset;
      OS << "\n";
    }
    for (const ItaniumTypeEntry &Type : LSDA.TypeTable) {
      OS << " * itanium.type[" << Type.Index << "]: ";
      if (Type.IsCatchAll)
        OS << "catch-all";
      else
        OS << "typeinfo=0x" << llvm::utohexstr(Type.TypeInfoVA);
      if (!Type.TypeName.empty())
        OS << " name=" << Type.TypeName;
      OS << "\n";
    }
    for (const ItaniumExceptionSpec &Spec : LSDA.ExceptionSpecs) {
      OS << " * itanium.spec[" << Spec.Index << "]: types=";
      if (Spec.TypeIndices.empty()) {
        OS << "none";
      } else {
        for (size_t I = 0; I < Spec.TypeIndices.size(); ++I)
          OS << (I ? "," : "") << Spec.TypeIndices[I];
      }
      OS << "\n";
    }
  }
  if (EH.GSCookie) {
    const GSCookieInfo &GS = *EH.GSCookie;
    OS << " * gs.cookie_offset=" << GS.CookieOffset
       << ", ehandler=" << GS.HasExceptionHandler
       << ", uhandler=" << GS.HasUnwindHandler;
    if (GS.HasAlignment)
      OS << ", alignment_base=" << GS.AlignmentBaseOffset
         << ", alignment=" << GS.Alignment;
    OS << "\n";
  }
  for (const std::string &Diagnostic : EH.Diagnostics)
    OS << " * diagnostic: " << Diagnostic << "\n";
  OS << " */\n";
}

void HighCWriter::writeFunction(const HighFunc &Func) {
  CurrentFunc = &Func;
  runAnalysisPasses(Func);

  bool NeedsFrameStorage = false;
  auto VisitFrameUses = [&](auto &&Self, const HighExpr &Expr,
                            bool IsValue) -> void {
    if (NeedsFrameStorage)
      return;
    if (Expr.Kind == ExprKind::Addr && !Expr.Operands.empty() &&
        Expr.Operands[0] && Expr.Operands[0]->Kind == ExprKind::Load) {
      for (const ExprPtr &AddressOperand : Expr.Operands[0]->Operands)
        if (AddressOperand)
          Self(Self, *AddressOperand, true);
      return;
    }
    if (IsValue && Expr.Kind == ExprKind::Load && !Expr.Operands.empty()) {
      std::string Addr = exprStr(*Expr.Operands[0]);
      auto FwdIt = Analysis.StoreFwd.find(Addr);
      if (FwdIt != Analysis.StoreFwd.end()) {
        auto DepIt = Analysis.StoreFwdDeps.find(Addr);
        NeedsFrameStorage = DepIt != Analysis.StoreFwdDeps.end() &&
                            DepIt->second.count("frame_base") != 0;
        return;
      }
    }
    if (Expr.Kind == ExprKind::Var && varName(Expr.Var) == "frame_base") {
      NeedsFrameStorage = true;
      return;
    }
    for (const ExprPtr &Operand : Expr.Operands)
      if (Operand)
        Self(Self, *Operand, true);
  };
  walkStmts(Func.Body, [&](const HighStmt &Stmt) {
    if (NeedsFrameStorage || Analysis.DeadStmts.count(&Stmt) ||
        (InferredVoid && Stmt.Kind == StmtKind::Return))
      return;
    if (Stmt.Kind == StmtKind::Assign && Stmt.Dst &&
        Stmt.Dst->Kind == ExprKind::Load)
      VisitFrameUses(VisitFrameUses, *Stmt.Dst, false);
    forEachRhsExpr(Stmt, [&](const ExprPtr &Expr) {
      if (Expr)
        VisitFrameUses(VisitFrameUses, *Expr, true);
    });
  });

  std::string RetType;
  if (InferredVoid)
    RetType = "void";
  else
    RetType = FuncReturnType ? typeToC(FuncReturnType) : "uint32_t";

  std::string FName = Func.Name;
  if (!FName.empty() && FName[0] == '_')
    FName = FName.substr(1);

  if (Opts.EmitComments && !Func.DebugName.empty() &&
      Func.DebugName != Func.Name) {
    OS << "/* " << Func.DebugName;
    if (!Func.SourceFile.empty())
      OS << " @ " << Func.SourceFile << ":" << Func.SourceLine;
    OS << " */\n";
  }

  writeExceptionAnnotation(Func);

  if (Func.DoesNotReturn)
    OS << "_Noreturn ";
  OS << RetType << " " << FName << "(";
  for (size_t I = 0; I < Func.Params.size(); ++I) {
    if (I > 0)
      OS << ", ";
    OS << typeToC(Func.Params[I].Type) << " " << Func.Params[I].Name;
  }
  if (Func.Params.empty())
    OS << "void";
  OS << ") {\n";

  std::set<std::string> ParamNames;
  for (auto &P : Func.Params)
    ParamNames.insert(P.Name);
  if (Func.FrameSize > 0 || Func.FrameHeadroom > 0)
    ParamNames.insert("frame_base");
  if (NeedsFrameStorage && (Func.FrameSize > 0 || Func.FrameHeadroom > 0)) {
    uint64_t LowerSize = checkedStackAlign(
        Func.FrameSize > 0 ? static_cast<uint64_t>(Func.FrameSize) : 0);
    uint64_t UpperSize = checkedStackAlign(
        Func.FrameHeadroom > 0 ? static_cast<uint64_t>(Func.FrameHeadroom) : 0);
    uint64_t EntryResidue =
        syntheticEntryStackResidue(Opts.TheArch, Opts.Format);
    uint64_t FrameBaseOffset = checkedStackAdd(LowerSize, EntryResidue);
    uint64_t StorageSize = checkedStackAdd(FrameBaseOffset, UpperSize);
    emitIndent(1);
    OS << "_Alignas(" << kSyntheticStackAlignment << ") uint8_t stack_storage["
       << StorageSize << "];\n";
    emitIndent(1);
    OS << "const uintptr_t frame_base = (uintptr_t)(stack_storage + "
       << FrameBaseOffset << ");\n";
  }
  emitLocalDecls(Func, ParamNames);

  writeStmts(Func.Body, 1);
  OS << "}\n";
}

} // namespace neverd
