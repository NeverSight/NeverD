//===- HighIRPrint.cpp - HighIR expression and statement printing ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// String representation for HighExpr and HighStmt nodes.  Separated from
/// the conversion logic in MedToHigh.cpp to keep the display layer thin
/// and independent.
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// HighExpr::str
//===----------------------------------------------------------------------===//

static const char *binopSymbol(NdOp Op) {
  switch (Op) {
  case NdOp::INT_ADD:
    return "+";
  case NdOp::INT_SUB:
    return "-";
  case NdOp::INT_MULT:
    return "*";
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
    return "/";
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
    return "%";
  case NdOp::INT_AND:
    return "&";
  case NdOp::INT_OR:
    return "|";
  case NdOp::INT_XOR:
    return "^";
  case NdOp::INT_LEFT:
    return "<<";
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
    return ">>";
  case NdOp::INT_EQUAL:
    return "==";
  case NdOp::INT_NOTEQUAL:
    return "!=";
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
    return "<";
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
    return "<=";
  case NdOp::BOOL_AND:
    return "&&";
  case NdOp::BOOL_OR:
    return "||";
  case NdOp::BOOL_XOR:
    return "^^";
  default:
    return "?Op?";
  }
}

static const char *memoryAddressSpaceQualifier(
    NdMemoryAddressSpace AddressSpace) {
  switch (AddressSpace) {
  case NdMemoryAddressSpace::Default:
    return "";
  case NdMemoryAddressSpace::X86FS:
    return "fs:";
  case NdMemoryAddressSpace::X86GS:
    return "gs:";
  }
  return "unknown-as:";
}

std::string HighExpr::str() const {
  switch (Kind) {
  case ExprKind::Var:
    return Var.display();
  case ExprKind::Const: {
    if (ConstVal <= limits::kDecimalConstThreshold)
      return std::to_string(ConstVal);
    if (ConstVal == 0xFFFFFFFF || ConstVal == 0xFFFFFFFFFFFFFFFFULL)
      return "-1";
    int64_t SV = static_cast<int64_t>(ConstVal);
    if (SV < 0 && SV >= -static_cast<int64_t>(limits::kDecimalConstThreshold))
      return "-" + std::to_string(static_cast<uint64_t>(-SV));
    int32_t SV32 = static_cast<int32_t>(ConstVal & 0xFFFFFFFF);
    if (ConstVal <= 0xFFFFFFFF && SV32 < 0 &&
        SV32 >= -static_cast<int32_t>(limits::kDecimalConstThreshold))
      return "-" + std::to_string(-SV32);
    return "0x" + llvm::utohexstr(ConstVal);
  }
  case ExprKind::Undef:
    return "undef";
  case ExprKind::BinOp:
    if (Op == NdOp::SELECT && Operands.size() == 3)
      return "(" + Operands[0]->str() + " ? " + Operands[1]->str() + " : " +
             Operands[2]->str() + ")";
    if (Operands.size() == 2)
      return "(" + Operands[0]->str() + " " + binopSymbol(Op) + " " +
             Operands[1]->str() + ")";
    return "?binop?";
  case ExprKind::UnaryOp:
    if (!Operands.empty()) {
      if (Op == NdOp::INT_NOT || Op == NdOp::INT_NEGATE)
        return "~" + Operands[0]->str();
      if (Op == NdOp::INT_NEG2)
        return "-" + Operands[0]->str();
      if (Op == NdOp::BOOL_NOT)
        return "!" + Operands[0]->str();
      if (Op == NdOp::INT_ZEXT || Op == NdOp::INT_SEXT)
        return "(" + (Type ? Type->str() : "?") + ")" + Operands[0]->str();
    }
    return "?unary?";
  case ExprKind::Load:
    if (!Operands.empty())
      return std::string(memoryAddressSpaceQualifier(MemoryAddressSpace)) +
             "*" + Operands[0]->str();
    return std::string(memoryAddressSpaceQualifier(MemoryAddressSpace)) + "*?";
  case ExprKind::Call: {
    std::string S = CallTarget.empty()
                        ? (kAutoFuncPrefix + llvm::utohexstr(CallAddr)).str()
                        : CallTarget;
    S += "(";
    for (size_t I = 0; I < Operands.size(); ++I) {
      if (I > 0)
        S += ", ";
      S += Operands[I]->str();
    }
    S += ")";
    return S;
  }
  case ExprKind::Cast:
    if (!Operands.empty() && CastTo)
      return "(" + CastTo->str() + ")" + Operands[0]->str();
    return "?cast?";
  default:
    return "?";
  }
}

//===----------------------------------------------------------------------===//
// HighExpr::structuralEq
//===----------------------------------------------------------------------===//

bool HighExpr::structuralEq(const HighExpr &Other) const {
  if (Kind != Other.Kind)
    return false;
  if (Op != Other.Op)
    return false;
  if (MemoryOrdering != Other.MemoryOrdering)
    return false;
  if (MemoryAddressSpace != Other.MemoryAddressSpace)
    return false;
  switch (Kind) {
  case ExprKind::Var:
    return Var == Other.Var;
  case ExprKind::Const:
    return ConstVal == Other.ConstVal;
  case ExprKind::Undef:
    return (!Type && !Other.Type) ||
           (Type && Other.Type && Type->Size == Other.Type->Size);
  case ExprKind::Call:
    if (CallTarget != Other.CallTarget || CallAddr != Other.CallAddr)
      return false;
    break;
  case ExprKind::Cast:
    if ((CastTo == nullptr) != (Other.CastTo == nullptr))
      return false;
    if (CastTo && Other.CastTo && CastTo->str() != Other.CastTo->str())
      return false;
    break;
  default:
    break;
  }
  if (Operands.size() != Other.Operands.size())
    return false;
  for (size_t I = 0; I < Operands.size(); ++I) {
    if (!Operands[I] || !Other.Operands[I]) {
      if (Operands[I] != Other.Operands[I])
        return false;
      continue;
    }
    if (!Operands[I]->structuralEq(*Other.Operands[I]))
      return false;
  }
  return true;
}

//===----------------------------------------------------------------------===//
// HighStmt::str
//===----------------------------------------------------------------------===//

std::string HighStmt::str(int Indent) const {
  std::string Pad(Indent * 2, ' ');
  switch (Kind) {
  case StmtKind::Assign:
    return Pad + (Dst ? Dst->str() : "?") + " = " + (Val ? Val->str() : "?") +
           ";";
  case StmtKind::Store:
    return Pad + memoryAddressSpaceQualifier(MemoryAddressSpace) + "*" +
           (StoreAddr ? StoreAddr->str() : "?") + " = " +
           (StoreVal ? StoreVal->str() : "?") + ";";
  case StmtKind::Return:
    return Pad + "return " + (RetVal ? RetVal->str() : "") + ";";
  case StmtKind::Call:
    return Pad + (CallExpr ? CallExpr->str() : "?()") + ";";
  case StmtKind::If: {
    std::string S = Pad + "if (" + (Cond ? Cond->str() : "?") + ") {{\n";
    for (auto &ST : Body)
      S += ST.str(Indent + 1) + "\n";
    S += Pad + "}";
    return S;
  }
  case StmtKind::IfElse: {
    std::string S = Pad + "if (" + (Cond ? Cond->str() : "?") + ") {{\n";
    for (auto &ST : Body)
      S += ST.str(Indent + 1) + "\n";
    S += Pad + "} else {\n";
    for (auto &ST : ElseBody)
      S += ST.str(Indent + 1) + "\n";
    S += Pad + "}";
    return S;
  }
  case StmtKind::While: {
    std::string S = Pad + "while (" + (Cond ? Cond->str() : "?") + ") {{\n";
    for (auto &ST : Body)
      S += ST.str(Indent + 1) + "\n";
    S += Pad + "}";
    return S;
  }
  case StmtKind::Goto:
    return Pad + "goto 0x" + llvm::utohexstr(GotoTarget) + ";";
  case StmtKind::Switch: {
    std::string S =
        Pad + "switch (" + (SwitchExpr ? SwitchExpr->str() : "?") + ") {{\n";
    for (auto &C : Cases) {
      S += Pad + "  case " + std::to_string(C.Value) + ":\n";
      for (auto &ST : C.Body)
        S += ST.str(Indent + 2) + "\n";
    }
    if (!DefaultBody.empty()) {
      S += Pad + "  default:\n";
      for (auto &ST : DefaultBody)
        S += ST.str(Indent + 2) + "\n";
    }
    S += Pad + "}";
    return S;
  }
  case StmtKind::Break:
    return Pad + "break;";
  case StmtKind::Continue:
    return Pad + "continue;";
  case StmtKind::SEHTry:
  case StmtKind::CxxTry:
  case StmtKind::ItaniumTry: {
    const char *Opener = Kind == StmtKind::SEHTry   ? "__try"
                         : Kind == StmtKind::CxxTry ? "cxx.try"
                                                    : "itanium.try";
    std::string S = Pad + Opener + " [0x" + llvm::utohexstr(EHRange.Begin) +
                    ", 0x" + llvm::utohexstr(EHRange.End) + ") {\n";
    for (const HighStmt &ST : Body)
      S += ST.str(Indent + 1) + "\n";
    S += Pad + "}";
    for (const HighEHClause &Clause : EHClauses) {
      S += "\n" + Pad + "handler 0x" +
           llvm::utohexstr(Clause.HandlerVA ? Clause.HandlerVA
                                            : Clause.FilterOrActionVA) +
           ";";
      if (Clause.Kind == HighEHClauseKind::CxxCleanup)
        S += " state=" + std::to_string(Clause.State) +
             " kind=" + getCxxUnwindActionKindName(Clause.UnwindActionKind) +
             " object_offset=" + std::to_string(Clause.UnwindObjectOffset) +
             ";";
      if (Clause.Kind == HighEHClauseKind::ItaniumCatch)
        S += " catch " +
             (Clause.TypeName.empty()
                  ? (Clause.TypeDescriptorVA
                         ? "typeinfo@0x" +
                               llvm::utohexstr(Clause.TypeDescriptorVA)
                         : std::string("..."))
                  : Clause.TypeName) +
             " filter=" + std::to_string(Clause.TypeFilter) + ";";
      if (Clause.Kind == HighEHClauseKind::ItaniumSpec) {
        S += " spec(";
        for (size_t I = 0; I < Clause.SpecTypeNames.size(); ++I)
          S += (I ? ", " : "") + Clause.SpecTypeNames[I];
        S += ") filter=" + std::to_string(Clause.TypeFilter) + ";";
      }
      for (size_t I = 1; I < Clause.LandingPadVAs.size(); ++I)
        S += " pad 0x" + llvm::utohexstr(Clause.LandingPadVAs[I]) + ";";
    }
    return S;
  }
  case StmtKind::Nop:
    return "";
  default:
    return Pad + "/* ? */";
  }
}

} // namespace neverd
