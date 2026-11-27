//===- PucInstructions.cpp - PUC Lua instruction semantics ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode register windows, jump polarity, closure bindings and arithmetic
/// fallback pairs.
//===----------------------------------------------------------------------===//

#include "Decode.h"

#include <utility>

using namespace neverd::lua;
void neverd::lua::decodePuc(Decoder &D) {
  auto &I = D.I;
  auto &P = D.P;
  int V = D.C.Version;
  bool New = V >= 0x54;
  auto W = P.Code[I.PC];
  auto Op = I.Op;
  int A = (W >> (New ? 7 : 6)) & 255, B = New ? (W >> 16) & 255 : W >> 23;
  int C = New ? W >> 24 : (W >> 14) & 511, Bx = W >> (New ? 15 : 14);
  bool K = New && ((W >> 15) & 1);
  auto R = [&](int X) { return D.reg(X); };
  auto RK = [&](int X) { return X & 256 ? D.k(X - 256) : R(X); };
  auto RC = [&] { return New ? K ? D.k(C) : R(C) : RK(C); };
  auto Field = [&](int X) {
    const auto &Value = D.constant(X);
    if (Value.Type != Constant::String)
      D.bad("Expected string field");
    return D.literal(Value);
  };
  auto Ax = [&](uint32_t Extra) {
    if (mnemonic(Family::Puc, V, Extra) != "EXTRAARG")
      D.bad("Expected EXTRAARG");
    return Extra >> (New ? 7 : 6);
  };
  auto Jump = [&](uint32_t J, int PC) {
    if (!New && V >= 0x52 && ((J >> 6) & 255))
      unsupported(P.CodeAt + 4 * PC, "Jump closes live upvalues");
    return PC + 1 + (New ? int(J >> 7) - 16777215 : int(J >> 14) - 131071);
  };
  if (Op == "MOVE")
    D.set(A, R(B));
  else if (Op == "LOADK" || Op == "LOADKX")
    D.set(A, D.k(Op == "LOADK" ? Bx : Ax(D.extra())));
  else if (Op == "LOADI" || Op == "LOADF")
    D.set(A, std::to_string(Bx - 65535) + (Op == "LOADF" ? ".0" : ""));
  else if (Op == "LOADTRUE" || Op == "LOADFALSE" || Op == "LOADBOOL" ||
           Op == "LFALSESKIP") {
    D.set(A, Op == "LOADTRUE" || (Op == "LOADBOOL" && B) ? "true" : "false");
    if (Op == "LFALSESKIP" || (Op == "LOADBOOL" && C)) {
      I.Kind = Form::Jump;
      I.Target = I.PC + 2;
    }
  } else if (Op == "LOADNIL")
    D.nils(A, V == 0x51 ? B : A + B);
  else if (Op == "GETUPVAL")
    D.set(A, D.up(B));
  else if (Op == "SETUPVAL")
    D.statement(D.up(B) + " = " + R(A));
  else if (Op == "GETGLOBAL")
    D.set(A, D.global(D.constant(Bx)));
  else if (Op == "SETGLOBAL")
    D.statement(D.global(D.constant(Bx)) + " = " + R(A));
  else if (Op == "GETTABLE" || Op == "GETTABUP" || Op == "GETI" ||
           Op == "GETFIELD") {
    auto Table = Op == "GETTABUP" ? D.up(B) : R(B);
    auto Key = Op == "GETI" ? std::to_string(C)
               : Op == "GETFIELD" || (New && Op == "GETTABUP") ? Field(C)
               : New                                           ? R(C)
                                                               : RK(C);
    D.set(A, Table + "[" + Key + "]");
  } else if (Op == "SETTABLE" || Op == "SETTABUP" || Op == "SETI" ||
             Op == "SETFIELD") {
    auto Table = Op == "SETTABUP" ? D.up(A) : R(A);
    auto Key = Op == "SETI" ? std::to_string(B)
               : Op == "SETFIELD" || (New && Op == "SETTABUP") ? Field(B)
               : New                                           ? R(B)
                                                               : RK(B);
    D.statement(Table + "[" + Key + "] = " + RC());
  } else if (Op == "SELF") {
    auto Receiver = R(B);
    auto Saved = R(A + 1);
    if (V == 0x54 && !K) {
      R(C);
      unsupported(P.CodeAt + 4 * I.PC,
                  "Method key in a register has no proven string type");
    }
    auto Key = New ? Field(C) : RK(C);
    // The lookup can invoke __index and observe the assigned receiver.
    auto Method = R(A);
    I.Writes.set(A);
    I.Writes.set(A + 1);
    D.statement(Saved + " = " + Receiver + "; " + Method + " = " + Saved + "[" +
                Key + "]");
  } else if (Op == "NEWTABLE") {
    if (New && bool(Ax(D.extra())) != K)
      D.bad("Inconsistent table size extension");
    D.set(A, "{}");
  } else if (Op == "SETLIST") {
    if (New && K)
      Ax(D.extra());
    if (!New && !C) {
      auto Extra = D.extra();
      if (V != 0x51)
        Ax(Extra);
    }
  } else if (Op == "UNM" || Op == "BNOT" || Op == "NOT" || Op == "LEN") {
    auto Symbol = Op == "UNM"    ? "-"
                  : Op == "BNOT" ? "~"
                  : Op == "NOT"  ? "not "
                                 : "#";
    D.set(A, Symbol + R(B));
  } else if (Op == "CONCAT") {
    int First = New ? A : B, Last = New ? A + B - 1 : C;
    if (First >= Last)
      D.bad("Invalid concatenation range");
    std::string S = R(First);
    for (int X = First + 1; X <= Last; ++X)
      S += " .. " + R(X);
    D.set(A, S);
  } else if (auto Bin = binary(Op.rtrim("KI")); !Bin.empty()) {
    bool Imm = Op.ends_with("I"), Const = Op.ends_with("K");
    std::string Left = New ? R(B) : RK(B), Right;
    if (New) {
      uint32_t M = D.extra();
      auto Mop = mnemonic(Family::Puc, V, M);
      if (Mop != (Imm ? "MMBINI" : Const ? "MMBINK" : "MMBIN"))
        D.bad("Missing arithmetic metamethod fallback");
      constexpr llvm::StringLiteral Operators[] = {
          "+", "-", "*", "%", "^", "/", "//", "&", "|", "~", "<<", ">>"};
      int Event = int(M >> 24) - 6, Arg = (M >> 16) & 255;
      if (Event < 0 || size_t(Event) >= std::size(Operators) ||
          int((M >> 7) & 255) != B)
        D.bad("Invalid arithmetic fallback");
      bool Flip = (M >> 15) & 1;
      // Negated immediates implement subtraction or a reversed shift. Emit
      // the fallback's original operation only when the fast path agrees.
      auto SourceOp = Operators[Event];
      bool Negated = Imm &&
                     ((Op == "ADDI" && SourceOp == "-") ||
                      (Op == "SHRI" && SourceOp == "<<")) &&
                     Arg - 127 == -(C - 127);
      if (!Negated && (SourceOp != Bin || Arg != C))
        D.bad("Inconsistent arithmetic operand pair");
      if (Flip && (Negated || (!Imm && !Const) ||
                   !(Op == "SHLI" || Bin == "+" || Bin == "*" || Bin == "&" ||
                     Bin == "|" || Bin == "~")))
        D.bad("Inconsistent arithmetic operand order");
      if (Op == "SHLI" && !Flip)
        D.bad("Invalid immediate left shift");
      if (Const) {
        auto T = D.constant(C).Type;
        if (T != Constant::Integer && T != Constant::Number)
          D.bad("Expected numeric arithmetic constant");
      }
      Right = Imm ? std::to_string(Arg - 127) : Const ? D.k(C) : R(C);
      if (Flip)
        std::swap(Left, Right);
      Bin = SourceOp;
    } else
      Right = RK(C);
    D.set(A, Left + " " + Bin.str() + " " + Right);
  } else if (Op == "CALL" || Op == "TAILCALL")
    D.call(A, A + 1, B - 1, C - 1, Op == "TAILCALL");
  else if (Op == "RETURN" || Op == "RETURN0" || Op == "RETURN1")
    D.returns(A, Op == "RETURN" ? B - 1 : Op == "RETURN1" ? 1 : 0);
  else if (Op == "VARARG") {
    if (V != 0x55 || !K)
      D.varargs(A, (New ? C : B) - 1);
  } else if (Op == "VARARGPREP") {
    if (!P.Vararg || I.PC != 0)
      D.bad("Invalid vararg prologue");
    if (V != 0x55 || !(P.Flags & 2))
      D.statement("");
  } else if (Op == "JMP") {
    I.Target = Jump(W, I.PC);
    I.Kind = Form::Jump;
  } else if (Op == "TEST" || Op == "TESTSET" || Op == "EQ" || Op == "LT" ||
             Op == "LE" || Op == "EQK" || Op == "EQI" || Op == "LTI" ||
             Op == "LEI" || Op == "GTI" || Op == "GEI") {
    auto J = D.extra();
    if (mnemonic(Family::Puc, V, J) != "JMP")
      D.bad("Conditional lacks its jump");
    bool Test = Op.starts_with("TEST");
    std::string Condition;
    if (Test)
      Condition = R(Op == "TEST" ? A : B);
    else {
      auto L = New ? R(A) : RK(B);
      auto X = Op.ends_with("I") ? std::to_string(B - 127)
               : Op == "EQK"     ? D.k(B)
               : New             ? R(B)
                                 : RK(C);
      if (New && Op.ends_with("I") && C)
        X += ".0";
      auto Sym = Op.starts_with("EQ")   ? " == "
                 : Op.starts_with("LT") ? " < "
                 : Op.starts_with("LE") ? " <= "
                 : Op == "GTI"          ? " > "
                                        : " >= ";
      Condition = L + Sym + X;
    }
    D.branch(Condition, New ? K : bool(Test ? C : A), Jump(J, I.PC + 1));
    if (Op == "TESTSET") {
      I.Taken = R(A) + " = " + R(B);
      I.Writes.set(A);
    }
  } else if (Op == "FORPREP" || Op == "FORLOOP") {
    bool Prep = Op == "FORPREP";
    I.Kind = !Prep ? Form::ForLoop : New ? Form::ForPrep : Form::ForPrepJump;
    I.Base = A;
    I.Target = int(I.PC) + 1 + (New ? Prep ? Bx + 1 : -Bx : Bx - 131071);
    I.First = A + (V == 0x55 ? 2 : 3);
    R(I.First);
  } else if (Op == "TFORPREP") {
    I.Kind = Form::GenericPrep;
    I.Target = int(I.PC) + 1 + Bx;
    I.Base = A;
  } else if (Op == "TFORCALL") {
    uint32_t L = D.extra();
    int LA = (L >> (New ? 7 : 6)) & 255;
    if (mnemonic(Family::Puc, V, L) != "TFORLOOP" || LA != (New ? A : A + 2))
      D.bad("Invalid generic loop pair");
    I.Kind = Form::GenericLoop;
    I.Base = A;
    I.Results = C;
    I.First = A + (V == 0x54 ? 4 : 3);
    if (!C)
      D.bad("Empty iterator result window");
    R(I.First + C - 1);
    I.Target = int(I.PC) + 2 + (New ? -int(L >> 15) : int(L >> 14) - 131071);
  } else if (Op == "TFORLOOP" && V == 0x51) {
    auto J = D.extra();
    if (mnemonic(Family::Puc, V, J) != "JMP" || !C)
      D.bad("Invalid generic loop latch");
    I.Kind = Form::GenericLoop;
    I.Base = A;
    I.Results = C;
    I.First = A + 3;
    R(I.First + C - 1);
    I.Target = Jump(J, I.PC + 1);
  } else if (Op == "CLOSE" || Op == "TBC") {
    R(A);
    I.Kind = Op == "TBC" ? Form::MarkClose : Form::Close;
    I.Base = A;
  } else if (Op == "CLOSURE") {
    if (size_t(Bx) >= P.Children.size())
      D.bad("Invalid child prototype index");
    auto Id = P.Children[Bx];
    D.capture(Id, A);
    for (auto &U : D.C.Functions[Id].Up) {
      if (V == 0x51) {
        auto Bind = D.extra();
        auto Name = mnemonic(Family::Puc, V, Bind);
        int Slot = Bind >> 23;
        if (Name != "MOVE" && Name != "GETUPVAL")
          D.bad("Invalid closure binding instruction");
        I.Captures.push_back(Name == "MOVE" ? D.localCapture(Slot)
                                            : Capture{false, D.up(Slot)});
      } else
        I.Captures.push_back(U.Local ? D.localCapture(U.Index)
                                     : Capture{false, D.up(U.Index)});
    }
  } else if (Op == "EXTRAARG" || Op.starts_with("MMBIN"))
    D.bad("Orphan auxiliary instruction");
}
