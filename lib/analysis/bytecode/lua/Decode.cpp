//===- Decode.cpp - Checked Lua source operands ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Qualified encodings have dialect-specific opcode numbers and one source
/// operand contract.
//===----------------------------------------------------------------------===//

#include "Decode.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"

using namespace neverd::lua;
namespace {
// Numeric wire order. A version selects a whole dictionary before any word is
// decoded.
auto names(llvm::StringRef Text) {
  llvm::SmallVector<llvm::StringRef, 96> List;
  Text.split(List, ' ');
  return List;
}
} // namespace
llvm::StringRef neverd::lua::mnemonic(Family F, int V, uint32_t W) {
  static const auto P51 =
      names("MOVE LOADK LOADBOOL LOADNIL GETUPVAL GETGLOBAL GETTABLE SETGLOBAL "
            "SETUPVAL SETTABLE NEWTABLE SELF ADD SUB MUL DIV MOD POW UNM NOT "
            "LEN CONCAT JMP EQ LT LE TEST TESTSET CALL TAILCALL RETURN FORLOOP "
            "FORPREP TFORLOOP SETLIST CLOSE CLOSURE VARARG");
  static const auto P52 = names(
      "MOVE LOADK LOADKX LOADBOOL LOADNIL GETUPVAL GETTABUP GETTABLE SETTABUP "
      "SETUPVAL SETTABLE NEWTABLE SELF ADD SUB MUL DIV MOD POW UNM NOT LEN "
      "CONCAT JMP EQ LT LE TEST TESTSET CALL TAILCALL RETURN FORLOOP FORPREP "
      "TFORCALL TFORLOOP SETLIST CLOSURE VARARG EXTRAARG");
  static const auto P53 =
      names("MOVE LOADK LOADKX LOADBOOL LOADNIL GETUPVAL GETTABUP GETTABLE "
            "SETTABUP SETUPVAL SETTABLE NEWTABLE SELF ADD SUB MUL MOD POW DIV "
            "IDIV BAND BOR BXOR SHL SHR UNM BNOT NOT LEN CONCAT JMP EQ LT LE "
            "TEST TESTSET CALL TAILCALL RETURN FORLOOP FORPREP TFORCALL "
            "TFORLOOP SETLIST CLOSURE VARARG EXTRAARG");
  static const auto P54 = names(
      "MOVE LOADI LOADF LOADK LOADKX LOADFALSE LFALSESKIP LOADTRUE LOADNIL "
      "GETUPVAL SETUPVAL GETTABUP GETTABLE GETI GETFIELD SETTABUP SETTABLE "
      "SETI SETFIELD NEWTABLE SELF ADDI ADDK SUBK MULK MODK POWK DIVK IDIVK "
      "BANDK BORK BXORK SHRI SHLI ADD SUB MUL MOD POW DIV IDIV BAND BOR BXOR "
      "SHL SHR MMBIN MMBINI MMBINK UNM BNOT NOT LEN CONCAT CLOSE TBC JMP EQ LT "
      "LE EQK EQI LTI LEI GTI GEI TEST TESTSET CALL TAILCALL RETURN RETURN0 "
      "RETURN1 FORLOOP FORPREP TFORPREP TFORCALL TFORLOOP SETLIST CLOSURE "
      "VARARG VARARGPREP EXTRAARG");
  static const auto P55 = names(
      "MOVE LOADI LOADF LOADK LOADKX LOADFALSE LFALSESKIP LOADTRUE LOADNIL "
      "GETUPVAL SETUPVAL GETTABUP GETTABLE GETI GETFIELD SETTABUP SETTABLE "
      "SETI SETFIELD NEWTABLE SELF ADDI ADDK SUBK MULK MODK POWK DIVK IDIVK "
      "BANDK BORK BXORK SHLI SHRI ADD SUB MUL MOD POW DIV IDIV BAND BOR BXOR "
      "SHL SHR MMBIN MMBINI MMBINK UNM BNOT NOT LEN CONCAT CLOSE TBC JMP EQ LT "
      "LE EQK EQI LTI LEI GTI GEI TEST TESTSET CALL TAILCALL RETURN RETURN0 "
      "RETURN1 FORLOOP FORPREP TFORPREP TFORCALL TFORLOOP SETLIST CLOSURE "
      "VARARG GETVARG ERRNNIL VARARGPREP EXTRAARG");
  static const auto J = names(
      "ISLT ISGE ISLE ISGT ISEQV ISNEV ISEQS ISNES ISEQN ISNEN ISEQP ISNEP "
      "ISTC ISFC IST ISF ISTYPE ISNUM MOV NOT UNM LEN ADDVN SUBVN MULVN DIVVN "
      "MODVN ADDNV SUBNV MULNV DIVNV MODNV ADDVV SUBVV MULVV DIVVV MODVV POW "
      "CAT KSTR KCDATA KSHORT KNUM KPRI KNIL UGET USETV USETS USETN USETP UCLO "
      "FNEW TNEW TDUP GGET GSET TGETV TGETS TGETB TGETR TSETV TSETS TSETB "
      "TSETM TSETR CALLM CALL CALLMT CALLT ITERC ITERN VARG ISNEXT RETM RET "
      "RET0 RET1 FORI JFORI FORL IFORL JFORL ITERL IITERL JITERL LOOP ILOOP "
      "JLOOP JMP BNOT BAND BOR BXOR BSHL BSHR BSAR");
  static const auto L = names(
      "NOP BREAK LOADNIL LOADB LOADN LOADK MOVE GETGLOBAL SETGLOBAL GETUPVAL "
      "SETUPVAL CLOSEUPVALS GETIMPORT GETTABLE SETTABLE GETTABLEKS SETTABLEKS "
      "GETTABLEN SETTABLEN NEWCLOSURE NAMECALL CALL RETURN JUMP JUMPBACK "
      "JUMPIF JUMPIFNOT JUMPIFEQ JUMPIFLE JUMPIFLT JUMPIFNOTEQ JUMPIFNOTLE "
      "JUMPIFNOTLT ADD SUB MUL DIV MOD POW ADDK SUBK MULK DIVK MODK POWK AND "
      "OR ANDK ORK CONCAT NOT MINUS LENGTH NEWTABLE DUPTABLE SETLIST FORNPREP "
      "FORNLOOP FORGLOOP FORGPREP_INEXT FASTCALL3 FORGPREP_NEXT NATIVECALL "
      "GETVARARGS DUPCLOSURE PREPVARARGS LOADKX JUMPX FASTCALL COVERAGE "
      "CAPTURE SUBRK DIVRK FASTCALL1 FASTCALL2 FASTCALL2K FORGPREP JUMPXEQKNIL "
      "JUMPXEQKB JUMPXEQKN JUMPXEQKS IDIV IDIVK");
  const auto &Dict = F == Family::Luau  ? L
                     : F == Family::Jit ? J
                     : V == 0x51        ? P51
                     : V == 0x52        ? P52
                     : V == 0x53        ? P53
                     : V == 0x54        ? P54
                                        : P55;
  auto Op = W & (F != Family::Puc ? 255 : V < 0x54 ? 63 : 127);
  return Op < Dict.size() ? Dict[Op] : llvm::StringRef();
}
llvm::StringRef neverd::lua::binary(llvm::StringRef Op) {
  return llvm::StringSwitch<llvm::StringRef>(Op)
      .Case("ADD", "+")
      .Case("SUB", "-")
      .Case("MUL", "*")
      .Case("MOD", "%")
      .Case("POW", "^")
      .Case("DIV", "/")
      .Case("IDIV", "//")
      .Case("BAND", "&")
      .Case("BOR", "|")
      .Case("BXOR", "~")
      .Case("SHL", "<<")
      .Case("SHR", ">>")
      .Case("AND", "and")
      .Case("OR", "or")
      .Default("");
}
void Decoder::bad(std::string Why) const {
  malformed(P.CodeAt + 4 * I.PC, std::move(Why));
}
std::string Decoder::reg(int N) const {
  if (N < 0 || unsigned(N) >= P.Stack)
    bad("Register outside prototype frame");
  I.Registers.set(N);
  return "r" + std::to_string(P.Id) + "_" + std::to_string(N);
}
std::string Decoder::up(int N) const {
  if (N < 0 || size_t(N) >= Up.size())
    bad("Invalid upvalue index");
  return Up[N];
}
const Constant &Decoder::constant(int N, bool GC) const {
  auto &Pool = GC ? P.GC : P.K;
  if (N < 0 || size_t(N) >= Pool.size())
    bad("Invalid constant index");
  return Pool[N];
}
std::string Decoder::literal(const Constant &K) const {
  switch (K.Type) {
  case Constant::Nil:
    return "nil";
  case Constant::Boolean:
    return K.I ? "true" : "false";
  case Constant::Integer:
  case Constant::Number: {
    auto S = K.Type == Constant::Integer
                 ? integer(K.I)
                 : number(K.F, C.Dialect == Family::Puc && C.Version >= 0x53);
    // A negative literal on the left of exponentiation needs parentheses.
    if (!S.empty())
      return S.front() == '-' ? "(" + S + ")" : S;
    break;
  }
  case Constant::String:
    C.Job.use(C.Job.Text, 4 * K.Bytes.size() + 2, P.CodeAt + 4 * I.PC);
    return quote(K.Bytes);
  default:
    break;
  }
  unsupported(P.CodeAt + 4 * I.PC, "Constant has no qualified source literal");
}
std::string Decoder::global(const Constant &K) const {
  if (K.Type != Constant::String)
    bad("Global name must be a string");
  auto S = K.Bytes;
  // Only an unshadowed dialect identifier can spell a 5.1-style environment
  // access.
  auto Letter = [](char C) {
    return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_';
  };
  static const auto Keywords =
      names("and break do else elseif end false for function goto if in local "
            "nil not or repeat return then true until while continue");
  if (S.empty() || !Letter(S.front()) ||
      !llvm::all_of(
          S, [&](char C) { return Letter(C) || (C >= '0' && C <= '9'); }) ||
      llvm::is_contained(Keywords, S) ||
      ((S.front() == 'r' || S.front() == 'u' || S.front() == 'v') &&
       S.size() > 1 && S[1] >= '0' && S[1] <= '9'))
    unsupported(P.CodeAt + 4 * I.PC,
                "Global key cannot be expressed as an unshadowed identifier");
  C.Job.use(C.Job.Text, S.size(), P.CodeAt + 4 * I.PC);
  return S.str();
}
uint32_t Decoder::extra() {
  if (I.PC + I.Width >= P.Code.size())
    bad("Truncated auxiliary instruction");
  return P.Code[I.PC + I.Width++];
}
void Decoder::set(int A, std::string Value) {
  bool Read = A >= 0 && A < 256 && I.Registers.test(A);
  auto Name = reg(A);
  if (!Read)
    I.Fresh.set(A);
  I.Writes.set(A);
  statement(Name + " = " + Value);
}
void Decoder::statement(std::string S) {
  I.Kind = Form::Statement;
  I.Text = std::move(S);
}
void Decoder::branch(std::string Condition, bool WhenTrue, int Target) {
  I.Kind = Form::Branch;
  I.Text = WhenTrue ? std::move(Condition) : "not (" + Condition + ")";
  I.Target = Target;
}
void Decoder::call(int Base, int First, int Args, int Results, bool Tail) {
  reg(Base);
  for (int N = 0; N < Args; ++N)
    reg(First + N);
  if (!Tail)
    for (int N = 0; N < Results; ++N) {
      reg(Base + N);
      I.Writes.set(Base + N);
    }
  I.Kind = Tail ? Form::TailCall : Form::Call;
  I.Base = Base;
  I.First = First;
  I.Args = Args;
  I.Results = Results;
}
void Decoder::returns(int First, int N) {
  for (int J = 0; J < N; ++J)
    reg(First + J);
  I.Kind = Form::Return;
  I.First = First;
  I.Args = N;
}
void Decoder::varargs(int A, int N) {
  if (!P.Vararg)
    bad("Vararg read in fixed-arity prototype");
  reg(A);
  for (int J = 0; J < N; ++J) {
    reg(A + J);
    I.Writes.set(A + J);
  }
  I.Kind = Form::Vararg;
  I.Base = A;
  I.Results = N;
}
void Decoder::nils(int A, int End) {
  if (A > End)
    bad("Invalid nil range");
  std::string S = reg(A);
  for (int N = A + 1; N <= End; ++N)
    S += ", " + reg(N);
  for (int N = A; N <= End; ++N) {
    I.Writes.set(N);
    I.Fresh.set(N);
  }
  statement(S + " = nil");
}
void Decoder::capture(unsigned Child, int A) {
  if (Child >= C.Functions.size())
    bad("Invalid child prototype index");
  I.Kind = Form::Closure;
  I.Base = Child;
  I.Text = reg(A);
  I.Writes.set(A);
}
