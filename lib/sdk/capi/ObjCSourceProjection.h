//===- ObjCSourceProjection.h - Objective-C body projection checks -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_OBJCSOURCEPROJECTION_H
#define NEVERD_SDK_CAPI_OBJCSOURCEPROJECTION_H

#include "../../loader/SourceUnwind.h"
#include "SourceProjectionFlow.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/StringRef.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::sdk {
namespace objc_projection_detail {

inline bool sameHint(const SourceFunctionTypeHint &Left,
                     const SourceFunctionTypeHint &Right) {
  return equalSourceABIs(Left, Right);
}

inline bool isPlainUnwind(const ExceptionFunction &Metadata) {
  return isPlainSourceUnwind(Metadata);
}

} // namespace objc_projection_detail

inline void collectSourceBodyDiagnostics(
    const HighFunc &Func, const SourceFunctionTypeHint &Hint,
    const PipelineFunctionAudit *Audit,
    const std::function<bool(const HighExpr &)> &CallAllowed,
    SourceProjectionDiagnostics &Diagnostics) {
  using namespace objc_projection_detail;
  if (!Func.SourceTypeHint || !sameHint(*Func.SourceTypeHint, Hint) ||
      !equalSourceTypes(Func.ReturnType, Hint.ReturnType) ||
      Func.Params.size() != Hint.Parameters.size() ||
      Hint.Parameters.size() > 64)
    Diagnostics.add(
        SourceProjectionIssue::Signature,
        "method source signature disagrees with its runtime type hint");
  if (Hint.Parameters.size() > 64)
    Diagnostics.Complete = false;
  std::string ABILimitation;
  if (Hint.HasExplicitABI && !validateSourceABI(Hint, ABILimitation))
    Diagnostics.add(SourceProjectionIssue::ABI, ABILimitation);
  if (!Hint.HasExplicitABI &&
      (Hint.Origin != SourceFunctionTypeHint::OriginKind::ObjCRuntime ||
       Hint.Parameters.size() < 2 || Hint.Parameters.size() > 8))
    Diagnostics.add(SourceProjectionIssue::ABI,
                    "method has no explicit source ABI binding");
  for (size_t Index = 0; Index < std::min({Func.Params.size(),
                                           Hint.Parameters.size(), size_t{64}});
       ++Index) {
    const auto &Parameter = Func.Params[Index];
    if (Parameter.Name != Hint.Parameters[Index].Name ||
        !equalSourceTypes(Parameter.Type, Hint.Parameters[Index].Type) ||
        Parameter.Type->Kind == NdTypeKind::Void)
      Diagnostics.add(
          SourceProjectionIssue::ParameterBinding,
          "method source parameter binding disagrees with its runtime type "
          "hint");
  }
  if (!Audit || Audit->Entry != Func.Entry ||
      Audit->Disposition != PipelineFunctionDisposition::Accepted ||
      !Audit->HasLowIR || !Audit->HasMedIR || !Audit->MedIRVerified ||
      !Audit->DecodedInstructions ||
      Audit->DecodedInstructions != Audit->LiftedInstructions ||
      !Audit->DecodeFailures.empty() ||
      !Audit->UnsupportedInstructions.empty() || !Audit->TruncatedPaths.empty())
    Diagnostics.add(
        SourceProjectionIssue::Audit,
        "native method decoding, lifting, or IR verification is incomplete");
  if (Func.Body.empty())
    Diagnostics.add(SourceProjectionIssue::Body,
                    "no function body was recovered");
  if ((Func.ExceptionMetadata && !isPlainUnwind(*Func.ExceptionMetadata)) ||
      Func.StructuredExceptionRegions || Func.UnstructuredExceptionRegions)
    Diagnostics.add(SourceProjectionIssue::Exception,
                    "exception-dependent method projection is not supported");
  if (Hint.ReturnType)
    SourceProjectionFlow(Func, Diagnostics)
        .collect(Hint.ReturnType->Kind != NdTypeKind::Void);
  else
    Diagnostics.Complete = false;

  // Iterate explicitly so malformed/deep HighIR cannot overflow this check's
  // own stack. HighC's expression renderer truncates beyond depth 200.
  std::vector<std::pair<const HighStmt *, unsigned>> Statements;
  std::vector<std::tuple<const HighExpr *, unsigned, va_t>> Expressions;
  size_t Budget = 1000000;
  auto Spend = [&](size_t Count) {
    if (Count > Budget) {
      Diagnostics.Complete = false;
      Diagnostics.add(SourceProjectionIssue::Budget,
                      "method source inspection exceeds its work limit");
      return false;
    }
    Budget -= Count;
    return true;
  };
  std::set<LocalIdentity> DefinedLocals;
  auto AddStatements = [&](const std::vector<HighStmt> &Body, unsigned Depth) {
    if (!Spend(Body.size()))
      return false;
    for (const auto &Statement : Body)
      Statements.emplace_back(&Statement, Depth);
    return true;
  };
  if (!AddStatements(Func.Body, 1))
    return;
  while (!Statements.empty()) {
    const auto [Statement, Depth] = Statements.back();
    Statements.pop_back();
    if (Depth > 200) {
      Diagnostics.Complete = false;
      Diagnostics.add(
          SourceProjectionIssue::Budget,
          "method control flow exceeds the source projection depth limit",
          Statement->Addr);
      continue;
    }
    if (Statement->Kind == StmtKind::SEHTry ||
        Statement->Kind == StmtKind::CxxTry ||
        Statement->Kind == StmtKind::ItaniumTry)
      Diagnostics.add(SourceProjectionIssue::Exception,
                      "exception-dependent method projection is not supported");
    if (Statement->Kind == StmtKind::Return && Hint.ReturnType &&
        Hint.ReturnType->Kind != NdTypeKind::Void) {
      if (!Statement->RetVal)
        Diagnostics.add(
            SourceProjectionIssue::ControlFlow,
            "non-void method has a return without a recovered value");
    }
    if (Statement->Kind == StmtKind::Assign && Statement->Dst &&
        Statement->Val &&
        (Statement->Dst->Kind == ExprKind::Var ||
         Statement->Dst->Kind == ExprKind::Phi))
      DefinedLocals.insert(localIdentity(Statement->Dst->Var));
    forEachExpr(*Statement, [&](const ExprPtr &Expression) {
      // A direct assignment destination defines a local value; it is not an
      // incoming read. Address expressions on compound destinations still are.
      if (Expression &&
          !(Expression == Statement->Dst && Expression->Kind == ExprKind::Var))
        Expressions.emplace_back(Expression.get(), 1, Statement->Addr);
    });
    if (!AddStatements(Statement->Body, Depth + 1) ||
        !AddStatements(Statement->ElseBody, Depth + 1) ||
        !Spend(Statement->Cases.size() + Statement->EHClauseBodies.size()))
      return;
    for (const auto &Case : Statement->Cases)
      if (!AddStatements(Case.Body, Depth + 1))
        return;
    if (!AddStatements(Statement->DefaultBody, Depth + 1))
      return;
    for (const auto &Clause : Statement->EHClauseBodies)
      if (!AddStatements(Clause, Depth + 1))
        return;
  }
  std::map<std::pair<const HighExpr *, va_t>, unsigned> SeenDepth;
  while (!Expressions.empty()) {
    const auto [Expression, Depth, Address] = Expressions.back();
    Expressions.pop_back();
    if (Depth > 200) {
      Diagnostics.Complete = false;
      Diagnostics.add(
          SourceProjectionIssue::Budget,
          "method expression exceeds the source projection depth limit",
          Address);
      continue;
    }
    const va_t IdentityAddress =
        Diagnostics.Collection == SourceProjectionDiagnostics::Mode::All
            ? Address
            : 0;
    auto [Position, Inserted] =
        SeenDepth.emplace(std::make_pair(Expression, IdentityAddress), Depth);
    if (!Inserted && Position->second >= Depth)
      continue;
    Position->second = Depth;
    if (Expression->Kind == ExprKind::Undef)
      Diagnostics.add(SourceProjectionIssue::UnresolvedValue,
                      "method contains an unresolved value", Address,
                      Expression);
    if (Expression->Kind == ExprKind::Call &&
        !(CallAllowed && CallAllowed(*Expression)) &&
        (Expression->IntrinsicId == Intrinsic::None ||
         !intrinsicCName(Expression->IntrinsicId) ||
         Expression->IsIndirectCall ||
         Expression->MemoryAddressSpace != NdMemoryAddressSpace::Default)) {
      Diagnostics.add(
          SourceProjectionIssue::CallBinding,
          "method calls a native or dynamic target without a source binding",
          Address, Expression);
    }
    if (Expression->Kind == ExprKind::Var ||
        Expression->Kind == ExprKind::Phi) {
      const MedVar &Variable = Expression->Var;
      if (Variable.Kind == MedVar::Param) {
        if (Variable.Id < 0 ||
            static_cast<size_t>(Variable.Id) >= Hint.Parameters.size() ||
            Variable.RenameTag >= 0 ||
            (Hint.HasExplicitABI && Variable.TheArch != Hint.Architecture) ||
            (Variable.TheArch != Arch::X64 &&
             Variable.TheArch != Arch::AArch64))
          Diagnostics.add(SourceProjectionIssue::ParameterBinding,
                          "method references an unbound source parameter",
                          Address, Expression);
        else if (Hint.HasExplicitABI) {
          const auto &Location = Hint.Parameters[Variable.Id].Location;
          if (!Hint.Parameters[Variable.Id].Components.empty()) {
            if (Variable.RegOff != 0 ||
                Variable.Size != Hint.Parameters[Variable.Id].Type->Size ||
                !equalSourceTypes(Expression->Type,
                                  Hint.Parameters[Variable.Id].Type))
              Diagnostics.add(SourceProjectionIssue::ParameterBinding,
                              "method record parameter has no logical binding",
                              Address, Expression);
          } else if (Location.Kind == SourceABICarrierKind::Stack) {
            if (Variable.StackOff != Location.EntryStackOffset)
              Diagnostics.add(
                  SourceProjectionIssue::ParameterBinding,
                  "method source parameter occupies the wrong stack "
                  "position",
                  Address, Expression);
          } else if (Variable.RegOff != Location.RegisterOffset) {
            Diagnostics.add(
                SourceProjectionIssue::ParameterBinding,
                "method source parameter occupies the wrong register "
                "position",
                Address, Expression);
          }
        } else {
          const auto &Registers =
              getTargetRegInfo(Variable.TheArch).IntParamRegs;
          if (static_cast<size_t>(Variable.Id) >= Registers.size() ||
              Registers[Variable.Id] != Variable.RegOff)
            Diagnostics.add(
                SourceProjectionIssue::ParameterBinding,
                "method source parameter occupies the wrong ABI position",
                Address, Expression);
        }
      } else if ((Variable.Kind == MedVar::Reg ||
                  Variable.Kind == MedVar::Flag) &&
                 Variable.SSAVer == 0 && Variable.RenameTag < 0) {
        if (!sourceFrameBase(Func, Variable) &&
            !DefinedLocals.count(localIdentity(Variable)))
          Diagnostics.add(
              SourceProjectionIssue::IncomingValue,
              "method contains an unexplained incoming register value", Address,
              Expression);
      } else if (Variable.Kind == MedVar::EHException ||
                 Variable.Kind == MedVar::EHSelector) {
        Diagnostics.add(
            SourceProjectionIssue::Exception,
            "exception-dependent method projection is not supported", Address,
            Expression);
      } else if (!DefinedLocals.count(localIdentity(Variable))) {
        // Keep checking even unreachable expressions that HighC still emits.
        // Reachable reads also passed the source CFG's must-defined analysis.
        Diagnostics.add(
            SourceProjectionIssue::LocalDefinition,
            "method reads a local value without a recovered definition",
            Address, Expression);
      }
    }
    if (!Spend(Expression->Operands.size()))
      return;
    for (const auto &Operand : Expression->Operands) {
      if (!Operand)
        Diagnostics.add(SourceProjectionIssue::MalformedExpression,
                        "method contains a missing expression operand", Address,
                        Expression);
      else
        Expressions.emplace_back(Operand.get(), Depth + 1, Address);
    }
  }
}

/// Empty diagnostics mean complete supported source representation, not a
/// proof of equivalence. Unknown graph edges and exhausted budgets explicitly
/// leave Complete false, while independent body checks can still contribute.
inline SourceProjectionDiagnostics sourceBodyDiagnostics(
    const HighFunc &Func, const SourceFunctionTypeHint &Hint,
    const PipelineFunctionAudit *Audit,
    const std::function<bool(const HighExpr &)> &CallAllowed = {},
    SourceProjectionDiagnostics::Mode Collection =
        SourceProjectionDiagnostics::Mode::All) {
  SourceProjectionDiagnostics Diagnostics(Collection);
  try {
    collectSourceBodyDiagnostics(Func, Hint, Audit, CallAllowed, Diagnostics);
  } catch (const SourceProjectionDiagnostics::Stop &) {
  }
  return Diagnostics;
}

inline std::string sourceBodyLimitation(
    const HighFunc &Func, const SourceFunctionTypeHint &Hint,
    const PipelineFunctionAudit *Audit,
    const std::function<bool(const HighExpr &)> &CallAllowed = {},
    const HighExpr **UnboundCall = nullptr) {
  if (UnboundCall)
    *UnboundCall = nullptr;
  const auto Diagnostics =
      sourceBodyDiagnostics(Func, Hint, Audit, CallAllowed,
                            SourceProjectionDiagnostics::Mode::FirstFailure);
  if (UnboundCall)
    *UnboundCall = Diagnostics.firstUnboundCall();
  return Diagnostics.limitation();
}

inline SourceProjectionDiagnostics objcSourceBodyDiagnostics(
    const HighFunc &Func, const SourceFunctionTypeHint &Hint,
    const PipelineFunctionAudit *Audit,
    const std::function<bool(const HighExpr &)> &CallAllowed = {},
    SourceProjectionDiagnostics::Mode Collection =
        SourceProjectionDiagnostics::Mode::All) {
  if (Hint.Origin == SourceFunctionTypeHint::OriginKind::ObjCRuntime)
    return sourceBodyDiagnostics(Func, Hint, Audit, CallAllowed, Collection);
  SourceProjectionDiagnostics Diagnostics(Collection);
  Diagnostics.Complete = false;
  Diagnostics.Items.push_back(
      {SourceProjectionIssue::Signature,
       "Objective-C method has a different source declaration origin"});
  return Diagnostics;
}

inline std::string objcSourceBodyLimitation(
    const HighFunc &Func, const SourceFunctionTypeHint &Hint,
    const PipelineFunctionAudit *Audit,
    const std::function<bool(const HighExpr &)> &CallAllowed = {},
    const HighExpr **UnboundCall = nullptr) {
  if (UnboundCall)
    *UnboundCall = nullptr;
  const auto Diagnostics = objcSourceBodyDiagnostics(
      Func, Hint, Audit, CallAllowed,
      SourceProjectionDiagnostics::Mode::FirstFailure);
  if (UnboundCall)
    *UnboundCall = Diagnostics.firstUnboundCall();
  return Diagnostics.limitation();
}

/// HighC currently returns success even when it emits a diagnostic placeholder.
/// Inspect actual block comments, excluding comment-like text in C literals.
inline std::string objcSourceTextLimitation(llvm::StringRef Source) {
  if (Source.trim().empty())
    return "method C source projection is empty";
  for (size_t Index = 0; Index < Source.size();) {
    if (Source[Index] == '"' || Source[Index] == '\'') {
      const char Quote = Source[Index++];
      while (Index < Source.size()) {
        if (Source[Index] == '\\')
          Index += Index + 1 < Source.size() ? 2 : 1;
        else if (Source[Index++] == Quote)
          break;
      }
    } else if (Source.substr(Index).starts_with("//")) {
      const size_t End = Source.find('\n', Index + 2);
      Index = End == llvm::StringRef::npos ? Source.size() : End + 1;
    } else if (Source.substr(Index).starts_with("/*")) {
      const size_t End = Source.find("*/", Index + 2);
      if (End == llvm::StringRef::npos)
        return "method C source projection contains an unterminated comment";
      const llvm::StringRef Comment = Source.slice(Index + 2, End).trim();
      if (Comment.starts_with("truncated:") || Comment.starts_with("bad ") ||
          Comment.starts_with("unknown_op(") || Comment == "unknown expr" ||
          Comment.starts_with("unary ") ||
          Comment == "caller-saved register clobbered by call: unknown")
        return "method C source projection contains an incomplete emitter "
               "placeholder";
      Index = End + 2;
    } else {
      ++Index;
    }
  }
  return {};
}

} // namespace neverd::sdk

#endif
