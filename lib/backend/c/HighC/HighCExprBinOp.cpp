//===- HighCExprBinOp.cpp - HighIR binary expression rendering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Binary operator rendering and precedence handling for the HighIR C emitter.
///
//===----------------------------------------------------------------------===//

#include "HighCWriter.h"

namespace neverd {

namespace {

int getOpPrecedence(NdOp Op) {
  switch (Op) {
  case NdOp::BOOL_OR:
    return 1;
  case NdOp::BOOL_AND:
    return 2;
  case NdOp::INT_OR:
    return 3;
  case NdOp::INT_XOR:
    return 4;
  case NdOp::INT_AND:
    return 5;
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
    return 6;
  case NdOp::INT_LESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESS:
  case NdOp::INT_SLESSEQUAL:
    return 7;
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
    return 8;
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::FLOAT_ADD:
  case NdOp::FLOAT_SUB:
    return 9;
  case NdOp::INT_MULT:
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
  case NdOp::FLOAT_MULT:
  case NdOp::FLOAT_DIV:
    return 10;
  default:
    return 0;
  }
}

} // anonymous namespace

std::string HighCWriter::renderBinOp(const HighExpr &E, int ParentPrec) {
  if (E.Op == NdOp::SELECT && E.Operands.size() == 3) {
    return "(" + exprStr(*E.Operands[0]) + " ? " + exprStr(*E.Operands[1]) +
           " : " + exprStr(*E.Operands[2]) + ")";
  }
  if (E.Operands.size() != 2)
    return "/* bad binop */";

  if (E.Op == NdOp::ATOMIC_XCHG)
    return atomicExchangeExpr(E.Type, exprStr(*E.Operands[0]),
                              exprStr(*E.Operands[1]), E.MemoryOrdering);
  if (E.Op == NdOp::ATOMIC_ADD)
    return atomicFetchAddExpr(E.Type, exprStr(*E.Operands[0]),
                              exprStr(*E.Operands[1]), E.MemoryOrdering);

  // FJCVTZS exactness compares the original double bit pattern with the
  // signed i32 result converted back to double.  High-C otherwise represents
  // register values as integers, so preserve the bit-cast on the FP operand
  // instead of rendering this particular FLOAT_EQUAL as an integer compare.
  if (E.Op == NdOp::FLOAT_EQUAL) {
    const HighExpr *Converted = nullptr;
    const HighExpr *Raw = nullptr;
    if (E.Operands[0]->Kind == ExprKind::UnaryOp &&
        E.Operands[0]->Op == NdOp::FLOAT_INT2FLOAT) {
      Converted = E.Operands[0].get();
      Raw = E.Operands[1].get();
    } else if (E.Operands[1]->Kind == ExprKind::UnaryOp &&
               E.Operands[1]->Op == NdOp::FLOAT_INT2FLOAT) {
      Converted = E.Operands[1].get();
      Raw = E.Operands[0].get();
    }
    if (Converted && Raw && Converted->Operands.size() == 1 && Raw->Type &&
        Raw->Type->Size == 8 && Converted->Operands[0]->Type &&
        Converted->Operands[0]->Type->Size == 4) {
      return "__builtin_bit_cast(double, (uint64_t)(" + exprStr(*Raw) +
             ")) == (double)(int32_t)(" + exprStr(*Converted->Operands[0]) +
             ")";
    }
  }

  switch (E.Op) {
  case NdOp::SUBBYTES: {
    std::string Src = exprStr(*E.Operands[0], 99);
    uint64_t ByteOff = 0;
    if (E.Operands[1]->Kind == ExprKind::Const)
      ByteOff = E.Operands[1]->ConstVal;
    std::string Ty = typeToC(E.Type);
    if (ByteOff == 0)
      return "(" + Ty + ")" + Src;
    return "(" + Ty + ")(" + Src + " >> " + std::to_string(ByteOff * 8) + ")";
  }
  case NdOp::CONCAT: {
    std::string Hi = exprStr(*E.Operands[0], 99);
    std::string Lo = exprStr(*E.Operands[1], 99);
    std::string Ty = typeToC(E.Type);
    int LoBits = E.Operands[1]->Type ? E.Operands[1]->Type->Size * 8 : 32;
    return "((" + Ty + ")(" + Hi + ") << " + std::to_string(LoBits) + " | (" +
           Ty + ")" + Lo + ")";
  }
  case NdOp::INT_CARRY:
    return "((" + exprStr(*E.Operands[0]) + " + " + exprStr(*E.Operands[1]) +
           ") < " + exprStr(*E.Operands[0]) + ")";
  case NdOp::INT_SOVF:
    return "__builtin_add_overflow_p(" + exprStr(*E.Operands[0]) + ", " +
           exprStr(*E.Operands[1]) + ", (" + typeToC(E.Operands[0]->Type) +
           ")0)";
  case NdOp::INT_SBOR:
    return "__builtin_sub_overflow_p(" + exprStr(*E.Operands[0]) + ", " +
           exprStr(*E.Operands[1]) + ", (" + typeToC(E.Operands[0]->Type) +
           ")0)";
  default:
    break;
  }

  const char *OpSym = nullptr;
  bool NeedsUnsignedCast = false;
  switch (E.Op) {
  case NdOp::INT_ADD:
    OpSym = " + ";
    break;
  case NdOp::INT_SUB:
    OpSym = " - ";
    break;
  case NdOp::INT_MULT:
    OpSym = " * ";
    break;
  case NdOp::INT_DIV:
    OpSym = " / ";
    NeedsUnsignedCast = true;
    break;
  case NdOp::INT_SDIV:
    OpSym = " / ";
    break;
  case NdOp::INT_REM:
    OpSym = " % ";
    NeedsUnsignedCast = true;
    break;
  case NdOp::INT_SREM:
    OpSym = " % ";
    break;
  case NdOp::INT_AND:
    OpSym = " & ";
    break;
  case NdOp::INT_OR:
    OpSym = " | ";
    break;
  case NdOp::INT_XOR:
    OpSym = " ^ ";
    break;
  case NdOp::INT_LEFT:
    OpSym = " << ";
    break;
  case NdOp::INT_RIGHT:
    OpSym = " >> ";
    NeedsUnsignedCast = true;
    break;
  case NdOp::INT_ASHR:
    OpSym = " >> ";
    break;
  case NdOp::INT_EQUAL:
    OpSym = " == ";
    break;
  case NdOp::INT_NOTEQUAL:
    OpSym = " != ";
    break;
  case NdOp::INT_LESS:
    OpSym = " < ";
    NeedsUnsignedCast = true;
    break;
  case NdOp::INT_LESSEQUAL:
    OpSym = " <= ";
    NeedsUnsignedCast = true;
    break;
  case NdOp::INT_SLESS:
    OpSym = " < ";
    break;
  case NdOp::INT_SLESSEQUAL:
    OpSym = " <= ";
    break;
  case NdOp::BOOL_AND:
    OpSym = " && ";
    break;
  case NdOp::BOOL_OR:
    OpSym = " || ";
    break;
  case NdOp::BOOL_XOR:
    OpSym = " ^ ";
    break;
  case NdOp::FLOAT_ADD:
    OpSym = " + ";
    break;
  case NdOp::FLOAT_SUB:
    OpSym = " - ";
    break;
  case NdOp::FLOAT_MULT:
    OpSym = " * ";
    break;
  case NdOp::FLOAT_DIV:
    OpSym = " / ";
    break;
  case NdOp::FLOAT_EQUAL:
    OpSym = " == ";
    break;
  case NdOp::FLOAT_NOTEQUAL:
    OpSym = " != ";
    break;
  case NdOp::FLOAT_LESS:
    OpSym = " < ";
    break;
  case NdOp::FLOAT_LESSEQUAL:
    OpSym = " <= ";
    break;
  default:
    return "/* unknown_op(" + std::to_string(static_cast<int>(E.Op)) +
           ") */ (" + exprStr(*E.Operands[0]) + " ?? " +
           exprStr(*E.Operands[1]) + ")";
  }

  int MyPrec = getOpPrecedence(E.Op);
  std::string LHS = exprStr(*E.Operands[0], MyPrec);
  std::string RHS = exprStr(*E.Operands[1], MyPrec);

  if (NeedsUnsignedCast && E.Operands[0]->Type) {
    auto UTy = typeToC(NdType::makeInt(E.Operands[0]->Type->Size, false));
    LHS = "(" + UTy + ")" + LHS;
    RHS = "(" + UTy + ")" + RHS;
  }

  std::string Result = LHS + OpSym + RHS;
  if (MyPrec > 0 && MyPrec <= ParentPrec)
    Result = "(" + Result + ")";
  return Result;
}

} // namespace neverd
