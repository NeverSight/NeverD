//===- Expressions.cpp - CPython operand stack to Python expressions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Lower proven stack operations while preserving call order, in-place
/// operations, binding defaults and the outer iterator of comprehensions.
//===----------------------------------------------------------------------===//

#include "SourceDetail.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"

#include <charconv>
#include <cmath>

using namespace llvm;
using namespace neverd::python;

bool neverd::python::identifier(StringRef S) {
  if (S.empty() || S == "__debug__" ||
      !(isAlpha(S.front()) || S.front() == '_') ||
      !llvm::all_of(S, [](char C) { return isAlnum(C) || C == '_'; }))
    return false;
  static constexpr StringLiteral Keywords[] = {
      "False",  "None",   "True",    "and",      "as",       "assert", "async",
      "await",  "break",  "class",   "continue", "def",      "del",    "elif",
      "else",   "except", "finally", "for",      "from",     "global", "if",
      "import", "in",     "is",      "lambda",   "nonlocal", "not",    "or",
      "pass",   "raise",  "return",  "try",      "while",    "with",   "yield"};
  return !llvm::is_contained(Keywords, S);
}
std::string Emitter::literal(const Object &O, unsigned Depth) {
  if (Depth > limits::kDepth)
    reject("Constant nesting limit");
  J.charge(J.Text, 1, limits::kTextBytes, O.Offset);
  switch (O.Type) {
  case Object::None:
    return "None";
  case Object::True:
    return "True";
  case Object::False:
    return "False";
  case Object::Ellipsis:
    return "...";
  case Object::Int: {
    auto N = std::to_string(O.Number);
    J.charge(J.Text, N.size() + 2, limits::kTextBytes, O.Offset);
    return O.Number < 0 ? "(" + N + ")" : N;
  }
  case Object::Long:
    J.charge(J.Text, O.Digits.size() + 2, limits::kTextBytes, O.Offset);
    return "(" + O.Digits + (C.Version < 300 ? "L)" : ")");
  case Object::Float: {
    if (!std::isfinite(O.Real))
      reject("Non-finite float constant");
    char Buffer[64];
    auto R = std::to_chars(Buffer, Buffer + sizeof(Buffer), O.Real);
    std::string N(Buffer, R.ptr);
    if (N.find_first_of(".eE") == std::string::npos)
      N += ".0";
    return "(" + N + ")";
  }
  case Object::Unicode: {
    J.charge(J.Text, 6 * O.Data.size() + 2, limits::kTextBytes, O.Offset);
    std::string S;
    raw_string_ostream OS(S);
    if (C.Version < 300) {
      if (!llvm::all_of(O.Data, [](unsigned char B) { return B < 128; }))
        reject("Non-ASCII Unicode literal requires a legacy source encoding");
      OS << 'u';
    }
    OS << json::Value(O.Data);
    return S;
  }
  case Object::Bytes: {
    J.charge(J.Text, O.Data.size() * 4, limits::kTextBytes, O.Offset);
    std::string S = C.Version >= 206 ? "b'" : "'";
    for (unsigned char B : O.Data) {
      S += "\\x";
      S += hexdigit(B >> 4);
      S += hexdigit(B & 15);
    }
    return S + "'";
  }
  case Object::Tuple: {
    std::vector<std::string> V;
    for (auto *Item : O.Items)
      V.push_back(literal(*Item, Depth + 1));
    return "(" + join(V, ", ") + (V.size() == 1 ? "," : "") + ")";
  }
  default:
    reject("Constant has no qualified Python expression");
  }
}
std::string Emitter::text(Expr E) {
  if (E.Type == Expr::Constant)
    return literal(*E.Value);
  if (E.Type == Expr::Tuple)
    return "(" + join(E.Values, ", ") + (E.Values.size() == 1 ? "," : "") + ")";
  if (E.Type != Expr::Plain)
    reject("Internal stack value escapes its owning operation");
  return std::move(E.Text);
}
std::vector<std::string> Emitter::tuple(Expr E, bool Strings) {
  if (E.Type == Expr::Tuple && !Strings)
    return std::move(E.Values);
  if (E.Type != Expr::Constant || E.Value->Type != Object::Tuple)
    reject("Expected a literal tuple");
  std::vector<std::string> Values;
  for (auto *K : E.Value->Items) {
    if (Strings) {
      if (K->Type != Object::Unicode || !identifier(K->Data))
        reject("Invalid keyword name");
      Values.push_back(K->Data.str());
    } else
      Values.push_back(literal(*K));
  }
  return Values;
}
std::string Emitter::name(const Instruction &I) const {
  const Object *N;
  switch (I.Opcode) {
  case Op::LOAD_FAST:
  case Op::STORE_FAST:
  case Op::DELETE_FAST:
    N = C.Vars->Items[I.Arg];
    break;
  case Op::LOAD_DEREF:
  case Op::STORE_DEREF:
  case Op::LOAD_CLOSURE:
  case Op::DELETE_DEREF:
    N = I.Arg < C.Cells->Items.size()
            ? C.Cells->Items[I.Arg]
            : C.Free->Items[I.Arg - C.Cells->Items.size()];
    break;
  default:
    N = C.Names->Items[I.Arg];
    break;
  }
  if (!identifier(N->Data))
    reject("Identifier cannot be represented in this profile");
  J.charge(J.Text, N->Data.size(), limits::kTextBytes, N->Offset);
  return N->Data.str();
}
Expr Emitter::pop(Stack &S) const {
  if (S.empty())
    invalid("Operand stack underflow");
  Expr V = std::move(S.back());
  S.pop_back();
  return V;
}
std::vector<std::string> Emitter::args(Stack &S, unsigned N) {
  if (N > S.size())
    invalid("Operand stack underflow");
  std::vector<std::string> V(N);
  while (N)
    V[--N] = text(pop(S));
  return V;
}
namespace {
StringRef binary(Op O) {
  switch (O) {
#define BIN(NAME, SPELLING)                                                    \
  case Op::BINARY_##NAME:                                                      \
  case Op::INPLACE_##NAME:                                                     \
    return SPELLING
    BIN(ADD, "+");
    BIN(SUBTRACT, "-");
    BIN(MULTIPLY, "*");
    BIN(MATRIX_MULTIPLY, "@");
    BIN(POWER, "**");
    BIN(TRUE_DIVIDE, "/");
    BIN(FLOOR_DIVIDE, "//");
    BIN(MODULO, "%");
    BIN(LSHIFT, "<<");
    BIN(RSHIFT, ">>");
    BIN(AND, "&");
    BIN(OR, "|");
    BIN(XOR, "^");
#undef BIN
  default:
    return {};
  }
}
} // namespace
bool Emitter::expression(size_t &P, size_t End, Stack &S) {
  Current = P;
  const Instruction &I = F.Code[P];
  const Op O = I.Opcode;
  auto Push = [&](std::string T) {
    J.charge(J.Text, T.size(), limits::kTextBytes,
             C.Bytes->DataOffset + I.Byte);
    S.push_back({Expr::Plain, std::move(T)});
  };
  if (auto Operator = binary(O); !Operator.empty()) {
    auto Right = text(pop(S));
    auto Left = text(pop(S));
    if (mnemonic(O).starts_with("INPLACE_"))
      S.push_back(
          {Expr::Augment, std::move(Left), Operator.str() + "= " + Right});
    else
      Push("(" + Left + " " + Operator.str() + " " + Right + ")");
    return true;
  }
  switch (O) {
  case Op::PushNull:
    S.push_back({Expr::NullCallSlot});
    break;
  case Op::SmallInteger:
    Push(std::to_string(I.Arg));
    break;
  case Op::CommonConstant:
  case Op::LOAD_CONST: {
    static const Object Common[] = {{Object::None},
                                    {Object::Unicode},
                                    {Object::True},
                                    {Object::False},
                                    {.Type = Object::Int, .Number = -1}};
    Expr V{Expr::Constant};
    V.Value = O == Op::CommonConstant ? &Common[I.Arg] : C.Consts->Items[I.Arg];
    S.push_back(std::move(V));
    break;
  }
  case Op::LOAD_FAST:
  case Op::LOAD_DEREF:
  case Op::LOAD_NAME:
  case Op::LOAD_GLOBAL:
    Push(name(I));
    break;
  case Op::LOAD_CLOSURE:
    S.push_back({Expr::Cell, name(I)});
    break;
  case Op::LOAD_ATTR:
  case Op::LOAD_METHOD: {
    auto V = text(pop(S));
    Push("(" + V + ")." + name(I));
    if (O == Op::LOAD_METHOD)
      S.back().Type = Expr::Method;
    break;
  }
  case Op::BINARY_SUBSCR: {
    auto K = text(pop(S));
    auto A = text(pop(S));
    Push("(" + A + ")[" + K + "]");
    break;
  }
  case Op::UNARY_NEGATIVE:
  case Op::UNARY_NOT:
  case Op::UNARY_INVERT:
  case Op::UNARY_POSITIVE: {
    StringRef T = O == Op::UNARY_NEGATIVE ? "-"
                  : O == Op::UNARY_NOT    ? "not "
                  : O == Op::UNARY_INVERT ? "~"
                                          : "+";
    auto A = text(pop(S));
    Push("(" + T.str() + A + ")");
    break;
  }
  case Op::COMPARE_OP:
  case Op::IS_OP:
  case Op::CONTAINS_OP: {
    static constexpr StringRef Comparisons[] = {
        "<", "<=", "==", "!=", ">", ">="};
    if (O != Op::COMPARE_OP && I.Arg > 1)
      invalid("Invalid comparison operand");
    StringRef T = O == Op::COMPARE_OP ? Comparisons[I.Arg]
                  : O == Op::IS_OP    ? (I.Arg ? "is not" : "is")
                                      : (I.Arg ? "not in" : "in");
    auto B = text(pop(S));
    auto A = text(pop(S));
    Push("(" + A + " " + T.str() + " " + B + ")");
    break;
  }
  case Op::BUILD_TUPLE: {
    if (I.Arg > S.size())
      invalid("Tuple stack underflow");
    bool Closure = I.Arg && S.back().Type == Expr::Cell;
    Expr V{Closure ? Expr::Cells : Expr::Tuple};
    V.Values.resize(I.Arg);
    for (unsigned N = I.Arg; N; --N) {
      auto E = pop(S);
      if (Closure && E.Type != Expr::Cell)
        reject("Mixed closure tuple");
      V.Values[N - 1] = Closure ? std::move(E.Text) : text(std::move(E));
    }
    S.push_back(std::move(V));
    break;
  }
  case Op::BUILD_LIST:
  case Op::BUILD_SET: {
    auto V = args(S, I.Arg);
    if (O == Op::BUILD_SET && V.empty())
      reject("Empty set construction");
    Push((O == Op::BUILD_LIST ? "[" : "{") + join(V, ", ") +
         (O == Op::BUILD_LIST ? "]" : "}"));
    break;
  }
  case Op::BUILD_CONST_KEY_MAP: {
    auto Keys = tuple(pop(S), true);
    if (Keys.size() != I.Arg)
      invalid("Keyword key count mismatch");
    auto Values = args(S, I.Arg);
    Expr E{Expr::KeywordDefaults};
    for (size_t N = 0; N < Keys.size(); ++N)
      if (!E.Keywords.emplace(Keys[N], Values[N]).second)
        invalid("Duplicate keyword");
    S.push_back(std::move(E));
    break;
  }
  case Op::MAKE_FUNCTION: {
    if (I.Arg & ~11u)
      reject("Unqualified function annotations or flags");
    if (C.Version >= 303 && C.Version <= 310) {
      auto Qual = pop(S);
      if (Qual.Type != Expr::Constant || Qual.Value->Type != Object::Unicode)
        invalid("Invalid function qualified name");
    }
    auto K = pop(S);
    if (K.Type != Expr::Constant || K.Value->Type != Object::CodeObject)
      invalid("Invalid function construction operands");
    Expr Def{Expr::Definition};
    Def.Value = K.Value;
    auto &Child = *Def.Value->Body;
    if (Child.Id <= C.Id)
      reject("Non-nested code binding");
    std::vector<std::string> Free;
    if (I.Arg & 8) {
      auto V = pop(S);
      if (V.Type != Expr::Cells)
        invalid("Missing closure tuple");
      Free = std::move(V.Values);
    }
    size_t FreeCount =
        Child.Free ? Child.Free->Items.size()
                   : llvm::count_if(Child.LocalKinds->Data,
                                    [](unsigned char K) { return K & 0x80; });
    if (Free.size() != FreeCount)
      invalid("Closure arity mismatch");
    for (size_t N = 0; N < Free.size(); ++N)
      if (Free[N] != Child.Free->Items[N]->Data)
        reject("Closure binding differs from lexical name");
    if (I.Arg & 2) {
      auto Kws = pop(S);
      if (Kws.Type != Expr::KeywordDefaults)
        reject("Dynamic keyword defaults");
      Def.Keywords = std::move(Kws.Keywords);
    }
    if (I.Arg & 1)
      Def.Values = tuple(pop(S));
    S.push_back(std::move(Def));
    break;
  }
  case Op::GET_ITER:
  case Op::GET_AWAITABLE:
  case Op::GET_YIELD_FROM_ITER: {
    auto A = text(pop(S));
    S.push_back({O == Op::GET_ITER        ? Expr::Iterator
                 : O == Op::GET_AWAITABLE ? Expr::Awaitable
                                          : Expr::YieldFrom,
                 std::move(A)});
    break;
  }
  case Op::YIELD_FROM: {
    auto Initial = pop(S), A = pop(S);
    if (Initial.Type != Expr::Constant || Initial.Value->Type != Object::None)
      reject("Non-initial delegation send");
    if (A.Type == Expr::Awaitable && (C.Flags & (0x80 | 0x200)))
      Push("(await " + A.Text + ")");
    else if (A.Type == Expr::YieldFrom && (C.Flags & 0x20))
      Push("(yield from " + A.Text + ")");
    else
      reject("Unqualified delegation mode");
    break;
  }
  case Op::YIELD_VALUE: {
    if (!(C.Flags & (0x20 | 0x200)))
      invalid("Yield in a non-generator");
    auto A = text(pop(S));
    Push("(yield " + A + ")");
    break;
  }
  case Op::CallWithSlot:
  case Op::CALL_FUNCTION:
  case Op::CALL_METHOD:
  case Op::CALL_FUNCTION_KW: {
    std::vector<std::string> Keys;
    if (O == Op::CALL_FUNCTION_KW)
      Keys = tuple(pop(S), true);
    if (O == Op::CALL_FUNCTION && I.Arg == 1 && S.size() >= 2 &&
        S.back().Type == Expr::Iterator &&
        S[S.size() - 2].Type == Expr::Definition) {
      auto Iter = pop(S);
      auto Def = pop(S);
      auto Id = Def.Value->Body->Id;
      if (!Bodies[Id].ComprehensionBody)
        reject("Unqualified comprehension body");
      Push(Bodies[Id].ComprehensionBody->apply(Iter.Text));
      break;
    }
    auto Values = args(S, I.Arg);
    if (Keys.size() > Values.size())
      invalid("Too many keyword names");
    SmallSet<StringRef, 8> Seen;
    for (size_t N = 0; N < Keys.size(); ++N) {
      if (!Seen.insert(Keys[N]).second)
        invalid("Duplicate call keyword");
      auto &V = Values[Values.size() - Keys.size() + N];
      V = Keys[N] + "=" + V;
    }
    auto Callable = pop(S);
    if (O == Op::CallWithSlot && Callable.Type != Expr::Method) {
      if (C.Version >= 313) {
        if (Callable.Type != Expr::NullCallSlot)
          reject("Call receiver has no proven function binding");
        Callable = pop(S);
      } else if (pop(S).Type != Expr::NullCallSlot)
        reject("Call receiver has no proven function binding");
    }
    if (O != Op::CallWithSlot &&
        (O == Op::CALL_METHOD) != (Callable.Type == Expr::Method))
      invalid("Call opcode disagrees with method lookup");
    auto Callee = Callable.Type == Expr::Method ? std::move(Callable.Text)
                                                : text(std::move(Callable));
    Push("(" + Callee + ")(" + join(Values, ", ") + ")");
    break;
  }
  case Op::DUP_TOP: {
    if (P + 1 >= End ||
        !(at(P + 1, Op::STORE_FAST) || at(P + 1, Op::STORE_NAME)))
      return false;
    auto V = text(pop(S));
    F.Lines[P + 1] = NextLine;
    Push("(" + name(F.Code[++P]) + " := " + V + ")");
    break;
  }
  case Op::JUMP_IF_FALSE_OR_POP:
  case Op::JUMP_IF_TRUE_OR_POP: {
    size_t T = target(P);
    if (T <= P + 1 || T > End)
      reject("Nonlocal short-circuit edge");
    auto Left = text(pop(S)), Right = expressionRange(P + 1, T);
    Push("(" + Left + (O == Op::JUMP_IF_FALSE_OR_POP ? " and " : " or ") +
         Right + ")");
    P = T - 1;
    break;
  }
  default:
    return false;
  }
  return true;
}
std::string Emitter::expressionRange(size_t B, size_t E) {
  if (ExpressionDepth >= limits::kDepth)
    throw Failure{"budget_exceeded", C.Bytes->DataOffset,
                  "Expression nesting limit exceeded"};
  SaveAndRestore Guard(ExpressionDepth, ExpressionDepth + 1);
  Stack Values;
  for (size_t P = B; P < E; ++P) {
    F.Lines[P] = NextLine;
    if (!expression(P, E, Values))
      reject("Non-expression in expression region");
    if (Values.size() > C.Stack)
      invalid("Expression exceeds declared stack size");
  }
  if (Values.size() != 1)
    reject("Expression stack does not have one result");
  return text(pop(Values));
}
