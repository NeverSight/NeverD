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

#include "../VariadicImportStub.h"
#include "../X86GetPcThunk.h"
#include "HighCWriter.h"

#include "neverd/ArchSupport.h"
#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/ir/SourceParameterPlacement.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
    return "__fastcall ";
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
  case ExceptionPersonality::GSHandlerCheck:
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

uint16_t pointerBytes(Arch A) {
  return (A == Arch::X86 || A == Arch::ARM) ? 4 : 8;
}

uint16_t countedBytes(const HighExpr &Operand) {
  if (Operand.Type)
    return Operand.Type->Kind == NdTypeKind::Int ? Operand.Type->Size : 0;
  return Operand.Kind == ExprKind::Var || Operand.Kind == ExprKind::Phi
             ? Operand.Var.Size
             : 0;
}

unsigned countedBits(const HighExpr &Operand) {
  switch (countedBytes(Operand)) {
  case 1:
  case 2:
  case 4:
    return 32;
  case 8:
    return 64;
  default:
    return 0;
  }
}

TypeRef HighCWriter::declaredFunctionReturnType(const HighFunc &Func) const {
  // A bound source ABI is authoritative even when optional debug information
  // disagrees. Debug-only projections still use one type for their declaration,
  // body and return-value conversion.
  if (Func.SourceTypeHint && Func.SourceTypeHint->ReturnType)
    return Func.SourceTypeHint->ReturnType;
  const auto DebugFn = debugFunction(Dbg, Func.Entry);
  if (!DebugFn || !DebugFn->ReturnType) {
    // An MSVC destructor's decoration has no return type, and its callers
    // declare it void: what its tail call leaves in RAX is not a result.
    if (isMsvcDestructorName(Func.Name))
      return NdType::makeVoid();
    return {};
  }
  if (isMsvcIndirectReturn(DebugFn->ReturnType, Opts.TheArch, Opts.Format)) {
    const NdType *Record = msvcIndirectReturnRecord(DebugFn->ReturnType,
                                                    Opts.TheArch, Opts.Format);
    return NdType::makePtr(
        NdType::makeNamedRecord(cNamedTypeSpelling(Record->SourceName),
                                Record->Size ? Record->Size : 8));
  }
  // A return type C cannot spell keeps the recovered machine type, as a
  // parameter's does.
  if (!hasCValueLayout(cDisplayType(DebugFn->ReturnType)))
    return {};
  return DebugFn->ReturnType;
}

void HighCWriter::prepareFunctionReturns(std::vector<HighFunc> &Funcs) const {
  for (HighFunc &Func : Funcs) {
    if (TypeRef Declared = declaredFunctionReturnType(Func))
      Func.ReturnType = std::move(Declared);
    if (!Func.ReturnType || Func.ReturnType->Kind != NdTypeKind::Void)
      continue;

    // A discarded result is not a discarded evaluation. Materialize effects
    // at the original return before liveness and frame analysis, so their
    // operands remain defined and their storage stays alive.
    auto Prepare = [&](auto &&Self, std::vector<HighStmt> &Body) -> void {
      std::vector<HighStmt> Projected;
      Projected.reserve(Body.size());
      for (HighStmt &Stmt : Body) {
        Self(Self, Stmt.Body);
        Self(Self, Stmt.ElseBody);
        Self(Self, Stmt.DefaultBody);
        for (auto &Case : Stmt.Cases)
          Self(Self, Case.Body);
        for (auto &Clause : Stmt.EHClauseBodies)
          Self(Self, Clause);
        if (Stmt.Kind == StmtKind::Return && Stmt.RetVal &&
            Stmt.CxxContinuationReturnTargets.empty()) {
          if (highCExpressionHasEffect(*Stmt.RetVal)) {
            HighStmt Evaluate;
            Evaluate.Kind = StmtKind::ExprStmt;
            Evaluate.Addr = Stmt.Addr;
            Evaluate.Val = Stmt.RetVal;
            Stmt.Addr = 0;
            Projected.push_back(std::move(Evaluate));
          }
          Stmt.RetVal.reset();
        }
        Projected.push_back(std::move(Stmt));
      }
      Body = std::move(Projected);
    };
    Prepare(Prepare, Func.Body);
  }
}

bool HighCWriter::highIRIncludesIndirectReturn(const HighFunc &Func,
                                               const FunctionSym &FS) const {
  return isMsvcIndirectReturn(FS.ReturnType, Opts.TheArch, Opts.Format) &&
         Func.Params.size() == FS.Params.size() + 1;
}

bool HighCWriter::isWin64MemberIndirectReturn(const FunctionSym &FS) const {
  return isMsvcIndirectReturn(FS.ReturnType, Opts.TheArch, Opts.Format) &&
         !FS.Params.empty() && FS.Params[0].first == "this";
}

int HighCWriter::indirectReturnParamId(const FunctionSym &FS) const {
  return isWin64MemberIndirectReturn(FS) ? 1 : 0;
}

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
  SEHExceptionCodeNames.clear();
  walkStmts(Func.Body, [&](const HighStmt &Stmt) {
    std::function<void(const HighExpr &)> Visit = [&](const HighExpr &E) {
      if ((E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) &&
          E.Var.Kind == MedVar::SEHExceptionCode)
        SEHExceptionCodeNames.emplace(E.Var.ConstVal, varName(E.Var));
      E.forEachChildExpr([&](const ExprPtr &Child) { Visit(*Child); });
    };
    for (const ExprPtr *E :
         {&Stmt.Cond, &Stmt.Dst, &Stmt.Val, &Stmt.CallExpr, &Stmt.RetVal,
          &Stmt.StoreAddr, &Stmt.StoreVal, &Stmt.SwitchExpr})
      if (*E)
        Visit(**E);
  });
  GotoTargets.clear();
  EmittedLabels.clear();
  GotoTargetUses.clear();
  walkStmts(Func.Body, [&](const HighStmt &Stmt) {
    if (Stmt.Kind == StmtKind::Goto)
      ++GotoTargetUses[Stmt.GotoTarget];
  });
  // Handler labels first: a try body keeps a trailing goto that owns one.
  walkStmts(Func.Body, [&](const HighStmt &Stmt) {
    for (const HighEHClause &Clause : Stmt.EHClauses) {
      auto Reachable = [&](va_t Address) {
        return Address && Func.ExceptionMetadata &&
               Func.ExceptionMetadata->ownsCode(Address);
      };
      if (Clause.Kind == HighEHClauseKind::SEHExcept &&
          Reachable(Clause.HandlerVA)) {
        // An attached `__except` body is already printed in-place.  A label
        // is only required when HighC still has to `goto` the handler.
        const size_t Index = &Clause - Stmt.EHClauses.data();
        if (Index < Stmt.EHClauseBodies.size() &&
            !Stmt.EHClauseBodies[Index].empty())
          continue;
        GotoTargets.insert(Clause.HandlerVA);
      }
      // An Itanium landing pad is a block of this function, so the clause
      // comment can point at a real label instead of a bare address.
      for (va_t Pad : Clause.LandingPadVAs)
        if (Reachable(Pad))
          GotoTargets.insert(Pad);
    }
  });
  FallthroughTryExits.clear();
  {
    std::set<std::pair<va_t, va_t>> Kept;
    decideTryExits(Func.Body, {}, Kept);
    for (const auto &Exit : Kept)
      FallthroughTryExits.erase(Exit);
  }
  collectGotoTargets(Func.Body);

  auto VarFn = [this](const MedVar &V) { return varName(V); };
  auto ExprFn = [this](const HighExpr &E) { return exprStr(E); };
  auto ArgLimit = [this](const HighExpr &E) { return debugCallArgLimit(E); };
  auto AllArgs = [](const HighExpr &E) { return E.Operands.size(); };
  collectUnknownOnlyNames(Func);
  analyzeDeadStores(Analysis, Func, VarFn, ExprFn);
  analyzeUnusedAssigns(Analysis, Func, VarFn, AllArgs);
  analyzeUnusedCallResults(Analysis, Func, VarFn, ArgLimit);
  analyzeStoreForwarding(Analysis, Func, VarFn, ExprFn);
  Analysis.AssignedVars.clear();
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (Analysis.DeadStmts.count(&S))
      return;
    const HighExpr *Call =
        S.Kind == StmtKind::Call ? S.CallExpr.get() : S.Val.get();
    if (Call && Call->Kind == ExprKind::Call)
      for (const MedVar &Output : Call->IntrinsicOutputs)
        Analysis.AssignedVars.insert(VarFn(Output));
    if (S.Kind != StmtKind::Assign || !S.Dst || S.Dst->Kind != ExprKind::Var)
      return;
    const HighExpr *Val = S.Val.get();
    if (Val &&
        (isNoreturnCallExpr(*Val) || Analysis.OmittedCallResults.count(&S)))
      return;
    Analysis.AssignedVars.insert(VarFn(S.Dst->Var));
  });
  const TypeRef DeclaredReturn = declaredFunctionReturnType(Func);
  InferredVoid = DeclaredReturn ? DeclaredReturn->Kind == NdTypeKind::Void
                                : analyzeVoidReturn(Func);

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

  FuncReturnType = InferredVoid
                       ? nullptr
                       : (DeclaredReturn ? DeclaredReturn : Func.ReturnType);
}

void HighCWriter::emitLocalDecls(const HighFunc &Func,
                                 const std::set<std::string> &ParamNames) {
  auto IsStorageSlotName = [this](llvm::StringRef Name) {
    if (!ProjectFrameAliasesIntoStorage)
      return false;
    for (const auto &[Disp, Slot] : FrameStorageSlots) {
      (void)Disp;
      if (Slot.Name == Name && !isCxxCatchObjectName(Name))
        return true;
    }
    return false;
  };
  auto VarFn = [this](const MedVar &V) {
    if (isCatchFuncletParentFrame(V))
      return std::string();
    return varName(V);
  };
  // Copy-forwarded temps print as their source (`arg0`), so collecting the
  // IR destination would declare a name that never appears in the body.
  auto PrintedVarFn = [this, &VarFn, &IsStorageSlotName](const MedVar &V) {
    if (isCatchFuncletParentFrame(V))
      return std::string();
    const std::string Name = copyForwardName(VarFn(V));
    // Frame-pointer temps print as `&var_N` / slot names, not `v0`.
    if (Name.find('.') != std::string::npos)
      return std::string();
    for (const auto &[Disp, Slot] : FrameSlots)
      if (Slot.Name == Name)
        return std::string();
    if (FrameAliases.count(Name) || IsStorageSlotName(Name) ||
        FieldForward.count(Name) || ValueForward.count(Name) ||
        CtorThisForward.count(Name) || CatchAliasTemps.count(Name) ||
        UnknownOnlyNames.count(Name))
      return std::string();
    if (auto It = CallResultNames.find(Name); It != CallResultNames.end())
      return It->second;
    return Name;
  };

  std::map<std::string, TypeRef> UsedVars;
  std::map<std::string, std::string> ExplicitDeclarations;
  std::set<std::string> VisibleAssigned;
  std::function<void(const std::vector<HighStmt> &, bool)> CollectPrinted;
  CollectPrinted = [&](const std::vector<HighStmt> &Stmts, bool InCleanup) {
    for (const HighStmt &S : Stmts) {
      const bool PrintDeadJoin =
          Analysis.DeadStmts.count(&S) && S.Kind == StmtKind::Assign && S.Dst &&
          (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi) &&
          AmbiguousFrameAliases.count(varName(S.Dst->Var));
      if ((!Analysis.DeadStmts.count(&S) || PrintDeadJoin) &&
          !stmtHiddenFromC(S)) {
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
        const HighExpr *Call =
            S.Kind == StmtKind::Call ? S.CallExpr.get() : S.Val.get();
        if (Call && Call->Kind == ExprKind::Call) {
          for (const MedVar &Output : Call->IntrinsicOutputs) {
            const std::string Name = varName(Output);
            if (Analysis.DeadVars.count(Name) || ParamNames.count(Name) ||
                Output.Size == 0)
              continue;
            UsedVars.try_emplace(Name, NdType::makeInt(Output.Size, false));
            VisibleAssigned.insert(Name);
          }
        }
        const bool SkipCleanupReturn = InCleanup && S.Kind == StmtKind::Return;
        if (!SkipCleanupReturn &&
            !(InferredVoid && S.Kind == StmtKind::Return)) {
          forEachRhsExpr(S, [&](const ExprPtr &E) {
            if (!E)
              return;
            int64_t Delta = 0;
            if (S.Kind == StmtKind::Store && E.get() == S.StoreVal.get() &&
                isInplaceAddStore(S, Delta))
              return;
            // `&var_N` / `var_N` frame addresses are already declared as
            // slots.  Walking into them would collect the frame-pointer
            // temp (`v2`) that never appears in C.
            std::function<void(const HighExpr &)> Rec = [&](const HighExpr &N) {
              if (namedFrameSlot(N))
                return;
              if (N.Kind == ExprKind::Load && !N.Operands.empty() &&
                  N.Operands[0] && namedFrameSlot(*N.Operands[0]))
                return;
              if (isNamedValueExpr(N)) {
                if (const std::string Name = PrintedVarFn(N.Var);
                    !Name.empty() && UsedVars.find(Name) == UsedVars.end())
                  UsedVars[Name] = N.Type;
                return;
              }
              size_t Limit = N.Operands.size();
              if (N.Kind == ExprKind::Call)
                Limit = std::min(Limit, debugCallArgLimit(N));
              for (size_t I = 0; I < Limit; ++I)
                if (N.Operands[I])
                  Rec(*N.Operands[I]);
              if (N.IndirectTarget)
                Rec(*N.IndirectTarget);
            };
            Rec(*E);
          });
        }
        if (S.Kind == StmtKind::Assign && S.Dst &&
            (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi) &&
            !isHiddenCopyForwardAssign(S)) {
          const HighExpr *Val = S.Val.get();
          const bool ResultOmitted =
              Val && (isNoreturnCallExpr(*Val) ||
                      Analysis.OmittedCallResults.count(&S));
          if (!ResultOmitted) {
            // Copy-forward aliases affect RHS uses, not a printed lvalue.
            // The assignment still names its destination (`t8_1 = var_m10`).
            const std::string Name =
                printedForwardedVar(varName(S.Dst->Var), 0);
            const bool Identifier =
                !Name.empty() &&
                (std::isalpha(static_cast<unsigned char>(Name.front())) ||
                 Name.front() == '_') &&
                std::all_of(Name.begin(), Name.end(), [](char C) {
                  return std::isalnum(static_cast<unsigned char>(C)) ||
                         C == '_';
                });
            // A frame alias that still prints as an ordinary assignment needs
            // its own C local. Only assignments projected to a storage slot
            // are suppressed by stmtHiddenFromC above.
            if (Identifier && !ParamNames.count(Name) &&
                !IsStorageSlotName(Name)) {
              UsedVars.try_emplace(Name, S.Dst->Type);
              VisibleAssigned.insert(Name);
            }
          }
        }
      }
      CollectPrinted(S.Body, InCleanup);
      CollectPrinted(S.ElseBody, InCleanup);
      for (const auto &C : S.Cases)
        CollectPrinted(C.Body, InCleanup);
      CollectPrinted(S.DefaultBody, InCleanup);
      for (size_t I = 0; I < S.EHClauseBodies.size(); ++I) {
        const bool Cleanup =
            I < S.EHClauses.size() &&
            S.EHClauses[I].Kind == HighEHClauseKind::CxxCleanup;
        CollectPrinted(S.EHClauseBodies[I], Cleanup);
      }
    }
  };
  CollectPrinted(Func.Body, false);

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

  // Each `__except` arm captures GetExceptionCode() into its handler's name,
  // so the name is declared here even when no printed statement reads it,
  // and never again as a late declaration.
  for (const auto &[HandlerVA, Name] : SEHExceptionCodeNames) {
    (void)HandlerVA;
    UsedVars.try_emplace(Name, NdType::makeInt(4, false));
    VisibleAssigned.insert(Name);
  }
  // A landing pad receives the exception object and its selector from the
  // unwinder, which no C statement models: the names are declared wherever
  // the pad's code reads them.
  walkStmts(Func.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::vector<const HighExpr *> Work{Root.get()};
      while (!Work.empty()) {
        const HighExpr *E = Work.back();
        Work.pop_back();
        if (!E)
          continue;
        if ((E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) &&
            (E->Var.Kind == MedVar::EHException ||
             E->Var.Kind == MedVar::EHSelector))
          VisibleAssigned.insert(varName(E->Var));
        for (const ExprPtr &Operand : E->Operands)
          Work.push_back(Operand.get());
        Work.push_back(E->IndirectTarget.get());
      }
    });
  });

  // Every variable the IR mentions, under its own and its forwarded name, is
  // a candidate for a late declaration if the rendered body names it.
  {
    std::map<std::string, TypeRef> AllVars;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) {
        if (!E)
          return;
        collectUsedVarsExpr(*E, AllVars, VarFn);
        collectUsedVarsExpr(*E, AllVars, PrintedVarFn);
      });
    });
    for (const auto &[Name, Ty] : AllVars)
      if (!Name.empty() && !UsedVars.count(Name))
        DeferredDecls.emplace(Name, DeferredDecl{Ty, {}});
  }

  std::set<std::string> DeclaredNames(ParamNames);
  for (auto &Local : Func.Locals) {
    MedVar StackVar;
    StackVar.Kind = MedVar::Stack;
    StackVar.StackOff = Local.StackOff;
    StackVar.Size = Local.Type ? Local.Type->Size : 0;
    const std::string Name = varName(StackVar);
    if (DeclaredNames.count(Name) || IsStorageSlotName(Name))
      continue;
    if (UsedVars.find(Name) == UsedVars.end() &&
        UsedVars.find(Local.Name) == UsedVars.end())
      continue;
    if ((CopyForward.count(Name) || CopyForward.count(Local.Name)) &&
        !VisibleAssigned.count(Name) && !VisibleAssigned.count(Local.Name)) {
      DeferredDecls.emplace(Name, DeferredDecl{Local.Type, {}});
      continue;
    }
    DeclaredNames.insert(Name);
    emitIndent(1);
    TypeRef Ty = Local.Type;
    if (Dbg && CurrentFunc) {
      // A debug type C cannot spell leaves the local its recovered type, as
      // debugTypeForDisplacement does.
      if (auto Var = Dbg->resolveVariable(CurrentFunc->Entry, Local.StackOff);
          Var && Var->Type && hasCValueLayout(cDisplayType(Var->Type)))
        Ty = cDisplayType(Var->Type);
    }
    auto ExplicitTy = ExplicitDeclarations.find(Name);
    if (ExplicitTy == ExplicitDeclarations.end())
      ExplicitTy = ExplicitDeclarations.find(Local.Name);
    OS << (ExplicitTy == ExplicitDeclarations.end()
               ? declarationToC(cDisplayType(Ty), Name)
               : ExplicitTy->second)
       << ";\n";
  }

  for (auto &[Name, Ty] : UsedVars) {
    if (auto It = CallResultTypes.find(Name);
        It != CallResultTypes.end() && It->second)
      Ty = It->second;
    if (auto It = EnumDestTypes.find(Name);
        It != EnumDestTypes.end() && It->second)
      Ty = It->second;
    if (auto It = PointerArgDestTypes.find(Name);
        It != PointerArgDestTypes.end() && It->second) {
      if (!Ty || Ty->Kind == NdTypeKind::Int ||
          Ty->Kind == NdTypeKind::Unknown ||
          (Ty->Kind == NdTypeKind::Ptr &&
           (!Ty->Pointee || Ty->Pointee->Kind != NdTypeKind::Struct ||
            Ty->Pointee->SourceName.empty())))
        Ty = It->second;
    }
    if (auto It = FieldForwardTypes.find(Name);
        It != FieldForwardTypes.end() && It->second) {
      const TypeRef Fwd = It->second;
      // Map cursor: untyped `int64_t v1` from `bins[i]` is a node pointer.
      // A join dest (`v5 = p->field`) must stay the scalar, not the
      // containing record FieldForwardTypes may have attached.
      if (Fwd->Kind == NdTypeKind::Ptr && Fwd->Pointee &&
          Fwd->Pointee->Kind == NdTypeKind::Struct &&
          !Fwd->Pointee->SourceName.empty() &&
          (!Ty || Ty->Kind == NdTypeKind::Int ||
           Ty->Kind == NdTypeKind::Unknown ||
           (Ty->Kind == NdTypeKind::Ptr &&
            (!Ty->Pointee || Ty->Pointee->Kind != NdTypeKind::Struct ||
             Ty->Pointee->SourceName.empty()))))
        Ty = Fwd;
    }
    if (Name.empty() || DeclaredNames.count(Name) || IsStorageSlotName(Name) ||
        (ProjectFrameAliasesIntoStorage && FrameAliases.count(Name) &&
         !VisibleAssigned.count(Name)))
      continue;
    if (CopyForward.count(Name) && !VisibleAssigned.count(Name)) {
      auto ExplicitTy = ExplicitDeclarations.find(Name);
      DeferredDecls.emplace(
          Name, DeferredDecl{Ty, ExplicitTy == ExplicitDeclarations.end()
                                     ? std::string()
                                     : ExplicitTy->second});
      continue;
    }
    // Frame-pointer temps and other HighIR names that never appear as a
    // C assignment must not be declared (`v2` inside `&var_m48`).
    // A join dest can still be used after both arm writes were hidden.
    if (!VisibleAssigned.count(Name) && !isEmittedParamName(Name) &&
        !JoinPhiNames.count(Name))
      continue;
    DeclaredNames.insert(Name);
    emitIndent(1);
    auto ExplicitTy = ExplicitDeclarations.find(Name);
    if (ExplicitTy == ExplicitDeclarations.end())
      DeclaredCTypes.emplace(Name, cDisplayType(Ty));
    OS << (ExplicitTy == ExplicitDeclarations.end()
               ? declarationToC(cDisplayType(Ty), Name)
               : ExplicitTy->second)
       << ";\n";
  }
  DeclaredCNames.insert(DeclaredNames.begin(), DeclaredNames.end());
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
  if (Opts.Image && EmitFunctionWrapper)
    if (auto Reg = x86GetPcThunkRegister(*Opts.Image, Func.Entry)) {
      c_stub::writeGetPcThunk(OS, functionIdentifier(Func),
                              typeToC(Func.ReturnType), *Reg);
      return;
    }

  if (Opts.Image)
    if (const std::string Import =
            c_stub::variadicImportOfStub(*Opts.Image, Func.Entry);
        !Import.empty()) {
      if (EmitFunctionWrapper)
        OS << "/* neverd.entry: 0x" << llvm::utohexstr(Func.Entry) << " */\n";
      HighExpr Slot;
      Slot.CallAddr = Func.Entry;
      c_stub::writeVariadicImportStub(
          OS,
          c_stub::variadicStubName(functionIdentifier(Func), Import,
                                   Func.Entry),
          Import, importSlotIdentifier(Slot),
          libc::libcVariadicForward(Import));
      return;
    }
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
  AmbiguousFrameAliases.clear();
  const bool SavedHandler = InEHClauseBody;
  std::function<void(const std::vector<HighStmt> &, bool, bool)> WalkAliases;
  WalkAliases = [&](const std::vector<HighStmt> &Stmts, bool InHandler,
                    bool InCleanup) {
    const bool Saved = InEHClauseBody;
    InEHClauseBody = InHandler;
    for (const HighStmt &S : Stmts) {
      if (!Analysis.DeadStmts.count(&S) && S.Kind == StmtKind::Assign &&
          S.Dst && S.Val &&
          (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi) &&
          S.Dst->Var.Kind != MedVar::Param) {
        const std::string Name = varName(S.Dst->Var);
        if (!Name.empty() && !InCleanup) {
          if (const auto Disp = frameDisplacement(*S.Val)) {
            if (AmbiguousFrameAliases.count(Name)) {
              FrameAliases.erase(Name);
            } else if (auto It = FrameAliases.find(Name);
                       It == FrameAliases.end()) {
              FrameAliases[Name] = *Disp;
            } else if (It->second != *Disp) {
              FrameAliases.erase(Name);
              AmbiguousFrameAliases.insert(Name);
            }
          } else if (typedMemberAccess(*S.Val)) {
            FrameAliases.erase(Name);
            AmbiguousFrameAliases.insert(Name);
          }
        }
      }
      WalkAliases(S.Body, InHandler, InCleanup);
      WalkAliases(S.ElseBody, InHandler, InCleanup);
      for (const auto &C : S.Cases)
        WalkAliases(C.Body, InHandler, InCleanup);
      WalkAliases(S.DefaultBody, InHandler, InCleanup);
      for (size_t I = 0; I < S.EHClauseBodies.size(); ++I) {
        const bool Cleanup =
            I < S.EHClauses.size() &&
            S.EHClauses[I].Kind == HighEHClauseKind::CxxCleanup;
        WalkAliases(S.EHClauseBodies[I], true, Cleanup);
      }
    }
    InEHClauseBody = Saved;
  };
  auto CollectAliases = [&]() {
    FrameAliases.clear();
    AmbiguousFrameAliases.clear();
    bool Grew = true;
    unsigned Guard = 0;
    while (Grew && Guard++ < limits::kMaxFrameAliasFixedPoint) {
      const auto Before = FrameAliases;
      WalkAliases(Func.Body, false, false);
      Grew = FrameAliases != Before;
    }
  };
  CollectAliases();
  std::map<int64_t, TypeRef> InferredSlotTypes;
  auto Note = [&](const HighExpr *Addr, const TypeRef &Ty,
                  bool AddressTaken = false, bool IsStore = false) {
    if (!Addr)
      return;
    const auto Disp = frameDisplacement(*Addr);
    if (!Disp)
      return;
    if (Ty) {
      auto &Inferred = InferredSlotTypes[*Disp];
      if (!Inferred || Ty->Size > Inferred->Size ||
          (Ty->Size == Inferred->Size && Ty->Kind == NdTypeKind::Struct &&
           Inferred->Kind != NdTypeKind::Struct))
        Inferred = cDisplayType(Ty);
    }
    NamedFrameSlot &Slot = FrameSlots[*Disp];
    if (AddressTaken)
      Slot.AddressTaken = true;
    else
      Slot.UsedAsMemory = true;
    const uint64_t Mag = static_cast<uint64_t>(*Disp < 0 ? -*Disp : *Disp);
    const std::string Synthetic =
        (*Disp < 0 ? "var_m" : "var_") + llvm::utohexstr(Mag);
    const std::string DebugName = debugNameForDisplacement(Func.Entry, *Disp);
    if (!DebugName.empty() && !isReservedParamDisplayName(DebugName))
      Slot.Name = DebugName;
    else if (Slot.Name.empty() || isReservedParamDisplayName(Slot.Name))
      Slot.Name = Synthetic;
    if (const TypeRef DebugTy = debugTypeForDisplacement(Func.Entry, *Disp))
      Slot.Type = DebugTy;
    else if (Ty &&
             (!Slot.Type || Ty->Size > Slot.Type->Size ||
              (Ty->Size == Slot.Type->Size && Ty->Kind == NdTypeKind::Struct &&
               Slot.Type->Kind != NdTypeKind::Struct)))
      Slot.Type = cDisplayType(Ty);
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
    // An indirect call's target is read like an operand: a function pointer
    // kept in a frame slot is that slot.
    if (E.IndirectTarget)
      Walk(*E.IndirectTarget, false);
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
  // CodeView's frame-relative local offsets subtract S_FRAMEPROC's stack
  // allocation, while HighIR's displacement also includes saved registers.
  // The same PDB local can therefore resolve both at its real SP-relative
  // slot and at a different frame-relative slot. Only the unique SP match
  // may keep the debug name; the other memory object needs its own identity.
  std::map<std::string, std::vector<int64_t>> NameDisplacements;
  for (const auto &[Disp, Slot] : FrameSlots)
    NameDisplacements[Slot.Name].push_back(Disp);
  for (const auto &[Name, Disps] : NameDisplacements) {
    if (Disps.size() < 2)
      continue;
    std::optional<int64_t> SPMatch;
    bool AmbiguousSPMatch = false;
    if (Dbg && Func.FrameSize > 0)
      for (int64_t Disp : Disps) {
        const auto Var = Dbg->resolveStackPointerVariable(
            Func.Entry, Disp + static_cast<int64_t>(Func.FrameSize));
        if (!Var || Var->IsParam || Var->Name != Name)
          continue;
        if (SPMatch)
          AmbiguousSPMatch = true;
        SPMatch = Disp;
      }
    if (AmbiguousSPMatch)
      SPMatch.reset();
    for (int64_t Disp : Disps) {
      if (SPMatch && Disp == *SPMatch)
        continue;
      NamedFrameSlot &Slot = FrameSlots[Disp];
      const uint64_t Mag = static_cast<uint64_t>(Disp < 0 ? -Disp : Disp);
      Slot.Name = (Disp < 0 ? "var_m" : "var_") + llvm::utohexstr(Mag);
      if (auto It = InferredSlotTypes.find(Disp); It != InferredSlotTypes.end())
        Slot.Type = It->second;
      else
        Slot.Type = NdType::makeInt(4);
    }
  }
  CollectAliases();
  for (auto &[Disp, Slot] : FrameSlots)
    if (!Slot.Type)
      Slot.Type = NdType::makeInt(4);

  // Slots that share bytes are one object, not separate variables: a byte
  // stored at -0x82 changes what a later read of the eight bytes at -0x84
  // sees.  Each group of overlapping slots lives in one storage, the group's
  // first slot; when that slot does not cover the whole group it is declared
  // as a byte array (the binary guarantees no alignment for it).  The other
  // slots are accessed through it.
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

void HighCWriter::invalidateJoinPhiFrameAliases(const HighFunc &Func) {
  std::set<std::string> HasFrame;
  std::set<std::string> HasOtherAddr;
  std::map<std::string, unsigned> AssignCount;
  std::function<void(const std::vector<HighStmt> &)> Walk;
  Walk = [&](const std::vector<HighStmt> &Stmts) {
    for (const HighStmt &S : Stmts) {
      if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
          S.Val->Kind != ExprKind::Undef &&
          (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi) &&
          S.Dst->Var.Kind != MedVar::Param) {
        const std::string Name = varName(S.Dst->Var);
        if (!Name.empty()) {
          ++AssignCount[Name];
          const bool Frame = frameDisplacement(*S.Val).has_value();
          if (Frame)
            HasFrame.insert(Name);
          if (typedMemberAccess(*S.Val) ||
              (!Frame && S.Val->Kind == ExprKind::BinOp &&
               (S.Val->Op == NdOp::INT_ADD || S.Val->Op == NdOp::INT_SUB)))
            HasOtherAddr.insert(Name);
        }
      }
      Walk(S.Body);
      Walk(S.ElseBody);
      for (const auto &C : S.Cases)
        Walk(C.Body);
      Walk(S.DefaultBody);
      for (size_t I = 0; I < S.EHClauseBodies.size(); ++I) {
        const bool Cleanup =
            I < S.EHClauses.size() &&
            S.EHClauses[I].Kind == HighEHClauseKind::CxxCleanup;
        if (!Cleanup)
          Walk(S.EHClauseBodies[I]);
      }
    }
  };
  Walk(Func.Body);
  for (const auto &[Name, Count] : AssignCount) {
    if (!HasOtherAddr.count(Name))
      continue;
    if (!HasFrame.count(Name) && Count < 2)
      continue;
    FrameAliases.erase(Name);
    CopyForward.erase(Name);
    AmbiguousFrameAliases.insert(Name);
  }
  if (AmbiguousFrameAliases.empty())
    return;
  // Each variable \p S reads, the address of an assigned memory location
  // included.
  auto ForEachRead = [&](const HighStmt &S,
                         const std::function<void(std::string)> &Fn) {
    std::function<void(const HighExpr &)> Visit = [&](const HighExpr &E) {
      if (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) {
        if (std::string Name = varName(E.Var); !Name.empty())
          Fn(std::move(Name));
        return;
      }
      E.forEachChildExpr([&](const ExprPtr &Child) {
        if (Child)
          Visit(*Child);
      });
    };
    forEachExpr(S, [&](const ExprPtr &E) {
      if (E && (E != S.Dst ||
                (E->Kind != ExprKind::Var && E->Kind != ExprKind::Phi)))
        Visit(*E);
    });
  };
  std::map<std::string, std::vector<const HighStmt *>> Definitions;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst &&
        (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi))
      Definitions[varName(S.Dst->Var)].push_back(&S);
  });
  // The alias prints by name, so its definitions print again only when a
  // printed statement reads it.  Reads continue through the definitions of
  // each name read, since a hidden definition may print where it is
  // forwarded.  The definitions of a name nothing printed reads stay hidden.
  std::set<std::string> Read;
  std::vector<std::string> Work;
  auto Reach = [&](std::string Name) {
    if (Read.insert(Name).second)
      Work.push_back(std::move(Name));
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (!Analysis.DeadStmts.count(&S))
      ForEachRead(S, Reach);
  });
  while (!Work.empty()) {
    const std::string Name = std::move(Work.back());
    Work.pop_back();
    for (const HighStmt *Def : Definitions[Name])
      ForEachRead(*Def, Reach);
  }
  for (auto It = AmbiguousFrameAliases.begin();
       It != AmbiguousFrameAliases.end();)
    It = Read.count(*It) ? std::next(It) : AmbiguousFrameAliases.erase(It);
  // A revived definition prints, so every definition it reads must print as
  // well, though an earlier pass may have dropped it as unused.
  std::set<std::string> Revived;
  auto Revive = [&](std::string Name) {
    if (Revived.insert(Name).second)
      Work.push_back(std::move(Name));
  };
  for (const std::string &Name : AmbiguousFrameAliases) {
    Analysis.DeadVars.erase(Name);
    Revive(Name);
  }
  while (!Work.empty()) {
    const std::string Name = std::move(Work.back());
    Work.pop_back();
    for (const HighStmt *Def : Definitions[Name])
      if (Analysis.DeadStmts.erase(Def)) {
        Analysis.DeadVars.erase(Name);
        ForEachRead(*Def, Revive);
      }
  }
}

void HighCWriter::applyDebugCallSlotTypes(const HighFunc &Func) {
  walkStmts(Func.Body, [&](const HighStmt &S) {
    const HighExpr *Call = nullptr;
    if (S.Kind == StmtKind::Call && S.CallExpr)
      Call = S.CallExpr.get();
    else if (S.Val && S.Val->Kind == ExprKind::Call)
      Call = S.Val.get();
    if (!Call)
      return;
    const size_t Limit = debugCallArgLimit(*Call);
    for (size_t I = 0; I < Call->Operands.size() && I < Limit; ++I) {
      const HighExpr *Op = Call->Operands[I].get();
      if (!Op)
        continue;
      const TypeRef Expected = displayCallArgType(*Call, I);
      if (!Expected || Expected->Kind != NdTypeKind::Ptr || !Expected->Pointee)
        continue;
      const HighExpr *Inner = unwrapIntegerView(Op);
      if (!Inner)
        Inner = Op;
      if (Inner->Kind == ExprKind::Addr && !Inner->Operands.empty() &&
          Inner->Operands[0]) {
        Inner = unwrapIntegerView(Inner->Operands[0].get());
        if (!Inner)
          continue;
        if (Inner->Kind == ExprKind::Load && !Inner->Operands.empty() &&
            Inner->Operands[0])
          Inner = unwrapIntegerView(Inner->Operands[0].get());
        if (!Inner)
          continue;
      }
      if (!namedFrameSlot(*Inner))
        continue;
      const auto Disp = frameDisplacement(*Inner);
      if (!Disp)
        continue;
      auto It = FrameSlots.find(*Disp);
      if (It == FrameSlots.end())
        continue;
      TypeRef Next = cDisplayType(Expected->Pointee);
      if (Next && (!It->second.CallType ||
                   Next->FieldDisplayNames.size() >=
                       It->second.CallType->FieldDisplayNames.size()))
        It->second.CallType = Next;
      if (It->second.Type && It->second.Type->FieldDisplayNames.size() >
                                 (Next ? Next->FieldDisplayNames.size() : 0))
        continue;
      It->second.Type = std::move(Next);
    }
  });
}

void HighCWriter::propagateFrameSlotCopyTypes(const HighFunc &Func) {
  if (Dbg)
    for (auto &[Disp, Slot] : FrameSlots) {
      (void)Disp;
      if (Slot.Type)
        Dbg->completeType(Slot.Type);
    }
  // SourceName is enough: PDB TPtr may still have an empty field list
  // when the packed Value sibling is typed from an ArgList overlay.
  auto NamedRecord = [](const TypeRef &Ty) {
    return Ty && Ty->Kind == NdTypeKind::Struct && !Ty->IsEnum &&
           !Ty->SourceName.empty();
  };
  auto Weaker = [&](const TypeRef &Ty) { return !NamedRecord(Ty); };
  auto Apply = [&](int64_t DestDisp, const TypeRef &SrcTy) {
    if (!NamedRecord(SrcTy))
      return false;
    auto It = FrameSlots.find(DestDisp);
    if (It == FrameSlots.end())
      return false;
    const uint16_t DestSize =
        It->second.Type && It->second.Type->Size ? It->second.Type->Size : 0;
    const uint16_t SrcSize = SrcTy->Size ? SrcTy->Size : DestSize;
    const bool ReplaceNamed =
        NamedRecord(It->second.Type) &&
        It->second.Type->SourceName != SrcTy->SourceName && DestSize &&
        SrcSize && DestSize == SrcSize;
    if (!Weaker(It->second.Type) && !ReplaceNamed)
      return false;
    if (DestSize && SrcSize && DestSize != SrcSize)
      return false;
    TypeRef Next = cDisplayType(SrcTy);
    It->second.Type = Next;
    // A TPtr/ArgList overlay dest keeps CallType so `values_` still prints.
    if (!ReplaceNamed &&
        (!It->second.CallType || It->second.CallType->FieldDisplayNames.size() <
                                     Next->FieldDisplayNames.size()))
      It->second.CallType = Next;
    return true;
  };

  bool Changed = true;
  unsigned Guard = 0;
  while (Changed && Guard++ < limits::kMaxFrameAliasFixedPoint) {
    Changed = false;
    std::map<std::tuple<uint8_t, int, int>, TypeRef> TempTypes;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
        return;
      if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
        return;
      const HighExpr *Val = unwrapIntegerView(S.Val.get());
      if (!Val || Val->Kind != ExprKind::Load || Val->Operands.empty() ||
          !Val->Operands[0])
        return;
      const auto SrcDisp = frameDisplacement(*Val->Operands[0]);
      if (!SrcDisp)
        return;
      auto It = FrameSlots.find(*SrcDisp);
      if (It == FrameSlots.end() || !NamedRecord(It->second.Type))
        return;
      TempTypes[{static_cast<uint8_t>(S.Dst->Var.Kind), S.Dst->Var.Id,
                 S.Dst->Var.SSAVer}] = It->second.Type;
    });
    auto SlotDispByName =
        [&](const std::string &Name) -> std::optional<int64_t> {
      if (Name.empty())
        return std::nullopt;
      if (auto Alias = FrameAliases.find(Name); Alias != FrameAliases.end())
        return Alias->second;
      std::optional<int64_t> Found;
      for (const auto &[Disp, Slot] : FrameSlots) {
        if (Slot.Name != Name)
          continue;
        if (Found)
          return std::nullopt;
        Found = Disp;
      }
      return Found;
    };
    walkStmts(Func.Body, [&](const HighStmt &S) {
      const HighExpr *Addr = nullptr;
      const HighExpr *Val = nullptr;
      std::optional<int64_t> Dest;
      if (S.Kind == StmtKind::Store && S.StoreAddr && S.StoreVal) {
        Addr = S.StoreAddr.get();
        Val = S.StoreVal.get();
      } else if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
                 S.Dst->Kind == ExprKind::Load && !S.Dst->Operands.empty()) {
        Addr = S.Dst->Operands[0].get();
        Val = S.Val.get();
      } else if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
                 (S.Dst->Kind == ExprKind::Var ||
                  S.Dst->Kind == ExprKind::Phi)) {
        Dest = SlotDispByName(copyForwardName(varName(S.Dst->Var)));
        Val = S.Val.get();
      }
      if (!Val)
        return;
      if (!Dest && Addr)
        Dest = frameDisplacement(*Addr);
      if (!Dest)
        return;
      Val = unwrapIntegerView(Val);
      if (!Val)
        return;
      TypeRef SrcTy;
      if (Val->Kind == ExprKind::Load && !Val->Operands.empty() &&
          Val->Operands[0]) {
        if (const auto SrcDisp = frameDisplacement(*Val->Operands[0])) {
          auto It = FrameSlots.find(*SrcDisp);
          if (It != FrameSlots.end())
            SrcTy = It->second.Type;
        }
      } else if (Val->Kind == ExprKind::Var || Val->Kind == ExprKind::Phi) {
        auto It = TempTypes.find({static_cast<uint8_t>(Val->Var.Kind),
                                  Val->Var.Id, Val->Var.SSAVer});
        if (It != TempTypes.end())
          SrcTy = It->second;
        const std::string Name = copyForwardName(varName(Val->Var));
        if (!NamedRecord(SrcTy)) {
          if (auto Alias = FrameAliases.find(Name);
              Alias != FrameAliases.end()) {
            auto Slot = FrameSlots.find(Alias->second);
            if (Slot != FrameSlots.end() && NamedRecord(Slot->second.Type))
              SrcTy = Slot->second.Type;
          }
        }
        if (!NamedRecord(SrcTy)) {
          for (const auto &[Disp, Slot] : FrameSlots) {
            (void)Disp;
            if (Slot.Name == Name && NamedRecord(Slot.Type)) {
              SrcTy = Slot.Type;
              break;
            }
          }
        }
      }
      if (const auto SrcDisp = frameDisplacement(*Val)) {
        auto It = FrameSlots.find(*SrcDisp);
        if (It != FrameSlots.end() && NamedRecord(It->second.Type))
          SrcTy = It->second.Type;
      }
      if (NamedRecord(Val->Type) &&
          (!NamedRecord(SrcTy) || (SrcTy->SourceName == Val->Type->SourceName &&
                                   SrcTy->FieldDisplayNames.size() <
                                       Val->Type->FieldDisplayNames.size())))
        SrcTy = Val->Type;
      if (Apply(*Dest, SrcTy))
        Changed = true;
    });
  }

  // Same-size leftover after `&first` inherits that record so the sibling
  // is not `__int128`. Do not walk `values_` here: that ran before
  // hideInterior and collapsed the pack slot.
  for (auto It = FrameSlots.begin(); It != FrameSlots.end(); ++It) {
    if (!Weaker(It->second.Type))
      continue;
    const uint16_t Size =
        It->second.Type && It->second.Type->Size ? It->second.Type->Size : 0;
    if (Size < 8)
      continue;
    auto Prev = FrameSlots.find(It->first - static_cast<int64_t>(Size));
    if (Prev == FrameSlots.end() || !Prev->second.AddressTaken)
      continue;
    TypeRef Src;
    if (NamedRecord(Prev->second.CallType))
      Src = Prev->second.CallType;
    else if (NamedRecord(Prev->second.Type))
      Src = Prev->second.Type;
    if (Src)
      Apply(It->first, Src);
  }
}

void HighCWriter::hideInteriorRecordFieldSlots() {
  std::vector<int64_t> Drop;
  for (const auto &[Disp, Slot] : FrameSlots) {
    if (!llvm::StringRef(Slot.Name).starts_with("var_"))
      continue;
    if (!frameTypedMemberAccess(Disp))
      continue;
    Drop.push_back(Disp);
  }
  for (int64_t Disp : Drop)
    FrameSlots.erase(Disp);
}

void HighCWriter::overlayPackedValueHomes(const HighFunc &Func) {
  auto NamedRecord = [](const TypeRef &Ty) {
    return Ty && Ty->Kind == NdTypeKind::Struct && !Ty->IsEnum &&
           !Ty->SourceName.empty();
  };
  auto Weaker = [&](const TypeRef &Ty) { return !NamedRecord(Ty); };
  auto HasField = [](const TypeRef &Ty, int64_t Off,
                     llvm::StringRef Name) -> bool {
    if (!Ty || Ty->FieldDisplayOffsets.size() != Ty->FieldDisplayNames.size())
      return false;
    for (size_t I = 0; I < Ty->FieldDisplayOffsets.size(); ++I) {
      if (Ty->FieldDisplayOffsets[I] == Off && Ty->FieldDisplayNames[I] == Name)
        return true;
    }
    return false;
  };
  auto IsArgList = [&](const TypeRef &Ty) {
    return NamedRecord(Ty) &&
           (Ty->SourceName == "ArgList" ||
            (HasField(Ty, 0, "types_") && HasField(Ty, 8, "values_")));
  };
  auto ArgListOf = [&](const NamedFrameSlot &Slot) -> TypeRef {
    if (IsArgList(Slot.CallType))
      return Slot.CallType;
    if (IsArgList(Slot.Type))
      return Slot.Type;
    return {};
  };
  auto Overlay = [&](int64_t DestDisp, const TypeRef &Src, bool ReplaceNamed) {
    if (!NamedRecord(Src))
      return;
    auto It = FrameSlots.find(DestDisp);
    if (It == FrameSlots.end())
      return;
    if (Weaker(It->second.Type) ||
        (ReplaceNamed && NamedRecord(It->second.Type) &&
         It->second.Type->SourceName != Src->SourceName &&
         It->second.Type->Size == Src->Size)) {
      It->second.Type = cDisplayType(Src);
      It->second.CallType = Src;
    }
  };

  std::map<int64_t, TypeRef> TakenArgList;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    const HighExpr *Addr = nullptr;
    const HighExpr *Val = nullptr;
    if (S.Kind == StmtKind::Store && S.StoreAddr && S.StoreVal) {
      Addr = S.StoreAddr.get();
      Val = S.StoreVal.get();
    } else if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
               S.Dst->Kind == ExprKind::Load && !S.Dst->Operands.empty()) {
      Addr = S.Dst->Operands[0].get();
      Val = S.Val.get();
    }
    if (!Addr || !Val)
      return;
    Val = unwrapIntegerView(Val);
    if (!Val)
      return;
    const HighExpr *AddrOf = Val;
    if (Val->Kind == ExprKind::Addr && !Val->Operands.empty() &&
        Val->Operands[0])
      AddrOf = Val->Operands[0].get();
    if (AddrOf->Kind == ExprKind::Load && !AddrOf->Operands.empty() &&
        AddrOf->Operands[0])
      AddrOf = AddrOf->Operands[0].get();
    std::optional<int64_t> Dest = frameDisplacement(*AddrOf);
    if (!Dest && (Val->Kind == ExprKind::Var || Val->Kind == ExprKind::Phi)) {
      const std::string Name = copyForwardName(varName(Val->Var));
      if (auto Alias = FrameAliases.find(Name); Alias != FrameAliases.end())
        Dest = Alias->second;
    }
    if (!Dest)
      return;
    auto DestIt = FrameSlots.find(*Dest);
    if (DestIt == FrameSlots.end() || !DestIt->second.AddressTaken)
      return;
    // Guest stores `*(fp+k) = (fp+n)`. The store address is the field
    // displacement (`values_` at +8), not `ADD(slot, 8)` of a named base.
    const auto StoreDisp = frameDisplacement(*Addr);
    if (!StoreDisp)
      return;
    std::optional<int64_t> Owner;
    int64_t Rel = 0;
    if (FrameSlots.count(*StoreDisp)) {
      Owner = *StoreDisp;
    } else {
      for (const auto &[SlotDisp, Slot] : FrameSlots) {
        uint16_t Size = Slot.Type && Slot.Type->Size ? Slot.Type->Size : 0;
        if (!Size && Slot.CallType)
          Size = Slot.CallType->Size;
        if (Size < 8)
          continue;
        if (SlotDisp <= *StoreDisp &&
            *StoreDisp < SlotDisp + static_cast<int64_t>(Size)) {
          Owner = SlotDisp;
          Rel = *StoreDisp - SlotDisp;
          break;
        }
      }
    }
    if (!Owner)
      return;
    auto OwnIt = FrameSlots.find(*Owner);
    if (OwnIt == FrameSlots.end())
      return;
    TypeRef Src = ArgListOf(OwnIt->second);
    if (!Src)
      return;
    if (!HasField(Src, Rel, "values_") &&
        !HasField(OwnIt->second.CallType, Rel, "values_"))
      return;
    TakenArgList[*Dest] = Src;
    if (Weaker(DestIt->second.Type))
      Overlay(*Dest, Src, /*ReplaceNamed=*/false);
  });

  for (auto It = FrameSlots.begin(); It != FrameSlots.end(); ++It) {
    if (It->second.AddressTaken)
      continue;
    TypeRef Src;
    for (const auto &[PrevDisp, Prev] : FrameSlots) {
      if (!Prev.AddressTaken)
        continue;
      const uint16_t PrevSize =
          Prev.Type && Prev.Type->Size ? Prev.Type->Size : 0;
      if (PrevSize < 8 ||
          PrevDisp + static_cast<int64_t>(PrevSize) != It->first)
        continue;
      auto Taken = TakenArgList.find(PrevDisp);
      if (Taken != TakenArgList.end())
        Src = Taken->second;
      else if (IsArgList(Prev.CallType))
        Src = Prev.CallType;
      else if (IsArgList(Prev.Type))
        Src = Prev.Type;
      if (Src)
        break;
    }
    if (Src)
      Overlay(It->first, Src, /*ReplaceNamed=*/true);
  }
}

namespace {
// FieldForward prints only the projected member/index. Keep any narrowing in
// the original assignment, including narrowing followed by a later widening.
bool fieldProjectionDoesNotNarrow(const HighExpr *Expr,
                                  const HighExpr *Source) {
  unsigned Depth = 0;
  while (Expr && Expr != Source &&
         Depth++ < limits::kMaxIntegerViewUnwrapDepth) {
    if (Expr->Operands.empty() || !Expr->Operands[0])
      return false;
    const HighExpr *Inner = Expr->Operands[0].get();
    const TypeRef &ViewType = Expr->Kind == ExprKind::Cast && Expr->CastTo
                                  ? Expr->CastTo
                                  : Expr->Type;
    if (!ViewType || !ViewType->Size || !Inner->Type || !Inner->Type->Size ||
        ViewType->Size < Inner->Type->Size)
      return false;
    Expr = Inner;
  }
  return Expr == Source;
}
} // namespace

void HighCWriter::collectFieldLoadTypes(const HighFunc &Func) {
  FieldForwardTypes.clear();
  // A cursor can acquire its pointer type through bins[i] and a copy before
  // its fields become recognizable. Discover those types without replacing
  // any value: the forwarding proof below must see the original definitions.
  bool Changed = true;
  unsigned Iteration = 0;
  while (Changed && Iteration++ < limits::kMaxIntegerViewUnwrapDepth) {
    Changed = false;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val ||
          (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi))
        return;
      const std::string Name = varName(S.Dst->Var);
      if (JoinPhiNames.count(Name) || FieldForwardTypes.count(Name))
        return;
      const HighExpr *Val = peelIntegerViewOps(S.Val.get());
      if (!Val || !fieldProjectionDoesNotNarrow(S.Val.get(), Val))
        return;
      TypeRef Type;
      if (Val->Kind == ExprKind::Load && !Val->Operands.empty() &&
          Val->Operands[0] && Val->MemoryOrdering == NdMemoryOrdering::None &&
          Val->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        const uint16_t Size = Val->Type ? Val->Type->Size : 0;
        Type = typedMemberType(*Val->Operands[0], Size);
        if (!Type)
          if (auto Index = typedIndexAccess(*Val->Operands[0]))
            Type = Index->ElemType;
      } else if (Val->Kind == ExprKind::Var || Val->Kind == ExprKind::Phi) {
        if (auto It = FieldForwardTypes.find(varName(Val->Var));
            It != FieldForwardTypes.end())
          Type = It->second;
      }
      if (Type) {
        FieldForwardTypes.emplace(Name, std::move(Type));
        Changed = true;
      }
    });
  }
}

void HighCWriter::collectFieldLoadForward(const HighFunc &Func) {
  FieldForward.clear();
  FieldForwardSources.clear();
  std::map<std::string, unsigned> AssignCount;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Assign || !S.Dst)
      return;
    if (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi)
      AssignCount[varName(S.Dst->Var)]++;
  });
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (stmtHiddenFromC(S) && !Analysis.DeadStmts.count(&S)) {
      const HighExpr *HiddenVal = S.Kind == StmtKind::Assign
                                      ? peelIntegerViewOps(S.Val.get())
                                      : nullptr;
      // Hidden CopyForward of a bins load (`v1 = t22`) must still publish
      // FieldForwardTypes onto the loop cursor.  Skipping non-Load dests
      // left `t19_2 = v1->m_nHash` unforwarded.
      if (!HiddenVal || (HiddenVal->Kind != ExprKind::Load &&
                         HiddenVal->Kind != ExprKind::Var &&
                         HiddenVal->Kind != ExprKind::Phi))
        return;
    }
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return;
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      return;
    const HighExpr *Val = peelIntegerViewOps(S.Val.get());
    if (!Val || !fieldProjectionDoesNotNarrow(S.Val.get(), Val))
      return;
    const std::string DestName = varName(S.Dst->Var);
    if (JoinPhiNames.count(DestName))
      return;
    // Live definitions may move to their uses only after the common proof
    // checks address dependencies, intervening writes and all use regions.
    // Dead loads retain display metadata for the legacy undefined-register
    // recovery below; they do not authorize replacing a live definition.
    const bool ForwardName =
        AssignCount[DestName] <= 1 &&
        (ValueForward.count(DestName) || Analysis.DeadStmts.count(&S));
    if (Val->Kind == ExprKind::Load && !Val->Operands.empty() &&
        Val->Operands[0] && Val->MemoryOrdering == NdMemoryOrdering::None) {
      const uint16_t AccessSize = Val->Type ? Val->Type->Size : 0;
      if (auto Member = typedMemberAccess(*Val->Operands[0], AccessSize)) {
        if (ForwardName) {
          FieldForward[DestName] = *Member;
          FieldForwardSources[DestName] = S.Val.get();
        }
        if (TypeRef Ty = typedMemberType(*Val->Operands[0], AccessSize))
          FieldForwardTypes[DestName] = std::move(Ty);
      } else if (auto Index = typedIndexAccess(*Val->Operands[0])) {
        if (ForwardName) {
          FieldForward[DestName] = Index->Base + "[" + Index->Index + "]";
          FieldForwardSources[DestName] = S.Val.get();
        }
        if (Index->ElemType)
          FieldForwardTypes[DestName] = Index->ElemType;
      }
      return;
    }
    if ((Val->Kind == ExprKind::Var || Val->Kind == ExprKind::Phi)) {
      const std::string Src = varName(Val->Var);
      if (ForwardName) {
        if (auto It = FieldForward.find(Src); It != FieldForward.end()) {
          FieldForward[DestName] = It->second;
          if (auto Source = FieldForwardSources.find(Src);
              Source != FieldForwardSources.end())
            FieldForwardSources[DestName] = Source->second;
        }
      }
      if (auto TypeIt = FieldForwardTypes.find(Src);
          TypeIt != FieldForwardTypes.end())
        FieldForwardTypes[DestName] = TypeIt->second;
    }
  });
  // A later `v1 = t22` may be the first time FieldForwardTypes reaches the
  // loop cursor.  Re-walk member loads so `t19_2 = v1->m_nHash` can hide.
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return;
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      return;
    const std::string DestName = varName(S.Dst->Var);
    if (JoinPhiNames.count(DestName) || FieldForward.count(DestName) ||
        AssignCount[DestName] > 1 ||
        (!ValueForward.count(DestName) && !Analysis.DeadStmts.count(&S)))
      return;
    const HighExpr *Val = peelIntegerViewOps(S.Val.get());
    if (!Val || !fieldProjectionDoesNotNarrow(S.Val.get(), Val) ||
        Val->Kind != ExprKind::Load || Val->Operands.empty() ||
        !Val->Operands[0] || Val->MemoryOrdering != NdMemoryOrdering::None)
      return;
    const uint16_t AccessSize = Val->Type ? Val->Type->Size : 0;
    if (auto Member = typedMemberAccess(*Val->Operands[0], AccessSize)) {
      FieldForward[DestName] = *Member;
      FieldForwardSources[DestName] = S.Val.get();
      if (TypeRef Ty = typedMemberType(*Val->Operands[0], AccessSize))
        FieldForwardTypes[DestName] = std::move(Ty);
    }
  });

  // A skipped Med COPY of a field load leaves the compare on the register
  // (v98) while the load dest is unused and dead. Alias that undeclared
  // same-size var when the list has exactly one unused field load.
  std::set<std::string> Assigned;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    const HighExpr *Call =
        S.Kind == StmtKind::Call ? S.CallExpr.get() : S.Val.get();
    if (Call && Call->Kind == ExprKind::Call)
      for (const MedVar &Output : Call->IntrinsicOutputs)
        Assigned.insert(varName(Output));
    if (S.Kind != StmtKind::Assign || !S.Dst)
      return;
    if (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi)
      Assigned.insert(varName(S.Dst->Var));
  });
  std::function<void(const std::vector<HighStmt> &)> Alias =
      [&](const std::vector<HighStmt> &Body) {
        for (const HighStmt &S : Body) {
          Alias(S.Body);
          Alias(S.ElseBody);
          Alias(S.DefaultBody);
          for (const auto &C : S.Cases)
            Alias(C.Body);
          for (const auto &Clause : S.EHClauseBodies)
            Alias(Clause);
        }
        std::string UnusedName;
        uint16_t UnusedSize = 0;
        unsigned UnusedCount = 0;
        for (const HighStmt &S : Body) {
          if (!Analysis.DeadStmts.count(&S) || S.Kind != StmtKind::Assign ||
              !S.Dst ||
              (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi))
            continue;
          const std::string Name = varName(S.Dst->Var);
          if (!FieldForward.count(Name))
            continue;
          ++UnusedCount;
          UnusedName = Name;
          UnusedSize = S.Dst->Var.Size;
        }
        if (UnusedCount != 1 || UnusedName.empty())
          return;
        const std::string Member = FieldForward[UnusedName];
        TypeRef MemberType;
        if (auto It = FieldForwardTypes.find(UnusedName);
            It != FieldForwardTypes.end())
          MemberType = It->second;
        std::function<void(const HighExpr &)> Note = [&](const HighExpr &E) {
          if (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) {
            const std::string Name = varName(E.Var);
            const bool SizeOk = E.Var.Size == UnusedSize ||
                                (UnusedSize == 4 && E.Var.Size == 8);
            if (!Name.empty() && E.Var.Kind != MedVar::Param &&
                !isReservedParamDisplayName(Name) && !Assigned.count(Name) &&
                !FieldForward.count(Name) && !JoinPhiNames.count(Name) &&
                SizeOk) {
              FieldForward[Name] = Member;
              if (MemberType)
                FieldForwardTypes[Name] = MemberType;
            }
          }
          E.forEachChildExpr([&](const ExprPtr &Op) { Note(*Op); });
        };
        for (const HighStmt &S : Body) {
          if (Analysis.DeadStmts.count(&S) || stmtHiddenFromC(S))
            continue;
          forEachRhsExpr(S, [&](const ExprPtr &E) {
            if (E)
              Note(*E);
          });
        }
      };
  Alias(Func.Body);

  // Alias maps an unused field load onto the skipped Med COPY register
  // (`v98`). A later `v97 = (v98 ?Op? 0)` is an assigned dest, so Alias
  // will not name it. Copy FieldForward through integer-view assigns so
  // cond/store uses print the member and the dest assign is hidden.
  auto PropagateViewCopies = [&] {
    bool Changed = true;
    unsigned Guard = 0;
    while (Changed && Guard++ < limits::kMaxIntegerViewUnwrapDepth) {
      Changed = false;
      walkStmts(Func.Body, [&](const HighStmt &S) {
        if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
          return;
        if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
          return;
        const std::string Dest = varName(S.Dst->Var);
        if (Dest.empty() || FieldForward.count(Dest))
          return;
        if (AssignCount[Dest] > 1 ||
            (!ValueForward.count(Dest) && !Analysis.DeadStmts.count(&S)))
          return;
        const HighExpr *Val = peelIntegerViewOps(S.Val.get());
        if (!Val || !fieldProjectionDoesNotNarrow(S.Val.get(), Val) ||
            (Val->Kind != ExprKind::Var && Val->Kind != ExprKind::Phi))
          return;
        auto It = FieldForward.find(varName(Val->Var));
        if (It == FieldForward.end())
          return;
        FieldForward[Dest] = It->second;
        if (auto Source = FieldForwardSources.find(varName(Val->Var));
            Source != FieldForwardSources.end())
          FieldForwardSources[Dest] = Source->second;
        if (auto TypeIt = FieldForwardTypes.find(varName(Val->Var));
            TypeIt != FieldForwardTypes.end())
          FieldForwardTypes[Dest] = TypeIt->second;
        Changed = true;
      });
    }
  };
  PropagateViewCopies();

  // `if (this->field) foo(t)` after Med inlined the load into the cond: t is
  // the same pointer and was never assigned. Alias only a standalone
  // null-check; `p && IsKind` must not steal later sibling-field args.
  auto IsZeroConst = [](const HighExpr *E) {
    return E && E->Kind == ExprKind::Const && E->ConstVal == 0;
  };
  auto IsAndOr = [](const HighExpr *E) {
    return E && E->Kind == ExprKind::BinOp &&
           (E->Op == NdOp::BOOL_AND || E->Op == NdOp::BOOL_OR);
  };
  std::function<std::optional<std::pair<std::string, TypeRef>>(
      const HighExpr *)>
      MemberOf = [&](const HighExpr *E)
      -> std::optional<std::pair<std::string, TypeRef>> {
    E = unwrapIntegerView(E);
    if (!E)
      return std::nullopt;
    if (E->Kind == ExprKind::Load && !E->Operands.empty() && E->Operands[0]) {
      if (auto Path = typedMemberAccess(*E->Operands[0]))
        return std::make_pair(*Path, typedMemberType(*E->Operands[0]));
      return std::nullopt;
    }
    if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi) {
      const std::string Name = varName(E->Var);
      if (auto It = FieldForward.find(Name); It != FieldForward.end()) {
        TypeRef Ty;
        if (auto TypeIt = FieldForwardTypes.find(Name);
            TypeIt != FieldForwardTypes.end())
          Ty = TypeIt->second;
        return std::make_pair(It->second, Ty);
      }
    }
    return std::nullopt;
  };
  auto GuardMember = [&](const HighExpr &Cond)
      -> std::optional<std::tuple<std::string, TypeRef, bool>> {
    const HighExpr *E = unwrapIntegerView(&Cond);
    if (!E || IsAndOr(E))
      return std::nullopt;
    if (E->Kind == ExprKind::UnaryOp && E->Op == NdOp::BOOL_NOT &&
        !E->Operands.empty() && E->Operands[0]) {
      const HighExpr *Inner = unwrapIntegerView(E->Operands[0].get());
      if (!Inner || IsAndOr(Inner))
        return std::nullopt;
      if (Inner->Kind == ExprKind::BinOp && Inner->Operands.size() == 2 &&
          (Inner->Op == NdOp::INT_EQUAL || Inner->Op == NdOp::INT_NOTEQUAL) &&
          Inner->Operands[0] && Inner->Operands[1]) {
        const HighExpr *LHS = unwrapIntegerView(Inner->Operands[0].get());
        const HighExpr *RHS = unwrapIntegerView(Inner->Operands[1].get());
        const HighExpr *Val = IsZeroConst(RHS)   ? LHS
                              : IsZeroConst(LHS) ? RHS
                                                 : nullptr;
        if (auto M = MemberOf(Val))
          return std::make_tuple(M->first, M->second,
                                 Inner->Op == NdOp::INT_EQUAL);
      }
      if (auto M = MemberOf(Inner))
        return std::make_tuple(M->first, M->second, false);
      return std::nullopt;
    }
    if (E->Kind == ExprKind::BinOp && E->Operands.size() == 2 &&
        (E->Op == NdOp::INT_EQUAL || E->Op == NdOp::INT_NOTEQUAL) &&
        E->Operands[0] && E->Operands[1]) {
      const HighExpr *LHS = unwrapIntegerView(E->Operands[0].get());
      const HighExpr *RHS = unwrapIntegerView(E->Operands[1].get());
      const HighExpr *Val = IsZeroConst(RHS)   ? LHS
                            : IsZeroConst(LHS) ? RHS
                                               : nullptr;
      if (auto M = MemberOf(Val))
        return std::make_tuple(M->first, M->second,
                               E->Op == NdOp::INT_NOTEQUAL);
      return std::nullopt;
    }
    if (auto M = MemberOf(E))
      return std::make_tuple(M->first, M->second, true);
    return std::nullopt;
  };
  std::function<void(const std::vector<HighStmt> &)> GuardAlias =
      [&](const std::vector<HighStmt> &Body) {
        for (const HighStmt &S : Body) {
          GuardAlias(S.Body);
          GuardAlias(S.ElseBody);
          GuardAlias(S.DefaultBody);
          for (const auto &C : S.Cases)
            GuardAlias(C.Body);
          for (const auto &Clause : S.EHClauseBodies)
            GuardAlias(Clause);
          if ((S.Kind != StmtKind::If && S.Kind != StmtKind::IfElse) || !S.Cond)
            continue;
          auto Guard = GuardMember(*S.Cond);
          if (!Guard) {
            const HighExpr *Or = unwrapIntegerView(S.Cond.get());
            auto IsCompareDisjunct = [&](auto &&Self,
                                         const HighExpr *E) -> bool {
              E = unwrapIntegerView(E);
              if (!E)
                return false;
              if (E->Kind == ExprKind::UnaryOp && E->Op == NdOp::BOOL_NOT &&
                  !E->Operands.empty())
                return Self(Self, E->Operands[0].get());
              if (E->Kind != ExprKind::BinOp || E->Operands.size() != 2)
                return E->Kind == ExprKind::Call || MemberOf(E).has_value();
              switch (E->Op) {
              case NdOp::INT_EQUAL:
              case NdOp::INT_NOTEQUAL:
              case NdOp::INT_LESS:
              case NdOp::INT_SLESS:
              case NdOp::INT_LESSEQUAL:
              case NdOp::INT_SLESSEQUAL:
              case NdOp::BOOL_OR:
              case NdOp::BOOL_AND:
                return true;
              case NdOp::INT_OR:
                return Self(Self, E->Operands[0].get()) &&
                       Self(Self, E->Operands[1].get());
              default:
                return false;
              }
            };
            auto IsLogicalOr = [&](const HighExpr *E) {
              if (!E || E->Kind != ExprKind::BinOp || E->Operands.size() != 2)
                return false;
              if (E->Op == NdOp::BOOL_OR)
                return true;
              return E->Op == NdOp::INT_OR &&
                     IsCompareDisjunct(IsCompareDisjunct,
                                       E->Operands[0].get()) &&
                     IsCompareDisjunct(IsCompareDisjunct, E->Operands[1].get());
            };
            if (IsLogicalOr(Or)) {
              std::vector<std::pair<std::string, TypeRef>> Live;
              bool Failed = false;
              std::function<void(const HighExpr *)> CollectOr =
                  [&](const HighExpr *E) {
                    E = unwrapIntegerView(E);
                    if (!E || Failed)
                      return;
                    if (E->Kind == ExprKind::BinOp && E->Op == NdOp::BOOL_AND) {
                      Failed = true;
                      return;
                    }
                    if (IsLogicalOr(E)) {
                      CollectOr(E->Operands[0].get());
                      CollectOr(E->Operands[1].get());
                      return;
                    }
                    if (auto One = GuardMember(*E)) {
                      Live.emplace_back(std::get<0>(*One), std::get<1>(*One));
                      return;
                    }
                  };
              CollectOr(Or);
              if (!Failed && Live.size() == 1)
                Guard = std::make_tuple(Live[0].first, Live[0].second, false);
            }
          }
          if (!Guard)
            continue;
          const std::string Member = std::get<0>(*Guard);
          const TypeRef MemberType = std::get<1>(*Guard);
          // Null-check alias is for pointer fields (`if (this->p) foo(t)`).
          // An integer field (`if (this->cnt)`) must not steal later
          // undeclared 8-byte call results (`cstr` / `GetLength`).
          if (!MemberType || MemberType->Kind != NdTypeKind::Ptr)
            continue;
          const std::vector<HighStmt> &Arm =
              std::get<2>(*Guard) ? S.Body : S.ElseBody;
          const uint16_t PtrSize = pointerBytes(Opts.TheArch);
          std::function<void(const HighExpr &, unsigned)> Note =
              [&](const HighExpr &E, unsigned Depth) {
                if (Depth > 8)
                  return;
                if (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) {
                  const std::string Name = varName(E.Var);
                  const bool SizeOk = E.Var.Size == 0 || E.Var.Size == PtrSize;
                  if (!Name.empty() && E.Var.Kind != MedVar::Param &&
                      !isReservedParamDisplayName(Name) &&
                      !Assigned.count(Name) && !FieldForward.count(Name) &&
                      SizeOk) {
                    FieldForward[Name] = Member;
                    if (MemberType)
                      FieldForwardTypes[Name] = MemberType;
                  }
                  if (auto It = ValueForward.find(Name);
                      It != ValueForward.end() && It->second &&
                      It->second != &E)
                    Note(*It->second, Depth + 1);
                }
                for (const ExprPtr &Op : E.Operands)
                  if (Op)
                    Note(*Op, Depth);
              };
          for (const HighStmt &ArmS : Arm) {
            if (Analysis.DeadStmts.count(&ArmS) || stmtHiddenFromC(ArmS)) {
              if (ArmS.Kind == StmtKind::Assign && ArmS.Val && ArmS.Dst &&
                  (ArmS.Dst->Kind == ExprKind::Var ||
                   ArmS.Dst->Kind == ExprKind::Phi) &&
                  ValueForward.count(varName(ArmS.Dst->Var)))
                Note(*ArmS.Val, 0);
              continue;
            }
            forEachRhsExpr(ArmS, [&](const ExprPtr &E) {
              if (E)
                Note(*E, 0);
            });
          }
        }
      };
  GuardAlias(Func.Body);
  PropagateViewCopies();
  // A join dest that also takes `0` / another value is not the member.
  // Forwarding it prints `this->m.p->id = 0` and composes Find's nKey.
  std::set<std::string> DropJoin;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return;
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      return;
    const std::string Name = varName(S.Dst->Var);
    auto It = FieldForward.find(Name);
    if (It == FieldForward.end())
      return;
    const HighExpr *Val = peelIntegerViewOps(S.Val.get());
    if (!Val)
      return;
    if (Val->Kind == ExprKind::Load && !Val->Operands.empty() &&
        Val->Operands[0]) {
      const uint16_t AccessSize = Val->Type ? Val->Type->Size : 0;
      if (auto Member = typedMemberAccess(*Val->Operands[0], AccessSize);
          Member && *Member == It->second)
        return;
      if (auto Index = typedIndexAccess(*Val->Operands[0])) {
        if (Index->Base + "[" + Index->Index + "]" == It->second)
          return;
      }
    }
    if (Val->Kind == ExprKind::Var || Val->Kind == ExprKind::Phi) {
      auto Src = FieldForward.find(varName(Val->Var));
      if (Src != FieldForward.end() && Src->second == It->second)
        return;
    }
    DropJoin.insert(Name);
  });
  for (const std::string &Name : JoinPhiNames)
    DropJoin.insert(Name);
  std::set<std::string> DropTypes;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return;
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      return;
    const std::string Name = varName(S.Dst->Var);
    if (!DropJoin.count(Name))
      return;
    const HighExpr *Val = peelIntegerViewOps(S.Val.get());
    if (Val && Val->Kind == ExprKind::Const)
      DropTypes.insert(Name);
  });
  for (const std::string &Name : JoinPhiNames)
    DropTypes.insert(Name);
  for (const std::string &Name : DropJoin)
    FieldForward.erase(Name);
  for (const std::string &Name : DropTypes)
    FieldForwardTypes.erase(Name);
  // A multi-assign map cursor (`v1 = bins[i]; v1 = v1->m_pNext`) must stay
  // a real variable.  CopyForward onto the bins temp would hide `v1 = t22`
  // and then rewrite `if (v1)` onto `t2_3`.
  for (const auto &[Name, Count] : AssignCount) {
    if (Count > 1) {
      FieldForward.erase(Name);
      if (FieldForwardTypes.count(Name))
        CopyForward.erase(Name);
    }
  }
}

void HighCWriter::noteDebugExtern(const std::string &Name,
                                  const FunctionSym &FS) {
  // A prototype in signature order would not take a call's arguments, which
  // come in the convention's order; the callee is declared without it.
  if (!positionalDebugSignature(FS))
    return;
  auto [It, Added] = DebugExternSigs.emplace(Name, FS);
  if (Added)
    return;
  if (debugSignatureKey(It->second) == debugSignatureKey(FS))
    return;
  if (debugSymRichness(FS) > debugSymRichness(It->second)) {
    DebugExternAlts[Name].push_back(It->second);
    It->second = FS;
    return;
  }
  DebugExternAlts[Name].push_back(FS);
}

void HighCWriter::noteDebugExternCallSret(const std::string &Name,
                                          const FunctionSym &FS,
                                          const HighExpr &Call) {
  if (!isMsvcPointerEncodedClassReturn(FS.ReturnType, Opts.TheArch,
                                       Opts.Format))
    return;
  const size_t SretIdx =
      isWin64MemberIndirectReturn(FS) ? static_cast<size_t>(1) : 0;
  if (SretIdx < Call.Operands.size() &&
      looksLikeHiddenSretOperand(Call.Operands[SretIdx].get()))
    DebugExternHiddenSret.insert(Name);
}

std::optional<std::string> HighCWriter::enumeratorDisplay(const TypeRef &Ty,
                                                          uint64_t Val) const {
  if (!Ty || Ty->Kind != NdTypeKind::Struct || !Ty->IsEnum)
    return std::nullopt;
  if (Dbg)
    Dbg->completeType(Ty);
  auto Name = Ty->displayFieldNameAt(Val);
  if (!Name)
    return std::nullopt;
  if (canonicalizeCProjectionIdentifier(*Name, "") != *Name)
    return std::nullopt;
  return Name;
}

TypeRef HighCWriter::enumTypeOfExpr(const HighExpr &E) const {
  const HighExpr *Inner = unwrapIntegerView(&E);
  if (!Inner)
    return nullptr;
  if (Inner->Kind == ExprKind::Load && !Inner->Operands.empty() &&
      Inner->Operands[0]) {
    if (TypeRef Member = typedMemberType(*Inner->Operands[0]);
        Member && Member->Kind == NdTypeKind::Struct && Member->IsEnum)
      return Member;
    if (Inner->Type && Inner->Type->Kind == NdTypeKind::Struct &&
        Inner->Type->IsEnum)
      return Inner->Type;
  }
  if (Inner->Kind != ExprKind::Var && Inner->Kind != ExprKind::Phi)
    return nullptr;
  const std::string Name = copyForwardName(varName(Inner->Var));
  if (auto It = FieldForwardTypes.find(Name);
      It != FieldForwardTypes.end() && It->second &&
      It->second->Kind == NdTypeKind::Struct && It->second->IsEnum)
    return It->second;
  if (auto It = EnumDestTypes.find(Name);
      It != EnumDestTypes.end() && It->second)
    return It->second;
  if (Inner->Type && Inner->Type->Kind == NdTypeKind::Struct &&
      Inner->Type->IsEnum)
    return Inner->Type;
  return nullptr;
}

TypeRef HighCWriter::enumTypeForCallArg(const HighExpr &Call, size_t Index,
                                        std::optional<uint64_t> Val) const {
  auto Consider = [&](const FunctionSym &FS) -> TypeRef {
    const TypeRef Ty = expectedDebugCallArgType(FS, Index);
    if (!Ty || Ty->Kind != NdTypeKind::Struct || !Ty->IsEnum)
      return nullptr;
    if (Dbg)
      Dbg->completeType(Ty);
    if (Val && !enumeratorDisplay(Ty, *Val))
      return nullptr;
    return Ty;
  };
  // Site FunctionSym first. debugCallee falls back to the colliding stem
  // winner when that site has no S_LOCAL params, which would hide an
  // enum overload behind a pointer prototype.
  if (Dbg && Call.CallAddr)
    if (auto FS = Dbg->resolveFunction(Call.CallAddr))
      if (TypeRef Ty = Consider(*FS))
        return Ty;
  if (auto FS = debugCallee(Call))
    if (TypeRef Ty = Consider(*FS))
      return Ty;
  const std::string Name = callIdentifier(Call);
  if (auto It = DebugExternSigs.find(Name); It != DebugExternSigs.end())
    if (TypeRef Ty = Consider(It->second))
      return Ty;
  if (auto It = DebugExternAlts.find(Name); It != DebugExternAlts.end()) {
    for (const FunctionSym &FS : It->second)
      if (TypeRef Ty = Consider(FS))
        return Ty;
  }
  return nullptr;
}

std::optional<std::string>
HighCWriter::enumConstDisplay(const std::string &DestName,
                              const HighExpr &Val) const {
  auto It = EnumDestTypes.find(DestName);
  if (It == EnumDestTypes.end() || !It->second)
    return std::nullopt;
  const HighExpr *Const = unwrapIntegerView(&Val);
  if (!Const || Const->Kind != ExprKind::Const)
    return std::nullopt;
  return enumeratorDisplay(It->second, Const->ConstVal);
}

void HighCWriter::collectEnumConstForward(const HighFunc &Func) {
  EnumDestTypes.clear();
  std::map<std::string, TypeRef> UsedAsEnum;
  std::set<std::string> Conflict;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    const HighExpr *Call = nullptr;
    if (S.Kind == StmtKind::Call && S.CallExpr)
      Call = S.CallExpr.get();
    else if (S.Val && S.Val->Kind == ExprKind::Call)
      Call = S.Val.get();
    if (!Call)
      return;
    const size_t Limit = debugCallArgLimit(*Call);
    for (size_t I = 0; I < Call->Operands.size() && I < Limit; ++I) {
      const HighExpr *Op = unwrapIntegerView(Call->Operands[I].get());
      if (!Op)
        continue;
      const TypeRef Expected = enumTypeForCallArg(*Call, I);
      if (!Expected)
        continue;
      if (Op->Kind != ExprKind::Var && Op->Kind != ExprKind::Phi)
        continue;
      const std::string Name = varName(Op->Var);
      if (Name.empty() || isReservedParamDisplayName(Name))
        continue;
      auto It = UsedAsEnum.find(Name);
      if (It != UsedAsEnum.end() &&
          (!It->second || It->second->SourceName != Expected->SourceName))
        Conflict.insert(Name);
      else
        UsedAsEnum[Name] = Expected;
    }
  });
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return;
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      return;
    const HighExpr *Val = unwrapIntegerView(S.Val.get());
    if (!Val || Val->Kind != ExprKind::Const)
      return;
    const std::string Name = varName(S.Dst->Var);
    if (Name.empty() || Conflict.count(Name))
      return;
    auto It = UsedAsEnum.find(Name);
    if (It == UsedAsEnum.end())
      return;
    EnumDestTypes[Name] = It->second;
  });
}

void HighCWriter::collectTypedPointerArgDests(const HighFunc &Func) {
  PointerArgDestTypes.clear();
  std::map<std::string, TypeRef> UsedAsPtr;
  std::set<std::string> Conflict;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    const HighExpr *Call = nullptr;
    if (S.Kind == StmtKind::Call && S.CallExpr)
      Call = S.CallExpr.get();
    else if (S.Val && S.Val->Kind == ExprKind::Call)
      Call = S.Val.get();
    if (!Call)
      return;
    const size_t Limit = debugCallArgLimit(*Call);
    for (size_t I = 0; I < Call->Operands.size() && I < Limit; ++I) {
      const HighExpr *Op = unwrapIntegerView(Call->Operands[I].get());
      if (!Op)
        continue;
      const TypeRef Expected = displayCallArgType(*Call, I);
      if (!Expected)
        continue;
      if (Op->Kind != ExprKind::Var && Op->Kind != ExprKind::Phi)
        continue;
      const std::string Name = varName(Op->Var);
      if (Name.empty() || isReservedParamDisplayName(Name))
        continue;
      auto It = UsedAsPtr.find(Name);
      if (It != UsedAsPtr.end() &&
          (!It->second || !It->second->Pointee || !Expected->Pointee ||
           It->second->Pointee->SourceName != Expected->Pointee->SourceName))
        Conflict.insert(Name);
      else
        UsedAsPtr[Name] = Expected;
    }
  });
  for (const auto &[Name, Ty] : UsedAsPtr) {
    if (!Conflict.count(Name))
      PointerArgDestTypes[Name] = Ty;
  }
}

namespace {
const HighExpr *peelCxxThrowDisplay(const HighExpr *E);
}

bool HighCWriter::isForwardableValueExpr(const HighExpr &E) const {
  // ABI widening of a parameter (`t = (i64)arg0`) is not source. Inline the
  // parameter when collectValueForward proves a single same-region use.
  if (isParamCopy(E))
    return true;
  if (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) {
    const std::string Name = varName(E.Var);
    if (auto It = ValueForward.find(Name);
        It != ValueForward.end() && It->second)
      return isForwardableValueExpr(*It->second);
    if (CopyForward.count(Name) || FieldForward.count(Name))
      return true;
    return E.Var.Kind != MedVar::Param;
  }
  if (const HighExpr *Inner = unwrapIntegerView(&E);
      Inner && Inner->Kind == ExprKind::Const)
    return true;
  if (const HighExpr *Inner = unwrapIntegerView(&E); Inner && Inner != &E)
    return isForwardableValueExpr(*Inner);
  if (E.Kind == ExprKind::BinOp && !E.Operands.empty() && E.Operands[0] &&
      E.MemoryOrdering == NdMemoryOrdering::None &&
      (E.Op == NdOp::INT_ZEXT || E.Op == NdOp::INT_SEXT ||
       (E.Op == NdOp::SUBBYTES && E.Operands.size() == 2 && E.Operands[1] &&
        E.Operands[1]->Kind == ExprKind::Const &&
        E.Operands[1]->ConstVal == 0)))
    return isForwardableValueExpr(*E.Operands[0]);
  if (E.Kind == ExprKind::BinOp && E.MemoryOrdering == NdMemoryOrdering::None &&
      (E.Op == NdOp::INT_ADD || E.Op == NdOp::INT_SUB ||
       E.Op == NdOp::INT_MULT)) {
    if (E.Operands.empty())
      return false;
    for (const ExprPtr &Op : E.Operands) {
      if (!Op || !isForwardableValueExpr(*Op))
        return false;
    }
    return true;
  }
  if (E.Kind == ExprKind::Load && !E.Operands.empty() && E.Operands[0] &&
      E.MemoryOrdering == NdMemoryOrdering::None &&
      E.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
    if (auto Slot = namedFrameSlot(*E.Operands[0])) {
      if (copyForwardName(*Slot) != *Slot)
        return true;
      // hideUnused already observed this load.  A single later use can
      // print the slot (`return var_m3C`) without dropping the store.
      // Catch objects stay on the pointer-temp path so `e.Value` remains.
      // A differently-typed carrier of a pointer slot must stay so the
      // bitcast remains (`t19 = (int64_t)(uintptr_t)(var_m8)`).
      if (!isCxxCatchObjectName(*Slot)) {
        if (const auto Disp = frameDisplacement(*E.Operands[0])) {
          if (auto It = FrameSlots.find(*Disp);
              It != FrameSlots.end() && It->second.Type && E.Type &&
              It->second.Type->Kind != E.Type->Kind) {
            // 16-byte xmm/record memcpy of an ArgList slot. 8-byte pointer
            // carriers stay so `(int64_t)(uintptr_t)(var_m8)` remains.
            if (E.Type->Size >= 16 && It->second.Type->Size == E.Type->Size)
              return true;
            // PDB names a record on a pointer-sized home. Printing already
            // uses the slot (`t2 = recordName`); keep pointer-as-int.
            if (It->second.Type->Kind == NdTypeKind::Struct &&
                !It->second.Type->IsEnum &&
                (E.Type->Kind == NdTypeKind::Int ||
                 E.Type->Kind == NdTypeKind::Ptr) &&
                E.Type->Size == getTargetRegInfo(Opts.TheArch).PointerSize)
              return true;
            return false;
          }
        }
        return true;
      }
    }
  }
  if (E.Kind == ExprKind::Call) {
    // Calls are sequenced at their HighIR statement. Folding a single-use
    // result into a later expression can move the call past another call or
    // store, even when the result itself is a scalar integer view. Keep the
    // original statement as the evaluation point.
    return false;
  }
  if (E.Kind == ExprKind::BinOp &&
      (E.Op == NdOp::ATOMIC_ADD || E.Op == NdOp::ATOMIC_XCHG ||
       E.Op == NdOp::ATOMIC_CMPXCHG) &&
      E.MemoryOrdering != NdMemoryOrdering::None)
    return true;
  if (E.Kind != ExprKind::Load || E.Operands.empty() || !E.Operands[0] ||
      E.MemoryOrdering != NdMemoryOrdering::None ||
      E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  if (typedMemberAccess(*E.Operands[0]))
    return true;
  if (cxxCatchFieldAccess(*E.Operands[0]) ||
      cxxCatchPointerName(*E.Operands[0]))
    return true;
  if (auto Slot = namedFrameSlot(*E.Operands[0]);
      Slot && isCxxCatchObjectName(*Slot))
    return true;
  const auto VA = constAddress(*E.Operands[0]);
  if (!VA)
    return false;
  const uint16_t Size = E.Type ? E.Type->Size : 0;
  return foldReadonlyScalar(*VA, Size).has_value() || imageObjectName(*VA);
}

bool HighCWriter::isImageObjectLoad(const HighExpr &E) const {
  const HighExpr *Cur = peelIntegerViewOps(&E);
  if (!Cur || Cur->Kind != ExprKind::Load || Cur->Operands.empty() ||
      !Cur->Operands[0] || Cur->MemoryOrdering != NdMemoryOrdering::None ||
      Cur->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto VA = constAddress(*Cur->Operands[0]);
  return VA && imageObjectName(*VA);
}

bool HighCWriter::isTypedMemberLoad(const HighExpr &E) const {
  const HighExpr *Cur = peelIntegerViewOps(&E);
  if (!Cur || Cur->Kind != ExprKind::Load || Cur->Operands.empty() ||
      !Cur->Operands[0] || Cur->MemoryOrdering != NdMemoryOrdering::None ||
      Cur->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  return typedMemberAccess(*Cur->Operands[0]).has_value();
}

bool HighCWriter::isTypedIndexLoad(const HighExpr &E) {
  const HighExpr *Cur = peelIntegerViewOps(&E);
  if (!Cur || Cur->Kind != ExprKind::Load || Cur->Operands.empty() ||
      !Cur->Operands[0] || Cur->MemoryOrdering != NdMemoryOrdering::None ||
      Cur->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  return typedIndexAccess(*Cur->Operands[0]).has_value();
}

bool HighCWriter::isCxxCatchObjectName(llvm::StringRef Name) const {
  for (const auto &[Clause, CatchName] : CxxCatchNames)
    if (CatchName == Name)
      return true;
  return false;
}

std::optional<std::string>
HighCWriter::cxxCatchPointerName(const HighExpr &E) const {
  const HighExpr *Cur = peelCxxThrowDisplay(&E);
  if (!Cur)
    return std::nullopt;
  if (Cur->Kind == ExprKind::Var || Cur->Kind == ExprKind::Phi) {
    const std::string Name = copyForwardName(varName(Cur->Var));
    if (auto Reach = ReachingCatchPtrs.find(Name);
        Reach != ReachingCatchPtrs.end())
      return Reach->second;
    if (auto Fwd = ValueForward.find(Name);
        Fwd != ValueForward.end() && Fwd->second)
      return cxxCatchPointerName(*Fwd->second);
    if (isCxxCatchObjectName(Name))
      return Name;
    return std::nullopt;
  }
  if (Cur->Kind == ExprKind::Load && !Cur->Operands.empty() &&
      Cur->Operands[0]) {
    if (ProjectFrameAliasesIntoStorage)
      if (const auto Disp = frameDisplacement(*Cur->Operands[0]))
        if (auto It = FrameStorageSlots.find(*Disp);
            It != FrameStorageSlots.end() &&
            isCxxCatchObjectName(It->second.Name))
          return It->second.Name;
    // Inside a catch body a load of its object's slot, even one folded into
    // a larger expression, is the object that clause names.  The slot may
    // since have moved into backing storage, so match its displacement.
    if (!OpenCatchObjects.empty())
      if (const auto Disp = frameDisplacement(*Cur->Operands[0]))
        if (auto It = CxxCatchObjectDisps.find(*Disp);
            It != CxxCatchObjectDisps.end() &&
            llvm::is_contained(OpenCatchObjects, It->second))
          return It->second;
    if (auto Slot = namedFrameSlot(*Cur->Operands[0]);
        Slot && isCxxCatchObjectName(*Slot))
      return *Slot;
  }
  return std::nullopt;
}

std::optional<std::string>
HighCWriter::cxxCatchFieldAccess(const HighExpr &Addr) const {
  const HighExpr *Cur = peelCxxThrowDisplay(&Addr);
  if (!Cur)
    return std::nullopt;
  uint64_t Off = 0;
  if (Cur->Kind == ExprKind::BinOp && Cur->Operands.size() == 2 &&
      Cur->Operands[0] && Cur->Operands[1] &&
      (Cur->Op == NdOp::INT_ADD || Cur->Op == NdOp::INT_SUB)) {
    const HighExpr *Base = peelCxxThrowDisplay(Cur->Operands[0].get());
    const HighExpr *Imm = peelCxxThrowDisplay(Cur->Operands[1].get());
    if (Imm && Imm->Kind != ExprKind::Const) {
      Base = peelCxxThrowDisplay(Cur->Operands[1].get());
      Imm = peelCxxThrowDisplay(Cur->Operands[0].get());
    }
    if (!Base || !Imm || Imm->Kind != ExprKind::Const)
      return std::nullopt;
    Off = Imm->ConstVal;
    if (Cur->Op == NdOp::INT_SUB)
      return std::nullopt;
    Cur = Base;
  }
  const auto Name = cxxCatchPointerName(*Cur);
  if (!Name)
    return std::nullopt;
  if (Off != 0 && Off != 8)
    return std::nullopt;
  return *Name + ".Value";
}

namespace {

bool valueForwardSplitsRegion(StmtKind Kind) {
  switch (Kind) {
  case StmtKind::If:
  case StmtKind::IfElse:
  case StmtKind::While:
  case StmtKind::DoWhile:
  case StmtKind::For:
  case StmtKind::Switch:
  case StmtKind::SEHTry:
  case StmtKind::CxxTry:
  case StmtKind::ItaniumTry:
    return true;
  default:
    return false;
  }
}

bool isTryKind(StmtKind Kind) {
  return Kind == StmtKind::SEHTry || Kind == StmtKind::CxxTry ||
         Kind == StmtKind::ItaniumTry;
}

bool isCxxThrowTypeIdent(llvm::StringRef Name) {
  if (Name.empty() ||
      (!std::isalpha(static_cast<unsigned char>(Name.front())) &&
       Name.front() != '_'))
    return false;
  return llvm::all_of(Name, [](char Ch) {
    return std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_';
  });
}

bool isMsvcZeroArgConstructor(llvm::StringRef Name) {
  // In a decorated MSVC constructor name, the final @XZ encodes a void
  // parameter list. Recovered call operands after `this` may be stale live
  // registers; they are not constructor arguments when this suffix is exact.
  return Name.starts_with("??0") &&
         Name.find("@@", 3) != llvm::StringRef::npos && Name.ends_with("@XZ");
}

std::optional<uint32_t> readImageU32(const BinaryImage &Img, va_t Addr) {
  const uint8_t *P = Img.readVA(Addr, 4);
  if (!P)
    return std::nullopt;
  return llvm::support::endian::read32le(P);
}

std::optional<va_t> msvcRvaOrVA(const BinaryImage &Img, uint32_t Encoded) {
  if (!Encoded)
    return std::nullopt;
  const va_t FromRva = Img.Base + Encoded;
  if (Img.containsVA(FromRva))
    return FromRva;
  if (!Img.is64Bit() && Img.containsVA(Encoded))
    return Encoded;
  return std::nullopt;
}

std::string readMsvcTypeDescriptorName(const BinaryImage &Img,
                                       va_t DescriptorVA) {
  const size_t PointerSize = Img.is64Bit() ? 8 : 4;
  if (!DescriptorVA || DescriptorVA > InvalidVA - 2 * PointerSize)
    return {};
  const va_t NameVA = DescriptorVA + 2 * PointerSize;
  std::string Name;
  for (size_t I = 0; I < limits::kMaxMsvcTypeDescriptorNameBytes; ++I) {
    if (I > InvalidVA - NameVA)
      return {};
    const uint8_t *Byte = Img.readVA(NameVA + I, 1);
    if (!Byte)
      return {};
    if (*Byte == 0)
      break;
    Name.push_back(static_cast<char>(*Byte));
  }
  if (Name.empty())
    return {};
  if (const std::string Spelling = msvcRttiTypeSpelling(Name);
      !Spelling.empty())
    return Spelling;
  llvm::StringRef Mangled(Name);
  if (Mangled.starts_with(".?A") && Mangled.size() > 4) {
    llvm::StringRef Rest = Mangled.drop_front(4);
    const size_t At = Rest.find('@');
    if (At != llvm::StringRef::npos)
      Rest = Rest.take_front(At);
    if (isCxxThrowTypeIdent(Rest))
      return Rest.str();
  }
  return {};
}

std::string readMsvcThrowInfoFirstType(const BinaryImage &Img,
                                       va_t ThrowInfoVA) {
  const auto CatchableArrayRva = readImageU32(Img, ThrowInfoVA + 12);
  if (!CatchableArrayRva)
    return {};
  const auto CatchableArray = msvcRvaOrVA(Img, *CatchableArrayRva);
  if (!CatchableArray)
    return {};
  const auto NumTypes = readImageU32(Img, *CatchableArray);
  if (!NumTypes || *NumTypes == 0)
    return {};
  const auto FirstTypeRva = readImageU32(Img, *CatchableArray + 4);
  if (!FirstTypeRva)
    return {};
  const auto CatchableType = msvcRvaOrVA(Img, *FirstTypeRva);
  if (!CatchableType)
    return {};
  const auto TypeDescRva = readImageU32(Img, *CatchableType + 4);
  if (!TypeDescRva)
    return {};
  const auto TypeDesc = msvcRvaOrVA(Img, *TypeDescRva);
  if (!TypeDesc)
    return {};
  return readMsvcTypeDescriptorName(Img, *TypeDesc);
}

const HighExpr *stmtCallExpr(const HighStmt &S) {
  if (S.Kind == StmtKind::Call && S.CallExpr)
    return S.CallExpr.get();
  if ((S.Kind == StmtKind::Assign || S.Kind == StmtKind::ExprStmt) && S.Val &&
      S.Val->Kind == ExprKind::Call)
    return S.Val.get();
  return nullptr;
}

const HighExpr *peelCxxThrowDisplay(const HighExpr *E) {
  unsigned Depth = 0;
  while (E && Depth++ < 8) {
    if ((E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast) &&
        !E->Operands.empty()) {
      E = E->Operands[0].get();
      continue;
    }
    if (E->Kind == ExprKind::UnaryOp && !E->Operands.empty() &&
        (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT ||
         E->Op == NdOp::SUBBYTES)) {
      E = E->Operands[0].get();
      continue;
    }
    break;
  }
  return E;
}

} // namespace

void HighCWriter::foldSignedJleConds(std::vector<HighStmt> &Stmts) {
  auto IsZero = [&](const HighExpr *Op) {
    Op = unwrapIntegerView(Op);
    return Op && Op->Kind == ExprKind::Const && Op->ConstVal == 0;
  };
  auto SameScalar = [&](const HighExpr *A, const HighExpr *B) {
    auto Peel = [&](const HighExpr *E) {
      unsigned Depth = 0;
      while (E && Depth++ < 6) {
        E = forwardedExpr(E);
        E = unwrapIntegerView(E);
        if (!E || E->Kind != ExprKind::BinOp || E->Operands.size() != 2)
          return E;
        if (E->Op == NdOp::INT_EQUAL || E->Op == NdOp::INT_NOTEQUAL ||
            E->Op == NdOp::INT_LESS || E->Op == NdOp::INT_LESSEQUAL ||
            E->Op == NdOp::INT_SLESS || E->Op == NdOp::INT_SLESSEQUAL)
          return E;
        if (IsZero(E->Operands[1].get()) && E->Operands[0]) {
          E = E->Operands[0].get();
          continue;
        }
        if (IsZero(E->Operands[0].get()) && E->Operands[1]) {
          E = E->Operands[1].get();
          continue;
        }
        return E;
      }
      return E;
    };
    A = Peel(A);
    B = Peel(B);
    return A && B && (A->Kind == ExprKind::Var || A->Kind == ExprKind::Phi) &&
           A->Kind == B->Kind && A->Var.Kind == B->Var.Kind &&
           A->Var.Id == B->Var.Id;
  };
  auto AsEqZero = [&](const HighExpr *Op, ExprPtr &X) {
    Op = forwardedExpr(Op);
    Op = unwrapIntegerView(Op);
    if (!Op || Op->Kind != ExprKind::BinOp || Op->Op != NdOp::INT_EQUAL ||
        Op->Operands.size() != 2)
      return false;
    if (IsZero(Op->Operands[1].get()) && Op->Operands[0]) {
      X = Op->Operands[0];
      return true;
    }
    if (IsZero(Op->Operands[0].get()) && Op->Operands[1]) {
      X = Op->Operands[1];
      return true;
    }
    return false;
  };
  auto AsSignedLtZero = [&](const HighExpr *Op, ExprPtr &X) {
    Op = forwardedExpr(Op);
    Op = unwrapIntegerView(Op);
    if (Op && Op->Kind == ExprKind::BinOp && Op->Op == NdOp::INT_NOTEQUAL &&
        Op->Operands.size() == 2) {
      const HighExpr *A = unwrapIntegerView(Op->Operands[0].get());
      const HighExpr *B = unwrapIntegerView(Op->Operands[1].get());
      if (A && A->Kind == ExprKind::BinOp && A->Op == NdOp::INT_SLESS &&
          IsZero(Op->Operands[1].get()))
        Op = A;
      else if (B && B->Kind == ExprKind::BinOp && B->Op == NdOp::INT_SLESS &&
               IsZero(Op->Operands[0].get()))
        Op = B;
    }
    if (!Op || Op->Kind != ExprKind::BinOp || Op->Op != NdOp::INT_SLESS ||
        Op->Operands.size() != 2 || !IsZero(Op->Operands[1].get()) ||
        !Op->Operands[0])
      return false;
    X = Op->Operands[0];
    return true;
  };
  std::function<void(ExprPtr &)> Fold = [&](ExprPtr &E) {
    if (!E)
      return;
    for (ExprPtr &Op : E->Operands)
      Fold(Op);
    if (E->Kind != ExprKind::UnaryOp || E->Op != NdOp::BOOL_NOT ||
        E->Operands.empty() || !E->Operands[0])
      return;
    const HighExpr *Inner = forwardedExpr(E->Operands[0].get());
    Inner = unwrapIntegerView(Inner);
    if (!Inner || Inner->Kind != ExprKind::BinOp ||
        (Inner->Op != NdOp::BOOL_OR && Inner->Op != NdOp::INT_OR) ||
        Inner->Operands.size() != 2)
      return;
    ExprPtr XEq;
    ExprPtr XLt;
    const bool LeftEq = AsEqZero(Inner->Operands[0].get(), XEq) &&
                        AsSignedLtZero(Inner->Operands[1].get(), XLt);
    const bool RightEq = AsEqZero(Inner->Operands[1].get(), XEq) &&
                         AsSignedLtZero(Inner->Operands[0].get(), XLt);
    if (!(LeftEq || RightEq) || !SameScalar(XEq.get(), XLt.get()))
      return;
    const uint16_t Sz = XEq->Type && XEq->Type->Size ? XEq->Type->Size : 4;
    E = HighExpr::makeUnary(NdOp::BOOL_NOT,
                            HighExpr::makeBinop(NdOp::INT_SLESSEQUAL, XEq,
                                                HighExpr::makeConst(0, Sz)));
  };
  std::function<void(std::vector<HighStmt> &)> Walk =
      [&](std::vector<HighStmt> &Body) {
        for (HighStmt &S : Body) {
          if (S.Cond)
            Fold(S.Cond);
          Walk(S.Body);
          Walk(S.ElseBody);
          for (SwitchCase &Case : S.Cases)
            Walk(Case.Body);
          Walk(S.DefaultBody);
          for (auto &ClauseBody : S.EHClauseBodies)
            Walk(ClauseBody);
        }
      };
  Walk(Stmts);
}

bool HighCWriter::readsThroughForwards(const HighExpr &E,
                                       const std::string &Name) const {
  std::set<std::string> Followed;
  // Expressions share subexpressions: one explored without finding the name
  // does not read it from anywhere else either.
  std::set<const HighExpr *> Explored;
  std::function<bool(const HighExpr &)> Reads = [&](const HighExpr &Cur) {
    if (!Explored.insert(&Cur).second)
      return false;
    if (Cur.Kind == ExprKind::Var || Cur.Kind == ExprKind::Phi) {
      const std::string Read = varName(Cur.Var);
      const std::string Printed = copyForwardName(Read);
      if (Read == Name || Printed == Name)
        return true;
      const auto Fwd = ValueForward.find(Printed);
      return Fwd != ValueForward.end() && Fwd->second &&
             Followed.insert(Printed).second && Reads(*Fwd->second);
    }
    bool Found = false;
    Cur.forEachChildExpr(
        [&](const ExprPtr &Child) { Found = Found || Reads(*Child); });
    return Found;
  };
  return Reads(E);
}

void HighCWriter::collectValueForward(const HighFunc &Func) {
  ValueForward.clear();
  auto containsName = [&](const HighExpr &E, const std::string &Name,
                          auto &&Self) -> bool {
    if ((E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) &&
        varName(E.Var) == Name)
      return true;
    for (const ExprPtr &Op : E.Operands)
      if (Op && Self(*Op, Name, Self))
        return true;
    return false;
  };
  struct Site {
    const HighStmt *Stmt = nullptr;
    std::string Name;
    uint64_t Region = 0;
    size_t Index = 0;
    bool Cleanup = false;
    bool Handler = false;
  };
  std::map<const HighStmt *, Site> Sites;
  std::map<uint64_t, std::vector<const HighStmt *>> Linear;
  std::map<uint64_t, uint64_t> JoinAfterTry;
  std::map<uint64_t, uint64_t> RegionParent;
  std::map<uint64_t, const HighStmt *> RegionSplit;
  std::map<uint64_t, const HighStmt *> TryForBody;
  uint64_t NextRegion = 1;
  std::function<bool(const std::vector<HighStmt> &)> RegionExits;
  RegionExits = [&](const std::vector<HighStmt> &Stmts) -> bool {
    const HighStmt *Last = nullptr;
    for (const HighStmt &S : Stmts) {
      if (Analysis.DeadStmts.count(&S) || stmtHiddenFromC(S) ||
          S.Kind == StmtKind::Nop)
        continue;
      Last = &S;
    }
    if (!Last)
      return false;
    if (Last->Kind == StmtKind::Return || Last->Kind == StmtKind::Goto)
      return true;
    if (CxxThrowPrints.count(Last))
      return true;
    if (Last->Kind == StmtKind::IfElse)
      return RegionExits(Last->Body) && RegionExits(Last->ElseBody);
    if (isTryKind(Last->Kind)) {
      if (!RegionExits(Last->Body))
        return false;
      if (Last->EHClauseBodies.empty())
        return true;
      for (const auto &ClauseBody : Last->EHClauseBodies)
        if (!RegionExits(ClauseBody))
          return false;
      return true;
    }
    const HighExpr *Call = stmtCallExpr(*Last);
    return Call && isMsvcCxxThrowCallName(Call->CallTarget);
  };
  auto catchJoinAfterTry = [&](const HighStmt &S) {
    bool HasCatch = false;
    for (size_t I = 0; I < S.EHClauseBodies.size(); ++I) {
      if (I < S.EHClauses.size() &&
          S.EHClauses[I].Kind == HighEHClauseKind::CxxCleanup)
        continue;
      HasCatch = true;
      if (!RegionExits(S.EHClauseBodies[I]))
        return false;
    }
    return HasCatch || S.EHClauseBodies.empty();
  };
  std::function<void(const std::vector<HighStmt> &, uint64_t, bool, bool)> Walk;
  Walk = [&](const std::vector<HighStmt> &Stmts, uint64_t Region, bool Cleanup,
             bool Handler) {
    auto &Seq = Linear[Region];
    for (const HighStmt &S : Stmts) {
      Site Info;
      Info.Stmt = &S;
      Info.Region = Region;
      Info.Index = Seq.size();
      Info.Cleanup = Cleanup;
      Info.Handler = Handler;
      Sites[&S] = Info;
      Seq.push_back(&S);
      if (valueForwardSplitsRegion(S.Kind)) {
        const uint64_t BodyRegion = NextRegion++;
        RegionParent[BodyRegion] = Region;
        RegionSplit[BodyRegion] = &S;
        Walk(S.Body, BodyRegion, false, Handler);
        if (isTryKind(S.Kind) && catchJoinAfterTry(S)) {
          JoinAfterTry[BodyRegion] = Region;
          TryForBody[BodyRegion] = &S;
        }
        if (!S.ElseBody.empty()) {
          const uint64_t ElseRegion = NextRegion++;
          RegionParent[ElseRegion] = Region;
          RegionSplit[ElseRegion] = &S;
          Walk(S.ElseBody, ElseRegion, false, Handler);
        }
        for (const SwitchCase &Case : S.Cases) {
          const uint64_t CaseRegion = NextRegion++;
          RegionParent[CaseRegion] = Region;
          RegionSplit[CaseRegion] = &S;
          Walk(Case.Body, CaseRegion, false, Handler);
        }
        if (!S.DefaultBody.empty()) {
          const uint64_t DefaultRegion = NextRegion++;
          RegionParent[DefaultRegion] = Region;
          RegionSplit[DefaultRegion] = &S;
          Walk(S.DefaultBody, DefaultRegion, false, Handler);
        }
        for (size_t I = 0; I < S.EHClauseBodies.size(); ++I) {
          const bool ClauseCleanup =
              I < S.EHClauses.size() &&
              S.EHClauses[I].Kind == HighEHClauseKind::CxxCleanup;
          const uint64_t ClauseRegion = NextRegion++;
          RegionParent[ClauseRegion] = Region;
          RegionSplit[ClauseRegion] = &S;
          Walk(S.EHClauseBodies[I], ClauseRegion, ClauseCleanup, true);
        }
        continue;
      }
      Walk(S.Body, Region, Cleanup, Handler);
      Walk(S.ElseBody, Region, Cleanup, Handler);
      for (const SwitchCase &Case : S.Cases)
        Walk(Case.Body, Region, Cleanup, Handler);
      Walk(S.DefaultBody, Region, Cleanup, Handler);
      for (const auto &ClauseBody : S.EHClauseBodies)
        Walk(ClauseBody, Region, Cleanup, true);
    }
  };
  Walk(Func.Body, NextRegion++, false, false);

  auto regionDominatedBy = [&](uint64_t Child, uint64_t Ancestor) {
    unsigned Depth = 0;
    while (Child && Depth++ < 64) {
      if (Child == Ancestor)
        return true;
      auto It = RegionParent.find(Child);
      if (It == RegionParent.end())
        return false;
      Child = It->second;
    }
    return false;
  };

  // isForwardableValueExpr keeps a call at its own statement so that folding
  // cannot reorder it with another call or store. Returning the result from
  // the next printed statement moves nothing: `v = f(x); return v;` is
  // `return f(x);`. That return must print its value, and no label may let
  // another path reach it without making the call.
  auto callResultReturnedNext = [&](const HighStmt &Def, const Site &Info) {
    const HighExpr &Call = *Def.Val;
    if (Call.Kind != ExprKind::Call || Info.Cleanup || Info.Handler ||
        InferredVoid || !IndirectReturnName.empty() ||
        Call.IntrinsicId != Intrinsic::None ||
        Call.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        (Call.CallTarget.empty() && Call.CallAddr == 0) ||
        isNoreturnCallExpr(Call) || isMsvcCxxThrowCallName(Call.CallTarget) ||
        knownVoidCall(Call) || isVoidSelfCall(Call))
      return false;
    const auto SeqIt = Linear.find(Info.Region);
    if (SeqIt == Linear.end())
      return false;
    const std::vector<const HighStmt *> &Seq = SeqIt->second;
    for (size_t I = Info.Index + 1; I < Seq.size(); ++I) {
      const HighStmt &Next = *Seq[I];
      if (isLabelAddress(Next.Addr))
        return false;
      if (Analysis.DeadStmts.count(&Next) || stmtHiddenFromC(Next) ||
          Next.Kind == StmtKind::Block)
        continue;
      if (Next.Kind != StmtKind::Return || !Next.RetVal)
        return false;
      const HighExpr *Returned = peelIntegerViewOps(Next.RetVal.get());
      return Returned &&
             (Returned->Kind == ExprKind::Var ||
              Returned->Kind == ExprKind::Phi) &&
             varName(Returned->Var) == varName(Def.Dst->Var);
    }
    return false;
  };

  // A test of the result right after the call moves nothing either:
  // `v = f(x); if (v < 0)` is `if (f(x) < 0)`.  The condition holds no
  // other variable, whose own folding could put a read beside the call, and
  // reaches the result only through operators that print each operand once
  // and always evaluate it, so the call runs once, where it ran.
  auto callResultTestedNext = [&](const HighStmt &Def, const Site &Info) {
    const HighExpr &Call = *Def.Val;
    if (Call.Kind != ExprKind::Call || Info.Cleanup || Info.Handler ||
        Def.Dst->Var.Kind == MedVar::Stack ||
        Call.IntrinsicId != Intrinsic::None || !Call.IntrinsicOutputs.empty() ||
        Call.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        (Call.CallTarget.empty() && Call.CallAddr == 0) ||
        isNoreturnCallExpr(Call) || isMsvcCxxThrowCallName(Call.CallTarget) ||
        knownVoidCall(Call) || isVoidSelfCall(Call))
      return false;
    const auto SeqIt = Linear.find(Info.Region);
    if (SeqIt == Linear.end())
      return false;
    const std::vector<const HighStmt *> &Seq = SeqIt->second;
    const std::string Name = varName(Def.Dst->Var);
    for (size_t I = Info.Index + 1; I < Seq.size(); ++I) {
      const HighStmt &Next = *Seq[I];
      if (isLabelAddress(Next.Addr))
        return false;
      if (Analysis.DeadStmts.count(&Next) || stmtHiddenFromC(Next) ||
          Next.Kind == StmtKind::Block)
        continue;
      if ((Next.Kind != StmtKind::If && Next.Kind != StmtKind::IfElse) ||
          !Next.Cond)
        return false;
      unsigned Reads = 0;
      std::function<bool(const HighExpr &)> Plain = [&](const HighExpr &E) {
        if (E.Kind == ExprKind::Const)
          return true;
        if (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) {
          ++Reads;
          return varName(E.Var) == Name;
        }
        const bool Once =
            (E.Kind == ExprKind::Cast || E.Kind == ExprKind::BitCast) ||
            (E.Kind == ExprKind::UnaryOp &&
             (E.Op == NdOp::INT_ZEXT || E.Op == NdOp::INT_SEXT ||
              E.Op == NdOp::BOOL_NOT)) ||
            (E.Kind == ExprKind::BinOp &&
             (E.Op == NdOp::INT_EQUAL || E.Op == NdOp::INT_NOTEQUAL ||
              E.Op == NdOp::INT_LESS || E.Op == NdOp::INT_LESSEQUAL ||
              E.Op == NdOp::INT_SLESS || E.Op == NdOp::INT_SLESSEQUAL ||
              E.Op == NdOp::INT_AND || E.Op == NdOp::SUBBYTES));
        if (!Once || E.IntrinsicId != Intrinsic::None ||
            !E.IntrinsicOutputs.empty() || E.IndirectTarget)
          return false;
        if (E.Kind == ExprKind::BinOp && E.Op == NdOp::SUBBYTES &&
            (E.Operands.size() != 2 || !E.Operands[1] ||
             E.Operands[1]->Kind != ExprKind::Const ||
             E.Operands[1]->ConstVal != 0))
          return false;
        return llvm::all_of(E.Operands, [&](const ExprPtr &Operand) {
          return Operand && Plain(*Operand);
        });
      };
      return Plain(*Next.Cond) && Reads == 1;
    }
    return false;
  };

  struct Candidate {
    const HighStmt *Stmt = nullptr;
    std::string Name;
    uint64_t Region = 0;
    size_t Index = 0;
    bool PureLoad = false;
    bool Cleanup = false;
  };
  std::vector<Candidate> Candidates;
  std::set<const HighStmt *> CandidateStmts;
  for (const auto &[Stmt, Info] : Sites) {
    (void)Info;
    if (Analysis.DeadStmts.count(Stmt) || stmtHiddenFromC(*Stmt))
      continue;
    if (Stmt->Kind != StmtKind::Assign || !Stmt->Dst || !Stmt->Val)
      continue;
    if (Stmt->Dst->Kind != ExprKind::Var)
      continue;
    if (Stmt->Dst->Var.Kind == MedVar::Param)
      continue;
    if (Stmt->Dst->Var.Kind == MedVar::Stack) {
      const int64_t Off = Stmt->Dst->Var.StackOff;
      // A named source local is part of the useful C projection. Keep its
      // assignment and declaration instead of folding it into the return.
      if (Dbg) {
        if (auto Var = Dbg->resolveVariable(Func.Entry, Off);
            Var && !Var->Name.empty())
          continue;
      }
      const uint64_t Mag =
          Off < 0 ? uint64_t{0} - uint64_t(Off) : uint64_t(Off);
      const std::string Generated =
          (Off < 0 ? "var_m" : "var_") + llvm::utohexstr(Mag);
      bool SourceNamed = false;
      for (const HighLocal &Local : Func.Locals)
        if (Local.StackOff == Off && !Local.Name.empty() &&
            Local.Name != Generated) {
          SourceNamed = true;
          break;
        }
      if (SourceNamed)
        continue;
    }
    if (Stmt->IsPhiCopy || Stmt->KeepsName)
      continue;
    const bool SavedEH = InEHClauseBody;
    if (Info.Handler)
      InEHClauseBody = true;
    const bool Fwdable = isForwardableValueExpr(*Stmt->Val) ||
                         isTypedIndexLoad(*Stmt->Val) ||
                         callResultReturnedNext(*Stmt, Info) ||
                         callResultTestedNext(*Stmt, Info);
    const HighExpr *Src = peelIntegerViewOps(Stmt->Val.get());
    const bool LosesFieldNarrowing =
        Src && !fieldProjectionDoesNotNarrow(Stmt->Val.get(), Src) &&
        (Src->Kind == ExprKind::Load ||
         ((Src->Kind == ExprKind::Var || Src->Kind == ExprKind::Phi) &&
          FieldForwardTypes.count(varName(Src->Var))));
    const bool NamedSlotLoad = Src && Src->Kind == ExprKind::Load &&
                               !Src->Operands.empty() && Src->Operands[0] &&
                               namedFrameSlot(*Src->Operands[0]);
    InEHClauseBody = SavedEH;
    if (Analysis.OmittedCallResults.count(Stmt) || !Fwdable ||
        LosesFieldNarrowing)
      continue;
    if (Info.Cleanup) {
      if (!Src || (Src->Kind != ExprKind::Var && Src->Kind != ExprKind::Phi &&
                   Src->Kind != ExprKind::Addr && !NamedSlotLoad))
        continue;
    }
    const std::string Name = varName(Stmt->Dst->Var);
    if (Name.empty() || FieldForward.count(Name) || CopyForward.count(Name) ||
        JoinPhiNames.count(Name) || isReservedParamDisplayName(Name) ||
        isAddressTakenSlot(Name) ||
        containsName(*Stmt->Val, Name, containsName))
      continue;
    // Store forwarding has already rendered these uses into cached text.
    // Hiding a definition here cannot rewrite the name inside that text, even
    // when the remaining HighIR contains only one ordinary use of it.
    if (llvm::any_of(Analysis.StoreFwdDeps, [&](const auto &Entry) {
          return Entry.second.count(Name) != 0;
        }))
      continue;
    Candidate C;
    C.Stmt = Stmt;
    C.Name = Name;
    C.Region = Info.Region;
    C.Index = Info.Index;
    C.PureLoad = Stmt->Val->Kind == ExprKind::Load;
    C.Cleanup = Info.Cleanup;
    Candidates.push_back(C);
    CandidateStmts.insert(Stmt);
  }

  // Each candidate below rescans every site.  Forwarding only folds
  // single-use temporaries into their use, so a body this large keeps them.
  if (Sites.size() > limits::kMaxValueForwardSites)
    return;
  // The candidate defining each name, whose value a fold may bring along.
  std::map<std::string, const HighStmt *> CandidateDefs;
  for (const Candidate &C : Candidates)
    CandidateDefs.emplace(C.Name, C.Stmt);
  // Which sites define each name, which name it, and how often each site
  // reads it, in site order: a candidate then looks only at the sites that
  // name it rather than rescanning every site.  A read in an assignment's
  // destination is counted apart, since it is skipped for the name the
  // destination writes.
  struct SiteNames {
    std::unordered_map<std::string, unsigned> Reads, DestReads;
    std::string DestName;
  };
  std::unordered_map<const HighStmt *, SiteNames> NamesAt;
  std::unordered_map<std::string, std::vector<const HighStmt *>> DefsOf;
  std::unordered_map<std::string, std::vector<const HighStmt *>> SitesNaming;
  std::function<void(const HighExpr &,
                     std::unordered_map<std::string, unsigned> &)>
      CountReads = [&](const HighExpr &E,
                       std::unordered_map<std::string, unsigned> &Into) {
        // A call's reads stop at its debug argument limit.
        if (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi)
          ++Into[varName(E.Var)];
        if (E.Kind == ExprKind::Call) {
          const size_t Limit = debugCallArgLimit(E);
          for (size_t I = 0; I < E.Operands.size() && I < Limit; ++I)
            if (E.Operands[I])
              CountReads(*E.Operands[I], Into);
          if (E.IndirectTarget)
            CountReads(*E.IndirectTarget, Into);
          return;
        }
        for (const ExprPtr &Op : E.Operands)
          if (Op)
            CountReads(*Op, Into);
      };
  for (const auto &[Stmt, Info] : Sites) {
    (void)Info;
    SiteNames &Names = NamesAt[Stmt];
    const bool Assign = Stmt->Kind == StmtKind::Assign && Stmt->Dst;
    if (Assign)
      Names.DestName = varName(Stmt->Dst->Var);
    forEachExpr(*Stmt, [&](const ExprPtr &E) {
      if (E)
        CountReads(*E,
                   Assign && E == Stmt->Dst ? Names.DestReads : Names.Reads);
    });
    std::set<std::string> Named;
    for (const auto &Entry : Names.Reads)
      Named.insert(Entry.first);
    for (const auto &Entry : Names.DestReads)
      Named.insert(Entry.first);
    if (Assign && (Stmt->Dst->Kind == ExprKind::Var ||
                   Stmt->Dst->Kind == ExprKind::Phi)) {
      DefsOf[Names.DestName].push_back(Stmt);
      Named.insert(Names.DestName);
    }
    for (const std::string &Name : Named)
      SitesNaming[Name].push_back(Stmt);
  }
  static const std::vector<const HighStmt *> NoSites;
  auto sitesIn =
      [](const auto &Index,
         const std::string &Name) -> const std::vector<const HighStmt *> & {
    const auto It = Index.find(Name);
    return It == Index.end() ? NoSites : It->second;
  };
  auto scalarSourceRedefined = [&](const Candidate &C) {
    if (!C.Stmt->Val)
      return true;
    const HighExpr *Src = peelIntegerViewOps(C.Stmt->Val.get());
    if (!Src || (Src->Kind != ExprKind::Var && Src->Kind != ExprKind::Phi))
      return true;
    const std::string SrcName = varName(Src->Var);
    for (const HighStmt *DefStmt : sitesIn(DefsOf, SrcName)) {
      if (DefStmt == C.Stmt)
        continue;
      const Site &DefInfo = Sites.at(DefStmt);
      if (DefInfo.Region == C.Region && DefInfo.Index < C.Index)
        continue;
      if (DefInfo.Region != C.Region &&
          regionDominatedBy(C.Region, DefInfo.Region))
        continue;
      return true;
    }
    return false;
  };

  for (const Candidate &C : Candidates) {
    unsigned Uses = 0;
    bool Redefined = false;
    bool OtherRegion = false;
    const HighStmt *UseStmt = nullptr;
    auto valIsReloadable = [&](const HighExpr &E) {
      if (isReloadableLoad(E))
        return true;
      const HighExpr *Cur = peelIntegerViewOps(&E);
      if (!Cur || (Cur->Kind != ExprKind::Var && Cur->Kind != ExprKind::Phi))
        return false;
      const std::string Src = varName(Cur->Var);
      for (const HighStmt *Stmt : sitesIn(DefsOf, Src))
        if (Stmt->Val && isReloadableLoad(*Stmt->Val))
          return true;
      return false;
    };
    const bool ReloadableLoad = C.Stmt->Val && valIsReloadable(*C.Stmt->Val);
    std::vector<const HighStmt *> UseStmts;
    std::set<uint64_t> DefiningRegions;
    for (const HighStmt *Stmt : sitesIn(DefsOf, C.Name))
      if (Stmt != C.Stmt)
        DefiningRegions.insert(Sites.at(Stmt).Region);
    if (!DefiningRegions.empty())
      continue;
    // A site that neither reads nor writes the name changes nothing below.
    for (const HighStmt *Stmt : sitesIn(SitesNaming, C.Name)) {
      const Site &Info = Sites.at(Stmt);
      if (Stmt == C.Stmt || Analysis.DeadStmts.count(Stmt))
        continue;
      if (stmtHiddenFromC(*Stmt)) {
        // A dest already forwarded in this pass still consumes its source
        // (`t99 = (i32)v117` then Format(t99)). Skipping it drops Uses to 0
        // when pointer order forwards the widening first.
        const bool ForwardedDest = Stmt->Kind == StmtKind::Assign &&
                                   Stmt->Dst &&
                                   (Stmt->Dst->Kind == ExprKind::Var ||
                                    Stmt->Dst->Kind == ExprKind::Phi) &&
                                   ValueForward.count(varName(Stmt->Dst->Var));
        if (!ForwardedDest)
          continue;
      }
      if (Info.Region != C.Region && DefiningRegions.count(Info.Region))
        continue;
      if (Stmt->Kind == StmtKind::Assign && Stmt->Dst &&
          (Stmt->Dst->Kind == ExprKind::Var ||
           Stmt->Dst->Kind == ExprKind::Phi) &&
          varName(Stmt->Dst->Var) == C.Name) {
        Redefined = true;
        continue;
      }
      const SiteNames &Names = NamesAt.at(Stmt);
      const auto ReadsOf = [&](const auto &Reads) {
        const auto It = Reads.find(C.Name);
        return It == Reads.end() ? 0u : It->second;
      };
      const unsigned Local =
          ReadsOf(Names.Reads) +
          (Names.DestName == C.Name ? 0u : ReadsOf(Names.DestReads));
      if (!Local)
        continue;
      Uses += Local;
      UseStmts.push_back(Stmt);
      const bool JoinUse = !Info.Cleanup && C.PureLoad &&
                           JoinAfterTry.count(C.Region) &&
                           JoinAfterTry[C.Region] == Info.Region;
      const bool NestedScalarView =
          !Info.Cleanup && C.Stmt->Val && isIntegerViewOfScalar(*C.Stmt->Val) &&
          regionDominatedBy(Info.Region, C.Region) && !scalarSourceRedefined(C);
      const bool NestedReloadableLoad =
          ReloadableLoad && !Info.Cleanup &&
          regionDominatedBy(Info.Region, C.Region);
      const bool SameCleanupRegion =
          C.Cleanup && Info.Cleanup && Info.Region == C.Region;
      if (Info.Cleanup && !SameCleanupRegion)
        OtherRegion = true;
      else if (Info.Region != C.Region && !JoinUse && !NestedScalarView &&
               !NestedReloadableLoad)
        OtherRegion = true;
      else
        UseStmt = Stmt;
    }
    if (Redefined || OtherRegion || !UseStmt || Uses == 0)
      continue;
    if (!ReloadableLoad && Uses != 1)
      continue;
    const auto UseIt = Sites.find(UseStmt);
    if (UseIt == Sites.end())
      continue;
    if (UseIt->second.Region == C.Region) {
      if (UseIt->second.Index <= C.Index)
        continue;
    } else if (C.Stmt->Val && isIntegerViewOfScalar(*C.Stmt->Val) &&
               regionDominatedBy(UseIt->second.Region, C.Region)) {
    } else if (ReloadableLoad &&
               regionDominatedBy(UseIt->second.Region, C.Region)) {
    } else {
      const auto TryIt = TryForBody.find(C.Region);
      const auto TrySite =
          TryIt == TryForBody.end() ? Sites.end() : Sites.find(TryIt->second);
      if (TrySite == Sites.end() ||
          UseIt->second.Index <= TrySite->second.Index)
        continue;
    }
    // A folded value is evaluated again at its use.  Nothing that runs
    // between the definition and a use may assign a variable it reads,
    // including through other candidates folded into it, nor, for a load,
    // write the memory it reads.  Candidate statements count: one that is
    // not folded stays and may reassign a merged name.  What runs first at a
    // use: the statements of C's region up to the one holding it, then,
    // inside each compound statement on the way down, its controlling
    // expression and the statements before the use; a loop on the way runs
    // its whole body between two evaluations.  In the use statement itself
    // no other side effect may be unsequenced with a load: one top-level
    // call holding the use is fine.  A use right after the try whose body
    // defines the value (every catch leaves) sees the rest of that body and
    // the statements between.
    {
      std::set<std::string> Reads;
      bool ReadsMemory = false;
      bool EffectfulLoadAddress = false;
      auto ExprWrites = [&](const HighExpr &E, const auto &Self) -> bool {
        if (E.Kind == ExprKind::Call || E.Kind == ExprKind::Store ||
            E.MemoryOrdering != NdMemoryOrdering::None)
          return true;
        bool Hit = false;
        E.forEachChildExpr(
            [&](const ExprPtr &Child) { Hit = Hit || Self(*Child, Self); });
        return Hit;
      };
      std::function<void(const HighExpr &, unsigned)> Collect =
          [&](const HighExpr &E, unsigned Depth) {
            if (Depth > 64) {
              ReadsMemory = true;
              EffectfulLoadAddress = true;
              return;
            }
            if (E.Kind == ExprKind::Load)
              E.forEachChildExpr([&](const ExprPtr &Address) {
                EffectfulLoadAddress |= ExprWrites(*Address, ExprWrites);
              });
            if (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) {
              const std::string Read = varName(E.Var);
              if (!Reads.insert(Read).second)
                return;
              if (auto It = ValueForward.find(Read);
                  It != ValueForward.end() && It->second)
                Collect(*It->second, Depth + 1);
              if (auto It = CandidateDefs.find(Read);
                  It != CandidateDefs.end() && It->second->Val)
                Collect(*It->second->Val, Depth + 1);
            }
            if (E.Kind == ExprKind::Load || E.Kind == ExprKind::Call ||
                E.Kind == ExprKind::Store)
              ReadsMemory = true;
            E.forEachChildExpr(
                [&](const ExprPtr &Child) { Collect(*Child, Depth + 1); });
          };
      Collect(*C.Stmt->Val, 0);
      // A snapshot cannot replay calls, stores or ordered loads embedded in
      // its address, even when no statement between the definition and use
      // changes memory. This also checks loads reached through candidates.
      if (EffectfulLoadAddress)
        continue;
      const HighExpr *Load = peelIntegerViewOps(C.Stmt->Val.get());
      if (!Load || Load->Kind != ExprKind::Load || Load->Operands.empty() ||
          !Load->Operands[0])
        Load = nullptr;
      const std::optional<int64_t> Slot =
          Load ? frameDisplacement(*Load->Operands[0]) : std::nullopt;
      const std::optional<std::string> SlotName =
          Load ? namedFrameSlot(*Load->Operands[0]) : std::nullopt;
      // Memory reached otherwise than through this one named slot.
      const bool Escapes = ReadsMemory && (!Load || !Slot || !SlotName ||
                                           isAddressTakenSlot(*SlotName) ||
                                           Load != C.Stmt->Val.get());
      const int64_t Width = Load && Load->Type ? Load->Type->Size : 8;
      auto Overlaps = [&](int64_t Disp, int64_t Size) {
        return Disp < *Slot + Width && *Slot < Disp + Size;
      };
      std::function<bool(const HighStmt &)> Changes =
          [&](const HighStmt &S) -> bool {
        if (Analysis.DeadStmts.count(&S))
          return false;
        if (S.Kind == StmtKind::Assign && S.Dst &&
            (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi) &&
            Reads.count(varName(S.Dst->Var)))
          return true;
        if (ReadsMemory) {
          const HighExpr *WriteAddr = nullptr;
          TypeRef WriteType;
          if (S.Kind == StmtKind::Store && S.StoreAddr) {
            WriteAddr = S.StoreAddr.get();
            WriteType = S.StoreVal ? S.StoreVal->Type : nullptr;
          } else if (S.Kind == StmtKind::Assign && S.Dst &&
                     S.Dst->Kind == ExprKind::Load &&
                     !S.Dst->Operands.empty() && S.Dst->Operands[0]) {
            // A load used as an assignment destination writes its memory.
            WriteAddr = S.Dst->Operands[0].get();
            WriteType = S.Dst->Type;
          }
          if (WriteAddr) {
            const auto Disp = frameDisplacement(*WriteAddr);
            const int64_t Size = WriteType ? WriteType->Size : 8;
            if (Escapes ? !Disp || !Slot || Overlaps(*Disp, Size)
                        : Disp && Overlaps(*Disp, Size))
              return true;
          }
          if (S.Kind == StmtKind::Assign && S.Dst &&
              S.Dst->Kind == ExprKind::Var &&
              S.Dst->Var.Kind == MedVar::Stack &&
              (!Slot || Overlaps(S.Dst->Var.StackOff, S.Dst->Var.Size)))
            return true;
        }
        bool Hit = false;
        forEachExpr(S, [&](const ExprPtr &E) {
          if (!E)
            return;
          Hit = Hit || (Escapes && ExprWrites(*E, ExprWrites));
          if (E->Kind == ExprKind::Call)
            for (const MedVar &Output : E->IntrinsicOutputs)
              Hit = Hit || Reads.count(varName(Output)) != 0;
        });
        if (Hit)
          return true;
        for (const auto *Child : {&S.Body, &S.ElseBody, &S.DefaultBody})
          for (const HighStmt &Inner : *Child)
            if (Changes(Inner))
              return true;
        for (const SwitchCase &Case : S.Cases)
          for (const HighStmt &Inner : Case.Body)
            if (Changes(Inner))
              return true;
        for (const std::vector<HighStmt> &Clause : S.EHClauseBodies)
          for (const HighStmt &Inner : Clause)
            if (Changes(Inner))
              return true;
        return false;
      };
      bool Changed = false;
      const auto SeqIt = Linear.find(C.Region);
      if (SeqIt == Linear.end())
        continue;
      const auto &Seq = SeqIt->second;
      auto ChangesBefore = [&](uint64_t Region, size_t Index) {
        const auto RegionSeq = Linear.find(Region);
        if (RegionSeq == Linear.end())
          return true;
        for (size_t I = 0; I < Index && I < RegionSeq->second.size(); ++I)
          if (RegionSeq->second[I] && Changes(*RegionSeq->second[I]))
            return true;
        return false;
      };
      size_t Gate = C.Index;
      for (const HighStmt *US : UseStmts) {
        const auto UIt = Sites.find(US);
        if (UIt == Sites.end()) {
          Changed = true;
          break;
        }
        if (ReadsMemory) {
          const HighExpr *Top = US->CallExpr ? US->CallExpr.get()
                                : US->Kind == StmtKind::Return
                                    ? US->RetVal.get()
                                    : US->Val.get();
          bool OtherEffect = false;
          forEachExpr(*US, [&](const ExprPtr &E) {
            if (!E)
              return;
            if (E.get() == Top && E->Kind == ExprKind::Call) {
              E->forEachChildExpr([&](const ExprPtr &Arg) {
                OtherEffect = OtherEffect || ExprWrites(*Arg, ExprWrites);
              });
              return;
            }
            OtherEffect = OtherEffect || ExprWrites(*E, ExprWrites);
          });
          Changed = Changed || (OtherEffect && Escapes) ||
                    (OtherEffect && US->Kind == StmtKind::Store);
        }
        if (const auto Join = JoinAfterTry.find(C.Region);
            Join != JoinAfterTry.end() && Join->second == UIt->second.Region) {
          const auto TryIt = TryForBody.find(C.Region);
          const auto TrySite = TryIt == TryForBody.end()
                                   ? Sites.end()
                                   : Sites.find(TryIt->second);
          const auto JoinSeq = Linear.find(UIt->second.Region);
          if (TrySite == Sites.end() || JoinSeq == Linear.end()) {
            Changed = true;
            break;
          }
          for (size_t I = C.Index + 1; !Changed && I < Seq.size(); ++I)
            Changed = Seq[I] && Changes(*Seq[I]);
          for (size_t I = TrySite->second.Index + 1;
               !Changed && I < UIt->second.Index && I < JoinSeq->second.size();
               ++I)
            Changed = JoinSeq->second[I] && Changes(*JoinSeq->second[I]);
          continue;
        }
        uint64_t Region = UIt->second.Region;
        size_t Index = UIt->second.Index;
        for (unsigned Depth = 0; !Changed && Region != C.Region; ++Depth) {
          const auto Parent = RegionParent.find(Region);
          const auto Split = RegionSplit.find(Region);
          if (Depth >= 64 || Parent == RegionParent.end() ||
              Split == RegionSplit.end() || !Split->second) {
            Changed = true;
            break;
          }
          const HighStmt &Holder = *Split->second;
          const auto HIt = Sites.find(&Holder);
          if (HIt == Sites.end()) {
            Changed = true;
            break;
          }
          if (Holder.Kind == StmtKind::While ||
              Holder.Kind == StmtKind::DoWhile ||
              Holder.Kind == StmtKind::For) {
            Changed = Changes(Holder);
          } else {
            Changed = ChangesBefore(Region, Index);
            forEachExpr(Holder, [&](const ExprPtr &E) {
              Changed = Changed || (E && Escapes && ExprWrites(*E, ExprWrites));
            });
          }
          Region = Parent->second;
          Index = HIt->second.Index;
        }
        if (Region != C.Region) {
          Changed = true;
          break;
        }
        Gate = std::max(Gate, Index);
      }
      for (size_t I = C.Index + 1; !Changed && I < Gate && I < Seq.size(); ++I)
        Changed = Seq[I] && Changes(*Seq[I]);
      if (Changed)
        continue;
    }
    auto stmtWrites = [&](const HighStmt &S) {
      if (S.Kind == StmtKind::Store)
        return true;
      bool HasCall = S.CallExpr != nullptr;
      forEachExpr(S, [&](const ExprPtr &E) {
        if (E && E->Kind == ExprKind::Call)
          HasCall = true;
      });
      return HasCall;
    };
    if (ReloadableLoad && (Uses != 1 || UseIt->second.Region != C.Region)) {
      const auto SeqIt = Linear.find(C.Region);
      if (SeqIt == Linear.end())
        continue;
      const auto &Seq = SeqIt->second;
      size_t LastGate = C.Index;
      std::set<const HighStmt *> ParentUses;
      for (const HighStmt *US : UseStmts) {
        const auto UIt = Sites.find(US);
        if (UIt == Sites.end())
          continue;
        if (UIt->second.Region == C.Region) {
          LastGate = std::max(LastGate, UIt->second.Index);
          ParentUses.insert(US);
          continue;
        }
        uint64_t Child = UIt->second.Region;
        for (unsigned Depth = 0; Child && Depth < 64; ++Depth) {
          auto Pit = RegionParent.find(Child);
          if (Pit == RegionParent.end())
            break;
          if (Pit->second == C.Region) {
            auto Sit = RegionSplit.find(Child);
            if (Sit != RegionSplit.end() && Sit->second) {
              auto SplitIt = Sites.find(Sit->second);
              if (SplitIt != Sites.end()) {
                LastGate = std::max(LastGate, SplitIt->second.Index);
                ParentUses.insert(Sit->second);
              }
            }
            break;
          }
          Child = Pit->second;
        }
      }
      bool Effect = false;
      for (size_t I = C.Index + 1; I <= LastGate && I < Seq.size(); ++I) {
        const HighStmt *Mid = Seq[I];
        if (!Mid || Analysis.DeadStmts.count(Mid) || stmtHiddenFromC(*Mid) ||
            CandidateStmts.count(Mid) || Mid->Kind == StmtKind::Nop ||
            Mid->Kind == StmtKind::Block)
          continue;
        if (ParentUses.count(Mid)) {
          if (stmtWrites(*Mid)) {
            for (const HighStmt *US : UseStmts) {
              const auto UIt = Sites.find(US);
              if (UIt != Sites.end() && UIt->second.Region != C.Region &&
                  regionDominatedBy(UIt->second.Region, C.Region)) {
                Effect = true;
                break;
              }
            }
          }
          if (Effect)
            break;
          continue;
        }
        if (stmtWrites(*Mid)) {
          Effect = true;
          break;
        }
      }
      if (Effect)
        continue;
    } else if (!C.PureLoad &&
               !(C.Stmt->Val && isIntegerViewOfScalar(*C.Stmt->Val))) {
      const auto SeqIt = Linear.find(C.Region);
      if (SeqIt == Linear.end())
        continue;
      const auto &Seq = SeqIt->second;
      bool Effect = false;
      for (size_t I = C.Index + 1; I < UseIt->second.Index && I < Seq.size();
           ++I) {
        const HighStmt *Mid = Seq[I];
        if (!Mid || Analysis.DeadStmts.count(Mid) || stmtHiddenFromC(*Mid) ||
            CandidateStmts.count(Mid) || Mid->Kind == StmtKind::Nop ||
            Mid->Kind == StmtKind::Block)
          continue;
        Effect = true;
        break;
      }
      if (Effect)
        continue;
    }
    if (C.Stmt->Dst->Type && C.Stmt->Dst->Type->Kind == NdTypeKind::Int &&
        C.Stmt->Val->Type && C.Stmt->Val->Type->Kind == NdTypeKind::Ptr) {
      // A pointer copied into an integer carrier must normally keep its
      // explicit uintptr_t bit view. The one display-only exception is a
      // single typed record-field access, which can name the source pointer
      // directly without changing the machine address or the field width.
      TypeRef Record = declaredRecordPointee(*C.Stmt->Val);
      if (!Record || Uses != 1)
        continue;
      if (Dbg)
        Dbg->completeType(Record);
      auto IsRecordFieldAddress = [&](const HighExpr &Addr,
                                      uint16_t AccessSize) {
        const auto Offset = typedPointerOffset(Addr);
        if (!Offset || !Offset->first ||
            (Offset->first->Kind != ExprKind::Var &&
             Offset->first->Kind != ExprKind::Phi) ||
            varName(Offset->first->Var) != C.Name)
          return false;
        return static_cast<bool>(
            Record->displayFieldPathAt(Offset->second, AccessSize));
      };
      bool TypedFieldUse = false;
      forEachExpr(*UseStmt, [&](const ExprPtr &E) {
        if (E && E->Kind == ExprKind::Load && !E->Operands.empty() &&
            E->Operands[0] &&
            IsRecordFieldAddress(*E->Operands[0], E->Type ? E->Type->Size : 0))
          TypedFieldUse = true;
      });
      if (UseStmt->Kind == StmtKind::Store && UseStmt->StoreAddr)
        TypedFieldUse |= IsRecordFieldAddress(
            *UseStmt->StoreAddr, UseStmt->StoreVal && UseStmt->StoreVal->Type
                                     ? UseStmt->StoreVal->Type->Size
                                     : 0);
      if (!TypedFieldUse)
        continue;
    }
    // A loop evaluates its condition again on every iteration.  A value read
    // there may be folded in only when it reads no memory and the loop
    // assigns none of its variables: a name can stand for several SSA values
    // (see coalesceHighPhiCopies), so the loop may change what it denotes.
    if (UseStmt->Kind == StmtKind::While ||
        UseStmt->Kind == StmtKind::DoWhile || UseStmt->Kind == StmtKind::For) {
      std::set<std::string> Reads;
      bool ReadsState = false;
      std::function<void(const HighExpr &, unsigned)> Collect =
          [&](const HighExpr &E, unsigned Depth) {
            if (Depth > 64) {
              ReadsState = true;
              return;
            }
            if (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) {
              const std::string Read = varName(E.Var);
              Reads.insert(Read);
              if (auto It = ValueForward.find(Read);
                  It != ValueForward.end() && It->second)
                Collect(*It->second, Depth + 1);
            }
            if (E.Kind == ExprKind::Load || E.Kind == ExprKind::Store ||
                E.Kind == ExprKind::Call)
              ReadsState = true;
            E.forEachChildExpr(
                [&](const ExprPtr &Child) { Collect(*Child, Depth + 1); });
          };
      Collect(*C.Stmt->Val, 0);
      std::function<bool(const std::vector<HighStmt> &)> Assigns =
          [&](const std::vector<HighStmt> &Stmts) {
            for (const HighStmt &S : Stmts) {
              if (S.Kind == StmtKind::Assign && S.Dst &&
                  (S.Dst->Kind == ExprKind::Var ||
                   S.Dst->Kind == ExprKind::Phi) &&
                  Reads.count(varName(S.Dst->Var)))
                return true;
              bool Writes = false;
              forEachExpr(S, [&](const ExprPtr &E) {
                if (E && E->Kind == ExprKind::Call)
                  for (const MedVar &Output : E->IntrinsicOutputs)
                    Writes |= Reads.count(varName(Output)) != 0;
              });
              if (Writes || Assigns(S.Body) || Assigns(S.ElseBody) ||
                  Assigns(S.DefaultBody))
                return true;
              for (const SwitchCase &Case : S.Cases)
                if (Assigns(Case.Body))
                  return true;
              for (const std::vector<HighStmt> &Clause : S.EHClauseBodies)
                if (Assigns(Clause))
                  return true;
            }
            return false;
          };
      if (ReadsState || Assigns(UseStmt->Body) || Assigns(UseStmt->ElseBody))
        continue;
    }
    // A reinterpretation of one variable at its own width reads as that
    // variable; a narrowed or extended view keeps its conversion.
    const HighExpr *Fwd = C.Stmt->Val.get();
    if (Fwd && (isParamCopy(*Fwd) || isIntegerViewOfScalar(*Fwd)))
      if (const HighExpr *Inner = sameWidthVariable(*Fwd))
        Fwd = Inner;
    if (!Fwd || readsThroughForwards(*Fwd, C.Name))
      continue;
    ValueForward[C.Name] = Fwd;
    // A field value is not a frame-address alias, but that alone must not
    // force its definition to print after this proof has folded every use.
    // Otherwise the forwarded member would become the assignment's lvalue.
    AmbiguousFrameAliases.erase(C.Name);
  }
}

void HighCWriter::aliasCtorReturnThis(const HighFunc &Func) {
  CtorThisForward.clear();
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (Analysis.DeadStmts.count(&S) || stmtHiddenFromC(S))
      return;
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return;
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      return;
    if (S.Val->Kind != ExprKind::Call || S.Val->IntrinsicId != Intrinsic::None)
      return;
    const std::string Callee = callIdentifier(*S.Val);
    const MsvcCallee *Msvc = msvcCallee(Callee, Opts.Format);
    if (!Msvc || Msvc->Kind != MsvcCalleeKind::Ctor)
      return;
    if (S.Val->Operands.empty() || !S.Val->Operands[0])
      return;
    const HighExpr *This = S.Val->Operands[0].get();
    if (isUnknownCallOperand(This))
      return;
    const std::string Dest = varName(S.Dst->Var);
    if (Dest.empty() || ValueForward.count(Dest) || FieldForward.count(Dest) ||
        CtorThisForward.count(Dest))
      return;
    std::function<bool(const HighExpr &)> Mentions = [&](const HighExpr &E) {
      if ((E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) &&
          varName(E.Var) == Dest)
        return true;
      for (const ExprPtr &Op : E.Operands)
        if (Op && Mentions(*Op))
          return true;
      return false;
    };
    if (Mentions(*This))
      return;
    CtorThisForward[Dest] = This;
    Analysis.OmittedCallResults.insert(&S);
  });
}

void HighCWriter::collectUnusedCallStoreAlias(const HighFunc &Func) {
  std::set<std::string> Assigned;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    const HighExpr *Call =
        S.Kind == StmtKind::Call ? S.CallExpr.get() : S.Val.get();
    if (Call && Call->Kind == ExprKind::Call)
      for (const MedVar &Output : Call->IntrinsicOutputs)
        Assigned.insert(varName(Output));
    if (S.Kind != StmtKind::Assign || !S.Dst)
      return;
    if (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi)
      Assigned.insert(varName(S.Dst->Var));
  });
  auto UniqueUndeclared = [&](const HighExpr *E,
                              uint16_t Size) -> std::optional<std::string> {
    std::string Found;
    std::function<void(const HighExpr *)> Walk = [&](const HighExpr *Cur) {
      Cur = unwrapIntegerView(Cur);
      if (!Cur)
        return;
      if (Cur->Kind == ExprKind::Var || Cur->Kind == ExprKind::Phi) {
        const std::string Name = varName(Cur->Var);
        const bool SizeOk = Cur->Var.Size == Size ||
                            (Size == 4 && Cur->Var.Size == 8) ||
                            (Size == 8 && Cur->Var.Size == 4);
        // The entry stack pointer is never assigned either, but the frame
        // storage defines it: it is no call's result.
        if (Name.empty() || Cur->Var.Kind == MedVar::Param ||
            isReservedParamDisplayName(Name) || Assigned.count(Name) ||
            ValueForward.count(Name) || FieldForward.count(Name) || !SizeOk ||
            isSyntheticEntryStackPointer(Cur->Var, Func, Opts.TheArch))
          return;
        if (Found.empty())
          Found = Name;
        else if (Found != Name)
          Found = "-";
        return;
      }
      for (const ExprPtr &Op : Cur->Operands)
        if (Op)
          Walk(Op.get());
    };
    Walk(E);
    if (Found.empty() || Found == "-")
      return std::nullopt;
    return Found;
  };
  std::function<void(const std::vector<HighStmt> &)> Alias =
      [&](const std::vector<HighStmt> &Body) {
        for (const HighStmt &S : Body) {
          Alias(S.Body);
          Alias(S.ElseBody);
          Alias(S.DefaultBody);
          for (const auto &C : S.Cases)
            Alias(C.Body);
          for (const auto &Clause : S.EHClauseBodies)
            Alias(Clause);
        }
        auto NextPrinted = [&](size_t I) -> const HighStmt * {
          for (size_t J = I + 1; J < Body.size(); ++J) {
            const HighStmt &N = Body[J];
            if (Analysis.DeadStmts.count(&N) || stmtHiddenFromC(N) ||
                N.Kind == StmtKind::Nop || N.Kind == StmtKind::Block)
              continue;
            return &N;
          }
          return nullptr;
        };
        for (size_t I = 0; I < Body.size(); ++I) {
          const HighStmt &S = Body[I];
          if (!Analysis.OmittedCallResults.count(&S) || !S.Val ||
              S.Val->Kind != ExprKind::Call || !S.Dst)
            continue;
          // A multi-result intrinsic writes its auxiliary temporaries even
          // when the primary result is omitted. They cannot be replaced by
          // the call expression in a following store.
          if (S.Val->IntrinsicId != Intrinsic::None &&
              intrinsicOutputCount(S.Val->IntrinsicId) != 0)
            continue;
          if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
            continue;
          const HighStmt *N = NextPrinted(I);
          if (!N)
            continue;
          const HighExpr *Val = nullptr;
          if (N->Kind == StmtKind::Store && N->StoreVal)
            Val = N->StoreVal.get();
          else if (N->Kind == StmtKind::Assign && N->Val && N->Dst &&
                   N->Dst->Kind == ExprKind::Load)
            Val = N->Val.get();
          if (!Val)
            continue;
          auto Name = UniqueUndeclared(Val, S.Dst->Var.Size);
          if (!Name || readsThroughForwards(*S.Val, *Name))
            continue;
          ValueForward[*Name] = S.Val.get();
          Analysis.DeadStmts.insert(&S);
        }
      };
  Alias(Func.Body);
}

namespace {

bool isCallResultIdent(llvm::StringRef Name) {
  if (Name.empty() ||
      (!std::isalpha(static_cast<unsigned char>(Name.front())) &&
       Name.front() != '_'))
    return false;
  for (char Ch : Name) {
    if (!std::isalnum(static_cast<unsigned char>(Ch)) && Ch != '_')
      return false;
  }
  return true;
}

std::string callResultStem(llvm::StringRef Target) {
  const size_t Sep = Target.rfind('_');
  llvm::StringRef Stem =
      Sep == llvm::StringRef::npos ? Target : Target.drop_front(Sep + 1);
  if (Stem.starts_with("Get") && Stem.size() > 3)
    Stem = Stem.drop_front(3);
  else if (Stem.starts_with("get") && Stem.size() > 3)
    Stem = Stem.drop_front(3);
  else
    return {};
  if (!isCallResultIdent(Stem) || isCProjectionKeyword(Stem))
    return {};
  return Stem.str();
}

} // namespace

void HighCWriter::collectCallResultNames(const HighFunc &Func) {
  CallResultNames.clear();
  CallResultTypes.clear();
  std::set<std::string> Taken;
  for (const auto &Param : Func.Params)
    if (!Param.Name.empty())
      Taken.insert(Param.Name);
  for (const auto &[Disp, Slot] : FrameSlots)
    if (!Slot.Name.empty())
      Taken.insert(Slot.Name);
  for (const auto &[Id, Name] : ParamDisplayNames)
    if (!Name.empty())
      Taken.insert(Name);
  Taken.insert({"this", "result", "item", "StackCookie", "retval"});
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (Analysis.DeadStmts.count(&S) || stmtHiddenFromC(S))
      return;
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return;
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      return;
    if (S.Val->Kind != ExprKind::Call || Analysis.OmittedCallResults.count(&S))
      return;
    // The destination does not receive a result: no type or name from it.
    if (knownVoidCall(*S.Val))
      return;
    const std::string Name = varName(S.Dst->Var);
    if (Name.empty() || ValueForward.count(Name) || FieldForward.count(Name) ||
        CopyForward.count(Name) || CallResultNames.count(Name))
      return;
    TypeRef ReturnType;
    if (const auto FS = debugCallee(*S.Val)) {
      if (Dbg)
        Dbg->completeType(FS->ReturnType);
      ReturnType = cDisplayType(FS->ReturnType);
      if (isMsvcIndirectReturn(FS->ReturnType, Opts.TheArch, Opts.Format) &&
          ReturnType && ReturnType->Kind != NdTypeKind::Ptr) {
        if (const NdType *Record = msvcIndirectReturnRecord(
                FS->ReturnType, Opts.TheArch, Opts.Format)) {
          if (ReturnType->Kind == NdTypeKind::Struct)
            ReturnType = NdType::makePtr(ReturnType);
          else
            ReturnType = NdType::makePtr(
                NdType::makeNamedRecord(cNamedTypeSpelling(Record->SourceName),
                                        Record->Size ? Record->Size : 8));
        }
      }
    } else if (const MsvcCallee *Msvc =
                   msvcCallee(callIdentifier(*S.Val), Opts.Format))
      ReturnType = msvcSyntheticReturn(Msvc->ReturnKind);
    // A result whose type C cannot spell keeps the destination's type.
    if (ReturnType && !hasCValueLayout(ReturnType))
      ReturnType = nullptr;
    if (ReturnType)
      CallResultTypes[Name] = ReturnType;
    std::string Stem = callResultStem(callIdentifier(*S.Val));
    if (Stem.empty() || isReservedParamDisplayName(Stem))
      return;
    if (Taken.count(Stem)) {
      auto Mentions = [&](const HighExpr *E, const std::string &Want) {
        if (!E)
          return false;
        bool Hit = false;
        std::function<void(const HighExpr &)> Rec = [&](const HighExpr &N) {
          if (Hit)
            return;
          if ((N.Kind == ExprKind::Var || N.Kind == ExprKind::Phi) &&
              varName(N.Var) == Want)
            Hit = true;
          for (const ExprPtr &Op : N.Operands)
            if (Op)
              Rec(*Op);
          if (N.IndirectTarget)
            Rec(*N.IndirectTarget);
        };
        Rec(*E);
        return Hit;
      };
      bool Overlap = false;
      for (const auto &[Prev, PrevStem] : CallResultNames) {
        if (PrevStem != Stem)
          continue;
        bool Seen = false;
        walkStmts(Func.Body, [&](const HighStmt &T) {
          if (Overlap)
            return;
          if (&T == &S) {
            Seen = true;
            return;
          }
          if (!Seen)
            return;
          if (T.Kind == StmtKind::Assign && T.Dst &&
              (T.Dst->Kind == ExprKind::Var || T.Dst->Kind == ExprKind::Phi) &&
              varName(T.Dst->Var) == Prev)
            return;
          forEachRhsExpr(T, [&](const ExprPtr &E) {
            if (Mentions(E.get(), Prev))
              Overlap = true;
          });
        });
        if (Overlap)
          break;
      }
      if (Overlap)
        return;
      CallResultNames[Name] = Stem;
      if (ReturnType && !CallResultTypes.count(Stem))
        CallResultTypes[Stem] = ReturnType;
      return;
    }
    Taken.insert(Stem);
    if (ReturnType)
      CallResultTypes[Stem] = ReturnType;
    CallResultNames[Name] = std::move(Stem);
  });
}

std::optional<int64_t> HighCWriter::stackParamOffset(const HighFunc &Func,
                                                     size_t Index) const {
  const HighParam &Param = Func.Params[Index];
  if (Param.RegOff != kNoParamReg || Param.MedIndex < 0)
    return std::nullopt;
  const auto Layout =
      getTargetRegInfo(Opts.TheArch).integerArgumentLayout(Opts.Format);
  if (!Layout.SlotBytes)
    return std::nullopt;
  // Stack arguments are numbered after every register position, or after the
  // registers the function uses where they follow those (i386 regparm).
  int First = static_cast<int>(Layout.Registers.size());
  if (const CallArgumentConvention *Convention =
          callArgumentConvention(Opts.TheArch, Opts.Format);
      Convention && Convention->StackArgumentsFollowUsedRegisters)
    First =
        static_cast<int>(llvm::count_if(Func.Params, [](const HighParam &P) {
          return P.RegOff != kNoParamReg;
        }));
  if (Param.MedIndex < First)
    return std::nullopt;
  return Layout.EntryStackBase +
         static_cast<int64_t>(Param.MedIndex - First) * Layout.SlotBytes;
}

void HighCWriter::bindParams(const HighFunc &Func) const {
  if (ParamCacheOf == &Func && ParamCacheEntry == Func.Entry &&
      ParamBindings.size() == Func.Params.size())
    return;
  ParamCacheOf = &Func;
  ParamCacheEntry = Func.Entry;
  ParamBindings.assign(Func.Params.size(), {});
  ParamDebugNames.assign(Func.Params.size(), {});
  ParamsPlaced = false;
  EmittedParamTypes.clear();
  for (const HighParam &Param : Func.Params)
    EmittedParamTypes.push_back(Param.Type);
  // A bound source ABI spells its own parameters (sourceParameterType).  A
  // function of a language with conventions of its own, such as Go, has no
  // rules that say where its parameters arrive.
  const auto DebugFn = debugFunction(Dbg, Func.Entry);
  if (!DebugFn || Func.SourceTypeHint || !DebugFn->PlatformConvention)
    return;
  const size_t Count = DebugFn->Params.size();
  auto Bind = [&](size_t PI, size_t DI) {
    ParamBindings[PI] = {static_cast<int>(DI), 0, true, false};
  };
  if (isMsvcIndirectReturn(DebugFn->ReturnType, Opts.TheArch, Opts.Format)) {
    // MSVC's hidden result pointer keeps its own positions.
    const bool HighIRIncludesSret =
        highIRIncludesIndirectReturn(Func, *DebugFn);
    const int SretId = indirectReturnParamId(*DebugFn);
    for (size_t PI = 0; PI < Func.Params.size(); ++PI) {
      if (HighIRIncludesSret && static_cast<int>(PI) == SretId) {
        ParamBindings[PI].Result = true;
        // The definition's `result`, a pointer to the record it returns.
        if (const TypeRef Result = declaredFunctionReturnType(Func);
            Result && Result->Kind == NdTypeKind::Ptr)
          EmittedParamTypes[PI] = Result;
        continue;
      }
      const size_t DI =
          HighIRIncludesSret && static_cast<int>(PI) > SretId ? PI - 1 : PI;
      if (DI < Count)
        Bind(PI, DI);
    }
  } else {
    std::vector<TypeRef> Types;
    for (const auto &[Name, Type] : DebugFn->Params) {
      (void)Name;
      if (Type)
        Dbg->completeType(Type);
      Types.push_back(Type);
    }
    if (DebugFn->ReturnType)
      Dbg->completeType(DebugFn->ReturnType);
    // fastcall and thiscall pass leading values in registers that the
    // ordinary i386 rules do not describe, and a function built without
    // lowering does not say where its parameters arrive.
    const bool OrdinaryConvention =
        Opts.TheArch != Arch::X86 ||
        DebugFn->CallConv == DebugCallConv::Unknown ||
        DebugFn->CallConv == DebugCallConv::Cdecl ||
        DebugFn->CallConv == DebugCallConv::Stdcall;
    const bool Located = llvm::any_of(Func.Params, [](const HighParam &P) {
      return P.RegOff != kNoParamReg || P.MedIndex >= 0;
    });
    const auto Placement =
        OrdinaryConvention && Located
            ? placeSourceParameters(Opts.TheArch, Opts.Format,
                                    DebugFn->ReturnType, Types)
            : std::nullopt;
    if (Placement) {
      ParamsPlaced = true;
      for (size_t PI = 0; PI < Func.Params.size(); ++PI) {
        const HighParam &Param = Func.Params[PI];
        const std::optional<int64_t> Stack = stackParamOffset(Func, PI);
        auto At = [&](const SourceABIValueLocation &Where, int64_t &Delta) {
          Delta = 0;
          if (Where.Kind == SourceABICarrierKind::Stack) {
            if (!Stack || *Stack < Where.EntryStackOffset ||
                *Stack >= Where.EntryStackOffset +
                              std::max<int64_t>(Where.ValueBytes, 1))
              return false;
            Delta = *Stack - Where.EntryStackOffset;
            return true;
          }
          return Param.RegOff != kNoParamReg &&
                 Where.RegisterOffset == Param.RegOff;
        };
        int64_t Delta = 0;
        if (Placement->ResultPointer && At(*Placement->ResultPointer, Delta)) {
          ParamBindings[PI].Result = true;
          continue;
        }
        for (size_t DI = 0; DI < Placement->Parameters.size(); ++DI)
          for (const SourceParameterPiece &Piece : Placement->Parameters[DI]) {
            if (!At(Piece.Location, Delta))
              continue;
            const TypeRef &Declared = Types[DI];
            const uint16_t Size = Declared ? Declared->Size : 0;
            const uint16_t Held = Param.Type ? Param.Type->Size : 0;
            // A record is the location's value only where the source says
            // the call passes its bytes: a C++ class can arrive as the
            // address of a copy at the same place.
            const bool Bytes = !Declared ||
                               Declared->Kind != NdTypeKind::Struct ||
                               Declared->IsEnum ||
                               Declared->Passing == NdRecordPassing::ByValue;
            DebugParamBinding &B = ParamBindings[PI];
            B.Index = static_cast<int>(DI);
            B.Offset = static_cast<uint16_t>(Piece.Offset + Delta);
            B.Whole = Bytes && !Piece.Indirect && Piece.Offset == 0 &&
                      Delta == 0 && Piece.Location.ValueBytes >= Size &&
                      (Piece.Location.Kind != SourceABICarrierKind::Stack ||
                       Held >= Size);
          }
      }
    } else {
      // Without rules or locations, a debug parameter is the one at its
      // position only while each before it fills one integer slot, as in
      // every convention.
      const uint16_t Word = pointerBytes(Opts.TheArch);
      for (size_t PI = 0; PI < std::min(Func.Params.size(), Count); ++PI) {
        const TypeRef &Type = DebugFn->Params[PI].second;
        if (!Type || Type->Size > Word ||
            !(Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Ptr ||
              (Type->Kind == NdTypeKind::Struct && Type->IsEnum)))
          break;
        Bind(PI, PI);
      }
    }
  }
  for (size_t PI = 0; PI < Func.Params.size(); ++PI) {
    const DebugParamBinding &B = ParamBindings[PI];
    if (B.Result)
      ParamDebugNames[PI] = "result";
    if (B.Index < 0 || static_cast<size_t>(B.Index) >= Count)
      continue;
    // A later piece of a value, such as a record's second register, is named
    // after the value and where the piece starts in it.
    const std::string &Name = DebugFn->Params[B.Index].first;
    ParamDebugNames[PI] = Name.empty() || B.Whole || B.Offset == 0
                              ? Name
                              : Name + "_" + std::to_string(B.Offset);
    if (!B.Whole)
      continue;
    // A debug record's byte size alone does not describe a C type: a type C
    // cannot spell, such as a pointer to a record without a printable name or
    // validated fields, keeps the recovered machine parameter.
    if (const TypeRef DebugType = cDisplayType(DebugFn->Params[B.Index].second);
        DebugType && hasCValueLayout(DebugType))
      EmittedParamTypes[PI] = DebugType;
  }
  // A piece's name, `result` or a source name such as `arg1` can meet another
  // parameter's name; every parameter keeps its machine name then.
  std::set<std::string> Spelled;
  for (size_t PI = 0; PI < Func.Params.size(); ++PI) {
    const std::string &Machine = Func.Params[PI].Name;
    const std::string Name = !ParamDebugNames[PI].empty() ? ParamDebugNames[PI]
                             : !Machine.empty() ? Machine
                                                : "arg" + std::to_string(PI);
    if (!Spelled.insert(Name).second) {
      ParamDebugNames.assign(Func.Params.size(), {});
      break;
    }
  }
}

HighCWriter::DebugParamBinding
HighCWriter::debugParamBinding(const HighFunc &Func, size_t Index) const {
  if (Index >= Func.Params.size())
    return {};
  bindParams(Func);
  return ParamBindings[Index];
}

std::string HighCWriter::debugParamName(const HighFunc &Func,
                                        size_t Index) const {
  if (Index >= Func.Params.size())
    return {};
  bindParams(Func);
  return ParamDebugNames[Index];
}

bool HighCWriter::positionalDebugSignature(const FunctionSym &FS) const {
  if (!FS.PlatformConvention)
    return false;
  if (isMsvcIndirectReturn(FS.ReturnType, Opts.TheArch, Opts.Format))
    return true;
  const uint16_t Word = pointerBytes(Opts.TheArch);
  // A record result in memory takes the first argument.
  if (FS.ReturnType && FS.ReturnType->Kind == NdTypeKind::Struct &&
      !FS.ReturnType->IsEnum && FS.ReturnType->Size > 2 * Word)
    return false;
  // i386 passes a double in two stack slots, which a call lists apart.
  bool Integers = true, Floats = Opts.TheArch != Arch::X86;
  for (const auto &[Name, Type] : FS.Params) {
    (void)Name;
    if (!Type)
      return false;
    Integers &=
        Type->Size <= Word &&
        (Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Ptr ||
         (Type->Kind == NdTypeKind::Struct && Type->IsEnum));
    Floats &= Type->Kind == NdTypeKind::Float && Type->Size <= sizeof(double);
  }
  return Integers || Floats;
}

TypeRef HighCWriter::emittedParamType(const HighFunc &Func,
                                      size_t Index) const {
  if (Index >= Func.Params.size())
    return nullptr;
  bindParams(Func);
  return EmittedParamTypes[Index];
}

size_t HighCWriter::emittedParamCount(const HighFunc &Func) const {
  return emittedParamIndices(Func).size();
}

std::vector<size_t>
HighCWriter::emittedParamIndices(const HighFunc &Func) const {
  std::vector<size_t> All;
  const auto DebugFn = debugFunction(Dbg, Func.Entry);
  const size_t N = Func.Params.size();
  if (DebugFn &&
      isMsvcIndirectReturn(DebugFn->ReturnType, Opts.TheArch, Opts.Format)) {
    All.resize(N);
    for (size_t I = 0; I < N; ++I)
      All[I] = I;
    return All;
  }
  const auto Key = std::make_pair(&Func, Func.Entry);
  if (auto It = EmittedParamIndices.find(Key);
      It != EmittedParamIndices.end() && It->second.size() <= N)
    return It->second;
  std::set<int> Used;
  // An indirect call's target is a use too.
  std::function<void(const HighExpr &)> Walk = [&](const HighExpr &E) {
    if ((E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) &&
        E.Var.Kind == MedVar::Param)
      Used.insert(E.Var.Id);
    E.forEachChildExpr([&](const ExprPtr &Child) { Walk(*Child); });
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (E)
        Walk(*E);
    });
  });
  if (DebugFn && !DebugFn->Params.empty()) {
    bindParams(Func);
    if (ParamsPlaced) {
      // Placed by location, a parameter is one the signature describes or
      // the code reads; the lift's other registers are none, as they are
      // without a signature.
      std::set<int> Kept = Used;
      for (size_t I = 0; I < N; ++I)
        if (ParamBindings[I].Index >= 0 || ParamBindings[I].Result)
          Kept.insert(static_cast<int>(I));
      Used = std::move(Kept);
    } else {
      // As many parameters as the debug signature lists (a stdcall
      // definition pops what they fill), and every one up to the last the
      // code reads.
      size_t Count = std::min(N, DebugFn->Params.size());
      for (size_t I = 0; I < N; ++I)
        if (ParamBindings[I].Index >= 0 || Used.count(static_cast<int>(I)))
          Count = std::max(Count, I + 1);
      All.resize(Count);
      for (size_t I = 0; I < Count; ++I)
        All[I] = I;
      EmittedParamIndices[Key] = All;
      return All;
    }
  } else if (Func.SourceTypeHint) {
    All.resize(N);
    for (size_t I = 0; I < N; ++I)
      All[I] = I;
    return All;
  }
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

void HighCWriter::hideX86SehRegistration(const HighFunc &Func) {
  // `_except_handler3` saves FS:[0] into a frame slot and restores it on
  // exit. The FS load/store already hide; the slot copies must not print.
  if (Opts.TheArch != Arch::X86 || !Func.ExceptionMetadata ||
      preservesRegistrationMemory(Func))
    return;

  struct Scalar {
    MedVar::VarKind Kind = MedVar::Temp;
    int Id = -1;
    int SSA = 0;
    bool valid() const { return Id >= 0; }
    bool operator==(const Scalar &O) const {
      return Kind == O.Kind && Id == O.Id && SSA == O.SSA;
    }
  };
  auto Peel = [this](const HighExpr *E) { return unwrapIntegerView(E); };
  auto ScalarOf = [&](const HighExpr *E) -> Scalar {
    E = Peel(E);
    if (!E || (E->Kind != ExprKind::Var && E->Kind != ExprKind::Phi))
      return {};
    return {E->Var.Kind, E->Var.Id, E->Var.SSAVer};
  };
  auto IsFSLoad = [&](const HighExpr *E) {
    E = Peel(E);
    return E && E->Kind == ExprKind::Load &&
           E->MemoryAddressSpace == NdMemoryAddressSpace::X86FS;
  };
  auto IsFSStore = [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Store)
      return S.MemoryAddressSpace == NdMemoryAddressSpace::X86FS;
    return S.Kind == StmtKind::Assign && S.Dst &&
           S.Dst->Kind == ExprKind::Load &&
           S.Dst->MemoryAddressSpace == NdMemoryAddressSpace::X86FS;
  };
  auto SlotOfStore = [&](const HighStmt &S) -> std::optional<std::string> {
    if (S.Kind == StmtKind::Store && S.StoreAddr)
      return namedFrameSlot(*S.StoreAddr);
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Load &&
        !S.Dst->Operands.empty() && S.Dst->Operands[0])
      return namedFrameSlot(*S.Dst->Operands[0]);
    return std::nullopt;
  };
  auto SlotOfLoadVal = [&](const HighExpr *E) -> std::optional<std::string> {
    E = Peel(E);
    if (!E || E->Kind != ExprKind::Load || E->Operands.empty() ||
        !E->Operands[0])
      return std::nullopt;
    return namedFrameSlot(*E->Operands[0]);
  };

  std::vector<Scalar> FSLoadDests;
  std::function<void(const std::vector<HighStmt> &)> Collect =
      [&](const std::vector<HighStmt> &Body) {
        for (const HighStmt &S : Body) {
          if (S.Kind == StmtKind::Assign && S.Dst && IsFSLoad(S.Val.get()))
            if (Scalar D = ScalarOf(S.Dst.get()); D.valid())
              FSLoadDests.push_back(D);
          Collect(S.Body);
          Collect(S.ElseBody);
          Collect(S.DefaultBody);
          for (const auto &C : S.Cases)
            Collect(C.Body);
          for (const auto &Clause : S.EHClauseBodies)
            Collect(Clause);
        }
      };
  Collect(Func.Body);

  auto IsFSLoadDest = [&](const HighExpr *E) {
    Scalar S = ScalarOf(E);
    if (!S.valid())
      return false;
    for (const Scalar &D : FSLoadDests)
      if (D == S)
        return true;
    return false;
  };

  std::set<std::string> SehSlots;
  std::function<void(const std::vector<HighStmt> &)> HideFS =
      [&](const std::vector<HighStmt> &Body) {
        for (const HighStmt &S : Body) {
          if (S.Kind == StmtKind::Assign && S.Dst && IsFSLoad(S.Val.get()))
            Analysis.DeadStmts.insert(&S);
          if (IsFSStore(S))
            Analysis.DeadStmts.insert(&S);
          const HighExpr *Val = nullptr;
          if (S.Kind == StmtKind::Store)
            Val = S.StoreVal.get();
          else if (S.Kind == StmtKind::Assign)
            Val = S.Val.get();
          if (Val && (IsFSLoad(Val) || IsFSLoadDest(Val)))
            if (auto Slot = SlotOfStore(S)) {
              SehSlots.insert(*Slot);
              Analysis.DeadStmts.insert(&S);
            }
          HideFS(S.Body);
          HideFS(S.ElseBody);
          HideFS(S.DefaultBody);
          for (const auto &C : S.Cases)
            HideFS(C.Body);
          for (const auto &Clause : S.EHClauseBodies)
            HideFS(Clause);
        }
      };
  HideFS(Func.Body);
  if (SehSlots.empty())
    return;

  auto Mentions = [&](const HighExpr *E, Scalar Want) -> bool {
    if (!E || !Want.valid())
      return false;
    std::function<bool(const HighExpr &)> Walk = [&](const HighExpr &N) {
      if (ScalarOf(&N) == Want)
        return true;
      for (const auto &Op : N.Operands)
        if (Op && Walk(*Op))
          return true;
      return false;
    };
    return Walk(*E);
  };
  auto LiveUse = [&](Scalar Want) {
    bool Found = false;
    std::function<void(const std::vector<HighStmt> &)> Walk =
        [&](const std::vector<HighStmt> &Body) {
          for (const HighStmt &S : Body) {
            if (!Analysis.DeadStmts.count(&S) && !IsFSStore(S)) {
              if (S.Dst && Mentions(S.Dst.get(), Want))
                Found = true;
              if (S.Val && Mentions(S.Val.get(), Want))
                Found = true;
              if (S.StoreAddr && Mentions(S.StoreAddr.get(), Want))
                Found = true;
              if (S.StoreVal && Mentions(S.StoreVal.get(), Want))
                Found = true;
              if (S.Cond && Mentions(S.Cond.get(), Want))
                Found = true;
              if (S.RetVal && Mentions(S.RetVal.get(), Want))
                Found = true;
              if (S.CallExpr && Mentions(S.CallExpr.get(), Want))
                Found = true;
            }
            Walk(S.Body);
            Walk(S.ElseBody);
            Walk(S.DefaultBody);
            for (const auto &C : S.Cases)
              Walk(C.Body);
            for (const auto &Clause : S.EHClauseBodies)
              Walk(Clause);
          }
        };
    Walk(Func.Body);
    return Found;
  };

  std::function<void(const std::vector<HighStmt> &)> HideRestore =
      [&](const std::vector<HighStmt> &Body) {
        for (const HighStmt &S : Body) {
          if (S.Kind == StmtKind::Assign && S.Dst && S.Val)
            if (auto Slot = SlotOfLoadVal(S.Val.get());
                Slot && SehSlots.count(*Slot)) {
              Scalar D = ScalarOf(S.Dst.get());
              if (!D.valid() || D.Kind == MedVar::Param || !LiveUse(D))
                Analysis.DeadStmts.insert(&S);
            }
          HideRestore(S.Body);
          HideRestore(S.ElseBody);
          HideRestore(S.DefaultBody);
          for (const auto &C : S.Cases)
            HideRestore(C.Body);
          for (const auto &Clause : S.EHClauseBodies)
            HideRestore(Clause);
        }
      };
  HideRestore(Func.Body);
}

void HighCWriter::hideX86CallPushSetup(const HighFunc &Func) {
  if (Opts.TheArch != Arch::X86)
    return;

  auto Peel = [this](const HighExpr *E) { return unwrapIntegerView(E); };
  auto IsImm = [&](const HighExpr *E, uint64_t Val) {
    E = Peel(E);
    return E && E->Kind == ExprKind::Const && E->ConstVal == Val;
  };
  auto IsSubImm = [&](const HighExpr *E, uint64_t Imm) {
    E = Peel(E);
    return E && E->Kind == ExprKind::BinOp && E->Op == NdOp::INT_SUB &&
           E->Operands.size() == 2 && E->Operands[0] && E->Operands[1] &&
           IsImm(E->Operands[1].get(), Imm);
  };
  auto SameScalar = [&](const HighExpr *A, const HighExpr *B) {
    A = Peel(A);
    B = Peel(B);
    if (!A || !B)
      return false;
    if (A->Kind == ExprKind::Const && B->Kind == ExprKind::Const)
      return A->ConstVal == B->ConstVal;
    if ((A->Kind == ExprKind::Var || A->Kind == ExprKind::Phi) &&
        A->Kind == B->Kind)
      return A->Var.Kind == B->Var.Kind && A->Var.Id == B->Var.Id &&
             A->Var.SSAVer == B->Var.SSAVer;
    return false;
  };
  auto MatchesCallOperand = [&](const HighExpr *Val, const HighExpr &Call) {
    if (!Val)
      return false;
    for (const auto &Op : Call.Operands)
      if (SameScalar(Val, Op.get()))
        return true;
    return false;
  };
  auto IsPushStore = [&](const HighStmt &S, const HighExpr &Call) {
    if (S.Kind != StmtKind::Store || !S.StoreAddr || !S.StoreVal)
      return false;
    if (S.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    if (namedFrameSlot(*S.StoreAddr) || typedMemberAccess(*S.StoreAddr))
      return false;
    if (!IsSubImm(S.StoreAddr.get(), 4))
      return false;
    const HighExpr *Val = Peel(S.StoreVal.get());
    if (!Val)
      return false;
    if (Val->Kind == ExprKind::Const || Val->Kind == ExprKind::Undef)
      return true;
    return MatchesCallOperand(S.StoreVal.get(), Call);
  };
  auto IsEspSubAssign = [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return false;
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      return false;
    if (S.Dst->Var.Kind == MedVar::Param)
      return false;
    if (namedFrameSlot(*S.Dst))
      return false;
    return IsSubImm(S.Val.get(), 4);
  };
  auto IsImageLoadAssign = [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return false;
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      return false;
    const HighExpr *Val = Peel(S.Val.get());
    if (!Val || Val->Kind != ExprKind::Load || Val->Operands.empty() ||
        !Val->Operands[0])
      return false;
    const HighExpr *Addr = Peel(Val->Operands[0].get());
    return Addr && Addr->Kind == ExprKind::Const;
  };

  std::function<void(const std::vector<HighStmt> &)> Walk =
      [&](const std::vector<HighStmt> &Body) {
        for (const HighStmt &S : Body) {
          Walk(S.Body);
          Walk(S.ElseBody);
          Walk(S.DefaultBody);
          for (const auto &C : S.Cases)
            Walk(C.Body);
          for (const auto &Clause : S.EHClauseBodies)
            Walk(Clause);
        }
        for (size_t I = 0; I < Body.size(); ++I) {
          const HighExpr *Call = stmtCallExpr(Body[I]);
          if (!Call || Call->Operands.empty())
            continue;
          std::vector<const HighStmt *> Run;
          bool Matched = false;
          for (size_t J = I; J > 0 && Run.size() < 8;) {
            --J;
            const HighStmt &P = Body[J];
            if (P.Kind == StmtKind::Nop ||
                (P.Kind == StmtKind::Block && P.Body.empty()))
              continue;
            if (IsImageLoadAssign(P)) {
              if (MatchesCallOperand(P.Dst.get(), *Call))
                Run.push_back(&P);
              continue;
            }
            if (IsPushStore(P, *Call)) {
              if (MatchesCallOperand(P.StoreVal.get(), *Call))
                Matched = true;
              Run.push_back(&P);
              continue;
            }
            if (IsEspSubAssign(P)) {
              Run.push_back(&P);
              continue;
            }
            break;
          }
          if (!Matched)
            continue;
          for (const HighStmt *P : Run)
            Analysis.DeadStmts.insert(P);
        }
      };
  Walk(Func.Body);
}

void HighCWriter::foldCxxThrowConstructors(const HighFunc &Func) {
  if (!Opts.StructuredExceptionSyntax)
    return;
  if (!Opts.Image)
    return;
  struct ThrownObject {
    std::optional<int64_t> Frame;
    std::optional<va_t> Image;
  };
  auto objectOf = [&](const HighExpr &E) -> ThrownObject {
    ThrownObject Obj;
    const HighExpr *Peeled = peelCxxThrowDisplay(&E);
    if (!Peeled)
      return Obj;
    Obj.Frame = frameDisplacement(*Peeled);
    Obj.Image = constAddress(*Peeled);
    return Obj;
  };
  auto sameObject = [](const ThrownObject &A, const ThrownObject &B) {
    if (A.Frame && B.Frame && *A.Frame == *B.Frame)
      return true;
    if (A.Image && B.Image && *A.Image == *B.Image)
      return true;
    return false;
  };
  auto foldRegion = [&](const std::vector<HighStmt> &Stmts,
                        auto &&Self) -> void {
    std::vector<const HighStmt *> Ctors;
    for (const HighStmt &S : Stmts) {
      switch (S.Kind) {
      case StmtKind::If:
      case StmtKind::While:
      case StmtKind::DoWhile:
      case StmtKind::For:
      case StmtKind::Block:
        Self(S.Body, Self);
        continue;
      case StmtKind::IfElse:
        Self(S.Body, Self);
        Self(S.ElseBody, Self);
        continue;
      case StmtKind::Switch:
        for (const SwitchCase &Case : S.Cases)
          Self(Case.Body, Self);
        Self(S.DefaultBody, Self);
        continue;
      case StmtKind::SEHTry:
      case StmtKind::CxxTry:
      case StmtKind::ItaniumTry:
        Self(S.Body, Self);
        for (const auto &ClauseBody : S.EHClauseBodies)
          Self(ClauseBody, Self);
        continue;
      default:
        break;
      }
      const HighExpr *Call = stmtCallExpr(S);
      if (!Call)
        continue;
      if (isMsvcCxxThrowCallName(Call->CallTarget)) {
        if (Call->Operands.size() < 2 || !Call->Operands[0] ||
            !Call->Operands[1])
          continue;
        const HighExpr *ObjExpr = peelCxxThrowDisplay(Call->Operands[0].get());
        if (!ObjExpr)
          continue;
        bool Rethrow = false;
        if (ObjExpr->Kind == ExprKind::Const && ObjExpr->ConstVal == 0)
          Rethrow = true;
        if (Rethrow)
          continue;
        const auto ThrowInfo = constAddress(*Call->Operands[1]);
        if (!ThrowInfo)
          continue;
        std::string Type = readMsvcThrowInfoFirstType(*Opts.Image, *ThrowInfo);
        Type = cNamedTypeSpelling(Type);
        if (Type.empty() || !isCxxThrowTypeIdent(Type))
          continue;
        const ThrownObject Thrown = objectOf(*Call->Operands[0]);
        const HighStmt *Ctor = nullptr;
        for (auto It = Ctors.rbegin(); It != Ctors.rend(); ++It) {
          const HighExpr *CtorCall = stmtCallExpr(**It);
          if (!CtorCall || CtorCall->Operands.empty() || !CtorCall->Operands[0])
            continue;
          if (sameObject(objectOf(*CtorCall->Operands[0]), Thrown)) {
            Ctor = *It;
            break;
          }
        }
        if (!Ctor) {
          bool AfterThrow = false;
          for (const HighStmt &Later : Stmts) {
            if (&Later == &S) {
              AfterThrow = true;
              continue;
            }
            if (!AfterThrow)
              continue;
            const HighExpr *CtorCall = stmtCallExpr(Later);
            if (!CtorCall || CtorCall->Operands.empty() ||
                !CtorCall->Operands[0])
              continue;
            if (isMsvcCxxThrowCallName(CtorCall->CallTarget))
              break;
            if (sameObject(objectOf(*CtorCall->Operands[0]), Thrown)) {
              Ctor = &Later;
              break;
            }
          }
        }
        CxxThrowPrint Print;
        Print.Type = std::move(Type);
        if (Ctor) {
          const HighExpr *CtorCall = stmtCallExpr(*Ctor);
          const size_t ArgEnd =
              isMsvcZeroArgConstructor(resolvedCallTarget(*CtorCall))
                  ? 1
                  : CtorCall->Operands.size();
          for (size_t I = 1; I < ArgEnd; ++I) {
            if (const HighExpr *Arg =
                    peelCxxThrowDisplay(CtorCall->Operands[I].get()))
              Print.Args.push_back(Arg);
          }
          Analysis.DeadStmts.insert(Ctor);
          const std::string Resolved = resolvedCallTarget(*CtorCall);
          if (!Resolved.empty()) {
            HiddenCxxCtorIdentifiers.insert(Resolved);
            HiddenCxxCtorIdentifiers.insert(functionIdentifier(Resolved));
          }
        }
        CxxThrowPrints[&S] = std::move(Print);
        auto writesThrown = [&](const HighExpr &Addr) {
          const ThrownObject Dst = objectOf(Addr);
          if (sameObject(Dst, Thrown))
            return true;
          if (Thrown.Frame && Dst.Frame) {
            const int64_t Delta = *Dst.Frame - *Thrown.Frame;
            return Delta >= 0 && Delta < 32;
          }
          return false;
        };
        auto hideThrownStore = [&](const HighStmt &Prev) {
          const HighExpr *Addr = nullptr;
          if (Prev.Kind == StmtKind::Store && Prev.StoreAddr)
            Addr = Prev.StoreAddr.get();
          else if (Prev.Kind == StmtKind::Assign && Prev.Dst && Prev.Val &&
                   Prev.Dst->Kind == ExprKind::Load &&
                   !Prev.Dst->Operands.empty())
            Addr = Prev.Dst->Operands[0].get();
          if (Addr && writesThrown(*Addr))
            Analysis.DeadStmts.insert(&Prev);
        };
        std::function<void(const std::vector<HighStmt> &)> HideNested =
            [&](const std::vector<HighStmt> &Nested) {
              for (const HighStmt &Prev : Nested) {
                hideThrownStore(Prev);
                HideNested(Prev.Body);
                HideNested(Prev.ElseBody);
                HideNested(Prev.DefaultBody);
                for (const SwitchCase &Case : Prev.Cases)
                  HideNested(Case.Body);
                for (const auto &ClauseBody : Prev.EHClauseBodies)
                  HideNested(ClauseBody);
              }
            };
        HideNested(Stmts);
        continue;
      }
      if (!Call->Operands.empty() && Call->Operands[0])
        Ctors.push_back(&S);
    }
  };
  foldRegion(Func.Body, foldRegion);
}

void HighCWriter::discoverHiddenCxxThrowCtors(
    const std::vector<HighFunc> &Funcs) {
  HiddenCxxCtorIdentifiers.clear();
  for (const HighFunc &Func : Funcs) {
    CurrentFunc = &Func;
    foldCxxThrowConstructors(Func);
  }
  CurrentFunc = nullptr;
  Analysis = {};
  CxxThrowPrints.clear();
}

void HighCWriter::nameCxxCatchObjects(const HighFunc &Func) {
  CxxCatchNames.clear();
  CxxCatchObjectDisps.clear();
  OpenCatchObjects.clear();
  if (!Opts.StructuredExceptionSyntax)
    return;
  auto nameTaken = [&](const std::string &Name) {
    if (Name.empty())
      return true;
    for (const auto &[Disp, Slot] : FrameSlots)
      if (Slot.Name == Name)
        return true;
    for (const auto &[Clause, Existing] : CxxCatchNames)
      if (Existing == Name)
        return true;
    return false;
  };
  auto allocateName = [&]() {
    std::string Name = "e";
    unsigned Suffix = 1;
    while (nameTaken(Name))
      Name = "e_" + std::to_string(Suffix++);
    return Name;
  };
  auto catchDisp = [&](int32_t Off) -> std::optional<int64_t> {
    if (!Off)
      return std::nullopt;
    const int64_t Direct = Off;
    const int64_t Rebased =
        static_cast<int64_t>(Off) - static_cast<int64_t>(Func.FrameSize);
    if (Off > 0 && Func.FrameSize > 0 && FrameSlots.count(Rebased))
      return Rebased;
    if (FrameSlots.count(Direct))
      return Direct;
    if (FrameSlots.count(-Direct))
      return -Direct;
    if (Off > 0 && Func.FrameSize > 0)
      return Rebased;
    return Direct;
  };
  std::function<void(const std::vector<HighStmt> &)> Walk =
      [&](const std::vector<HighStmt> &Stmts) {
        for (const HighStmt &S : Stmts) {
          if (S.Kind == StmtKind::CxxTry) {
            for (size_t I = 0; I < S.EHClauses.size(); ++I) {
              const HighEHClause &Clause = S.EHClauses[I];
              if (Clause.Kind != HighEHClauseKind::CxxCatch ||
                  Clause.TypeName.empty() || CxxCatchNames.count(&Clause))
                continue;
              std::optional<int64_t> Disp = catchDisp(Clause.CatchObjectOffset);
              if (!Disp || !FrameSlots.count(*Disp)) {
                Disp.reset();
                if (I < S.EHClauseBodies.size()) {
                  for (const HighStmt &BodyStmt : S.EHClauseBodies[I]) {
                    if (BodyStmt.Kind != StmtKind::Assign || !BodyStmt.Val)
                      continue;
                    const HighExpr *Loaded = BodyStmt.Val.get();
                    if (Loaded->Kind != ExprKind::Load ||
                        Loaded->Operands.empty() || !Loaded->Operands[0])
                      continue;
                    const auto BodyDisp =
                        frameDisplacement(*Loaded->Operands[0]);
                    if (!BodyDisp || !FrameSlots.count(*BodyDisp))
                      continue;
                    const std::string &Existing = FrameSlots[*BodyDisp].Name;
                    if (isCxxCatchObjectName(Existing))
                      continue;
                    Disp = BodyDisp;
                    break;
                  }
                }
              }
              if (!Disp || !FrameSlots.count(*Disp))
                continue;
              const std::string Name = allocateName();
              FrameSlots[*Disp].Name = Name;
              CxxCatchNames[&Clause] = Name;
              CxxCatchObjectDisps[*Disp] = Name;
            }
          }
          Walk(S.Body);
          Walk(S.ElseBody);
          for (const SwitchCase &Case : S.Cases)
            Walk(Case.Body);
          Walk(S.DefaultBody);
          for (const auto &ClauseBody : S.EHClauseBodies)
            Walk(ClauseBody);
        }
      };
  Walk(Func.Body);
}

void HighCWriter::noteCatchReaching(const HighStmt &Stmt) {
  if (Stmt.Kind != StmtKind::Assign || !Stmt.Dst || !Stmt.Val ||
      Stmt.Dst->Kind != ExprKind::Var)
    return;
  const std::string Dest = varName(Stmt.Dst->Var);
  if (Dest.empty() || isCxxCatchObjectName(Dest))
    return;
  // FrameSlots may have been replaced by contiguous backing storage after
  // simulation hid this definition. Replay that exact statement's proof;
  // the reaching maps still follow the branch scopes of statement emission.
  if (auto It = CatchAliasDefinitions.find(&Stmt);
      It != CatchAliasDefinitions.end()) {
    if (It->second.IsPointer) {
      ReachingCatchPtrs[Dest] = It->second.Name;
      ReachingCatchFields.erase(Dest);
    } else {
      ReachingCatchFields[Dest] = It->second.Name;
      ReachingCatchPtrs.erase(Dest);
    }
    return;
  }
  if (auto Ptr = cxxCatchPointerName(*Stmt.Val)) {
    CatchAliasDefinitions[&Stmt] = {*Ptr, true};
    ReachingCatchPtrs[Dest] = *Ptr;
    ReachingCatchFields.erase(Dest);
    return;
  }
  if (Stmt.Val->Kind == ExprKind::Load && !Stmt.Val->Operands.empty() &&
      Stmt.Val->Operands[0]) {
    if (auto Field = cxxCatchFieldAccess(*Stmt.Val->Operands[0])) {
      CatchAliasDefinitions[&Stmt] = {*Field, false};
      ReachingCatchFields[Dest] = *Field;
      ReachingCatchPtrs.erase(Dest);
      return;
    }
  }
  ReachingCatchPtrs.erase(Dest);
  ReachingCatchFields.erase(Dest);
}

void HighCWriter::simulateCatchReaching(const HighFunc &Func) {
  CatchAliasTemps.clear();
  CatchAliasDefinitions.clear();
  ReachingCatchPtrs.clear();
  ReachingCatchFields.clear();
  auto Snapshot = [&]() {
    return std::pair{ReachingCatchPtrs, ReachingCatchFields};
  };
  auto Restore =
      [&](const std::pair<std::map<std::string, std::string>,
                          std::map<std::string, std::string>> &Saved) {
        ReachingCatchPtrs = Saved.first;
        ReachingCatchFields = Saved.second;
      };
  std::function<void(const std::vector<HighStmt> &)> Walk =
      [&](const std::vector<HighStmt> &Stmts) {
        for (const HighStmt &S : Stmts) {
          switch (S.Kind) {
          case StmtKind::If:
          case StmtKind::While:
          case StmtKind::DoWhile:
          case StmtKind::For:
          case StmtKind::Block: {
            const auto Saved = Snapshot();
            Walk(S.Body);
            Restore(Saved);
            continue;
          }
          case StmtKind::IfElse: {
            const auto Saved = Snapshot();
            Walk(S.Body);
            Restore(Saved);
            Walk(S.ElseBody);
            Restore(Saved);
            continue;
          }
          case StmtKind::Switch: {
            const auto Saved = Snapshot();
            for (const SwitchCase &Case : S.Cases) {
              Walk(Case.Body);
              Restore(Saved);
            }
            Walk(S.DefaultBody);
            Restore(Saved);
            continue;
          }
          case StmtKind::SEHTry:
          case StmtKind::CxxTry:
          case StmtKind::ItaniumTry: {
            const auto Saved = Snapshot();
            Walk(S.Body);
            Restore(Saved);
            for (const auto &ClauseBody : S.EHClauseBodies) {
              const auto ClauseSaved = Snapshot();
              Walk(ClauseBody);
              Restore(ClauseSaved);
            }
            continue;
          }
          default:
            break;
          }
          noteCatchReaching(S);
          if (S.Kind == StmtKind::Assign && S.Dst &&
              S.Dst->Kind == ExprKind::Var) {
            const std::string Dest = varName(S.Dst->Var);
            if (ReachingCatchPtrs.count(Dest) ||
                ReachingCatchFields.count(Dest)) {
              Analysis.DeadStmts.insert(&S);
              CatchAliasTemps.insert(Dest);
            }
          }
        }
      };
  Walk(Func.Body);
  ReachingCatchPtrs.clear();
  ReachingCatchFields.clear();
}

void HighCWriter::writeFunctionProjection(const HighFunc &Func) {
  CurrentFunc = &Func;
  FrameStorageActive = false;
  ProjectFrameAliasesIntoStorage = false;
  FrameStorageSlots.clear();
  CopyForward.clear();
  JoinPhiNames.clear();
  FieldForward.clear();
  FieldForwardSources.clear();
  FieldForwardTypes.clear();
  EnumDestTypes.clear();
  PointerArgDestTypes.clear();
  AmbiguousFrameAliases.clear();
  ValueForward.clear();
  AssignedValuesByName.clear();
  AssignedValuesIndexed = false;
  CallResultNames.clear();
  CallResultTypes.clear();
  UnknownOnlyNames.clear();
  AssignedNames.clear();
  CtorSourceNames.clear();
  CxxThrowPrints.clear();
  CxxCatchNames.clear();
  CatchAliasTemps.clear();
  CatchAliasDefinitions.clear();
  DeclaredCNames.clear();
  ReachingCatchPtrs.clear();
  ReachingCatchFields.clear();
  ParamDisplayNames.clear();
  // Fixed source declarations may omit parameter spellings. Allocate their
  // names before any body analysis so the declaration, uses and local-variable
  // exclusion all observe the same identifier. Debug declarations retain
  // their separate parameter mapping (including hidden result parameters).
  if (Func.SourceTypeHint && !debugFunction(Dbg, Func.Entry)) {
    CProjectionIdentifierAllocator Identifiers;
    for (const auto &Param : Func.Params)
      if (!Param.Name.empty())
        Identifiers.allocate(Param.Name, "nd_arg");
    for (size_t I = 0; I < Func.Params.size(); ++I)
      if (Func.Params[I].Name.empty())
        ParamDisplayNames[static_cast<int>(I)] =
            Identifiers.allocate("arg" + std::to_string(I), "nd_arg");
  }
  MemoryIdentifiers = GlobalIdentifierAllocator;
  MemoryTemporaries.clear();
  AddressTakenNames.clear();
  DeclaredCTypes.clear();
  PrintedIntegerTypes.clear();
  UnsignedCarrierTexts.clear();
  for (const auto &Param : Func.Params)
    MemoryIdentifiers.allocate(Param.Name);
  for (const auto &Local : Func.Locals)
    MemoryIdentifiers.allocate(Local.Name);
  std::set<const HighExpr *> MemorySeen;
  std::function<void(const ExprPtr &)> ReserveNames = [&](const ExprPtr &E) {
    if (!E || !MemorySeen.insert(E.get()).second)
      return;
    if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi)
      MemoryIdentifiers.allocate(varName(E->Var));
    if (E->Kind == ExprKind::Addr && E->Operands.size() == 1 &&
        E->Operands[0] &&
        (E->Operands[0]->Kind == ExprKind::Var ||
         E->Operands[0]->Kind == ExprKind::Phi))
      AddressTakenNames.insert(varName(E->Operands[0]->Var));
    E->forEachChildExpr(ReserveNames);
  };
  walkStmts(Func.Body,
            [&](const HighStmt &Stmt) { forEachExpr(Stmt, ReserveNames); });
  IndirectReturnName.clear();
  if (const auto DebugFn = debugFunction(Dbg, Func.Entry);
      !Func.SourceTypeHint && DebugFn &&
      isMsvcIndirectReturn(DebugFn->ReturnType, Opts.TheArch, Opts.Format))
    IndirectReturnName = "result";
  PrintedIndirectReturn = false;
  Analysis = {};
  runAnalysisPasses(Func);
  collectNamedFrameSlots(Func);
  // A fixed slot write can be read later through an indexed address even if
  // no named slot load mentions it. Preserve those writes before the
  // local-only dead-store pass runs; the backing store is selected below.
  bool HasDynamicFrameIndex = false;
  auto CheckDynamicFrameIndex = [&](auto &&Self, const HighExpr &Expr) -> void {
    if (HasDynamicFrameIndex)
      return;
    if (Expr.Kind == ExprKind::BinOp &&
        (Expr.Op == NdOp::INT_ADD || Expr.Op == NdOp::INT_SUB) &&
        Expr.Operands.size() == 2 && Expr.Operands[0] && Expr.Operands[1] &&
        !certifiedFrameStorageDisplacement(Expr) &&
        (certifiedFrameStorageDisplacement(*Expr.Operands[0]) ||
         certifiedFrameStorageDisplacement(*Expr.Operands[1]))) {
      HasDynamicFrameIndex = true;
      return;
    }
    for (const ExprPtr &Operand : Expr.Operands)
      if (Operand)
        Self(Self, *Operand);
  };
  walkStmts(Func.Body, [&](const HighStmt &Stmt) {
    if (Stmt.Dst)
      CheckDynamicFrameIndex(CheckDynamicFrameIndex, *Stmt.Dst);
    if (Stmt.StoreAddr)
      CheckDynamicFrameIndex(CheckDynamicFrameIndex, *Stmt.StoreAddr);
    forEachRhsExpr(Stmt, [&](const ExprPtr &Expr) {
      if (Expr)
        CheckDynamicFrameIndex(CheckDynamicFrameIndex, *Expr);
    });
  });
  // Escaping object addresses make neighboring fields observable even when
  // no named load reads them. Indexed and escaped frames keep their stores.
  const bool PreserveFrameStores =
      HasDynamicFrameIndex || llvm::any_of(FrameSlots, [](const auto &Entry) {
        return Entry.second.AddressTaken;
      });
  // Copy propagation belongs to the HighIR passes, which prove reaching
  // definitions. A function-wide textual alias map cannot represent loop
  // backedges, branches, or a later redefinition of a printed variable.
  hideX86SehRegistration(Func);
  if (!PreserveFrameStores)
    hideUnusedFrameSlotWrites(Func);
  {
    auto VarFn = [this](const MedVar &V) { return varName(V); };
    auto ArgLimit = [this](const HighExpr &E) { return debugCallArgLimit(E); };
    auto AllArgs = [](const HighExpr &E) { return E.Operands.size(); };
    markHiddenControlDead(Func.Body);
    analyzeUnusedAssigns(Analysis, Func, VarFn, AllArgs);
    analyzeUnusedCallResults(Analysis, Func, VarFn, ArgLimit);
  }
  // Epilogue reloads of callee-save / param homes become DeadStmts above.
  // Re-hide those stores so unused `var_m10 = this` / `var_m18 = 0` drop.
  if (!PreserveFrameStores)
    hideUnusedFrameSlotWrites(Func);
  hideBitClearSlotCopies(Func);
  hideX86CallPushSetup(Func);
  collectNamedFrameSlots(Func);
  applyDebugCallSlotTypes(Func);
  propagateFrameSlotCopyTypes(Func);
  hideInteriorRecordFieldSlots();
  overlayPackedValueHomes(Func);
  // Reviving a merged definition after proving a fold would omit a write
  // that the generated C still executes. Settle visibility before the proof.
  collectFieldLoadTypes(Func);
  invalidateJoinPhiFrameAliases(Func);
  // Display-only: one mention of `x` so ValueForward can compose GetLength.
  foldSignedJleConds(const_cast<std::vector<HighStmt> &>(Func.Body));
  collectValueForward(Func);
  hideCleanupSlotCopyIntoCall(Func);
  aliasCtorReturnThis(Func);
  hideIncrementOnlyLoads(Func);
  collectFieldLoadForward(Func);
  collectEnumConstForward(Func);
  collectTypedPointerArgDests(Func);
  foldCxxThrowConstructors(Func);
  nameCxxCatchObjects(Func);
  simulateCatchReaching(Func);
  collectUnusedCallStoreAlias(Func);
  collectCtorSourceNames(Func);
  collectCallResultNames(Func);
  {
    const std::vector<size_t> Indices = emittedParamIndices(Func);
    if (Indices.size() != Func.Params.size())
      for (size_t I = 0; I < Indices.size(); ++I)
        ParamDisplayNames[static_cast<int>(Indices[I])] =
            "arg" + std::to_string(I);
    if (!Indices.empty() &&
        stripLeadingUnderscores(Func.Name) == "security_check_cookie")
      ParamDisplayNames[static_cast<int>(Indices[0])] = "StackCookie";
  }

  // Escaping frame addresses may designate a multi-field object (for example
  // a stack Block). Separate C locals do not preserve offsets or adjacency.
  bool NeedsFrameStorage = llvm::any_of(
      FrameSlots, [](const auto &Entry) { return Entry.second.AddressTaken; });
  auto ContainsAlignedFrame = [&](auto &&Self, const HighExpr &Expr,
                                  unsigned Depth) -> bool {
    if (Depth == limits::kMaxFrameDisplacementDepth)
      return false;
    const HighExpr *Inner = unwrapIntegerView(&Expr);
    if (!Inner)
      return false;
    if (Inner->Kind == ExprKind::BinOp && Inner->Op == NdOp::INT_AND &&
        certifiedFrameStorageDisplacement(*Inner))
      return true;
    for (const ExprPtr &Operand : Inner->Operands)
      if (Operand && Self(Self, *Operand, Depth + 1))
        return true;
    return false;
  };
  auto VisitFrameUses = [&](auto &&Self, const HighExpr &Expr) -> void {
    if (NeedsFrameStorage && ProjectFrameAliasesIntoStorage)
      return;
    // A frame alias plus a run-time index cannot be a standalone C local.
    // Fixed slot accesses must share its backing bytes, including across a
    // catch funclet where Param 1 is the parent's establisher frame.
    if (Expr.Kind == ExprKind::BinOp &&
        (Expr.Op == NdOp::INT_ADD || Expr.Op == NdOp::INT_SUB) &&
        Expr.Operands.size() == 2 && Expr.Operands[0] && Expr.Operands[1] &&
        !certifiedFrameStorageDisplacement(Expr) &&
        (certifiedFrameStorageDisplacement(*Expr.Operands[0]) ||
         certifiedFrameStorageDisplacement(*Expr.Operands[1]))) {
      NeedsFrameStorage = true;
      ProjectFrameAliasesIntoStorage = true;
      return;
    }
    if (isNamedFrameMemory(Expr) || namedFrameSlot(Expr) ||
        frameDisplacement(Expr))
      return;
    if (Expr.Kind == ExprKind::Load && !Expr.Operands.empty() &&
        Expr.Operands[0] && frameDisplacement(*Expr.Operands[0]))
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
    if (Expr.IndirectTarget)
      Self(Self, *Expr.IndirectTarget);
  };
  auto ConsiderFrameUse = [&](const HighStmt &Stmt) {
    if ((NeedsFrameStorage && ProjectFrameAliasesIntoStorage) ||
        Analysis.DeadStmts.count(&Stmt) ||
        (InferredVoid && Stmt.Kind == StmtKind::Return))
      return;
    // A masked alignment of an authenticated frame address still needs the
    // aligned byte backing, even when an enclosing constant offset or a named
    // slot would otherwise stop the ordinary frame-use walk early.
    auto NoteAlignedFrame = [&](const ExprPtr &Expr) {
      if (Expr && ContainsAlignedFrame(ContainsAlignedFrame, *Expr, 0)) {
        NeedsFrameStorage = true;
        ProjectFrameAliasesIntoStorage = true;
      }
    };
    NoteAlignedFrame(Stmt.Dst);
    NoteAlignedFrame(Stmt.StoreAddr);
    forEachRhsExpr(Stmt, NoteAlignedFrame);
    if (NeedsFrameStorage && ProjectFrameAliasesIntoStorage)
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
  };
  std::function<void(const std::vector<HighStmt> &, bool)> WalkFrameUses;
  WalkFrameUses = [&](const std::vector<HighStmt> &Stmts, bool InHandler) {
    const bool Saved = InEHClauseBody;
    InEHClauseBody = InHandler;
    for (const HighStmt &Stmt : Stmts) {
      ConsiderFrameUse(Stmt);
      WalkFrameUses(Stmt.Body, InHandler);
      WalkFrameUses(Stmt.ElseBody, InHandler);
      WalkFrameUses(Stmt.DefaultBody, InHandler);
      for (const auto &C : Stmt.Cases)
        WalkFrameUses(C.Body, InHandler);
      for (size_t I = 0; I < Stmt.EHClauseBodies.size(); ++I) {
        const bool Cleanup =
            I < Stmt.EHClauses.size() &&
            Stmt.EHClauses[I].Kind == HighEHClauseKind::CxxCleanup;
        if (!Cleanup)
          WalkFrameUses(Stmt.EHClauseBodies[I], true);
      }
    }
    InEHClauseBody = Saved;
  };
  WalkFrameUses(Func.Body, false);
  FrameStorageActive = NeedsFrameStorage;
  // Rebuilt storage has the host's pointer width. A target-width integer
  // temporary would truncate its address when 32-bit source is recompiled
  // on a 64-bit host. Keep certified aliases on that same byte backing.
  if (NeedsFrameStorage && getTargetRegInfo(Opts.TheArch).PointerSize == 4)
    ProjectFrameAliasesIntoStorage = true;
  if (NeedsFrameStorage || !Analysis.StoreFwd.empty()) {
    // Integer store-to-load forwarding and named C locals are exclusive.
    // Mixing them leaves later loads on `frame_base` after the seed `arg0`
    // store has already been deleted.
    // Member text and copy aliases chosen before this point can name a slot
    // that byte storage now replaces. Rewrite each such name as a typed view
    // of the same storage bytes.
    std::map<std::string, std::string> StorageViews;
    for (const auto &[Disp, Slot] : FrameSlots) {
      if (Slot.Name.empty())
        continue;
      const std::string Address =
          "(uintptr_t)(" + frameStorageAddress(Disp) + ")";
      if (Slot.RegionBytes)
        StorageViews[Slot.Name] = "(*(uint8_t (*)[" +
                                  std::to_string(Slot.RegionBytes) + "])" +
                                  Address + ")";
      else
        StorageViews[Slot.Name] =
            "(*(" +
            declarationToC(
                Slot.Type ? cDisplayType(Slot.Type) : NdType::makeInt(1), "*") +
            ")" + Address + ")";
    }
    auto RewriteSlotNames = [&](std::string &Text) {
      const auto IsIdent = [](char C) {
        return std::isalnum(static_cast<unsigned char>(C)) || C == '_';
      };
      std::string Out;
      for (size_t Pos = 0; Pos < Text.size();) {
        if (!IsIdent(Text[Pos]) || (Pos && IsIdent(Text[Pos - 1]))) {
          Out += Text[Pos++];
          continue;
        }
        size_t End = Pos;
        while (End < Text.size() && IsIdent(Text[End]))
          ++End;
        const std::string Token = Text.substr(Pos, End - Pos);
        const bool Member =
            Pos && (Text[Pos - 1] == '.' ||
                    (Pos > 1 && Text[Pos - 1] == '>' && Text[Pos - 2] == '-'));
        const auto View =
            Member ? StorageViews.end() : StorageViews.find(Token);
        Out += View == StorageViews.end() ? Token : View->second;
        Pos = End;
      }
      Text = std::move(Out);
    };
    if (!StorageViews.empty()) {
      for (auto &[Name, Text] : FieldForward)
        RewriteSlotNames(Text);
      for (auto &[Name, Source] : CopyForward)
        RewriteSlotNames(Source);
    }
    if (ProjectFrameAliasesIntoStorage)
      FrameStorageSlots = FrameSlots;
    FrameSlots.clear();
    if (!ProjectFrameAliasesIntoStorage)
      FrameAliases.clear();
  } else {
    walkStmts(Func.Body, [&](const HighStmt &Stmt) {
      if (Stmt.Kind != StmtKind::Assign || !Stmt.Dst || !Stmt.Val ||
          Stmt.Dst->Kind != ExprKind::Var ||
          Stmt.Dst->Var.Kind == MedVar::Param || !frameDisplacement(*Stmt.Val))
        return;
      const std::string Name = varName(Stmt.Dst->Var);
      // A join PHI of several `&slot` homes still prints (`v26 = &a` /
      // `v26 = &b`). Hiding it as a unique frame alias drops the
      // declaration while writeStmt keeps the assigns.
      if (AmbiguousFrameAliases.count(Name))
        return;
      Analysis.DeadStmts.insert(&Stmt);
      Analysis.DeadVars.insert(Name);
    });
  }

  const auto DebugFn = debugFunction(Dbg, Func.Entry);
  TypeRef ReturnType = InferredVoid ? NdType::makeVoid() : FuncReturnType;
  TypeRef IndirectReturnPtr;
  if (!Func.SourceTypeHint && DebugFn &&
      isMsvcIndirectReturn(DebugFn->ReturnType, Opts.TheArch, Opts.Format)) {
    IndirectReturnName = "result";
    InferredVoid = false;
    IndirectReturnPtr = FuncReturnType;
    ReturnType = IndirectReturnPtr;
    FuncReturnType = IndirectReturnPtr;
  }

  std::string FName = functionIdentifier(Func);

  if (EmitFunctionWrapper && Func.Entry)
    OS << "/* neverd.entry: 0x" << llvm::utohexstr(Func.Entry) << " */\n";
  // A definition whose identifier is spelled from its symbol names the symbol
  // as its language does (`QDomNode::nodeType() const`, `main.main`), unless
  // the debug name's comment below says the same.
  const bool DebugNameComment =
      !Func.DebugName.empty() && Func.DebugName != Func.Name;
  if (Opts.EmitComments)
    if (auto Symbol = FunctionSymbolNames.find(&Func);
        Symbol != FunctionSymbolNames.end())
      if (const std::string Demangled = demangledComment(Symbol->second);
          !Demangled.empty() &&
          !(DebugNameComment && Demangled == Func.DebugName)) {
        if (!EmitFunctionWrapper)
          emitIndent(1);
        OS << "/* " << Demangled << " */\n";
      }

  if (Opts.EmitComments && DebugNameComment) {
    if (!EmitFunctionWrapper)
      emitIndent(1);
    OS << "/* " << Func.DebugName;
    if (!Func.SourceFile.empty())
      OS << " @ " << Func.SourceFile << ":" << Func.SourceLine;
    OS << " */\n";
  }

  writeExceptionAnnotation(Func);
  if (Opts.EmitComments && DebugFn) {
    OS << "/* neverd.debug: return=";
    if (const TypeRef Return = cDisplayType(DebugFn->ReturnType); !Return)
      OS << "(none)";
    else if (hasCSpelling(Return))
      OS << typeToC(Return);
    else
      OS << "(no C type)";
    OS << " params=" << DebugFn->Params.size() << " */\n";
  }

  if (EmitFunctionWrapper) {
    if (Opts.TheArch == Arch::X86 || Opts.TheArch == Arch::X64) {
      const std::string TargetFeatures = x86CIntrinsicTargetFeatures(Func);
      if (!TargetFeatures.empty())
        OS << "__attribute__((target(\"" << TargetFeatures << "\")))\n";
    }

    if (Func.DoesNotReturn)
      OS << "_Noreturn ";
    if (Func.SourceTypeHint)
      OS << sourceConventionAttribute(Func.SourceTypeHint->Convention);
    else if (Opts.TheArch == Arch::X64 && DebugFn) {
      const bool Member =
          !DebugFn->Params.empty() && DebugFn->Params[0].first == "this";
      const bool Sret =
          isMsvcIndirectReturn(DebugFn->ReturnType, Opts.TheArch, Opts.Format);
      DebugCallConv CC = DebugFn->CallConv;
      if (CC == DebugCallConv::Thiscall)
        CC = DebugCallConv::Fastcall;
      if (Member || Sret || CC == DebugCallConv::Fastcall)
        OS << "__fastcall ";
      else if (CC != DebugCallConv::Cdecl && CC != DebugCallConv::Unknown)
        OS << debugCallConvAttribute(CC);
    } else if (DebugFn && DebugFn->CallConv != DebugCallConv::Unknown) {
      OS << debugCallConvAttribute(DebugFn->CallConv);
    }
    CProjectionIdentifierAllocator ParameterIdentifiers;
    std::string Declarator = FName + "(";
    const std::vector<size_t> ParamIndices = emittedParamIndices(Func);
    const bool Indirect = !IndirectReturnName.empty();
    size_t Emitted = 0;
    auto emitParam = [&](TypeRef Ty, std::string Name) {
      if (Emitted > 0)
        Declarator += ", ";
      const std::string Identifier =
          ParameterIdentifiers.allocate(Name, "nd_arg");
      Declarator += declarationToC(Ty, Identifier);
      // A parameter's name has its declared type, as a local's has.
      if (const TypeRef Display = cDisplayType(Ty);
          Display && Display->Kind == NdTypeKind::Int && !Display->IsEnum)
        DeclaredCTypes.emplace(Identifier, Display);
      ++Emitted;
    };
    const bool HighIRIncludesSret =
        Indirect && DebugFn && highIRIncludesIndirectReturn(Func, *DebugFn);
    const bool MemberSret =
        Indirect && DebugFn && isWin64MemberIndirectReturn(*DebugFn);
    const int SretId = DebugFn ? indirectReturnParamId(*DebugFn) : 0;
    auto emitHighIRParam = [&](size_t PI) {
      TypeRef Ty = emittedParamType(Func, PI);
      std::string Name = Func.Params[PI].Name;
      if (auto It = ParamDisplayNames.find(static_cast<int>(PI));
          It != ParamDisplayNames.end())
        Name = It->second;
      if (DebugFn && !Func.SourceTypeHint)
        if (std::string Debug = debugParamName(Func, PI); !Debug.empty())
          Name = std::move(Debug);
      if (Func.SourceTypeHint && PI < Func.SourceTypeHint->Parameters.size()) {
        if (Emitted)
          Declarator += ", ";
        auto Parameter = Func.SourceTypeHint->Parameters[PI];
        Parameter.Type = Ty;
        Declarator += sourceParameterType(
            Parameter, ParameterIdentifiers.allocate(Name, "nd_arg"));
        ++Emitted;
      } else
        emitParam(Ty, Name);
    };
    if (MemberSret && HighIRIncludesSret) {
      if (!ParamIndices.empty())
        emitHighIRParam(ParamIndices[0]);
      emitParam(IndirectReturnPtr, IndirectReturnName);
      for (size_t I = 1; I < ParamIndices.size(); ++I) {
        const size_t PI = ParamIndices[I];
        if (static_cast<int>(PI) == SretId)
          continue;
        emitHighIRParam(PI);
      }
    } else {
      if (Indirect && !MemberSret)
        emitParam(IndirectReturnPtr, IndirectReturnName);
      for (size_t I = 0; I < ParamIndices.size(); ++I) {
        const size_t PI = ParamIndices[I];
        if (HighIRIncludesSret && !MemberSret && PI == 0)
          continue;
        emitHighIRParam(PI);
      }
      if (Indirect && MemberSret && !HighIRIncludesSret)
        emitParam(IndirectReturnPtr, IndirectReturnName);
    }
    if (Emitted == 0)
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
  if (!IndirectReturnName.empty())
    ParamNames.insert(IndirectReturnName);
  if (DebugFn) {
    for (const auto &Param : DebugFn->Params)
      if (!Param.first.empty())
        ParamNames.insert(Param.first);
    // What the parameters print as, such as `p_8` for a record's second
    // register.
    for (size_t PI = 0; PI < Func.Params.size(); ++PI)
      if (std::string Name = debugParamName(Func, PI); !Name.empty())
        ParamNames.insert(std::move(Name));
  }
  for (const auto &[_, Name] : ParamDisplayNames)
    if (!Name.empty())
      ParamNames.insert(Name);
  if (Func.FrameSize > 0 || Func.FrameHeadroom > 0)
    ParamNames.insert("frame_base");
  if (NeedsFrameStorage && (Func.FrameSize > 0 || Func.FrameHeadroom > 0)) {
    uint64_t LowerSize = checkedStackAlign(
        Func.FrameSize > 0 ? static_cast<uint64_t>(Func.FrameSize) : 0);
    uint64_t UpperSize = checkedStackAlign(
        Func.FrameHeadroom > 0 ? static_cast<uint64_t>(Func.FrameHeadroom) : 0);
    uint64_t EntryResidue =
        syntheticEntryStackResidue(Opts.TheArch, Opts.Format, Func.EntryKind);
    uint64_t FrameBaseOffset = checkedStackAdd(LowerSize, EntryResidue);
    uint64_t StorageSize = checkedStackAdd(FrameBaseOffset, UpperSize);
    emitIndent(1);
    OS << "_Alignas(" << kSyntheticStackAlignment << ") uint8_t stack_storage["
       << StorageSize << "];\n";
    emitIndent(1);
    OS << "const uintptr_t frame_base = (uintptr_t)(stack_storage + "
       << FrameBaseOffset << ");\n";
    // The rebuilt storage has the host's pointer width, which a 64-bit
    // target's source is compiled with: frame arithmetic is uint64_t already.
    const uint16_t PointerBytes = getTargetRegInfo(Opts.TheArch).PointerSize;
    if (PointerBytes == sizeof(uint64_t))
      DeclaredCTypes.emplace("frame_base",
                             NdType::makeInt(PointerBytes, /*Signed=*/false));
  }
  std::set<std::string> PrintedAddrSlots;
  std::set<std::string> PrintedSlotStores;
  std::set<std::string> PrintedSlotLoads;
  auto WalkPrintedAddr = [&](const HighExpr &Root, bool AsAddress = false) {
    std::function<void(const HighExpr &, bool)> Walk = [&](const HighExpr &N,
                                                           bool AsAddress) {
      if (N.Kind == ExprKind::Var || N.Kind == ExprKind::Phi) {
        const std::string Name = copyForwardName(varName(N.Var));
        if (auto Fwd = ValueForward.find(Name);
            Fwd != ValueForward.end() && Fwd->second)
          Walk(*Fwd->second, AsAddress);
        return;
      }
      if (!AsAddress && N.Kind == ExprKind::Addr && !N.Operands.empty() &&
          N.Operands[0] && N.Operands[0]->Kind == ExprKind::Load &&
          !N.Operands[0]->Operands.empty() && N.Operands[0]->Operands[0]) {
        if (auto Name = namedFrameSlot(*N.Operands[0]->Operands[0])) {
          if (!isCxxCatchObjectName(*Name) &&
              !cxxCatchFieldAccess(*N.Operands[0]->Operands[0]) &&
              !cxxCatchPointerName(*N.Operands[0]->Operands[0]) &&
              !typedMemberAccess(*N.Operands[0]->Operands[0]))
            PrintedAddrSlots.insert(*Name);
        }
        Walk(*N.Operands[0]->Operands[0], true);
        return;
      }
      if (!AsAddress &&
          (N.Kind == ExprKind::BinOp || N.Kind == ExprKind::Addr)) {
        if (auto Name = namedFrameSlot(N)) {
          if (!isCxxCatchObjectName(*Name) && !cxxCatchFieldAccess(N) &&
              !cxxCatchPointerName(N) && !typedMemberAccess(N))
            PrintedAddrSlots.insert(*Name);
          for (const ExprPtr &Op : N.Operands)
            if (Op)
              Walk(*Op, true);
          return;
        }
      }
      if (N.Kind == ExprKind::Load && !N.Operands.empty() && N.Operands[0]) {
        if (auto Name = namedFrameSlot(*N.Operands[0]);
            Name && !isCxxCatchObjectName(*Name))
          PrintedSlotLoads.insert(*Name);
        Walk(*N.Operands[0], true);
        return;
      }
      if (N.Kind == ExprKind::Store && N.Operands.size() >= 2) {
        if (N.Operands[0]) {
          if (auto Name = namedFrameSlot(*N.Operands[0]);
              Name && !isCxxCatchObjectName(*Name))
            PrintedSlotStores.insert(*Name);
          Walk(*N.Operands[0], true);
        }
        if (N.Operands[1])
          Walk(*N.Operands[1], false);
        return;
      }
      for (const ExprPtr &Op : N.Operands)
        if (Op)
          Walk(*Op, AsAddress);
      // An indirect call reads its target as a value.
      if (N.IndirectTarget)
        Walk(*N.IndirectTarget, false);
    };
    Walk(Root, AsAddress);
  };
  std::function<void(const std::vector<HighStmt> &, bool)> CollectPrinted;
  CollectPrinted = [&](const std::vector<HighStmt> &Stmts, bool InHandler) {
    const bool Saved = InEHClauseBody;
    InEHClauseBody = InHandler;
    for (size_t I = 0; I < Stmts.size(); ++I) {
      const HighStmt &S = Stmts[I];
      switch (S.Kind) {
      case StmtKind::If:
      case StmtKind::While:
      case StmtKind::DoWhile:
      case StmtKind::For:
      case StmtKind::Block:
        CollectPrinted(S.Body, InHandler);
        if (S.Cond)
          WalkPrintedAddr(*S.Cond);
        continue;
      case StmtKind::IfElse:
        CollectPrinted(S.Body, InHandler);
        CollectPrinted(S.ElseBody, InHandler);
        if (S.Cond)
          WalkPrintedAddr(*S.Cond);
        continue;
      case StmtKind::Switch:
        for (const SwitchCase &Case : S.Cases)
          CollectPrinted(Case.Body, InHandler);
        CollectPrinted(S.DefaultBody, InHandler);
        if (S.SwitchExpr)
          WalkPrintedAddr(*S.SwitchExpr);
        continue;
      case StmtKind::SEHTry:
      case StmtKind::CxxTry:
      case StmtKind::ItaniumTry:
        // writeTryBody keeps the outer InEHClauseBody, so a nested try
        // inside catch still rewrites `[frame]=val; return` as `return val`.
        CollectPrinted(S.Body, InHandler);
        for (const auto &ClauseBody : S.EHClauseBodies)
          CollectPrinted(ClauseBody, true);
        continue;
      default:
        break;
      }
      if (Analysis.DeadStmts.count(&S) || stmtHiddenFromC(S) ||
          CxxThrowPrints.count(&S))
        continue;
      const bool BecomesParentReturn =
          InHandler && !InferredVoid && parentFrameStoredValue(S);
      if (BecomesParentReturn) {
        size_t J = I + 1;
        while (J < Stmts.size() &&
               (stmtHiddenFromC(Stmts[J]) || Stmts[J].Kind == StmtKind::Nop))
          ++J;
        if (J < Stmts.size() && Stmts[J].Kind == StmtKind::Return)
          continue;
      }
      auto NotePrintedStore = [&](const HighExpr *Addr, const HighExpr *Val) {
        if (!Addr || !Val || isCompilerEHConstant(*Val))
          return;
        if (auto Name = namedFrameSlot(*Addr);
            Name && !isCxxCatchObjectName(*Name))
          PrintedSlotStores.insert(*Name);
      };
      if (S.Kind == StmtKind::Store) {
        NotePrintedStore(S.StoreAddr.get(), S.StoreVal.get());
        if (S.StoreAddr)
          WalkPrintedAddr(*S.StoreAddr, true);
      }
      if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
          S.Dst->Kind == ExprKind::Load && !S.Dst->Operands.empty()) {
        NotePrintedStore(S.Dst->Operands[0].get(), S.Val.get());
        if (S.Dst->Operands[0])
          WalkPrintedAddr(*S.Dst->Operands[0], true);
      }
      forEachRhsExpr(S, [&](const ExprPtr &E) {
        if (!E)
          return;
        // A direct frame address identifies the destination, but a load
        // inside that address still reads its pointer home.
        WalkPrintedAddr(*E, S.Kind == StmtKind::Store &&
                                E.get() == S.StoreAddr.get());
      });
    }
    InEHClauseBody = Saved;
  };
  CollectPrinted(Func.Body, false);
  {
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
    auto ValIsUnusedHome = [&](const HighExpr &Val) {
      if (ExprHasEffect(Val))
        return false;
      if (isUnknownCallOperand(&Val))
        return true;
      const HighExpr *Inner = unwrapIntegerView(&Val);
      if (!Inner)
        Inner = &Val;
      if (Inner->Kind == ExprKind::Const && Inner->ConstVal == 0)
        return true;
      return isParamCopy(Val) || isParamCopy(*Inner);
    };
    std::function<void(const std::vector<HighStmt> &, bool)> HideHomes;
    HideHomes = [&](const std::vector<HighStmt> &Stmts, bool InHandler) {
      for (const HighStmt &S : Stmts) {
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
          if (Addr && Val && ValIsUnusedHome(*Val) &&
              S.MemoryOrdering == NdMemoryOrdering::None &&
              S.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
              !typedMemberAccess(*Addr)) {
            if (auto Name = namedFrameSlot(*Addr);
                Name && !PrintedAddrSlots.count(*Name) &&
                !PrintedSlotLoads.count(*Name)) {
              bool OverlapsPrinted = false;
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
                  if (!PrintedAddrSlots.count(Obs.Name) &&
                      !PrintedSlotLoads.count(Obs.Name))
                    continue;
                  uint16_t ObsSize = Obs.Type ? Obs.Type->Size : 1;
                  if (ObsSize == 0)
                    ObsSize = 1;
                  const int64_t ObsEnd =
                      ObsDisp + static_cast<int64_t>(ObsSize);
                  if (Begin < ObsEnd && ObsDisp < End) {
                    OverlapsPrinted = true;
                    break;
                  }
                }
              }
              if (!OverlapsPrinted) {
                Analysis.DeadStmts.insert(&S);
                bool StillPrinted = false;
                std::function<void(const std::vector<HighStmt> &, bool)> Find =
                    [&](const std::vector<HighStmt> &Inner, bool Handler) {
                      if (StillPrinted)
                        return;
                      const bool SavedEH = InEHClauseBody;
                      InEHClauseBody = Handler;
                      for (const HighStmt &T : Inner) {
                        if (T.Kind == StmtKind::If ||
                            T.Kind == StmtKind::While ||
                            T.Kind == StmtKind::DoWhile ||
                            T.Kind == StmtKind::For ||
                            T.Kind == StmtKind::Block) {
                          Find(T.Body, Handler);
                          continue;
                        }
                        if (T.Kind == StmtKind::IfElse) {
                          Find(T.Body, Handler);
                          Find(T.ElseBody, Handler);
                          continue;
                        }
                        if (T.Kind == StmtKind::Switch) {
                          for (const auto &C : T.Cases)
                            Find(C.Body, Handler);
                          Find(T.DefaultBody, Handler);
                          continue;
                        }
                        if (T.Kind == StmtKind::SEHTry ||
                            T.Kind == StmtKind::CxxTry ||
                            T.Kind == StmtKind::ItaniumTry) {
                          Find(T.Body, Handler);
                          for (const auto &Clause : T.EHClauseBodies)
                            Find(Clause, true);
                          continue;
                        }
                        if (&T == &S || Analysis.DeadStmts.count(&T) ||
                            stmtHiddenFromC(T))
                          continue;
                        const HighExpr *LiveAddr = nullptr;
                        if (T.Kind == StmtKind::Store)
                          LiveAddr = T.StoreAddr.get();
                        else if (T.Kind == StmtKind::Assign && T.Dst &&
                                 T.Dst->Kind == ExprKind::Load &&
                                 !T.Dst->Operands.empty())
                          LiveAddr = T.Dst->Operands[0].get();
                        if (LiveAddr)
                          if (auto Live = namedFrameSlot(*LiveAddr);
                              Live && *Live == *Name)
                            StillPrinted = true;
                      }
                      InEHClauseBody = SavedEH;
                    };
                Find(Func.Body, false);
                if (!StillPrinted)
                  PrintedSlotStores.erase(*Name);
              }
            }
          }
        }
        HideHomes(S.Body, InHandler);
        HideHomes(S.ElseBody, InHandler);
        for (const auto &C : S.Cases)
          HideHomes(C.Body, InHandler);
        HideHomes(S.DefaultBody, InHandler);
        for (const auto &ClauseBody : S.EHClauseBodies)
          HideHomes(ClauseBody, true);
      }
    };
    HideHomes(Func.Body, false);
  }
  // An enclosing slot is printed whenever one of its interior accesses is.
  for (const auto &[Disp, Slot] : FrameSlots)
    if (!Slot.Interior.empty() && (PrintedAddrSlots.count(Slot.Interior) ||
                                   PrintedSlotStores.count(Slot.Interior) ||
                                   PrintedSlotLoads.count(Slot.Interior)))
      PrintedAddrSlots.insert(Slot.Outer);
  for (const auto &[Disp, Slot] : FrameSlots) {
    if (!Slot.Interior.empty() && !Slot.RegionBytes)
      continue;
    if (ParamNames.count(Slot.Name))
      continue;
    DeferredDecl Decl;
    if (Slot.RegionBytes)
      Decl.Text =
          "uint8_t " + Slot.Name + "[" + std::to_string(Slot.RegionBytes) + "]";
    else
      Decl.Type = cDisplayType(Slot.Type);
    const bool Printed = PrintedAddrSlots.count(Slot.Name) ||
                         PrintedSlotStores.count(Slot.Name) ||
                         PrintedSlotLoads.count(Slot.Name);
    if (!Printed ||
        (!Slot.AddressTaken && (CopyForward.count(Slot.Name) ||
                                Analysis.DeadVars.count(Slot.Name)))) {
      DeferredDecls.emplace(Slot.Name, std::move(Decl));
      continue;
    }
    ParamNames.insert(Slot.Name);
    DeclaredCNames.insert(Slot.Name);
    emitIndent(1);
    OS << (Decl.Text.empty() ? declarationToC(Decl.Type, Slot.Name) : Decl.Text)
       << ";\n";
  }
  emitLocalDecls(Func, ParamNames);
  for (const auto &Name : DeclaredCNames)
    MemoryIdentifiers.allocate(Name);
  for (const auto &[Name, Decl] : DeferredDecls)
    MemoryIdentifiers.allocate(Name);
  for (const auto &Name : ParamNames)
    MemoryIdentifiers.allocate(Name);

  // Render the body first: a name the declaration pass expected to be
  // forwarded or dead may still be printed, and it must be declared.
  std::string Body;
  {
    llvm::raw_string_ostream BodyOS(Body);
    llvm::raw_ostream *Saved = Out.redirect(&BodyOS);
    writeStmts(Func.Body, 1);
    Out.redirect(Saved);
  }
  std::unordered_set<std::string_view> BodyIdents;
  if (!DeferredDecls.empty() || !MemoryTemporaries.empty()) {
    auto IsIdent = [](char C) {
      return std::isalnum(static_cast<unsigned char>(C)) || C == '_';
    };
    for (size_t Pos = 0; Pos < Body.size();) {
      if (!IsIdent(Body[Pos])) {
        ++Pos;
        continue;
      }
      size_t End = Pos;
      while (End < Body.size() && IsIdent(Body[End]))
        ++End;
      BodyIdents.insert(std::string_view(Body).substr(Pos, End - Pos));
      Pos = End;
    }
  }
  for (const auto &[Name, Decl] : DeferredDecls) {
    if (ParamNames.count(Name) || !BodyIdents.count(Name))
      continue;
    ParamNames.insert(Name);
    emitIndent(1);
    OS << (Decl.Text.empty() ? declarationToC(Decl.Type, Name) : Decl.Text)
       << ";\n";
  }
  DeferredDecls.clear();
  for (const auto &[Name, Type] : MemoryTemporaries) {
    if (!BodyIdents.count(Name))
      continue;
    emitIndent(1);
    OS << Type << " " << Name << ";\n";
  }
  OS << Body;
  if (EmitFunctionWrapper && !IndirectReturnName.empty() &&
      !PrintedIndirectReturn) {
    emitIndent(1);
    OS << "return " << IndirectReturnName << ";\n";
  }
  if (EmitFunctionWrapper)
    OS << "}\n";
}

} // namespace neverd
