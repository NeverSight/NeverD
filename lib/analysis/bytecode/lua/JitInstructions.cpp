//===- JitInstructions.cpp - LuaJIT v2 source operations ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reads LuaJIT's split constant pools and FR-dependent call windows.
//===----------------------------------------------------------------------===//

#include "Decode.h"

using namespace neverd::lua;
void neverd::lua::decodeJit(Decoder &D) {
  auto &I = D.I;
  auto Op = I.Op;
  uint32_t W = D.P.Code[I.PC];
  int A = (W >> 8) & 255, B = W >> 24, C = (W >> 16) & 255, X = W >> 16;
  int Target = int(I.PC) + 1 + X - 32768;
  auto R = [&](int N) { return D.reg(N); };
  auto G = [&](int N, Constant::Kind Type) -> const Constant & {
    auto &V = D.constant(N, true);
    if (V.Type != Type)
      D.bad("Invalid collectable constant kind");
    return V;
  };
  auto Pri = [&](int N) -> std::string {
    if (N > 2)
      D.bad("Invalid primitive constant");
    return N == 0 ? "nil" : N == 1 ? "false" : "true";
  };
  if (Op == "MOV")
    D.set(A, R(X));
  else if (Op == "NOT" || Op == "UNM" || Op == "LEN")
    D.set(A, (Op == "NOT" ? "not " : Op == "UNM" ? "-" : "#") + R(X));
  else if (Op == "KSTR")
    D.set(A, D.literal(G(X, Constant::String)));
  else if (Op == "KSHORT")
    D.set(A, std::to_string(int16_t(X)));
  else if (Op == "KNUM")
    D.set(A, D.k(X));
  else if (Op == "KPRI")
    D.set(A, Pri(X));
  else if (Op == "KNIL")
    D.nils(A, X);
  else if (Op == "TNEW")
    D.set(A, "{}");
  else if (Op == "TDUP")
    G(X, Constant::Table);
  else if (Op == "KCDATA")
    G(X, Constant::CData);
  else if (Op == "GGET")
    D.set(A, D.global(G(X, Constant::String)));
  else if (Op == "GSET")
    D.statement(D.global(G(X, Constant::String)) + " = " + R(A));
  else if (Op == "UGET")
    D.set(A, D.up(X));
  else if (Op.starts_with("USET")) {
    auto Val = Op == "USETV"   ? R(X)
               : Op == "USETP" ? Pri(X)
               : Op == "USETN" ? D.k(X)
                               : D.literal(G(X, Constant::String));
    D.statement(D.up(A) + " = " + Val);
  } else if (Op == "FNEW") {
    unsigned Child = G(X, Constant::Closure).I;
    D.capture(Child, A);
    for (auto U : D.C.Functions[Child].Up)
      I.Captures.push_back(U.Local ? D.localCapture(U.Index)
                                   : Capture{false, D.up(U.Index)});
  } else if (Op == "TGETV" || Op == "TGETS" || Op == "TGETB" || Op == "TSETV" ||
             Op == "TSETS" || Op == "TSETB") {
    auto Key = Op.ends_with("V")   ? R(C)
               : Op.ends_with("B") ? std::to_string(C)
                                   : D.literal(G(C, Constant::String));
    auto Item = R(B) + "[" + Key + "]";
    if (Op.starts_with("TGET"))
      D.set(A, Item);
    else
      D.statement(Item + " = " + R(A));
  } else if (Op == "POW" || Op.ends_with("VN") || Op.ends_with("NV") ||
             Op.ends_with("VV")) {
    auto Sym = binary(Op.take_front(3)).str();
    auto L = R(B), H = Op == "POW" || Op.ends_with("VV") ? R(C) : D.k(C);
    if (Op.ends_with("NV"))
      std::swap(L, H);
    D.set(A, L + " " + Sym + " " + H);
  } else if (Op == "CAT") {
    if (C <= B)
      D.bad("Invalid concatenation range");
    std::string S = R(B);
    for (int J = B + 1; J <= C; ++J)
      S += " .. " + R(J);
    D.set(A, S);
  } else if (Op == "CALL" || Op == "CALLM" || Op == "CALLT" || Op == "CALLMT") {
    bool Tail = Op.ends_with("T"), Open = Op.contains("M");
    int Start = A + 1 + D.C.FR2, Count = Tail ? X : C;
    if (!Open && !Count)
      D.bad("Invalid fixed LuaJIT call window");
    D.call(A, Start, Open ? -1 : Count - 1, B - 1, Tail);
    if (Open)
      I.OpenAt = Start + Count;
  } else if (Op == "VARG")
    D.varargs(A, B - 1);
  else if (Op.starts_with("RET")) {
    int N = Op == "RETM" ? -1 : Op == "RET0" ? 0 : Op == "RET1" ? 1 : X - 1;
    D.returns(A, N);
    if (Op == "RETM")
      I.OpenAt = A + X;
  } else if (Op == "JMP" || Op == "UCLO") {
    I.Target = Target;
    I.Kind = Form::Jump;
    if (Op == "UCLO") {
      if (Target < 0 || size_t(Target) >= D.P.Code.size())
        D.bad("Invalid close destination");
      auto Destination = mnemonic(Family::Jit, D.C.Version, D.P.Code[Target]);
      if (!Destination.starts_with("RET") && Destination != "CALLT" &&
          Destination != "CALLMT")
        I.Kind = Form::Unsupported;
    }
  } else if (Op == "ITERC") {
    uint32_t L = D.extra();
    if (mnemonic(Family::Jit, D.C.Version, L) != "ITERL" ||
        ((L >> 8) & 255) != A || C != 3 || B < 2 || A < 3)
      D.bad("Invalid iterator latch");
    I.Kind = Form::GenericLoop;
    I.Base = A - 3;
    I.First = A;
    I.Results = B - 1;
    R(A + B - 2);
    I.Target = int(I.PC) + 2 + int(L >> 16) - 32768;
  } else if (Op == "FORI" || Op == "FORL") {
    I.Kind = Op == "FORI" ? Form::ForPrep : Form::ForLoop;
    I.Base = A;
    I.First = A + 3;
    R(I.First);
    I.Target = Target;
  } else if (Op.starts_with("IS") && Op != "ISNEXT" && Op != "ISTYPE" &&
             Op != "ISNUM") {
    auto J = D.extra();
    if (mnemonic(Family::Jit, D.C.Version, J) != "JMP")
      D.bad("Test lacks its jump");
    int Dest = int(I.PC) + 2 + int(J >> 16) - 32768;
    if (Op == "IST" || Op == "ISF" || Op == "ISTC" || Op == "ISFC") {
      D.branch(R(X), Op.starts_with("IST"), Dest);
      if (Op.ends_with("C")) {
        I.Taken = R(A) + " = " + R(X);
        I.Writes.set(A);
      }
    } else {
      auto Val = Op.ends_with("S")   ? D.literal(G(X, Constant::String))
                 : Op.ends_with("N") ? D.k(X)
                 : Op.ends_with("P") ? Pri(X)
                                     : R(X);
      bool EQ = Op.starts_with("ISEQ") || Op.starts_with("ISNE");
      auto Sym = EQ ? " == " : Op == "ISLT" || Op == "ISGE" ? " < " : " <= ";
      D.branch(R(A) + Sym + Val,
               !(Op.starts_with("ISNE") || Op == "ISGE" || Op == "ISGT"), Dest);
    }
  }
}
