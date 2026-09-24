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
#include "neverd/Limits.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <map>
#include <optional>

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

llvm::StringRef debugCallConvAttribute(DebugCallConv CC) {
  switch (CC) {
  case DebugCallConv::Cdecl:
    return "__attribute__((cdecl)) ";
  case DebugCallConv::Stdcall:
    return "__attribute__((stdcall)) ";
  case DebugCallConv::Thiscall:
    return "__attribute__((thiscall)) ";
  case DebugCallConv::Fastcall:
    return "__attribute__((fastcall)) ";
  case DebugCallConv::Unknown:
    return "";
  }
  return "";
}

std::optional<FunctionSym> debugFunction(DebugContext *Dbg, va_t Entry) {
  if (!Dbg)
    return std::nullopt;
  return Dbg->resolveFunction(Entry);
}

bool isWindowsLanguagePersonality(ExceptionPersonality Personality) {
  switch (Personality) {
  case ExceptionPersonality::CSpecificHandler:
  case ExceptionPersonality::CxxFrameHandler3:
  case ExceptionPersonality::CxxFrameHandler4:
  case ExceptionPersonality::GSHandlerCheckSEH:
  case ExceptionPersonality::GSHandlerCheckEH:
  case ExceptionPersonality::GSHandlerCheckEH4:
  case ExceptionPersonality::ExceptHandler3:
  case ExceptionPersonality::ExceptHandler4:
  case ExceptionPersonality::CxxFrameHandlerX86:
  case ExceptionPersonality::GxxPersonalitySEH0:
  case ExceptionPersonality::GccPersonalitySEH0:
  case ExceptionPersonality::GnuObjCPersonalitySEH0:
  case ExceptionPersonality::GnatPersonalitySEH0:
  case ExceptionPersonality::GdcPersonalitySEH0:
  case ExceptionPersonality::DelphiX86Handler:
  case ExceptionPersonality::DelphiExceptionHandler:
  case ExceptionPersonality::GoSEHTrampoline:
    return true;
  default:
    return false;
  }
}

bool hasX64LanguageHandlerFlags(const ExceptionFunction &EH) {
  if (EH.Encoding != ExceptionEncoding::X64UnwindV1 &&
      EH.Encoding != ExceptionEncoding::X64UnwindV2 &&
      EH.Encoding != ExceptionEncoding::X64UnwindV3)
    return false;
  // UNW_FLAG_EHANDLER and UNW_FLAG_UHANDLER are the low two flag bits.  The
  // third bit is a structural chained-unwind record and remains executable.
  return (EH.UnwindFlags & 0x3u) != 0;
}

bool carriesWindowsLanguageSemantics(const ExceptionFunction &EH) {
  if (EH.SEH || EH.Cxx || EH.GSCookie || EH.Registration || EH.Delphi ||
      EH.DelphiScopes)
    return true;

  const ExceptionModel Model = EH.model();
  if (isWindowsLanguagePersonality(EH.Personality))
    return true;

  // Every normalized registration record is itself language dispatch data;
  // there is no unwind-only x86 registration encoding in this model.
  if (Model == ExceptionModel::WindowsRegistration)
    return true;

  if (Model != ExceptionModel::WindowsTable)
    return false;

  // A Windows table record is executable only after positively proving that
  // it carries unwind state and nothing that could dispatch a handler.  Missing
  // or internally inconsistent decode evidence therefore fails closed.
  if (EH.ParseStatus != ExceptionParseStatus::Complete ||
      EH.Personality != ExceptionPersonality::None ||
      !EH.PersonalityName.empty() || EH.PersonalityVA != 0 ||
      EH.HandlerDataVA != 0 || hasX64LanguageHandlerFlags(EH) || EH.Rust ||
      EH.ObjC || EH.Go)
    return true;

  if (EH.DecodeProvenance && (EH.DecodeProvenance->Structural.ParseStatus !=
                                  ExceptionParseStatus::Complete ||
                              EH.DecodeProvenance->Language.ParseStatus !=
                                  ExceptionParseStatus::Complete))
    return true;

  return false;
}

struct X86CIntrinsicFeatures {
  unsigned GfniWidth = 0;
  bool HasVdbpsadbw = false;
  bool NeedsVdbpsadbwVL = false;
};

X86CIntrinsicFeatures collectX86CIntrinsicFeatures(const HighFunc &Func) {
  X86CIntrinsicFeatures Features;
  std::set<const HighExpr *> Seen;
  std::function<void(const HighExpr &)> Visit = [&](const HighExpr &Expr) {
    if (!Seen.insert(&Expr).second)
      return;
    using I = Intrinsic;
    if (Expr.IntrinsicId == I::Gf2p8MulB ||
        Expr.IntrinsicId == I::Gf2p8AffineQb ||
        Expr.IntrinsicId == I::Gf2p8AffineInvQb) {
      if (Expr.Type)
        Features.GfniWidth =
            std::max<unsigned>(Features.GfniWidth, Expr.Type->Size);
    } else if (Expr.IntrinsicId == I::Vdbpsadbw) {
      Features.HasVdbpsadbw = true;
      if (Expr.Type && Expr.Type->Size != 64)
        Features.NeedsVdbpsadbwVL = true;
    }
    for (const ExprPtr &Operand : Expr.Operands)
      if (Operand)
        Visit(*Operand);
  };
  walkStmts(Func.Body, [&](const HighStmt &Stmt) {
    forEachExpr(Stmt, [&](const ExprPtr &Expr) {
      if (Expr)
        Visit(*Expr);
    });
  });
  return Features;
}

std::string x86CIntrinsicTargetFeatures(const HighFunc &Func) {
  const X86CIntrinsicFeatures Required = collectX86CIntrinsicFeatures(Func);
  std::vector<std::string> Features;
  if (Required.GfniWidth == 32)
    Features.emplace_back("avx");
  else if (Required.GfniWidth == 64)
    Features.emplace_back("avx512f");
  if (Required.HasVdbpsadbw)
    Features.emplace_back("avx512bw");
  if (Required.NeedsVdbpsadbwVL)
    Features.emplace_back("avx512vl");
  if (Required.GfniWidth != 0)
    Features.emplace_back("gfni");

  std::string Result;
  for (const std::string &Feature : Features) {
    if (!Result.empty())
      Result += ",";
    Result += Feature;
  }
  return Result;
}

} // anonymous namespace

bool HighCWriter::isAnalysisOnlyFunction(const HighFunc &Func) const {
  if (Func.ExceptionMetadata &&
      carriesWindowsLanguageSemantics(*Func.ExceptionMetadata))
    return true;

  bool HasWindowsRegion = false;
  walkStmts(Func.Body, [&](const HighStmt &Stmt) {
    HasWindowsRegion |=
        Stmt.Kind == StmtKind::SEHTry || Stmt.Kind == StmtKind::CxxTry;
  });
  return HasWindowsRegion;
}

void HighCWriter::runAnalysisPasses(const HighFunc &Func) {
  GotoTargets.clear();
  EmittedLabels.clear();
  GotoTargetUses.clear();
  walkStmts(Func.Body, [&](const HighStmt &Stmt) {
    if (Stmt.Kind == StmtKind::Goto)
      ++GotoTargetUses[Stmt.GotoTarget];
  });
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
  analyzeUnusedAssigns(Analysis, Func, VarFn);
  analyzeStoreForwarding(Analysis, Func, VarFn, ExprFn);
  analyzeInferredNoreturn(Analysis, Func, VarFn);
  Analysis.AssignedVars.clear();
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (Analysis.DeadStmts.count(&S))
      return;
    if (S.Kind != StmtKind::Assign || !S.Dst || S.Dst->Kind != ExprKind::Var)
      return;
    const HighExpr *Val = S.Val.get();
    if (Val && isNoreturnCallExpr(Analysis, *Val))
      return;
    Analysis.AssignedVars.insert(VarFn(S.Dst->Var));
  });
  InferredVoid = analyzeVoidReturn(Analysis, Func, VarFn, ExprFn);

  HiLoPairs.clear();
  MultiOutputRenderedStmts.clear();
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
      if (!Rendered.empty()) {
        Analysis.DeadVars.insert(varName(S.Dst->Var));
        MultiOutputRenderedStmts.insert(&S);
      }
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
  auto VarFn = [this](const MedVar &V) {
    if (isCatchFuncletParentFrame(V))
      return std::string();
    return varName(V);
  };
  // Copy-forwarded temps print as their source (`arg0`), so collecting the
  // IR destination would declare a name that never appears in the body.
  auto PrintedVarFn = [this, &VarFn](const MedVar &V) {
    if (isCatchFuncletParentFrame(V))
      return std::string();
    return copyForwardName(VarFn(V));
  };

  std::map<std::string, TypeRef> UsedVars;
  std::map<std::string, std::string> ExplicitDeclarations;
  std::set<std::string> VisibleAssigned;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (Analysis.DeadStmts.count(&S) || stmtHiddenFromC(S))
      return;
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        S.Dst->Kind == ExprKind::Var) {
      std::string Name = varName(S.Dst->Var);
      if (S.Val->Kind == ExprKind::BinOp &&
          (S.Val->Op == NdOp::ATOMIC_ADD ||
           S.Val->Op == NdOp::ATOMIC_CMPXCHG) &&
          S.Val->Type) {
        ExplicitDeclarations[Name] = declarationToC(S.Val->Type, Name);
      } else if (S.Val->Kind == ExprKind::Call &&
                 S.Val->IntrinsicId == Intrinsic::A64_SvePtrue) {
        ExplicitDeclarations[Name] = "svbool_t " + Name;
      } else if (S.Val->Kind == ExprKind::Call &&
                 (S.Val->IntrinsicId == Intrinsic::A64_SveDup ||
                  S.Val->IntrinsicId == Intrinsic::A64_SveIndex)) {
        uint64_t ElemBytes = 1;
        size_t ElemIndex =
            S.Val->IntrinsicId == Intrinsic::A64_SveIndex ? 2 : 1;
        if (S.Val->Operands.size() > ElemIndex &&
            S.Val->Operands[ElemIndex]->Kind == ExprKind::Const)
          ElemBytes = S.Val->Operands[ElemIndex]->ConstVal;
        const std::string Type = ElemBytes == 2   ? "svuint16_t"
                                 : ElemBytes == 4 ? "svuint32_t"
                                 : ElemBytes == 8 ? "svuint64_t"
                                                  : "svuint8_t";
        ExplicitDeclarations[Name] = Type + " " + Name;
      }
    }
    if (!(InferredVoid && S.Kind == StmtKind::Return)) {
      forEachRhsExpr(S, [&](const ExprPtr &E) {
        if (E)
          collectUsedVarsExpr(*E, UsedVars, PrintedVarFn);
      });
    }
    // A multi-output intrinsic statement prints its own outputs and never
    // assigns the primary destination.
    if (S.Kind == StmtKind::Assign && S.Dst &&
        (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi) &&
        !isHiddenCopyForwardAssign(S) && !MultiOutputRenderedStmts.count(&S)) {
      const HighExpr *Val = S.Val.get();
      const bool ResultOmitted = Val && isNoreturnCallExpr(Analysis, *Val);
      if (!ResultOmitted) {
        collectUsedVarsExpr(*S.Dst, UsedVars, VarFn);
        VisibleAssigned.insert(varName(S.Dst->Var));
      }
    }
  });

  if (!Analysis.StoreFwd.empty()) {
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Store || !S.StoreAddr || !S.StoreVal)
        return;
      std::string Addr = exprStr(*S.StoreAddr);
      bool Forwarded = Analysis.StoreFwd.count(Addr) != 0;
      if (!Forwarded) {
        auto Key = Analysis.AddressKeys.find(S.StoreAddr.get());
        Forwarded = Key != Analysis.AddressKeys.end() &&
                    Analysis.StoreFwdByAddressKey.count(Key->second) != 0;
      }
      if (Forwarded)
        collectUsedVarsExpr(*S.StoreVal, UsedVars, PrintedVarFn);
    });
  }

  std::set<std::string> DeclaredNames(ParamNames);
  for (auto &Local : Func.Locals) {
    MedVar StackVar;
    StackVar.Kind = MedVar::Stack;
    StackVar.StackOff = Local.StackOff;
    StackVar.Size = Local.Type ? Local.Type->Size : 0;
    const std::string Name = varName(StackVar);
    if (DeclaredNames.count(Name))
      continue;
    if (UsedVars.find(Name) == UsedVars.end() &&
        UsedVars.find(Local.Name) == UsedVars.end())
      continue;
    if ((CopyForward.count(Name) || CopyForward.count(Local.Name)) &&
        !VisibleAssigned.count(Name) && !VisibleAssigned.count(Local.Name))
      continue;
    DeclaredNames.insert(Name);
    emitIndent(1);
    TypeRef Ty = Local.Type;
    if (Dbg && CurrentFunc) {
      if (auto Var = Dbg->resolveVariable(CurrentFunc->Entry, Local.StackOff);
          Var && Var->Type)
        Ty = Var->Type;
    }
    auto ExplicitTy = ExplicitDeclarations.find(Name);
    if (ExplicitTy == ExplicitDeclarations.end())
      ExplicitTy = ExplicitDeclarations.find(Local.Name);
    OS << (ExplicitTy == ExplicitDeclarations.end() ? declarationToC(Ty, Name)
                                                    : ExplicitTy->second)
       << ";\n";
  }

  for (auto &[Name, Ty] : UsedVars) {
    if (Name.empty() || DeclaredNames.count(Name))
      continue;
    if (CopyForward.count(Name) && !VisibleAssigned.count(Name))
      continue;
    DeclaredNames.insert(Name);
    emitIndent(1);
    auto ExplicitTy = ExplicitDeclarations.find(Name);
    OS << (ExplicitTy == ExplicitDeclarations.end() ? declarationToC(Ty, Name)
                                                    : ExplicitTy->second)
       << ";\n";
  }
  if (!DeclaredNames.empty())
    OS << "\n";
}

void HighCWriter::writeExceptionAnnotation(const HighFunc &Func) {
  if (!Opts.EmitComments || !Func.ExceptionMetadata)
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
      if (Scope.NormalizedFilterVA)
        OS << " normalized_filter=0x"
           << llvm::utohexstr(Scope.NormalizedFilterVA);
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
  if (GuardAnalysisOnlyFunctions && isAnalysisOnlyFunction(Func)) {
    writeAnalysisOnlyFunction(Func);
    return;
  }
  writeFunctionProjection(Func);
}

void HighCWriter::writeAnalysisOnlyFunction(const HighFunc &Func) {
  if (Opts.EmitComments) {
    OS << "/* neverd.analysis-only: recovered Windows SEH/C++ as readable C. "
          "*/\n";
  }
  writeFunctionProjection(Func);
}

void HighCWriter::collectNamedFrameSlots(const HighFunc &Func) {
  FrameSlots.clear();
  FrameAliases.clear();
  const bool SavedHandler = InEHClauseBody;
  std::function<void(const std::vector<HighStmt> &, bool)> WalkAliases;
  WalkAliases = [&](const std::vector<HighStmt> &Stmts, bool InHandler) {
    const bool Saved = InEHClauseBody;
    InEHClauseBody = InHandler;
    for (const HighStmt &S : Stmts) {
      if (!Analysis.DeadStmts.count(&S) && S.Kind == StmtKind::Assign &&
          S.Dst && S.Val && S.Dst->Kind == ExprKind::Var &&
          S.Dst->Var.Kind != MedVar::Param) {
        if (const auto Disp = frameDisplacement(*S.Val)) {
          const std::string Name = varName(S.Dst->Var);
          auto It = FrameAliases.find(Name);
          if (It == FrameAliases.end() || It->second != *Disp) {
            FrameAliases[Name] = *Disp;
          }
        }
      }
      WalkAliases(S.Body, InHandler);
      WalkAliases(S.ElseBody, InHandler);
      for (const auto &C : S.Cases)
        WalkAliases(C.Body, InHandler);
      WalkAliases(S.DefaultBody, InHandler);
      for (const auto &ClauseBody : S.EHClauseBodies)
        WalkAliases(ClauseBody, true);
    }
    InEHClauseBody = Saved;
  };
  bool Grew = true;
  unsigned Guard = 0;
  while (Grew && Guard++ < limits::kMaxFrameAliasFixedPoint) {
    const auto Before = FrameAliases;
    WalkAliases(Func.Body, false);
    Grew = FrameAliases != Before;
  }
  auto Note = [&](const HighExpr *Addr, const TypeRef &Ty,
                  bool AddressTaken = false, bool IsStore = false) {
    if (!Addr)
      return;
    const auto Disp = frameDisplacement(*Addr);
    if (!Disp)
      return;
    NamedFrameSlot &Slot = FrameSlots[*Disp];
    if (AddressTaken)
      Slot.AddressTaken = true;
    else
      Slot.UsedAsMemory = true;
    if (Slot.Name.empty()) {
      const uint64_t Mag = static_cast<uint64_t>(*Disp < 0 ? -*Disp : *Disp);
      Slot.Name = (*Disp < 0 ? "var_m" : "var_") + llvm::utohexstr(Mag);
      if (Dbg) {
        if (auto Var = Dbg->resolveVariable(Func.Entry, *Disp);
            Var && !Var->Name.empty())
          Slot.Name = Var->Name;
      }
    }
    if (Ty && (!Slot.Type || Ty->Size > Slot.Type->Size))
      Slot.Type = Ty;
    if (IsStore && Ty && Ty->Size &&
        (!Slot.MinStoreSize || Ty->Size < Slot.MinStoreSize))
      Slot.MinStoreSize = Ty->Size;
  };
  // A frame displacement used as a value prints as `&var_N`. Mark it
  // address-taken so copy-forward/DeadVars cannot drop the declaration.
  std::function<void(const HighExpr &, bool)> Walk = [&](const HighExpr &E,
                                                         bool AsAddress) {
    if (E.Kind == ExprKind::Load && !E.Operands.empty() && E.Operands[0]) {
      Note(E.Operands[0].get(), E.Type);
      Walk(*E.Operands[0], true);
      return;
    }
    if (E.Kind == ExprKind::Store && E.Operands.size() >= 2) {
      Note(E.Operands[0].get(), E.Operands[1] ? E.Operands[1]->Type : nullptr,
           /*AddressTaken=*/false, /*IsStore=*/true);
      if (E.Operands[0])
        Walk(*E.Operands[0], true);
      if (E.Operands[1])
        Walk(*E.Operands[1], false);
      return;
    }
    if (E.Kind == ExprKind::Addr && !E.Operands.empty() && E.Operands[0] &&
        E.Operands[0]->Kind == ExprKind::Load &&
        !E.Operands[0]->Operands.empty()) {
      Note(E.Operands[0]->Operands[0].get(), E.Operands[0]->Type,
           /*AddressTaken=*/true);
      if (E.Operands[0]->Operands[0])
        Walk(*E.Operands[0]->Operands[0], true);
      return;
    }
    if (!AsAddress && frameDisplacement(E)) {
      Note(&E, E.Type, /*AddressTaken=*/true);
      return;
    }
    for (const ExprPtr &Op : E.Operands)
      if (Op)
        Walk(*Op, AsAddress);
  };
  std::function<void(const std::vector<HighStmt> &, bool)> WalkNotes;
  WalkNotes = [&](const std::vector<HighStmt> &Stmts, bool InHandler) {
    const bool Saved = InEHClauseBody;
    InEHClauseBody = InHandler;
    for (const HighStmt &S : Stmts) {
      if (!Analysis.DeadStmts.count(&S) && !stmtHiddenFromC(S)) {
        if (S.Kind == StmtKind::Store && S.StoreAddr)
          Note(S.StoreAddr.get(), S.StoreVal ? S.StoreVal->Type : nullptr,
               /*AddressTaken=*/false, /*IsStore=*/true);
        if (S.Kind == StmtKind::Assign && S.Dst &&
            S.Dst->Kind == ExprKind::Load && !S.Dst->Operands.empty())
          Note(S.Dst->Operands[0].get(), S.Dst->Type, /*AddressTaken=*/false,
               /*IsStore=*/true);
        if (S.StoreAddr)
          Walk(*S.StoreAddr, true);
        if (S.StoreVal)
          Walk(*S.StoreVal, false);
        if (S.Dst)
          Walk(*S.Dst, S.Dst->Kind == ExprKind::Load);
        if (S.Val)
          Walk(*S.Val, false);
        if (S.Cond)
          Walk(*S.Cond, false);
        if (S.RetVal)
          Walk(*S.RetVal, false);
        if (S.CallExpr)
          Walk(*S.CallExpr, false);
        if (S.SwitchExpr)
          Walk(*S.SwitchExpr, false);
      }
      WalkNotes(S.Body, InHandler);
      WalkNotes(S.ElseBody, InHandler);
      for (const auto &C : S.Cases)
        WalkNotes(C.Body, InHandler);
      WalkNotes(S.DefaultBody, InHandler);
      for (const auto &ClauseBody : S.EHClauseBodies)
        WalkNotes(ClauseBody, true);
    }
    InEHClauseBody = Saved;
  };
  WalkNotes(Func.Body, false);
  InEHClauseBody = SavedHandler;
  for (auto &[Disp, Slot] : FrameSlots)
    if (!Slot.Type)
      Slot.Type = NdType::makeInt(4);

  // Slots that share bytes are one object, not separate variables: a byte
  // stored at -0x82 changes what a later read of the eight bytes at -0x84
  // sees.  Each group of overlapping slots lives in one storage, the group's
  // first slot; when that slot does not cover the whole group it is declared
  // as a byte array (the binary guarantees no alignment for it).  The other slots are accessed through it.
  SharedFrameStorage.clear();
  auto SlotEnd = [](const std::pair<const int64_t, NamedFrameSlot> &Entry) {
    return Entry.first +
           static_cast<int64_t>(std::max<uint64_t>(Entry.second.Type->Size, 1));
  };
  for (auto It = FrameSlots.begin(); It != FrameSlots.end();) {
    auto First = It;
    int64_t End = SlotEnd(*It);
    for (++It; It != FrameSlots.end() && It->first < End; ++It)
      End = std::max(End, SlotEnd(*It));
    if (std::next(First) == It)
      continue;
    NamedFrameSlot &Owner = First->second;
    const bool Covers = SlotEnd(*First) >= End;
    if (!Covers)
      Owner.RegionBytes = End - First->first;
    for (auto Member = First; Member != It; ++Member) {
      if (Member == First && Covers)
        continue;
      NamedFrameSlot &Slot = Member->second;
      Slot.Outer = Owner.Name;
      Slot.OuterOffset = Member->first - First->first;
      Slot.Interior = "(*(" + memoryTypeName(Slot.Type) + " *)((char *)&" +
                      Slot.Outer + " + " + std::to_string(Slot.OuterOffset) +
                      "))";
      SharedFrameStorage.insert(Slot.Interior);
    }
    SharedFrameStorage.insert(Owner.Name);
  }
  // A store narrower than its slot changes only some of the slot's bytes.
  for (auto &[Disp, Slot] : FrameSlots)
    if (Slot.Interior.empty() && Slot.MinStoreSize &&
        Slot.MinStoreSize < Slot.Type->Size)
      SharedFrameStorage.insert(Slot.Name);
  for (auto &[Disp, Slot] : FrameSlots)
    if (SharedFrameStorage.count(Slot.Name))
      Slot.AddressTaken = true;
}

size_t HighCWriter::emittedParamCount(const HighFunc &Func) const {
  return emittedParamIndices(Func).size();
}

std::vector<size_t>
HighCWriter::emittedParamIndices(const HighFunc &Func) const {
  std::vector<size_t> All;
  const auto DebugFn = debugFunction(Dbg, Func.Entry);
  const size_t N = Func.Params.size();
  if (DebugFn && !DebugFn->Params.empty()) {
    const size_t Count = std::min(N, DebugFn->Params.size());
    All.resize(Count);
    for (size_t I = 0; I < Count; ++I)
      All[I] = I;
    return All;
  }
  if (Func.SourceTypeHint) {
    All.resize(N);
    for (size_t I = 0; I < N; ++I)
      All[I] = I;
    return All;
  }
  std::set<int> Used;
  std::function<void(const HighExpr &)> Walk = [&](const HighExpr &E) {
    if ((E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) &&
        E.Var.Kind == MedVar::Param)
      Used.insert(E.Var.Id);
    for (const ExprPtr &Op : E.Operands)
      if (Op)
        Walk(*Op);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (E)
        Walk(*E);
    });
  });
  size_t Unused = 0;
  for (size_t I = 0; I < N; ++I)
    if (!Used.count(static_cast<int>(I)))
      ++Unused;
  if (Unused < limits::kMinUnusedParamsToCompact || Used.empty()) {
    All.resize(N);
    for (size_t I = 0; I < N; ++I)
      All[I] = I;
    return All;
  }
  std::vector<int> Order(Used.begin(), Used.end());
  std::sort(Order.begin(), Order.end());
  for (int Id : Order)
    if (Id >= 0 && static_cast<size_t>(Id) < N)
      All.push_back(static_cast<size_t>(Id));
  if (All.empty()) {
    All.resize(N);
    for (size_t I = 0; I < N; ++I)
      All[I] = I;
  }
  return All;
}

void HighCWriter::writeFunctionProjection(const HighFunc &Func) {
  CurrentFunc = &Func;
  CopyForward.clear();
  ParamDisplayNames.clear();
  Analysis = {};
  runAnalysisPasses(Func);
  collectNamedFrameSlots(Func);
  collectCopyForward(Func);
  {
    const std::vector<size_t> Indices = emittedParamIndices(Func);
    if (Indices.size() != Func.Params.size())
      for (size_t I = 0; I < Indices.size(); ++I)
        ParamDisplayNames[static_cast<int>(Indices[I])] =
            "arg" + std::to_string(I);
  }

  bool NeedsFrameStorage = false;
  auto VisitFrameUses = [&](auto &&Self, const HighExpr &Expr) -> void {
    if (NeedsFrameStorage)
      return;
    if (isNamedFrameMemory(Expr) || namedFrameSlot(Expr))
      return;
    if (Expr.Kind == ExprKind::Load && !Expr.Operands.empty()) {
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
        Self(Self, *Operand);
  };
  walkStmts(Func.Body, [&](const HighStmt &Stmt) {
    if (NeedsFrameStorage || Analysis.DeadStmts.count(&Stmt) ||
        (InferredVoid && Stmt.Kind == StmtKind::Return))
      return;
    if (Stmt.Kind == StmtKind::Assign && Stmt.Dst && Stmt.Val &&
        Stmt.Dst->Kind == ExprKind::Var &&
        Stmt.Dst->Var.Kind != MedVar::Param && frameDisplacement(*Stmt.Val))
      return;
    if (Stmt.Kind == StmtKind::Store && Stmt.StoreAddr &&
        namedFrameSlot(*Stmt.StoreAddr)) {
      if (Stmt.StoreVal)
        VisitFrameUses(VisitFrameUses, *Stmt.StoreVal);
      return;
    }
    if (Stmt.Kind == StmtKind::Assign && Stmt.Dst &&
        Stmt.Dst->Kind == ExprKind::Load && !Stmt.Dst->Operands.empty() &&
        namedFrameSlot(*Stmt.Dst->Operands[0])) {
      if (Stmt.Val)
        VisitFrameUses(VisitFrameUses, *Stmt.Val);
      return;
    }
    if (Stmt.Kind == StmtKind::Assign && Stmt.Dst)
      VisitFrameUses(VisitFrameUses, *Stmt.Dst);
    forEachRhsExpr(Stmt, [&](const ExprPtr &Expr) {
      if (Expr)
        VisitFrameUses(VisitFrameUses, *Expr);
    });
  });
  // The structural scan above only sees stack addresses.  A value computed
  // from the entry stack pointer itself (for example the flags PUSHFQ saves
  // after `sub rsp, 10h`) still prints `frame_base`; declare it as the
  // entry stack pointer instead of giving up the named frame slots.
  // Statement-only intrinsics (division preconditions, string operations)
  // cannot be printed as expressions; scan their operands instead.
  auto ScanText = [&](auto &&Self, const HighExpr &Expr) -> std::string {
    if (Expr.Kind != ExprKind::Call || Expr.IntrinsicId == Intrinsic::None)
      return exprStr(Expr);
    std::string Text;
    for (const ExprPtr &Operand : Expr.Operands)
      if (Operand)
        Text += Self(Self, *Operand) + " ";
    return Text;
  };
  bool NeedsEntryStackPointer = false;
  if (!NeedsFrameStorage)
    walkStmts(Func.Body, [&](const HighStmt &Stmt) {
      if (NeedsEntryStackPointer || Analysis.DeadStmts.count(&Stmt) ||
          stmtHiddenFromC(Stmt) ||
          (InferredVoid && Stmt.Kind == StmtKind::Return))
        return;
      forEachRhsExpr(Stmt, [&](const ExprPtr &Expr) {
        if (Expr &&
            ScanText(ScanText, *Expr).find("frame_base") != std::string::npos)
          NeedsEntryStackPointer = true;
      });
    });
  if (NeedsFrameStorage) {
    FrameSlots.clear();
    FrameAliases.clear();
  } else {
    std::map<std::string, std::vector<const HighStmt *>> Displacements;
    walkStmts(Func.Body, [&](const HighStmt &Stmt) {
      if (Stmt.Kind != StmtKind::Assign || !Stmt.Dst || !Stmt.Val ||
          Stmt.Dst->Kind != ExprKind::Var ||
          Stmt.Dst->Var.Kind == MedVar::Param || !frameDisplacement(*Stmt.Val))
        return;
      const std::string Name = varName(Stmt.Dst->Var);
      Analysis.DeadStmts.insert(&Stmt);
      Analysis.DeadVars.insert(Name);
      Displacements[Name].push_back(&Stmt);
    });
    // A frame address is normally printed as `&var_mN` wherever it is used.
    // One still printed by name (arithmetic on the address, such as the
    // flags of `sub rsp, 10h`) needs its assignment.
    if (!Displacements.empty())
      walkStmts(Func.Body, [&](const HighStmt &Stmt) {
        if (Analysis.DeadStmts.count(&Stmt) || stmtHiddenFromC(Stmt))
          return;
        forEachRhsExpr(Stmt, [&](const ExprPtr &Expr) {
          // A whole-value use is a copy of the address, which the frame
          // alias printing already handles; only arithmetic needs the name.
          if (!Expr || Displacements.empty() || Expr->Kind == ExprKind::Var)
            return;
          const std::string Text = ScanText(ScanText, *Expr);
          for (auto It = Displacements.begin(); It != Displacements.end();) {
            const std::string &Name = It->first;
            bool Used = false;
            for (size_t Pos = Text.find(Name); Pos != std::string::npos;
                 Pos = Text.find(Name, Pos + 1)) {
              auto Ident = [](char C) {
                return std::isalnum(static_cast<unsigned char>(C)) || C == '_';
              };
              if ((Pos == 0 || !Ident(Text[Pos - 1])) &&
                  (Pos + Name.size() >= Text.size() ||
                   !Ident(Text[Pos + Name.size()]))) {
                Used = true;
                break;
              }
            }
            if (!Used) {
              ++It;
              continue;
            }
            for (const HighStmt *Def : It->second)
              Analysis.DeadStmts.erase(Def);
            Analysis.DeadVars.erase(Name);
            It = Displacements.erase(It);
          }
        });
      });
  }

  const auto ReturnType = InferredVoid ? NdType::makeVoid() : FuncReturnType;

  std::string FName = functionIdentifier(Func);

  if (EmitFunctionWrapper && Func.Entry)
    OS << "/* neverd.entry: 0x" << llvm::utohexstr(Func.Entry) << " */\n";

  if (Opts.EmitComments && !Func.DebugName.empty() &&
      Func.DebugName != Func.Name) {
    if (!EmitFunctionWrapper)
      emitIndent(1);
    OS << "/* " << Func.DebugName;
    if (!Func.SourceFile.empty())
      OS << " @ " << Func.SourceFile << ":" << Func.SourceLine;
    OS << " */\n";
  }

  writeExceptionAnnotation(Func);

  if (EmitFunctionWrapper) {
    if (Opts.TheArch == Arch::X86 || Opts.TheArch == Arch::X64) {
      const std::string TargetFeatures = x86CIntrinsicTargetFeatures(Func);
      if (!TargetFeatures.empty())
        OS << "__attribute__((target(\"" << TargetFeatures << "\")))\n";
    }

    if (Func.DoesNotReturn)
      OS << "_Noreturn ";
    const auto DebugFn = debugFunction(Dbg, Func.Entry);
    if (DebugFn && DebugFn->CallConv != DebugCallConv::Unknown)
      OS << debugCallConvAttribute(DebugFn->CallConv);
    else if (Func.SourceTypeHint)
      OS << sourceConventionAttribute(Func.SourceTypeHint->Convention);
    CProjectionIdentifierAllocator ParameterIdentifiers;
    std::string Declarator = FName + "(";
    const std::vector<size_t> ParamIndices = emittedParamIndices(Func);
    for (size_t I = 0; I < ParamIndices.size(); ++I) {
      if (I > 0)
        Declarator += ", ";
      const size_t PI = ParamIndices[I];
      TypeRef Ty = Func.Params[PI].Type;
      std::string Name = Func.Params[PI].Name;
      if (auto It = ParamDisplayNames.find(static_cast<int>(PI));
          It != ParamDisplayNames.end())
        Name = It->second;
      if (DebugFn && PI < DebugFn->Params.size()) {
        if (DebugFn->Params[PI].second)
          Ty = DebugFn->Params[PI].second;
        if (!DebugFn->Params[PI].first.empty())
          Name = DebugFn->Params[PI].first;
      }
      Declarator +=
          declarationToC(Ty, ParameterIdentifiers.allocate(Name, "nd_arg"));
    }
    if (ParamIndices.empty())
      Declarator += "void";
    OS << declarationToC(ReturnType, Declarator + ")") << " {\n";
  }
  if (Func.Body.empty()) {
    // Conversion produced no statements.  An empty `{ }` looks like a
    // successful leaf; trap instead so coverage cannot treat this as a body.
    emitIndent(1);
    OS << "/* neverd: no structured body */\n";
    if (EmitFunctionWrapper) {
      emitIndent(1);
      OS << "__builtin_trap();\n";
      OS << "}\n";
    }
    return;
  }

  std::set<std::string> ParamNames;
  for (auto &P : Func.Params)
    ParamNames.insert(P.Name);
  if (Func.FrameSize > 0 || Func.FrameHeadroom > 0 || NeedsEntryStackPointer)
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
  } else if (NeedsEntryStackPointer) {
    emitIndent(1);
    OS << "const uintptr_t frame_base = "
          "(uintptr_t)_AddressOfReturnAddress();\n";
  }
  std::set<std::string> PrintedAddrSlots;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (Analysis.DeadStmts.count(&S) || stmtHiddenFromC(S))
      return;
    if (S.Dst && S.Dst->Kind == ExprKind::Load && !S.Dst->Operands.empty() &&
        S.Dst->Operands[0]) {
      if (auto Name = namedFrameSlot(*S.Dst->Operands[0]);
          Name && !CopyForward.count(*Name))
        PrintedAddrSlots.insert(*Name);
    }
    // A store prints its slot by name even when the address is a variable
    // that aliases the frame.
    if (S.Kind == StmtKind::Store && S.StoreAddr)
      if (auto Name = namedFrameSlot(*S.StoreAddr);
          Name && !CopyForward.count(*Name))
        PrintedAddrSlots.insert(*Name);
    forEachRhsExpr(S, [&](const ExprPtr &E) {
      if (!E)
        return;
      std::function<void(const HighExpr &, bool)> Walk = [&](const HighExpr &N,
                                                             bool AsAddress) {
        // Vars that alias a frame displacement still print as the variable.
        // Only BinOp/Addr value uses become `&var_N` in C.
        if (!AsAddress &&
            (N.Kind == ExprKind::BinOp || N.Kind == ExprKind::Addr))
          if (auto Name = namedFrameSlot(N))
            PrintedAddrSlots.insert(*Name);
        if (N.Kind == ExprKind::Load && !N.Operands.empty() && N.Operands[0]) {
          if (auto Name = namedFrameSlot(*N.Operands[0]);
              Name && !CopyForward.count(*Name))
            PrintedAddrSlots.insert(*Name);
          Walk(*N.Operands[0], true);
          return;
        }
        if (N.Kind == ExprKind::Store && N.Operands.size() >= 2) {
          if (N.Operands[0]) {
            if (auto Name = namedFrameSlot(*N.Operands[0]);
                Name && !CopyForward.count(*Name))
              PrintedAddrSlots.insert(*Name);
            Walk(*N.Operands[0], true);
          }
          if (N.Operands[1])
            Walk(*N.Operands[1], false);
          return;
        }
        for (const ExprPtr &Op : N.Operands)
          if (Op)
            Walk(*Op, AsAddress);
      };
      Walk(*E, false);
    });
  });
  // An enclosing slot is printed whenever one of its interior accesses is.
  for (const auto &[Disp, Slot] : FrameSlots)
    if (!Slot.Interior.empty() && PrintedAddrSlots.count(Slot.Interior))
      PrintedAddrSlots.insert(Slot.Outer);
  for (const auto &[Disp, Slot] : FrameSlots) {
    if (!Slot.Interior.empty() && !Slot.RegionBytes)
      continue;
    if (ParamNames.count(Slot.Name))
      continue;
    if (!PrintedAddrSlots.count(Slot.Name))
      continue;
    if (!Slot.AddressTaken &&
        (CopyForward.count(Slot.Name) || Analysis.DeadVars.count(Slot.Name)))
      continue;
    ParamNames.insert(Slot.Name);
    emitIndent(1);
    if (Slot.RegionBytes)
      OS << "uint8_t " << Slot.Name << "[" << Slot.RegionBytes << "];\n";
    else
      OS << declarationToC(Slot.Type, Slot.Name) << ";\n";
  }
  emitLocalDecls(Func, ParamNames);

  writeStmts(Func.Body, 1);
  if (EmitFunctionWrapper)
    OS << "}\n";
}

} // namespace neverd
