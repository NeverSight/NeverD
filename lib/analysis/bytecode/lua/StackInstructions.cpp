//===- StackInstructions.cpp - PUC Lua stack operations -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Translate proved stack windows to named slots. An instruction with an
/// unknown effect ends stack tracking for the rest of the prototype.
//===----------------------------------------------------------------------===//

#include "Decode.h"

#include "llvm/ADT/StringSwitch.h"

#include <bit>

using namespace neverd::lua;
using namespace llvm;

void neverd::lua::decodeStack(Decoder &D, const StackInstruction &I, int &Top) {
  StringRef Op = I.Operation;
  int A = I.Operand;
  const int V = D.C.Version;
  if (Op == "END" || Op == "ENDCODE") {
    D.returns(0, 0);
    return;
  }
  if (Top < 0)
    return;
  auto Require = [&](int N) {
    if (N < 0 || N > Top)
      D.bad("Operand exceeds the active Lua stack");
  };
  auto Push = [&](std::string Value) { D.set(Top++, std::move(Value)); };
  auto Pop = [&] {
    Require(1);
    return D.reg(--Top);
  };
  if (Op == "PUSHNUMBER" || Op == "PUSHNUMBERNEG" || Op == "PUSHINT") {
    int Value = Op == "PUSHINT" ? A - 33554431 : Op == "PUSHNUMBERNEG" ? -A : A;
    Push(std::to_string(Value));
  } else if (Op == "PUSHFLOAT") {
    Push(D.literal(
        {Constant::Number, 0, std::bit_cast<float>(uint32_t(I.Operand))}));
  } else if (Op == "PUSHNUM" || Op == "PUSHNEGNUM") {
    auto K = D.constant(D.P.Legacy->FirstNumber + A);
    if (K.Type != Constant::Number)
      D.bad("Number operand does not name a numeric constant");
    if (Op == "PUSHNEGNUM")
      K.F = -K.F;
    Push(D.literal(K));
  } else if (Op == "PUSHSTRING" || Op == "PUSHCONSTANT") {
    const auto &K = D.constant(A);
    if (K.Type == Constant::Closure && Op == "PUSHCONSTANT") {
      if (D.P.Id == D.C.Root)
        D.capture(K.I, Top++);
    } else {
      if (Op == "PUSHSTRING" && K.Type != Constant::String)
        D.bad("String operand does not name a string");
      Push(D.literal(K));
    }
  } else if (Op == "PUSHFUNCTION") {
    if (!A)
      D.bad("Missing legacy function binding");
    D.capture(A, Top++);
  } else if (Op == "CLOSURE" && V == 0x40 && !(A & 511)) {
    unsigned Index = unsigned(A) >> 9;
    if (Index >= D.P.Children.size())
      D.bad("Invalid Lua 4 child prototype");
    D.capture(D.P.Children[Index], Top++);
  } else if (Op == "GETGLOBAL" || Op == "PUSHGLOBAL") {
    Push(D.global(D.constant(A)));
  } else if (Op == "SETGLOBAL" || Op == "STOREGLOBAL") {
    auto Value = Pop();
    D.statement(D.global(D.constant(A)) + " = " + Value);
  } else if (Op == "PUSHLOCAL" || Op == "GETLOCAL") {
    Require(A + 1);
    Push(D.reg(A));
  } else if (Op == "SETLOCAL" || Op == "STORELOCAL") {
    auto Value = Pop();
    Require(A + 1);
    D.set(A, std::move(Value));
  } else if (Op == "PUSHNIL") {
    int N = V <= 0x25 ? 1 : V == 0x40 ? A : A + 1;
    if (!N || N > int(D.P.Stack) - Top)
      D.bad("Invalid nil window");
    D.nils(Top, Top + N - 1);
    Top += N;
  } else if (Op == "ADJUST" || Op == "ADJUST0") {
    if (A > Top)
      D.nils(Top, A - 1);
    else
      D.statement("");
    Top = A;
  } else if (Op == "POP") {
    int N = V <= 0x25 ? 1 : V == 0x31 ? A + 1 : A;
    Require(N);
    Top -= N;
    D.statement("");
  } else if (Op == "RETCODE" || Op == "RETCODE0" || Op == "RETURN") {
    Require(A);
    D.returns(A, Top - A);
  } else if (Op == "SETLINE") {
    D.statement("");
  } else if (Op == "MINUS" || Op == "MINUSOP" || Op == "NOT" || Op == "NOTOP") {
    auto Value = Pop();
    Push((Op.starts_with("NOT") ? "not " : "-") + Value);
  } else if (Op == "ADDI") {
    auto Left = Pop();
    Push(Left + " + (" + std::to_string(A - 33554431) + ")");
  } else if (Op == "GETINDEXED" || Op == "GETDOTTED") {
    // GETINDEXED in Lua 4 indexes with a local. In 3.x GETDOTTED uses a
    // constant.
    if (Op == "GETINDEXED") {
      Require(A + 1);
      auto Table = Pop();
      Push(Table + "[" + D.reg(A) + "]");
    } else {
      auto Table = Pop();
      Push(Table + "[" + D.k(A) + "]");
    }
  } else if (Op == "GETTABLE" || Op == "PUSHINDEXED") {
    auto Key = Pop();
    auto Table = Pop();
    Push(Table + "[" + Key + "]");
  } else if (Op == "CALL" || Op == "CALLFUNC") {
    int Args, Results;
    if (V == 0x40) {
      int Base = A >> 9;
      Require(Base + 1);
      Args = Top - Base - 1;
      Results = A & 511;
    } else if (V <= 0x25) {
      Args = A;
      Results = I.Extra;
    } else if (V == 0x31 && (I.Name == "CALLFUNC0" || I.Name == "CALLFUNC1")) {
      Args = A;
      Results = I.Name == "CALLFUNC1";
    } else {
      Results = A;
      Args = I.Extra;
    }
    if (Results == 255)
      return;
    Require(Args + 1);
    int Base = Top - Args - 1;
    D.call(Base, Base + 1, Args, Results);
    Top = Base + Results;
  } else {
    StringRef Operator = StringSwitch<StringRef>(Op)
                             .Cases({"ADD", "ADDOP"}, "+")
                             .Cases({"SUB", "SUBOP"}, "-")
                             .Cases({"MULT", "MULTOP"}, "*")
                             .Cases({"DIV", "DIVOP"}, "/")
                             .Cases({"POW", "POWOP"}, "^")
                             .Case("CONCOP", "..")
                             .Case("EQOP", "==")
                             .Case("NEQOP", "~=")
                             .Case("LTOP", "<")
                             .Case("LEOP", "<=")
                             .Case("GTOP", ">")
                             .Case("GEOP", ">=")
                             .Default("");
    if (Operator.empty())
      return;
    auto Right = Pop();
    auto Left = Pop();
    Push(Left + " " + Operator.str() + " " + Right);
  }
}
