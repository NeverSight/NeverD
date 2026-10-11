//===- SourceEvaluate.cpp - Finite primitive evaluation ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Finite primitive evaluation.
///
//===----------------------------------------------------------------------===//

#include "PrimitiveNumbers.h"
#include "SourceModel.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"
#include "neverd/web/SourceValues.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace neverd::web {
namespace {
using llvm::APFloat;
using llvm::APInt;
constexpr auto Rounding = APFloat::rmNearestTiesToEven;
constexpr unsigned BigWidth = 2 * MaxJavaScriptBigIntBits + 2;
constexpr uint32_t None = UINT32_MAX;
struct ValueLimit {};

SourceValue refused(std::string_view Status, std::string_view Reason) {
  return {std::string(Status), std::string(Reason), {}};
}
SourceValue constant(SourcePrimitive V) {
  return {"constant", "",
          std::make_shared<const SourcePrimitive>(std::move(V))};
}
SourceValue boolean(bool B) {
  SourcePrimitive V;
  V.Kind = PrimitiveKind::Boolean;
  V.Boolean = B;
  return constant(std::move(V));
}
APFloat floating(uint64_t Bits) {
  return APFloat(APFloat::IEEEdouble(), APInt(64, Bits));
}
SourceValue number(APFloat F) {
  SourcePrimitive V;
  V.Kind = PrimitiveKind::Number;
  // JS cannot observe a NaN payload. Use one deterministic representation.
  V.NumberBits = F.isNaN() ? UINT64_C(0x7ff8000000000000)
                           : F.bitcastToAPInt().getZExtValue();
  return constant(std::move(V));
}
SourceValue integer32(uint32_t Bits, bool Signed = true) {
  APFloat F(APFloat::IEEEdouble());
  F.convertFromAPInt(APInt(32, Bits), Signed, Rounding);
  return number(std::move(F));
}
uint32_t uint32(uint64_t Bits) {
  const auto Exponent = unsigned((Bits >> 52) & 0x7ff);
  if (Exponent == 0 || Exponent == 0x7ff || Exponent < 1023)
    return 0;
  const int Shift = int(Exponent) - 1023 - 52;
  const uint64_t Mantissa =
      (Bits & UINT64_C(0xfffffffffffff)) | (UINT64_C(1) << 52);
  uint32_t Result = Shift >= 32  ? 0
                    : Shift >= 0 ? uint32_t(Mantissa << Shift)
                                 : uint32_t(Mantissa >> -Shift);
  return Bits >> 63 ? uint32_t(0) - Result : Result;
}
bool nullish(const SourcePrimitive &V) {
  return V.Kind == PrimitiveKind::Null || V.Kind == PrimitiveKind::Undefined;
}
bool whitespace(char16_t C) {
  return C == 0x9 || C == 0xb || C == 0xc || C == 0x20 || C == 0xa0 ||
         C == 0xfeff || C == 0xa || C == 0xd || C == 0x2028 || C == 0x2029 ||
         C == 0x1680 || (C >= 0x2000 && C <= 0x200a) || C == 0x202f ||
         C == 0x205f || C == 0x3000;
}
int digit(char C) {
  if (C >= '0' && C <= '9')
    return C - '0';
  if (C >= 'a' && C <= 'f')
    return C - 'a' + 10;
  if (C >= 'A' && C <= 'F')
    return C - 'A' + 10;
  return -1;
}

class Evaluator {
  const SourceAnalysis &Source;
  SourceValueAnalysis Result;

  void step(uint64_t N = 1) {
    if (N > MaxJavaScriptValueSteps - Result.Steps)
      throw Error("source_value_budget_exceeded");
    Result.Steps += N;
  }
  void stringBudget(uint64_t Units) {
    if (Units > MaxJavaScriptValueStringUnits)
      throw ValueLimit{};
    if (Units > MaxJavaScriptValueAllocatedUnits - Result.AllocatedStringUnits)
      throw Error("source_value_storage_exceeded");
    step(Units + 1);
    Result.AllocatedStringUnits += Units;
  }
  SourceValue string(std::u16string_view Text) {
    stringBudget(Text.size());
    SourcePrimitive V;
    V.Kind = PrimitiveKind::String;
    V.String = Text;
    return constant(std::move(V));
  }
  SourceValue ascii(std::string_view Text) {
    stringBudget(Text.size());
    SourcePrimitive V;
    V.Kind = PrimitiveKind::String;
    V.String.assign(Text.begin(), Text.end());
    return constant(std::move(V));
  }
  SourceValue concat(const SourcePrimitive &A, const SourcePrimitive &B) {
    const auto Length = A.String.size() + B.String.size();
    stringBudget(Length);
    SourcePrimitive V;
    V.Kind = PrimitiveKind::String;
    V.String.reserve(Length);
    V.String.append(A.String);
    V.String.append(B.String);
    return constant(std::move(V));
  }
  uint32_t child(uint32_t I, std::string_view Field) const {
    for (const auto &C : Source.Nodes[I].Children)
      if (C.Field == Field)
        return C.Index;
    return None;
  }
  const SourceValue &value(uint32_t I, std::string_view Field) const {
    const auto C = child(I, Field);
    if (C == None)
      throw Error("invalid_source_model");
    return Result.Nodes[C];
  }
  std::u16string_view text(uint32_t I, std::string_view Field) const {
    const auto *V = Source.Nodes[I].text(Field);
    if (!V)
      throw Error("invalid_source_model");
    return *V;
  }
  static bool known(const SourceValue &V) { return V.Status == "constant"; }
  // A later exception is not guaranteed if an earlier operand was unknown.
  SourceValue operands(const SourceValue &A, const SourceValue &B) {
    return !known(A) ? A : B;
  }
  APInt big(const SourcePrimitive &V) {
    step(V.BigInt.size() + 1);
    auto Digits = llvm::StringRef(V.BigInt);
    const bool Negative = Digits.consume_front("-");
    APInt N;
    if (Digits.getAsInteger(10, N) ||
        N.getActiveBits() > MaxJavaScriptBigIntBits)
      throw Error("invalid_primitive_value");
    N = N.zextOrTrunc(BigWidth);
    return Negative ? -N : N;
  }
  void bigBudget(const APInt &N) {
    step(BigWidth / 8);
    if (N.abs().getActiveBits() > MaxJavaScriptBigIntBits)
      throw ValueLimit{};
  }
  SourceValue bigint(APInt N) {
    bigBudget(N);
    llvm::SmallString<320> Text;
    N.toString(Text, 10, true);
    SourcePrimitive V;
    V.Kind = PrimitiveKind::BigInt;
    V.BigInt = Text.str().str();
    return constant(std::move(V));
  }
  SourceValue bigLiteral(std::u16string_view Text) {
    if (Text.empty() || Text.back() != u'n')
      throw Error("invalid_source_model");
    Text.remove_suffix(1);
    if (Text.size() > 4096)
      throw ValueLimit{};
    step(Text.size() * 16 + 1);
    std::string Digits;
    for (const auto C : Text) {
      if (C > 127)
        throw Error("invalid_source_model");
      if (C != '_')
        Digits.push_back(char(C));
    }
    // The pinned parser preserves the literal spelling, including its n.
    unsigned Radix = 10;
    std::string_view Body = Digits;
    if (Body.size() > 2 && Body[0] == '0') {
      if (Body[1] == 'x' || Body[1] == 'X')
        Radix = 16;
      else if (Body[1] == 'o' || Body[1] == 'O')
        Radix = 8;
      else if (Body[1] == 'b' || Body[1] == 'B')
        Radix = 2;
      if (Radix != 10)
        Body.remove_prefix(2);
    }
    APInt N;
    if (llvm::StringRef(Body).getAsInteger(Radix, N))
      throw Error("invalid_source_model");
    if (N.getActiveBits() > MaxJavaScriptBigIntBits)
      throw ValueLimit{};
    return bigint(N.zextOrTrunc(BigWidth));
  }
  SourceValue toString(const SourceValue &V) {
    switch (V.Value->Kind) {
    case PrimitiveKind::Undefined:
      return ascii("undefined");
    case PrimitiveKind::Null:
      return ascii("null");
    case PrimitiveKind::Boolean:
      return ascii(V.Value->Boolean ? "true" : "false");
    case PrimitiveKind::Number:
      return ascii(primitiveNumberToString(V.Value->NumberBits));
    case PrimitiveKind::String:
      return V;
    case PrimitiveKind::BigInt:
      return ascii(V.Value->BigInt);
    }
    throw Error("invalid_primitive_value");
  }
  SourceValue stringNumber(std::u16string_view Text) {
    step(Text.size() + 1);
    while (!Text.empty() && whitespace(Text.front()))
      Text.remove_prefix(1);
    while (!Text.empty() && whitespace(Text.back()))
      Text.remove_suffix(1);
    if (Text.empty())
      return integer32(0);
    if (Text.size() > 4096)
      throw ValueLimit{};
    step(Text.size() * 16 + 1);
    auto NaN = [] { return number(APFloat::getNaN(APFloat::IEEEdouble())); };
    std::string S;
    for (auto C : Text) {
      if (C > 127)
        return NaN();
      S.push_back(char(C));
    }
    if (S == "Infinity" || S == "+Infinity" || S == "-Infinity")
      return number(APFloat::getInf(APFloat::IEEEdouble(), S[0] == '-'));
    unsigned Radix = 0;
    if (S.size() >= 2 && S[0] == '0') {
      if (S[1] == 'x' || S[1] == 'X')
        Radix = 16;
      if (S[1] == 'b' || S[1] == 'B')
        Radix = 2;
      if (S[1] == 'o' || S[1] == 'O')
        Radix = 8;
    }
    APFloat F(APFloat::IEEEdouble());
    if (Radix) {
      const auto Digits = llvm::StringRef(S).drop_front(2);
      if (Digits.empty() ||
          std::any_of(Digits.begin(), Digits.end(), [&](char C) {
            return digit(C) < 0 || unsigned(digit(C)) >= Radix;
          }))
        return NaN();
      APInt N;
      if (Digits.getAsInteger(Radix, N))
        return NaN();
      F.convertFromAPInt(N, false, Rounding);
    } else {
      // Validate StringNumericLiteral before passing decimal text to LLVM.
      // In particular, reject separators, hex floats, inf/nan and signs
      // before non-decimal prefixes, even if another parser accepts them.
      size_t P = (S[0] == '+' || S[0] == '-') ? 1 : 0;
      const auto Digits = [&] {
        const auto Begin = P;
        while (P < S.size() && S[P] >= '0' && S[P] <= '9')
          ++P;
        return P - Begin;
      };
      auto Count = Digits();
      if (P < S.size() && S[P] == '.') {
        ++P;
        Count += Digits();
      }
      if (!Count)
        return NaN();
      if (P < S.size() && (S[P] == 'e' || S[P] == 'E')) {
        ++P;
        if (P < S.size() && (S[P] == '+' || S[P] == '-'))
          ++P;
        if (!Digits())
          return NaN();
      }
      if (P != S.size())
        return NaN();
      auto Status = F.convertFromString(S, Rounding);
      if (!Status) {
        llvm::consumeError(Status.takeError());
        throw Error("primitive_decimal_conversion_failed");
      }
    }
    return number(std::move(F));
  }
  SourceValue toNumber(const SourceValue &V) {
    switch (V.Value->Kind) {
    case PrimitiveKind::Undefined:
      return number(APFloat::getNaN(APFloat::IEEEdouble()));
    case PrimitiveKind::Null:
      return integer32(0);
    case PrimitiveKind::Boolean:
      return integer32(V.Value->Boolean ? 1 : 0);
    case PrimitiveKind::Number:
      return V;
    case PrimitiveKind::String:
      return stringNumber(V.Value->String);
    case PrimitiveKind::BigInt:
      return refused("would_throw", "bigint_to_number_type_error");
    }
    throw Error("invalid_primitive_value");
  }
  SourceValue equality(const SourceValue &A, const SourceValue &B,
                       bool Strict) {
    const auto AK = A.Value->Kind, BK = B.Value->Kind;
    if (AK == BK) {
      switch (AK) {
      case PrimitiveKind::Undefined:
      case PrimitiveKind::Null:
        return boolean(true);
      case PrimitiveKind::Boolean:
        return boolean(A.Value->Boolean == B.Value->Boolean);
      case PrimitiveKind::Number:
        return boolean(floating(A.Value->NumberBits)
                           .compare(floating(B.Value->NumberBits)) ==
                       APFloat::cmpEqual);
      case PrimitiveKind::String:
        step(std::min(A.Value->String.size(), B.Value->String.size()) + 1);
        return boolean(A.Value->String == B.Value->String);
      case PrimitiveKind::BigInt:
        return boolean(A.Value->BigInt == B.Value->BigInt);
      }
    }
    if (Strict)
      return boolean(false);
    if (nullish(*A.Value) || nullish(*B.Value))
      return boolean(nullish(*A.Value) && nullish(*B.Value));
    if (AK == PrimitiveKind::Boolean)
      return equality(toNumber(A), B, false);
    if (BK == PrimitiveKind::Boolean)
      return equality(A, toNumber(B), false);
    if (AK == PrimitiveKind::BigInt || BK == PrimitiveKind::BigInt)
      return refused("unsupported", "mixed_bigint_comparison");
    return equality(toNumber(A), toNumber(B), true);
  }
  SourceValue unary(std::u16string_view Op, const SourceValue &A) {
    if (!known(A))
      return A;
    if (Op == u"!")
      return boolean(!sourcePrimitiveTruthy(*A.Value));
    if (Op == u"void")
      return constant({});
    if (Op == u"typeof") {
      const auto Kind = A.Value->Kind;
      return ascii(Kind == PrimitiveKind::Null ? "object"
                                               : sourcePrimitiveKindName(Kind));
    }
    if (Op == u"delete")
      return boolean(true); // primitive, never a Reference
    if (A.Value->Kind == PrimitiveKind::BigInt && Op != u"+") {
      const auto N = big(*A.Value);
      if (Op == u"-")
        return bigint(-N);
      if (Op == u"~")
        return bigint(~N);
      return refused("unsupported", "unqualified_unary_operator");
    }
    auto N = toNumber(A);
    if (!known(N))
      return N;
    if (Op == u"+")
      return N;
    if (Op == u"-") {
      auto F = floating(N.Value->NumberBits);
      F.changeSign();
      return number(std::move(F));
    }
    if (Op == u"~")
      return integer32(~uint32(N.Value->NumberBits));
    return refused("unsupported", "unqualified_unary_operator");
  }
  SourceValue bigBinary(std::u16string_view Op, const SourceValue &A,
                        const SourceValue &B) {
    auto X = big(*A.Value), Y = big(*B.Value);
    if (Op == u"+")
      return bigint(X + Y);
    if (Op == u"-")
      return bigint(X - Y);
    if (Op == u"*")
      return bigint(X * Y);
    if (Op == u"/" || Op == u"%") {
      if (Y.isZero())
        return refused("would_throw", "bigint_divide_by_zero");
      return bigint(Op == u"/" ? X.sdiv(Y) : X.srem(Y));
    }
    if (Op == u"&")
      return bigint(X & Y);
    if (Op == u"|")
      return bigint(X | Y);
    if (Op == u"^")
      return bigint(X ^ Y);
    if (Op == u"<")
      return boolean(X.slt(Y));
    if (Op == u">")
      return boolean(X.sgt(Y));
    if (Op == u"<=")
      return boolean(X.sle(Y));
    if (Op == u">=")
      return boolean(X.sge(Y));
    if (Op == u">>>")
      return refused("would_throw", "bigint_unsigned_shift_type_error");
    if (Op == u"<<" || Op == u">>") {
      const bool Left = (Op == u"<<") != Y.isNegative();
      const auto Count = Y.abs().getLimitedValue(MaxJavaScriptBigIntBits + 1);
      if (X.isZero())
        return bigint(X);
      if (Count > MaxJavaScriptBigIntBits) {
        if (Left)
          throw ValueLimit{};
        return bigint(APInt(BigWidth, X.isNegative() ? uint64_t(-1) : 0, true));
      }
      return bigint(Left ? X.shl(unsigned(Count)) : X.ashr(unsigned(Count)));
    }
    if (Op == u"**") {
      if (Y.isNegative())
        return refused("would_throw", "bigint_negative_exponent");
      if (Y.isZero())
        return bigint(APInt(BigWidth, 1));
      if (X.isZero() || X == 1)
        return bigint(X);
      if (X.isAllOnes())
        return bigint(Y[0] ? X : APInt(BigWidth, 1));
      auto Power = Y.getLimitedValue(MaxJavaScriptBigIntBits + 1);
      if (Power > MaxJavaScriptBigIntBits)
        throw ValueLimit{};
      APInt Acc(BigWidth, 1);
      while (Power) {
        if (Power & 1) {
          Acc *= X;
          bigBudget(Acc);
        }
        Power >>= 1;
        if (Power) {
          X *= X;
          bigBudget(X);
        }
      }
      return bigint(std::move(Acc));
    }
    return refused("unsupported", "unqualified_binary_operator");
  }
  SourceValue binary(std::u16string_view Op, const SourceValue &A,
                     const SourceValue &B) {
    if (!known(A) || !known(B))
      return operands(A, B);
    if (Op == u"in" || Op == u"instanceof")
      return refused("would_throw", "primitive_rhs_type_error");
    if (Op == u"==" || Op == u"!=" || Op == u"===" || Op == u"!==") {
      auto R = equality(A, B, Op == u"===" || Op == u"!==");
      return known(R) && (Op == u"!=" || Op == u"!==")
                 ? boolean(!R.Value->Boolean)
                 : R;
    }
    if (Op == u"+" && (A.Value->Kind == PrimitiveKind::String ||
                       B.Value->Kind == PrimitiveKind::String)) {
      const auto X = toString(A), Y = toString(B);
      return concat(*X.Value, *Y.Value);
    }
    const bool Compare = Op == u"<" || Op == u">" || Op == u"<=" || Op == u">=";
    if (Compare && A.Value->Kind == PrimitiveKind::String &&
        B.Value->Kind == PrimitiveKind::String) {
      step(std::min(A.Value->String.size(), B.Value->String.size()) + 1);
      const auto C = A.Value->String.compare(B.Value->String);
      return boolean(Op == u"<"    ? C < 0
                     : Op == u">"  ? C > 0
                     : Op == u"<=" ? C <= 0
                                   : C >= 0);
    }
    if (A.Value->Kind == PrimitiveKind::BigInt ||
        B.Value->Kind == PrimitiveKind::BigInt) {
      if (A.Value->Kind != B.Value->Kind)
        return refused(Compare ? "unsupported" : "would_throw",
                       Compare ? "mixed_bigint_comparison"
                               : "mixed_bigint_arithmetic_type_error");
      return bigBinary(Op, A, B);
    }
    const auto NX = toNumber(A), NY = toNumber(B);
    if (!known(NX) || !known(NY))
      return operands(NX, NY);
    auto X = floating(NX.Value->NumberBits), Y = floating(NY.Value->NumberBits);
    if (Compare) {
      const auto C = X.compare(Y);
      return boolean(C != APFloat::cmpUnordered &&
                     (Op == u"<"    ? C == APFloat::cmpLessThan
                      : Op == u">"  ? C == APFloat::cmpGreaterThan
                      : Op == u"<=" ? C != APFloat::cmpGreaterThan
                                    : C != APFloat::cmpLessThan));
    }
    if (Op == u"+")
      X.add(Y, Rounding);
    else if (Op == u"-")
      X.subtract(Y, Rounding);
    else if (Op == u"*")
      X.multiply(Y, Rounding);
    else if (Op == u"/")
      X.divide(Y, Rounding);
    else if (Op == u"%")
      X.mod(Y); // truncating JS remainder, not IEEE remainder
    else if (Op == u"**")
      return refused("unsupported", "number_exponentiation_profile_required");
    else {
      const auto L = uint32(NX.Value->NumberBits),
                 R = uint32(NY.Value->NumberBits);
      const auto Count = R & 31;
      if (Op == u"&")
        return integer32(L & R);
      if (Op == u"|")
        return integer32(L | R);
      if (Op == u"^")
        return integer32(L ^ R);
      if (Op == u"<<")
        return integer32(L << Count);
      if (Op == u">>>")
        return integer32(L >> Count, false);
      if (Op == u">>")
        return integer32(uint32_t(APInt(32, L).ashr(Count).getZExtValue()));
      return refused("unsupported", "unqualified_binary_operator");
    }
    return number(std::move(X));
  }
  SourceValue evaluate(uint32_t I) {
    step();
    const auto &N = Source.Nodes[I];
    const auto &K = N.Kind;
    if (K == "NullLiteral") {
      SourcePrimitive V;
      V.Kind = PrimitiveKind::Null;
      return constant(std::move(V));
    }
    if (K == "BooleanLiteral")
      return boolean(N.flag("value"));
    if (K == "NumericLiteral") {
      const auto *A = N.attribute("value");
      const auto *D = A ? std::get_if<double>(&A->Value) : nullptr;
      if (!D)
        throw Error("invalid_source_model");
      uint64_t Bits;
      std::memcpy(&Bits, D, sizeof(Bits));
      return number(floating(Bits));
    }
    if (K == "StringLiteral" || K == "DirectiveLiteral")
      return string(text(I, "value"));
    if (K == "BigIntLiteral")
      return bigLiteral(text(I, "bigint"));
    if (K == "UnaryExpression")
      return unary(text(I, "operator"), value(I, "argument"));
    if (K == "BinaryExpression")
      return binary(text(I, "operator"), value(I, "left"), value(I, "right"));
    if (K == "LogicalExpression") {
      const auto &L = value(I, "left");
      if (!known(L))
        return L;
      const auto Op = text(I, "operator");
      const bool Truthy = sourcePrimitiveTruthy(*L.Value);
      if (Op == u"&&")
        return Truthy ? value(I, "right") : L;
      if (Op == u"||")
        return Truthy ? L : value(I, "right");
      if (Op == u"??")
        return nullish(*L.Value) ? value(I, "right") : L;
      return refused("unsupported", "unqualified_logical_operator");
    }
    if (K == "ConditionalExpression") {
      const auto &Test = value(I, "test");
      if (!known(Test))
        return Test;
      return value(I, sourcePrimitiveTruthy(*Test.Value) ? "consequent"
                                                         : "alternate");
    }
    if (K == "SequenceExpression") {
      SourceValue Last = refused("unsupported", "empty_sequence");
      for (const auto &C : N.Children) {
        step();
        Last = Result.Nodes[C.Index];
        if (!known(Last))
          return Last;
      }
      return Last;
    }
    if (K == "TemplateElement") {
      const auto *Cooked = N.text("cooked");
      return Cooked ? string(*Cooked)
                    : refused("unsupported", "uncooked_template_element");
    }
    if (K == "TemplateLiteral") {
      std::vector<uint32_t> Quasis, Expressions;
      for (const auto &C : N.Children) {
        step();
        (C.Field == "quasis" ? Quasis : Expressions).push_back(C.Index);
      }
      if (Quasis.size() != Expressions.size() + 1)
        throw Error("invalid_source_model");
      auto Out = Result.Nodes[Quasis[0]];
      for (size_t J = 0; J < Expressions.size(); ++J) {
        if (!known(Out))
          return Out;
        const auto &E = Result.Nodes[Expressions[J]];
        if (!known(E))
          return E;
        const auto &Q = Result.Nodes[Quasis[J + 1]];
        if (!known(Q))
          return Q;
        const auto S = toString(E);
        Out = concat(*Out.Value, *S.Value);
        Out = concat(*Out.Value, *Q.Value);
      }
      return Out;
    }
    if (K == "Identifier")
      return refused("unknown", "runtime_binding_value");
    if (K == "MemberExpression" || K == "OptionalMemberExpression")
      return refused("unknown", "property_access");
    if (K == "CallExpression" || K == "OptionalCallExpression" ||
        K == "NewExpression" || K == "TaggedTemplateExpression")
      return refused("unknown", "target_call");
    if (K == "FunctionExpression" || K == "ArrowFunctionExpression" ||
        K == "ObjectExpression" || K == "ArrayExpression" ||
        K == "RegExpLiteral" || K == "ClassExpression")
      return refused("unknown", "non_primitive_value");
    if (K == "ThisExpression" || K == "Super" || K == "MetaProperty" ||
        K == "AssignmentExpression" || K == "UpdateExpression" ||
        K == "AwaitExpression" || K == "YieldExpression" ||
        K == "ImportExpression")
      return refused("unknown", "runtime_expression");
    if (K == "Program" || K == "BlockStatement" || K == "StaticBlock" ||
        K == "ExpressionStatement" || K == "EmptyStatement" || K == "Empty" ||
        K == "DebuggerStatement" || K == "VariableDeclaration" ||
        K == "VariableDeclarator" || K == "FunctionDeclaration" ||
        K == "ClassDeclaration" || K == "ClassBody" || K == "ClassProperty" ||
        K == "ClassPrivateProperty" || K == "MethodDefinition" ||
        K == "Property" || K == "PrivateName" || K == "ArrayPattern" ||
        K == "ObjectPattern" || K == "AssignmentPattern" ||
        K == "RestElement" || K == "SpreadElement" || K == "Directive" ||
        K == "IfStatement" || K == "WhileStatement" ||
        K == "DoWhileStatement" || K == "ForStatement" ||
        K == "ForInStatement" || K == "ForOfStatement" ||
        K == "SwitchStatement" || K == "SwitchCase" || K == "CatchClause" ||
        K == "TryStatement" || K == "WithStatement" || K == "ThrowStatement" ||
        K == "ReturnStatement" || K == "BreakStatement" ||
        K == "ContinueStatement" || K == "LabeledStatement" ||
        K == "ImportDeclaration" || K == "ImportSpecifier" ||
        K == "ImportDefaultSpecifier" || K == "ImportNamespaceSpecifier" ||
        K == "ImportAttribute" || K == "ExportNamedDeclaration" ||
        K == "ExportDefaultDeclaration" || K == "ExportAllDeclaration" ||
        K == "ExportSpecifier" || K == "ExportNamespaceSpecifier")
      return {};
    return refused("unsupported", "unqualified_value_node");
  }

public:
  explicit Evaluator(const SourceAnalysis &S) : Source(S) {
    Result.SourceID = S.ID;
    Result.ID = identity("source-values", {S.ID, JavaScriptValueProfile});
    Result.Status = "ok";
  }
  SourceValueAnalysis run() {
    if (Source.ParseStatus != "parsed") {
      Result.Status = "unavailable";
      Result.Diagnostics.push_back({"source_not_parsed", -1});
      return std::move(Result);
    }
    step(validateSourceModel(Source));
    Result.Nodes.resize(Source.Nodes.size());
    try {
      for (size_t I = Source.Nodes.size(); I-- != 0;) {
        try {
          Result.Nodes[I] = evaluate(uint32_t(I));
        } catch (const ValueLimit &) {
          Result.Nodes[I] = refused("budget_exceeded", "primitive_value_limit");
        }
        if (Result.Nodes[I].Status == "unsupported" ||
            Result.Nodes[I].Status == "budget_exceeded")
          Result.Status = "partial";
      }
    } catch (const Error &E) {
      Result.Nodes.clear();
      const std::string Code = E.what();
      Result.Status = Code == "source_value_budget_exceeded" ||
                              Code == "source_value_storage_exceeded"
                          ? "budget_exceeded"
                          : "unsupported";
      Result.Diagnostics.push_back({Code, -1});
    }
    return std::move(Result);
  }
};
} // namespace

std::string_view sourcePrimitiveKindName(PrimitiveKind Kind) {
  switch (Kind) {
  case PrimitiveKind::Undefined:
    return "undefined";
  case PrimitiveKind::Null:
    return "null";
  case PrimitiveKind::Boolean:
    return "boolean";
  case PrimitiveKind::Number:
    return "number";
  case PrimitiveKind::String:
    return "string";
  case PrimitiveKind::BigInt:
    return "bigint";
  }
  return "invalid";
}
bool sourcePrimitiveTruthy(const SourcePrimitive &V) {
  switch (V.Kind) {
  case PrimitiveKind::Undefined:
  case PrimitiveKind::Null:
    return false;
  case PrimitiveKind::Boolean:
    return V.Boolean;
  case PrimitiveKind::Number: {
    const auto F = floating(V.NumberBits);
    return !F.isZero() && !F.isNaN();
  }
  case PrimitiveKind::String:
    return !V.String.empty();
  case PrimitiveKind::BigInt:
    return V.BigInt != "0";
  }
  return false;
}
SourceValueAnalysis analyzeSourceValues(const SourceAnalysis &Source) {
  return Evaluator(Source).run();
}
} // namespace neverd::web
