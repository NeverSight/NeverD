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

#include "llvm/Support/ErrorHandling.h"

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
  case NdOp::BOOL_XOR:
    return 4;
  case NdOp::INT_AND:
    return 5;
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::FLOAT_EQUAL:
  case NdOp::FLOAT_NOTEQUAL:
    return 6;
  case NdOp::INT_LESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESS:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::FLOAT_LESS:
  case NdOp::FLOAT_LESSEQUAL:
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
  auto IsZeroLike = [this](const ExprPtr &Op) {
    if (!Op)
      return false;
    const HighExpr *Cur = unwrapIntegerView(Op.get());
    return Cur && (Cur->Kind == ExprKind::Undef ||
                   (Cur->Kind == ExprKind::Const && Cur->ConstVal == 0));
  };
  if (E.Operands.size() == 2 &&
      (E.Op == NdOp::INT_ADD || E.Op == NdOp::INT_OR ||
       E.Op == NdOp::INT_XOR)) {
    if (IsZeroLike(E.Operands[0]) && E.Operands[1])
      return exprStr(*E.Operands[1], ParentPrec);
    if (IsZeroLike(E.Operands[1]) && E.Operands[0])
      return exprStr(*E.Operands[0], ParentPrec);
  }
  if (E.Op == NdOp::INT_SUB && E.Operands.size() == 2 &&
      IsZeroLike(E.Operands[1]) && E.Operands[0])
    return exprStr(*E.Operands[0], ParentPrec);
  if ((E.Op == NdOp::FLOAT_MINNUM || E.Op == NdOp::FLOAT_MAXNUM) &&
      E.Operands.size() == 2 && E.Operands[0] && E.Operands[1]) {
    const bool IsFloat = E.Type && E.Type->Size == 4;
    const char *Name = E.Op == NdOp::FLOAT_MINNUM
                           ? (IsFloat ? "__builtin_fminf" : "__builtin_fmin")
                           : (IsFloat ? "__builtin_fmaxf" : "__builtin_fmax");
    return std::string(Name) + "(" + exprStr(*E.Operands[0]) + ", " +
           exprStr(*E.Operands[1]) + ")";
  }
  // MSVC `sbb r, r` after a compare is `0 - (CF - 0)` / `0 - (undef - cond)`.
  // Fold to `cond` so the printed body is not `0 /* unknown */`.
  if (E.Op == NdOp::INT_SUB && E.Operands.size() == 2 &&
      IsZeroLike(E.Operands[0]) && E.Operands[1]) {
    const HighExpr *Inner = unwrapIntegerView(E.Operands[1].get());
    if (Inner && Inner->Kind == ExprKind::BinOp && Inner->Op == NdOp::INT_SUB &&
        Inner->Operands.size() == 2 && IsZeroLike(Inner->Operands[0]) &&
        Inner->Operands[1])
      return exprStr(*Inner->Operands[1], ParentPrec);
  }

  // `(x << n) | (x >> (width-n))` is a rotate.  Print a rotate builtin
  // instead of the nested unsigned-shift casts Hex-Rays beats with `__ROL8__`.
  if (E.Op == NdOp::INT_OR && E.Operands.size() == 2 && E.Operands[0] &&
      E.Operands[1]) {
    auto ShiftCount = [this](const HighExpr *Op, NdOp Kind, uint64_t &Count,
                             const HighExpr *&Src) -> bool {
      Op = unwrapIntegerView(Op);
      if (!Op || Op->Kind != ExprKind::BinOp || Op->Op != Kind ||
          Op->Operands.size() != 2 || !Op->Operands[0] || !Op->Operands[1] ||
          Op->Operands[1]->Kind != ExprKind::Const)
        return false;
      Count = Op->Operands[1]->ConstVal;
      Src = unwrapIntegerView(Op->Operands[0].get());
      return Src != nullptr;
    };
    const HighExpr *LeftSrc = nullptr;
    const HighExpr *RightSrc = nullptr;
    uint64_t LeftCount = 0;
    uint64_t RightCount = 0;
    const bool LeftIsShl =
        ShiftCount(E.Operands[0].get(), NdOp::INT_LEFT, LeftCount, LeftSrc) &&
        ShiftCount(E.Operands[1].get(), NdOp::INT_RIGHT, RightCount, RightSrc);
    const bool LeftIsShr =
        !LeftIsShl &&
        ShiftCount(E.Operands[0].get(), NdOp::INT_RIGHT, RightCount,
                   RightSrc) &&
        ShiftCount(E.Operands[1].get(), NdOp::INT_LEFT, LeftCount, LeftSrc);
    if ((LeftIsShl || LeftIsShr) && LeftSrc && RightSrc &&
        exprStr(*LeftSrc) == exprStr(*RightSrc)) {
      const uint16_t Size = E.Type && E.Type->Kind == NdTypeKind::Int
                                ? E.Type->Size
                                : (LeftSrc->Type ? LeftSrc->Type->Size : 0);
      const uint64_t Bits = static_cast<uint64_t>(Size) * 8u;
      if (Size == 1 || Size == 2 || Size == 4 || Size == 8) {
        if (LeftCount < Bits && RightCount < Bits &&
            LeftCount + RightCount == Bits) {
          return "__builtin_rotateleft" + std::to_string(Bits) + "(" +
                 exprStr(*LeftSrc) + ", " + std::to_string(LeftCount) + ")";
        }
      }
    }
  }

  if (E.Op == NdOp::FLOAT_FMA) {
    if (E.Operands.size() != 3 || !E.Type ||
        E.Type->Kind != NdTypeKind::Float ||
        (E.Type->Size != 4 && E.Type->Size != 8))
      llvm::report_fatal_error(
          "HighC cannot render an invalid fused multiply-add");
    std::string Result =
        E.Type->Size == 4 ? "__builtin_fmaf(" : "__builtin_fma(";
    for (unsigned I = 0; I < 3; ++I) {
      const auto &Operand = E.Operands[I];
      if (!Operand || !Operand->Type ||
          Operand->Type->Kind != NdTypeKind::Float ||
          Operand->Type->Size != E.Type->Size)
        llvm::report_fatal_error(
            "HighC fused multiply-add has incompatible operands");
      if (I)
        Result += ", ";
      Result += "(" + typeToC(E.Type) + ")(" + exprStr(*Operand) + ")";
    }
    return Result + ")";
  }
  if (E.Op == NdOp::SELECT && E.Operands.size() == 3) {
    return "(" + exprStr(*E.Operands[0]) + " ? " + exprStr(*E.Operands[1]) +
           " : " + exprStr(*E.Operands[2]) + ")";
  }
  if (E.Op == NdOp::ATOMIC_CMPXCHG && E.Operands.size() == 3)
    return atomicCompareExchangeExpr(
        E.Type, exprStr(*E.Operands[0]), exprStr(*E.Operands[1]),
        exprStr(*E.Operands[2]), E.MemoryOrdering, E.MemoryAddressSpace);
  if (E.Operands.size() != 2)
    return "/* bad binop */";

  if (E.Op == NdOp::ATOMIC_XCHG)
    return atomicExchangeExpr(E.Type, exprStr(*E.Operands[0]),
                              exprStr(*E.Operands[1]), E.MemoryOrdering,
                              E.MemoryAddressSpace);
  if (E.Op == NdOp::ATOMIC_ADD)
    return atomicFetchAddExpr(E.Type, exprStr(*E.Operands[0]),
                              exprStr(*E.Operands[1]), E.MemoryOrdering,
                              E.MemoryAddressSpace);

  if ((E.Op == NdOp::INT_ADD || E.Op == NdOp::INT_SUB ||
       E.Op == NdOp::INT_MULT) &&
      E.Type && E.Type->Kind == NdTypeKind::Int) {
    const uint16_t Size = E.Type->Size;
    const auto IntegerWidth = [](uint16_t Width) {
      return Width >= 1 && Width <= 16;
    };
    if (IntegerWidth(Size) && E.Operands[0]->Type && E.Operands[1]->Type &&
        E.Operands[0]->Type->Kind == NdTypeKind::Int &&
        E.Operands[1]->Type->Kind == NdTypeKind::Int &&
        IntegerWidth(E.Operands[0]->Type->Size) &&
        IntegerWidth(E.Operands[1]->Type->Size)) {
      const auto Unsigned = typeToC(NdType::makeInt(Size, false));
      // Machine arithmetic wraps at its result width. C's signed overflow
      // and integer promotions do not: even uint16_t multiplication can
      // overflow promoted int. Compute in an unsigned carrier, truncate,
      // then restore the signed interpretation without a numeric conversion.
      const auto Carrier = typeToC(NdType::makeInt(Size < 4 ? 4 : Size, false));
      auto Operand = [&](const ExprPtr &Value) {
        const auto Bits = typeToC(NdType::makeInt(Value->Type->Size, false));
        return (Carrier == Bits ? "" : "(" + Carrier + ")") + "(" + Bits +
               ")(" + exprStr(*Value) + ")";
      };
      const char *Symbol = E.Op == NdOp::INT_ADD   ? " + "
                           : E.Op == NdOp::INT_SUB ? " - "
                                                   : " * ";
      const auto Value = "(" + Unsigned + ")(" + Operand(E.Operands[0]) +
                         Symbol + Operand(E.Operands[1]) + ")";
      return E.Type->IsSigned ? "(" + typeToC(E.Type) + ")" + Value
                              : "(" + Value + ")";
    }
  }

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
        Raw->Type->Kind == NdTypeKind::Int && Raw->Type->Size == 8 &&
        Converted->Operands[0]->Type &&
        Converted->Operands[0]->Type->Size == 4) {
      return "__builtin_bit_cast(double, (uint64_t)(" + exprStr(*Raw) +
             ")) == (double)(int32_t)(" + exprStr(*Converted->Operands[0]) +
             ")";
    }
  }

  switch (E.Op) {
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR: {
    const uint16_t Size = E.Operands[0]->Type ? E.Operands[0]->Type->Size : 0;
    if (Size == 0 || Size > 16)
      break;
    const bool Arithmetic = E.Op == NdOp::INT_ASHR;
    const auto SourceType = typeToC(NdType::makeInt(Size, Arithmetic));
    const auto ResultType =
        typeToC(E.Type ? E.Type : NdType::makeInt(Size, false));
    const auto RestoreType = [&](const std::string &Value) {
      return "(" + ResultType + ")(" + Value + ")";
    };
    const auto Left = "(" + SourceType + ")(" + exprStr(*E.Operands[0]) + ")";
    const auto Limit = std::to_string(Size * 8u);
    // The opcode determines sign extension independently of inferred types.
    // Preserve the count's own width, then guard C's undefined overshifts.
    const auto Fallback =
        Arithmetic ? "((" + Left + ") >> " + std::to_string(Size * 8u - 1) + ")"
                   : "0";
    if (E.Operands[1]->Kind == ExprKind::Const) {
      const uint64_t Count = E.Operands[1]->ConstVal;
      return RestoreType(Count < Size * 8u ? "((" + Left + ") >> " +
                                                 std::to_string(Count) + ")"
                                           : Fallback);
    }
    const auto CountType = typeToC(NdType::makeInt(
        E.Operands[1]->Type ? E.Operands[1]->Type->Size : 8, false));
    const auto Right = "(" + CountType + ")(" + exprStr(*E.Operands[1]) + ")";
    return RestoreType("((" + Right + ") < " + Limit + " ? ((" + Left +
                       ") >> (" + Right + ")) : " + Fallback + ")");
  }
  case NdOp::INT_LEFT: {
    const uint16_t Size = E.Type ? E.Type->Size : 0;
    if (Size == 0 || Size > 16)
      break;
    const uint16_t SourceSize =
        E.Operands[0]->Type ? E.Operands[0]->Type->Size : Size;
    const auto ResultType = typeToC(NdType::makeInt(Size, false));
    const auto SourceType = typeToC(NdType::makeInt(SourceSize, false));
    // uint8_t/uint16_t still undergo integer promotion in C. Shift in at
    // least unsigned int, then restore the operation's exact result width.
    const auto CarrierType =
        typeToC(NdType::makeInt(Size < 4 ? 4 : Size, false));
    const std::string Left = exprStr(*E.Operands[0]);
    const std::string Right = exprStr(*E.Operands[1]);
    const auto Shift = "(" + ResultType + ")((" + CarrierType + ")(" +
                       SourceType + ")(" + Left + ") << (" + Right + "))";
    if (E.Operands[1]->Kind == ExprKind::Const)
      return E.Operands[1]->ConstVal < Size * 8u ? Shift : "0";
    const auto CountType = typeToC(NdType::makeInt(
        E.Operands[1]->Type ? E.Operands[1]->Type->Size : 8, false));
    return "((" + CountType + ")(" + Right + ") < " +
           std::to_string(Size * 8u) + " ? " + Shift + " : 0)";
  }
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
    // Concatenation shifts bit patterns, including a high half whose sign bit
    // is set. Use an unsigned result-width carrier before the left shift.
    std::string Ty = typeToC(NdType::makeInt(E.Type ? E.Type->Size : 8, false));
    int LoBits = E.Operands[1]->Type ? E.Operands[1]->Type->Size * 8 : 32;
    auto unsignedOperandType = [](const ExprPtr &Operand) {
      uint16_t Size = Operand->Type ? Operand->Type->Size : 4;
      return typeToC(NdType::makeInt(Size, false));
    };
    std::string HiTy = unsignedOperandType(E.Operands[0]);
    std::string LoTy = unsignedOperandType(E.Operands[1]);
    return "((" + Ty + ")((" + HiTy + ")(" + Hi + ")) << " +
           std::to_string(LoBits) + " | (" + Ty + ")((" + LoTy + ")(" + Lo +
           ")))";
  }
  case NdOp::INT_CARRY: {
    const uint16_t Size = E.Operands[0]->Type ? E.Operands[0]->Type->Size : 8;
    const auto OperandType = typeToC(NdType::makeInt(Size, false));
    const auto CarrierType =
        typeToC(NdType::makeInt(Size < 4 ? 4 : Size, false));
    const auto Left = "(" + OperandType + ")(" + exprStr(*E.Operands[0]) + ")";
    const auto Right = "(" + OperandType + ")(" + exprStr(*E.Operands[1]) + ")";
    // Carry uses unsigned bit patterns and the operand width, not the boolean
    // result width. Restore that width after C promotes narrow operands.
    return "((" + OperandType + ")((" + CarrierType + ")(" + Left + ") + (" +
           CarrierType + ")(" + Right + ")) < (" + Left + "))";
  }
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR: {
    // Clang supports the pointer-result builtins, but not GCC's *_overflow_p.
    // Signed overflow is defined by the operand bits even when HighIR carries
    // them through unsigned locals. Bitcast both inputs at the original width;
    // a compound literal receives the unused arithmetic result.
    const auto Size = E.Operands[0]->Type->Size;
    const auto SignedType = typeToC(NdType::makeInt(Size, true));
    const auto UnsignedType = typeToC(NdType::makeInt(Size, false));
    const auto SignedBits = [&](const HighExpr &Operand) {
      return "__builtin_bit_cast(" + SignedType + ", (" + UnsignedType +
             ")(" + exprStr(Operand) + "))";
    };
    return std::string(E.Op == NdOp::INT_SOVF ? "__builtin_add_overflow("
                                              : "__builtin_sub_overflow(") +
           SignedBits(*E.Operands[0]) + ", " + SignedBits(*E.Operands[1]) +
           ", &(" + SignedType + "){0})";
  }
  default:
    break;
  }

  const char *OpSym = nullptr;
  bool NeedsUnsignedCast = false;
  bool NeedsSignedCast = false;
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
    NeedsSignedCast = true;
    OpSym = " / ";
    break;
  case NdOp::INT_REM:
    OpSym = " % ";
    NeedsUnsignedCast = true;
    break;
  case NdOp::INT_SREM:
    NeedsSignedCast = true;
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
    NeedsUnsignedCast = true;
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
    NeedsSignedCast = true;
    OpSym = " < ";
    break;
  case NdOp::INT_SLESSEQUAL:
    NeedsSignedCast = true;
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

  if ((NeedsUnsignedCast || NeedsSignedCast) && E.Operands[0]->Type) {
    uint16_t Size = E.Operands[0]->Type->Size;
    // A comparison may be inverted by swapping its operands. A narrow
    // immediate on the left must not truncate the wider machine value.
    const bool Comparison =
        E.Op == NdOp::INT_LESS || E.Op == NdOp::INT_LESSEQUAL ||
        E.Op == NdOp::INT_SLESS || E.Op == NdOp::INT_SLESSEQUAL;
    if (Comparison && E.Operands[1]->Type)
      Size = std::max(Size, E.Operands[1]->Type->Size);
    auto UTy = typeToC(NdType::makeInt(Size, NeedsSignedCast));
    auto CastOperand = [&](const ExprPtr &Operand) {
      std::string Cast = "(" + UTy + ")";
      // Mixed-width machine operands are zero-extended to their common
      // width before applying the signed/unsigned comparison, as in MedLLVM.
      if (Comparison && Operand->Type && Operand->Type->Size < Size)
        Cast +=
            "(" + typeToC(NdType::makeInt(Operand->Type->Size, false)) + ")";
      return Cast + exprStr(*Operand, 99);
    };
    LHS = CastOperand(E.Operands[0]);
    RHS = CastOperand(E.Operands[1]);
  }

  std::string Result = LHS + OpSym + RHS;
  if (MyPrec > 0 && MyPrec <= ParentPrec)
    Result = "(" + Result + ")";
  return Result;
}

} // namespace neverd
