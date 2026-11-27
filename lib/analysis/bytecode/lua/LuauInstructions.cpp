//===- LuauInstructions.cpp - Upstream Luau source operations ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// AUX data and closure captures occupy words, never independent jump
/// destinations.
//===----------------------------------------------------------------------===//

#include "Decode.h"

using namespace neverd::lua;
void neverd::lua::decodeLuau(Decoder &D) {
  auto &I = D.I;
  auto Op = I.Op;
  uint32_t W = D.P.Code[I.PC];
  int A = (W >> 8) & 255, B = (W >> 16) & 255, C = W >> 24,
      X = int16_t(W >> 16);
  int Target = int(I.PC) + 1 + X;
  bool Compare =
      Op.starts_with("JUMPIF") && Op != "JUMPIF" && Op != "JUMPIFNOT";
  bool Aux = Compare || Op.starts_with("JUMPXEQ") || Op == "GETGLOBAL" ||
             Op == "SETGLOBAL" || Op == "GETIMPORT" || Op == "GETTABLEKS" ||
             Op == "SETTABLEKS" || Op == "NAMECALL" || Op == "NEWTABLE" ||
             Op == "SETLIST" || Op == "FORGLOOP" || Op == "LOADKX" ||
             Op == "FASTCALL2" || Op == "FASTCALL2K" || Op == "FASTCALL3" ||
             Op == "GETUDATAKS" || Op == "SETUDATAKS" ||
             Op == "NAMECALLUDATA" || Op == "NEWCLASSMEMBER" ||
             Op == "CALLFB" || Op == "CMPPROTO";
  uint32_t Data = Aux ? D.extra() : 0;
  auto R = [&](int N) { return D.reg(N); };
  auto Field = [&]() -> const Constant & {
    auto &K = D.constant(Data);
    if (K.Type != Constant::String)
      D.bad("Expected string key");
    return K;
  };
  if (Op == "LOADNIL")
    D.set(A, "nil");
  else if (Op == "LOADN")
    D.set(A, std::to_string(X));
  else if (Op == "LOADB") {
    if (B > 1)
      D.bad("Invalid boolean operand");
    D.set(A, B ? "true" : "false");
    if (C) {
      I.Kind = Form::Jump;
      I.Target = I.PC + 1 + C;
    }
  } else if (Op == "MOVE")
    D.set(A, R(B));
  else if (Op == "LOADK" || Op == "LOADKX")
    D.set(A, D.k(Op == "LOADK" ? int(W >> 16) : int(Data)));
  else if (Op == "GETGLOBAL")
    D.set(A, D.global(Field()));
  else if (Op == "SETGLOBAL")
    D.statement(D.global(Field()) + " = " + R(A));
  else if (Op == "GETUPVAL")
    D.set(A, D.up(B));
  else if (Op == "SETUPVAL")
    D.statement(D.up(B) + " = " + R(A));
  else if (Op == "CLOSEUPVALS") {
    R(A);
    I.Kind = Form::Close;
    I.Base = A;
  } else if (Op == "GETIMPORT") {
    const auto &K = D.constant(X);
    if (K.Type != Constant::Import || K.I != Data)
      D.bad("Invalid import constant");
    // An import may be resolved at load time in a safe environment.
  } else if (Op.starts_with("GETTABLE") || Op.starts_with("SETTABLE")) {
    auto Key = Op.ends_with("KS")  ? D.literal(Field())
               : Op.ends_with("N") ? std::to_string(C + 1)
                                   : R(C);
    auto Item = R(B) + "[" + Key + "]";
    if (Op.starts_with("GET"))
      D.set(A, Item);
    else
      D.statement(Item + " = " + R(A));
  } else if (Op == "NEWTABLE")
    D.set(A, "{}");
  else if (Op == "DUPTABLE") {
    if (D.constant(X).Type != Constant::Table)
      D.bad("Invalid table template index");
  } else if (Op == "NAMECALL")
    Field();
  else if (Op == "NEWCLOSURE" || Op == "DUPCLOSURE") {
    unsigned Child;
    if (Op == "NEWCLOSURE") {
      if (X < 0 || size_t(X) >= D.P.Children.size())
        D.bad("Invalid child prototype index");
      Child = D.P.Children[X];
    } else {
      const auto &K = D.constant(X);
      if (K.Type != Constant::Closure)
        D.bad("Expected closure constant");
      Child = K.I;
    }
    D.capture(Child, A);
    for (size_t U = 0; U < D.C.Functions[Child].Up.size(); ++U) {
      uint32_t Bind = D.extra();
      int Type = (Bind >> 8) & 255, Index = (Bind >> 16) & 255;
      if (mnemonic(Family::Luau, D.C.Version, Bind) != "CAPTURE" || Type > 2)
        D.bad("Invalid closure capture");
      I.Captures.push_back(Type == 2 ? Capture{false, D.up(Index)}
                                     : D.localCapture(Index, Type == 0));
    }
    // This opcode can reuse an existing closure identity.
    if (Op == "DUPCLOSURE")
      I.Kind = Form::Unsupported;
  } else if (Op == "CAPTURE")
    D.bad("Orphan capture");
  else if (Op == "NOT" || Op == "MINUS" || Op == "LENGTH")
    D.set(A, (Op == "NOT" ? "not " : Op == "MINUS" ? "-" : "#") + R(B));
  else if (Op == "SUBRK" || Op == "DIVRK")
    D.set(A, D.k(B) + (Op == "SUBRK" ? " - " : " / ") + R(C));
  else if (auto Sym = binary(Op.rtrim("K")); !Sym.empty())
    D.set(A,
          R(B) + " " + Sym.str() + " " + (Op.ends_with("K") ? D.k(C) : R(C)));
  else if (Op == "CONCAT") {
    if (C <= B)
      D.bad("Invalid concatenation range");
    std::string S = R(B);
    for (int J = B + 1; J <= C; ++J)
      S += " .. " + R(J);
    D.set(A, S);
  } else if (Op == "CALL" || Op == "CALLFB") {
    if (Op == "CALLFB" && Data != UINT32_MAX &&
        Data >= D.P.FeedbackSlots.value_or(0))
      D.bad("Call feedback slot outside prototype");
    D.call(A, A + 1, B - 1, C - 1);
  } else if (Op == "GETVARARGS")
    D.varargs(A, B - 1);
  else if (Op == "PREPVARARGS") {
    if (!D.P.Vararg || I.PC || A != D.P.Params)
      D.bad("Invalid vararg prologue");
    D.statement("");
  } else if (Op == "RETURN")
    D.returns(A, B - 1);
  else if (Op == "JUMP" || Op == "JUMPBACK" || Op == "JUMPX") {
    I.Kind = Form::Jump;
    I.Target = Op == "JUMPX" ? int(I.PC) + 1 + (int32_t(W) >> 8) : Target;
  } else if (Op == "JUMPIF" || Op == "JUMPIFNOT")
    D.branch(R(A), Op == "JUMPIF", Target);
  else if (Op == "JUMPIFEQK" || Op == "JUMPIFNOTEQK")
    D.branch(R(A) + " == " + D.k(Data), Op == "JUMPIFEQK", Target);
  else if (Compare) {
    auto Sym = Op.ends_with("EQ")   ? " == "
               : Op.ends_with("LE") ? " <= "
                                    : " < ";
    D.branch(R(A) + Sym + R(Data), !Op.contains("NOT"), Target);
  } else if (Op.starts_with("JUMPXEQ")) {
    int Id = Data & 0xffffff;
    auto Val = Op.ends_with("NIL")  ? "nil"
               : Op.ends_with("KB") ? (Data & 1 ? "true" : "false")
                                    : D.k(Id);
    D.branch(R(A) + " == " + Val, !(Data >> 31), Target);
  } else if (Op == "FORNPREP" || Op == "FORNLOOP") {
    I.Kind = Op == "FORNPREP" ? Form::ForPrep : Form::ForLoop;
    I.Base = A;
    I.First = A + 2;
    R(I.First);
    I.Target = Target;
  } else if (Op.starts_with("FORGPREP")) {
    I.Kind = Form::GenericPrep;
    I.Base = A;
    I.Target = Target;
  } else if (Op == "FORGLOOP") {
    I.Kind = Form::GenericLoop;
    I.Results = Data & 255;
    I.Base = A;
    I.First = A + 3;
    I.Target = Target;
    if (!I.Results)
      D.bad("Empty iterator variable window");
    R(I.First + I.Results - 1);
  } else if (Op == "NOP")
    D.statement("");
}
