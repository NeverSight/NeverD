//===- ObjCSourceProjection.h - Objective-C body projection checks -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_OBJCSOURCEPROJECTION_H
#define NEVERD_SDK_CAPI_OBJCSOURCEPROJECTION_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/StringRef.h"

#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::sdk {
namespace objc_projection_detail {

using LocalIdentity = std::tuple<int, int, int, int64_t>;

inline LocalIdentity localIdentity(const MedVar &Variable) {
  // Phi cleanup can merge different SSA values/kinds into one emitted local.
  // Conversely, unrenamed temporaries, registers, and stack slots must not
  // lend definitions to one another merely because their numeric IDs match.
  if (Variable.RenameTag >= 0)
    return {-1, Variable.RenameTag, 0, 0};
  if (Variable.Kind == MedVar::Stack)
    return {MedVar::Stack, 0, 0, Variable.StackOff};
  if (Variable.Kind == MedVar::RetVal)
    return {MedVar::RetVal, 0, 0, 0};
  return {Variable.Kind, Variable.Id, Variable.SSAVer, 0};
}

inline bool sameType(const TypeRef &Left, const TypeRef &Right,
                     unsigned Depth = 0) {
  if (!Left || !Right || Depth > 16 || Left->Kind != Right->Kind ||
      Left->Size != Right->Size || Left->IsSigned != Right->IsSigned)
    return false;
  switch (Left->Kind) {
  case NdTypeKind::Void:
    return Left->Size == 0;
  case NdTypeKind::Int:
    return Left->Size == 1 || Left->Size == 2 || Left->Size == 4 ||
           Left->Size == 8;
  case NdTypeKind::Ptr:
    return Left->Size == 8 &&
           sameType(Left->Pointee, Right->Pointee, Depth + 1);
  default:
    return false;
  }
}

inline bool sameHint(const SourceFunctionTypeHint &Left,
                     const SourceFunctionTypeHint &Right) {
  if (Left.Origin != SourceFunctionTypeHint::OriginKind::ObjCRuntime ||
      Left.Origin != Right.Origin ||
      !sameType(Left.ReturnType, Right.ReturnType) ||
      Left.Parameters.size() != Right.Parameters.size())
    return false;
  for (size_t Index = 0; Index < Left.Parameters.size(); ++Index)
    if (Left.Parameters[Index].Name != Right.Parameters[Index].Name ||
        !sameType(Left.Parameters[Index].Type, Right.Parameters[Index].Type))
      return false;
  return true;
}

inline bool isPlainUnwind(const ExceptionFunction &Metadata) {
  // Darwin emits unwind ranges for ordinary leaf methods too. Only a fully
  // decoded structural frame with no language-dispatch state is benign here.
  if (Metadata.ParseStatus != ExceptionParseStatus::Complete ||
      Metadata.Personality != ExceptionPersonality::None ||
      Metadata.PersonalityVA || !Metadata.PersonalityName.empty() ||
      Metadata.HandlerDataVA || Metadata.hasLanguageTable() ||
      Metadata.GSCookie || Metadata.ARMEHABI || Metadata.Rust || Metadata.ObjC)
    return false;
  if (Metadata.Encoding == ExceptionEncoding::CompactUnwind) {
    if (!Metadata.Compact)
      return false;
  } else if (Metadata.Encoding == ExceptionEncoding::DwarfFDE) {
    if (!Metadata.Dwarf)
      return false;
  } else {
    return false;
  }
  if (Metadata.Compact &&
      (Metadata.Compact->PersonalityVA || Metadata.Compact->HasLSDA ||
       Metadata.Compact->LSDAVA ||
       Metadata.Compact->SemanticStatus !=
           CompactUnwindSemanticStatus::Complete))
    return false;
  return !Metadata.Dwarf || Metadata.Dwarf->LSDAVA == 0;
}

} // namespace objc_projection_detail

/// Empty means the projection has a complete supported source representation.
/// This checks coverage and binding consistency, not semantic equivalence.
inline std::string
objcSourceBodyLimitation(const HighFunc &Func,
                         const SourceFunctionTypeHint &Hint,
                         const PipelineFunctionAudit *Audit) {
  using namespace objc_projection_detail;
  if (!Func.SourceTypeHint || !sameHint(*Func.SourceTypeHint, Hint) ||
      !sameType(Func.ReturnType, Hint.ReturnType) ||
      Func.Params.size() != Hint.Parameters.size() ||
      Hint.Parameters.size() < 2 || Hint.Parameters.size() > 8)
    return "method source signature disagrees with its runtime type hint";
  for (size_t Index = 0; Index < Func.Params.size(); ++Index) {
    const auto &Parameter = Func.Params[Index];
    if (Parameter.Name != Hint.Parameters[Index].Name ||
        !sameType(Parameter.Type, Hint.Parameters[Index].Type) ||
        Parameter.Type->Kind == NdTypeKind::Void)
      return "method source parameter binding disagrees with its runtime type "
             "hint";
  }
  if (!Audit || Audit->Entry != Func.Entry ||
      Audit->Disposition != PipelineFunctionDisposition::Accepted ||
      !Audit->HasLowIR || !Audit->HasMedIR || !Audit->MedIRVerified ||
      !Audit->DecodedInstructions ||
      Audit->DecodedInstructions != Audit->LiftedInstructions ||
      !Audit->DecodeFailures.empty() ||
      !Audit->UnsupportedInstructions.empty() || !Audit->TruncatedPaths.empty())
    return "native method decoding, lifting, or IR verification is incomplete";
  if (Func.Body.empty())
    return "no function body was recovered";
  if ((Func.ExceptionMetadata && !isPlainUnwind(*Func.ExceptionMetadata)) ||
      Func.StructuredExceptionRegions || Func.UnstructuredExceptionRegions)
    return "exception-dependent method projection is not supported";

  // Iterate explicitly so malformed/deep HighIR cannot overflow this check's
  // own stack. HighC's expression renderer truncates beyond depth 200.
  std::vector<std::pair<const HighStmt *, unsigned>> Statements;
  std::vector<std::pair<const HighExpr *, unsigned>> Expressions;
  std::set<LocalIdentity> DefinedLocals;
  auto AddStatements = [&](const std::vector<HighStmt> &Body, unsigned Depth) {
    for (const auto &Statement : Body)
      Statements.emplace_back(&Statement, Depth);
  };
  AddStatements(Func.Body, 1);
  bool HasValueReturn = false;
  while (!Statements.empty()) {
    const auto [Statement, Depth] = Statements.back();
    Statements.pop_back();
    if (Depth > 200)
      return "method control flow exceeds the source projection depth limit";
    if (Statement->Kind == StmtKind::SEHTry ||
        Statement->Kind == StmtKind::CxxTry ||
        Statement->Kind == StmtKind::ItaniumTry)
      return "exception-dependent method projection is not supported";
    if (Statement->Kind == StmtKind::Return &&
        Hint.ReturnType->Kind != NdTypeKind::Void) {
      if (!Statement->RetVal)
        return "non-void method has a return without a recovered value";
      HasValueReturn = true;
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
        Expressions.emplace_back(Expression.get(), 1);
    });
    AddStatements(Statement->Body, Depth + 1);
    AddStatements(Statement->ElseBody, Depth + 1);
    for (const auto &Case : Statement->Cases)
      AddStatements(Case.Body, Depth + 1);
    AddStatements(Statement->DefaultBody, Depth + 1);
    for (const auto &Clause : Statement->EHClauseBodies)
      AddStatements(Clause, Depth + 1);
  }
  if (Hint.ReturnType->Kind != NdTypeKind::Void && !Func.DoesNotReturn &&
      !HasValueReturn)
    return "non-void method has no recovered return value";

  std::map<const HighExpr *, unsigned> SeenDepth;
  while (!Expressions.empty()) {
    const auto [Expression, Depth] = Expressions.back();
    Expressions.pop_back();
    if (Depth > 200)
      return "method expression exceeds the source projection depth limit";
    auto [Position, Inserted] = SeenDepth.emplace(Expression, Depth);
    if (!Inserted && Position->second >= Depth)
      continue;
    Position->second = Depth;
    if (Expression->Kind == ExprKind::Undef)
      return "method contains an unresolved value";
    if (Expression->Kind == ExprKind::Call &&
        (Expression->IntrinsicId == Intrinsic::None ||
         !intrinsicCName(Expression->IntrinsicId) ||
         Expression->IsIndirectCall ||
         Expression->MemoryAddressSpace != NdMemoryAddressSpace::Default))
      return "method calls a native or dynamic target without a source binding";
    if (Expression->Kind == ExprKind::Var ||
        Expression->Kind == ExprKind::Phi) {
      const MedVar &Variable = Expression->Var;
      if (Variable.Kind == MedVar::Param) {
        if (Variable.Id < 0 ||
            static_cast<size_t>(Variable.Id) >= Hint.Parameters.size() ||
            Variable.RenameTag >= 0 ||
            (Variable.TheArch != Arch::X64 &&
             Variable.TheArch != Arch::AArch64))
          return "method references an unbound source parameter";
        const auto &Registers = getTargetRegInfo(Variable.TheArch).IntParamRegs;
        if (static_cast<size_t>(Variable.Id) >= Registers.size() ||
            Registers[Variable.Id] != Variable.RegOff)
          return "method source parameter occupies the wrong ABI position";
      } else if ((Variable.Kind == MedVar::Reg ||
                  Variable.Kind == MedVar::Flag) &&
                 Variable.SSAVer == 0 && Variable.RenameTag < 0) {
        const bool FramePointer =
            Variable.Kind == MedVar::Reg &&
            (Variable.TheArch == Arch::X64 ||
             Variable.TheArch == Arch::AArch64) &&
            (Func.FrameSize > 0 || Func.FrameHeadroom > 0) &&
            Variable.RegOff == getTargetRegInfo(Variable.TheArch).StackPointer;
        if (!FramePointer)
          return "method contains an unexplained incoming register value";
      } else if (Variable.Kind == MedVar::EHException ||
                 Variable.Kind == MedVar::EHSelector) {
        return "exception-dependent method projection is not supported";
      } else if (!DefinedLocals.count(localIdentity(Variable))) {
        // The MedIR audit predates HighIR cleanup. It cannot establish that
        // every local survived lowering with a definition. This is a minimal
        // source completeness check, not an all-path definite-assignment proof.
        return "method reads a local value without a recovered definition";
      }
    }
    for (const auto &Operand : Expression->Operands) {
      if (!Operand)
        return "method contains a missing expression operand";
      Expressions.emplace_back(Operand.get(), Depth + 1);
    }
  }
  return {};
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
