//===- SymParse.cpp - Textual syntax for symbolic expressions -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements both halves of the infix syntax: \c SymContext::toString and
/// \c parseSymExpr.
///
/// They live together because they are inverses, and the thing that makes them
/// inverses is the single precedence ladder declared at the top of this file.
/// The printer parenthesises exactly where the parser would otherwise regroup,
/// so a rule written by hand, printed back out, and read again is the same
/// expression each time.  Splitting them across translation units would let
/// one drift.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymParse.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neverd::symbolic {

namespace {

/// Binding strength, loosest first, following C.  A fragment records the
/// strength of its outermost operator; a parent wraps it in parentheses when
/// that is below what the position demands.  The parser climbs the same
/// numbers, which is what ties printing and parsing together.
enum Prec : int {
  PrecTernary = 1,
  PrecLogOr,
  PrecLogAnd,
  PrecBitOr,
  PrecBitXor,
  PrecBitAnd,
  PrecEquality,
  PrecRelational,
  PrecShift,
  PrecAdditive,
  PrecMultiplicative,
  PrecUnary,
  PrecPrimary,
};

/// Upper bound on a width written in the text.  Real targets top out at the
/// 256 bits of an EVM word; the bound only exists so that a typo cannot ask
/// for an \c APInt of a few hundred megabytes.
constexpr uint64_t kMaxWidth = 1u << 16;

//===----------------------------------------------------------------------===//
// Printing
//===----------------------------------------------------------------------===//

/// A rendered subexpression, tagged with the strength of its outermost
/// operator so the parent can decide about parentheses.
struct Fragment {
  std::string Text;
  int Prec = PrecPrimary;
};

std::string wrap(const Fragment &F, int Need) {
  if (F.Prec >= Need)
    return F.Text;
  return "(" + F.Text + ")";
}

void appendDigits(std::string &Out, const llvm::APInt &V) {
  llvm::SmallString<48> S;
  // Small numbers read as quantities and belong in decimal.  Larger ones are
  // nearly always masks or magic constants, where the hex form is the one that
  // carries the meaning.
  if (V.ule(9999)) {
    V.toString(S, 10, /*Signed=*/false);
  } else {
    Out += "0x";
    V.toString(S, 16, /*Signed=*/false);
  }
  Out.append(S.begin(), S.end());
}

class Printer {
public:
  Printer(const SymContext &Ctx, uint32_t Ambient)
      : Ctx(Ctx), Ambient(Ambient) {}

  std::string run(SymRef Root);

private:
  const Fragment &frag(SymRef R) const { return Memo.at(R.index()); }
  /// A child in a comma-separated argument list, which never needs brackets.
  const std::string &arg(SymRef R) const { return frag(R).Text; }
  std::string sub(SymRef R, int Need) const { return wrap(frag(R), Need); }

  std::string join(llvm::ArrayRef<SymRef> Ops, const char *Sep, int Need) const;
  std::string call(const char *Name, llvm::ArrayRef<SymRef> Ops) const;

  Fragment leaf(std::string Text, uint32_t W) const {
    if (W != Ambient) {
      Text += '#';
      Text += std::to_string(W);
    }
    return {std::move(Text), PrecPrimary};
  }

  Fragment literal(const llvm::APInt &Mag, uint32_t W) const {
    std::string T;
    appendDigits(T, Mag);
    return leaf(std::move(T), W);
  }

  /// The unsigned magnitude of a term that is worth writing behind a minus
  /// sign, and whether that sign can ride on a leading literal coefficient
  /// instead of governing the whole term.
  struct Magnitude {
    Fragment Frag;
    bool CoeffLed = false;
  };

  std::optional<Fragment> negatedLiteral(const llvm::APInt &V) const;
  std::optional<Magnitude> negatedProduct(SymRef R) const;
  std::optional<Magnitude> negatedTerm(SymRef R) const;
  std::string negate(const Magnitude &M) const;

  Fragment render(SymRef R) const;

  const SymContext &Ctx;
  uint32_t Ambient;
  std::unordered_map<uint32_t, Fragment> Memo;
};

std::string Printer::join(llvm::ArrayRef<SymRef> Ops, const char *Sep,
                          int Need) const {
  std::string T;
  for (unsigned I = 0, E = Ops.size(); I < E; ++I) {
    if (I)
      T += Sep;
    T += sub(Ops[I], Need);
  }
  return T;
}

std::string Printer::call(const char *Name, llvm::ArrayRef<SymRef> Ops) const {
  std::string T = Name;
  T += '(';
  for (unsigned I = 0, E = Ops.size(); I < E; ++I) {
    if (I)
      T += ", ";
    T += arg(Ops[I]);
  }
  T += ')';
  return T;
}

/// The magnitude of \p V, when writing it behind a minus sign is faithful.
std::optional<Fragment> Printer::negatedLiteral(const llvm::APInt &V) const {
  // At one bit, -1 and 1 are the same value and the minus sign only confuses.
  if (V.getBitWidth() == 1 || !V.isNegative())
    return std::nullopt;
  llvm::APInt Mag = -V;
  // The most negative value negates to itself, so there is no magnitude to
  // put after the sign.
  if (Mag.isNegative())
    return std::nullopt;
  return literal(Mag, V.getBitWidth());
}

/// The magnitude of a product with a negative leading coefficient, so a sum
/// prints `x - 2*y` rather than `x + 0xfffffffe*y`.  The caller writes the
/// sign.  `mkNeg` is multiplication by all-ones and `mkMul` folds the constant
/// factor back in, so both spellings intern to the same node.
std::optional<Printer::Magnitude> Printer::negatedProduct(SymRef R) const {
  llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);
  if (Ops.size() < 2 || !Ctx.isConst(Ops[0]))
    return std::nullopt;
  llvm::APInt C = Ctx.constValue(Ops[0]);
  std::optional<Fragment> Coeff = negatedLiteral(C);
  if (!Coeff)
    return std::nullopt;

  Magnitude M;
  M.CoeffLed = !(-C).isOne();
  llvm::ArrayRef<SymRef> Factors = Ops.drop_front();
  std::string T;
  if (M.CoeffLed) {
    T = Coeff->Text;
    T += " * ";
  }
  // Division sits at the same strength as multiplication but does not
  // associate with it, so a quotient among the factors has to be bracketed or
  // the chain regroups: `d * (b / c)` must not flatten to `d * b / c`.
  T += join(Factors, " * ", PrecMultiplicative + 1);

  // A unit coefficient over a single factor contributes no operator of its
  // own, so the magnitude is that factor's text and keeps its strength — or is
  // primary, if the join had to bracket it.
  int P = PrecMultiplicative;
  if (!M.CoeffLed && Factors.size() == 1) {
    int FactorPrec = frag(Factors[0]).Prec;
    P = FactorPrec > PrecMultiplicative ? FactorPrec : PrecPrimary;
  }
  M.Frag = {std::move(T), P};
  return M;
}

/// Split a term of a sum, or a product standing on its own, into a sign and a
/// magnitude.  Nothing is returned when the term reads better as written.
std::optional<Printer::Magnitude> Printer::negatedTerm(SymRef R) const {
  if (Ctx.isConst(R)) {
    if (std::optional<Fragment> Mag = negatedLiteral(Ctx.constValue(R)))
      return Magnitude{std::move(*Mag), /*CoeffLed=*/true};
  } else if (Ctx.op(R) == SymOp::Mul) {
    return negatedProduct(R);
  }
  return std::nullopt;
}

/// Write \p M with its sign, in a position where the minus is unary.
///
/// A leading literal coefficient absorbs the sign — `-2 * x` is `(-2) * x`,
/// and multiplication is flattened, so that is the same node.  Without one the
/// minus governs the whole term and has to bind at least as tightly as it
/// does, or `-(e / a)` would come back as `(-e) / a`.
std::string Printer::negate(const Magnitude &M) const {
  return "-" + (M.CoeffLed ? M.Frag.Text : wrap(M.Frag, PrecUnary));
}

Fragment Printer::render(SymRef R) const {
  const SymNode &N = Ctx.node(R);
  llvm::ArrayRef<SymRef> Ops = Ctx.operands(R);

  switch (N.Op) {
  case SymOp::Const: {
    llvm::APInt V = Ctx.constValue(R);
    if (std::optional<Fragment> Mag = negatedLiteral(V))
      return {"-" + Mag->Text, PrecUnary};
    return literal(V, N.Width);
  }
  case SymOp::Var:
    return leaf(Ctx.varInfo(Ctx.varId(R)).Name, N.Width);

  case SymOp::Add: {
    std::string T;
    for (unsigned I = 0, E = Ops.size(); I < E; ++I) {
      std::optional<Magnitude> Neg = negatedTerm(Ops[I]);
      if (I == 0) {
        // The leading sign is unary; every later one is the binary minus of
        // the sum itself, whose right operand only has to out-bind addition.
        T += Neg ? negate(*Neg) : wrap(frag(Ops[I]), PrecAdditive);
        continue;
      }
      T += Neg ? " - " : " + ";
      T += wrap(Neg ? Neg->Frag : frag(Ops[I]), PrecAdditive + 1);
    }
    return {std::move(T), PrecAdditive};
  }
  case SymOp::Mul:
    if (std::optional<Magnitude> Neg = negatedProduct(R))
      return {negate(*Neg),
              Neg->CoeffLed ? PrecMultiplicative : PrecUnary};
    return {join(Ops, " * ", PrecMultiplicative + 1), PrecMultiplicative};

  case SymOp::And:
    return {join(Ops, " & ", PrecBitAnd), PrecBitAnd};
  case SymOp::Or:
    return {join(Ops, " | ", PrecBitOr), PrecBitOr};
  case SymOp::Xor:
    return {join(Ops, " ^ ", PrecBitXor), PrecBitXor};

  case SymOp::Not:
    // `~(a == b)` is what mkNe leaves behind, and `a != b` is both shorter and
    // exactly what the parser rebuilds.
    if (Ctx.op(Ops[0]) == SymOp::Eq) {
      llvm::ArrayRef<SymRef> Cmp = Ctx.operands(Ops[0]);
      return {sub(Cmp[0], PrecEquality) + " != " + sub(Cmp[1], PrecEquality + 1),
              PrecEquality};
    }
    return {"~" + sub(Ops[0], PrecUnary), PrecUnary};

  case SymOp::Shl:
    return {sub(Ops[0], PrecShift) + " << " + sub(Ops[1], PrecShift + 1),
            PrecShift};
  case SymOp::LShr:
    return {sub(Ops[0], PrecShift) + " >> " + sub(Ops[1], PrecShift + 1),
            PrecShift};
  case SymOp::AShr:
    return {call("ashr", Ops), PrecPrimary};

  case SymOp::UDiv:
    return {sub(Ops[0], PrecMultiplicative) + " / " +
                sub(Ops[1], PrecMultiplicative + 1),
            PrecMultiplicative};
  case SymOp::URem:
    return {sub(Ops[0], PrecMultiplicative) + " % " +
                sub(Ops[1], PrecMultiplicative + 1),
            PrecMultiplicative};
  case SymOp::SDiv:
    return {call("sdiv", Ops), PrecPrimary};
  case SymOp::SRem:
    return {call("srem", Ops), PrecPrimary};
  case SymOp::Rol:
    return {call("rol", Ops), PrecPrimary};
  case SymOp::Ror:
    return {call("ror", Ops), PrecPrimary};

  case SymOp::Extract:
    return {"extract(" + arg(Ops[0]) + ", " + std::to_string(N.Aux) + ", " +
                std::to_string(N.Width) + ")",
            PrecPrimary};
  case SymOp::Concat:
    return {call("concat", Ops), PrecPrimary};
  case SymOp::ZExt:
    return {"zext(" + arg(Ops[0]) + ", " + std::to_string(N.Width) + ")",
            PrecPrimary};
  case SymOp::SExt:
    return {"sext(" + arg(Ops[0]) + ", " + std::to_string(N.Width) + ")",
            PrecPrimary};
  case SymOp::Ite:
    return {sub(Ops[0], PrecLogOr) + " ? " + sub(Ops[1], PrecTernary) + " : " +
                sub(Ops[2], PrecTernary),
            PrecTernary};

  case SymOp::Eq:
    return {sub(Ops[0], PrecEquality) + " == " + sub(Ops[1], PrecEquality + 1),
            PrecEquality};
  case SymOp::Ult:
    return {sub(Ops[0], PrecRelational) + " < " +
                sub(Ops[1], PrecRelational + 1),
            PrecRelational};
  case SymOp::Ule:
    return {sub(Ops[0], PrecRelational) + " <= " +
                sub(Ops[1], PrecRelational + 1),
            PrecRelational};
  case SymOp::Slt:
    return {call("slt", Ops), PrecPrimary};
  case SymOp::Sle:
    return {call("sle", Ops), PrecPrimary};
  }
  llvm_unreachable("unhandled SymOp in render");
}

std::string Printer::run(SymRef Root) {
  // Interning appends a node only once its operands exist, so ascending index
  // order over the reachable set reaches every child before its parent.  The
  // walk is iterative and the memo is keyed per node, so a DAG that an
  // obfuscator made deep or wide costs stack proportional to nothing and time
  // proportional to its node count rather than its tree size.
  std::vector<uint32_t> Order;
  llvm::DenseSet<uint32_t> Seen;
  llvm::SmallVector<SymRef, 64> Work{Root};
  while (!Work.empty()) {
    SymRef R = Work.pop_back_val();
    if (!Seen.insert(R.index()).second)
      continue;
    Order.push_back(R.index());
    for (SymRef C : Ctx.operands(R))
      Work.push_back(C);
  }
  llvm::sort(Order);

  Memo.reserve(Order.size());
  for (uint32_t I : Order)
    Memo.emplace(I, render(SymRef(I)));
  return Memo.at(Root.index()).Text;
}

//===----------------------------------------------------------------------===//
// Lexing
//===----------------------------------------------------------------------===//

struct Token {
  enum Kind { End, Ident, Number, Punct };
  Kind K = End;
  /// Spelling, for an identifier or a punctuator.
  llvm::StringRef Text;
  /// Digits with any radix prefix removed, for a number.
  llvm::StringRef Digits;
  unsigned Radix = 10;
  size_t Offset = 0;
};

bool isIdentStart(char C) {
  return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_';
}

/// Decompiler-generated names carry `.` and `$`, so an identifier admits them
/// rather than forcing every caller to quote.
bool isIdentBody(char C) {
  return isIdentStart(C) || (C >= '0' && C <= '9') || C == '.' || C == '$';
}

bool isDigit(char C) { return C >= '0' && C <= '9'; }

bool isRadixDigit(char C, unsigned Radix) {
  switch (Radix) {
  case 16:
    return isDigit(C) || (C >= 'a' && C <= 'f') || (C >= 'A' && C <= 'F');
  case 2:
    return C == '0' || C == '1';
  default:
    return isDigit(C);
  }
}

/// Punctuators, longest first, so that scanning them in order is maximal
/// munch: `<<` is found before `<`, and `!=` before `!`.
constexpr const char *kPuncts[] = {
    "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "?", ":", "|", "^", "&",
    "<",  ">",  "+",  "-",  "*",  "/",  "%",  "~",  "!", "(", ")", ",", "#",
};

class Lexer {
public:
  explicit Lexer(llvm::StringRef T) : Text(T) { advance(); }

  const Token &peek() const { return Cur; }
  Token take() {
    Token T = Cur;
    advance();
    return T;
  }

private:
  void advance();

  llvm::StringRef Text;
  size_t Pos = 0;
  Token Cur;
};

void Lexer::advance() {
  while (Pos < Text.size() && std::isspace(static_cast<unsigned char>(Text[Pos])))
    ++Pos;

  Cur = Token{};
  Cur.Offset = Pos;
  if (Pos >= Text.size())
    return;

  char C = Text[Pos];

  if (isIdentStart(C)) {
    size_t Start = Pos;
    while (Pos < Text.size() && isIdentBody(Text[Pos]))
      ++Pos;
    Cur.K = Token::Ident;
    Cur.Text = Text.slice(Start, Pos);
    return;
  }

  if (isDigit(C)) {
    unsigned Radix = 10;
    if (C == '0' && Pos + 1 < Text.size()) {
      char P = Text[Pos + 1];
      if (P == 'x' || P == 'X')
        Radix = 16;
      else if (P == 'b' || P == 'B')
        Radix = 2;
      if (Radix != 10)
        Pos += 2;
    }
    size_t Start = Pos;
    while (Pos < Text.size() && isRadixDigit(Text[Pos], Radix))
      ++Pos;
    Cur.K = Token::Number;
    Cur.Digits = Text.slice(Start, Pos);
    Cur.Radix = Radix;
    return;
  }

  for (const char *P : kPuncts) {
    llvm::StringRef S(P);
    if (Text.substr(Pos).starts_with(S)) {
      Cur.K = Token::Punct;
      Cur.Text = Text.slice(Pos, Pos + S.size());
      Pos += S.size();
      return;
    }
  }

  // Anything else comes back as a one-character punctuator, so the parser
  // reports it as unexpected at the right offset instead of looping.
  Cur.K = Token::Punct;
  Cur.Text = Text.slice(Pos, Pos + 1);
  ++Pos;
}

//===----------------------------------------------------------------------===//
// Parsing
//===----------------------------------------------------------------------===//

class Parser {
public:
  Parser(SymContext &Ctx, llvm::StringRef Text, uint32_t Width)
      : Ctx(Ctx), Lex(Text), Width(Width) {}

  SymParseResult run();

private:
  /// Recursion is bounded so that pathologically nested text is a diagnostic
  /// rather than a crash.  Precedence climbing keeps a flat chain like
  /// `a+b+c+...` in one frame, so only bracket and unary nesting counts here.
  static constexpr unsigned kMaxDepth = 512;

  class DepthGuard {
  public:
    DepthGuard(Parser &P, size_t Offset) : P(P) {
      if (++P.Depth > kMaxDepth)
        P.fail(Offset, "expression nests too deeply");
    }
    ~DepthGuard() { --P.Depth; }

  private:
    Parser &P;
  };

  SymRef parseExpr() { return parseTernary(); }
  SymRef parseTernary();
  SymRef parseBinary(int MinPrec);
  SymRef parseUnary();
  SymRef parsePrimary();
  SymRef parseCall(const Token &Name);
  SymRef parseVariadicCall(const Token &Name);
  SymRef parseWidthCall(const Token &Name);
  SymRef makeLiteral(const Token &N);

  bool parsePlainInt(uint64_t &Out);
  uint32_t parseWidthSuffix(uint32_t Default);

  SymRef applyBinary(llvm::StringRef Spelling, SymRef A, SymRef B);
  SymRef applyVariadic(const Token &Name, llvm::MutableArrayRef<SymRef> Args);

  /// Widen the narrower operand so a mixed-width expression reads the way C
  /// would read it.  Every builder requires its operands to agree.
  void unify(SymRef &A, SymRef &B) {
    uint32_t WA = Ctx.width(A), WB = Ctx.width(B);
    if (WA < WB)
      A = Ctx.mkZExt(A, WB);
    else if (WB < WA)
      B = Ctx.mkZExt(B, WA);
  }
  void unifyAll(llvm::MutableArrayRef<SymRef> Args);

  /// Reduce a value to the single bit a condition has to be.
  SymRef toBool(SymRef A) {
    uint32_t W = Ctx.width(A);
    return W == 1 ? A : Ctx.mkNe(A, Ctx.mkZero(W));
  }

  SymRef makeIte(SymRef C, SymRef T, SymRef E) {
    unify(T, E);
    return Ctx.mkIte(toBool(C), T, E);
  }

  bool isPunct(llvm::StringRef S) const {
    return Lex.peek().K == Token::Punct && Lex.peek().Text == S;
  }
  bool consumePunct(llvm::StringRef S) {
    if (!isPunct(S))
      return false;
    Lex.take();
    return true;
  }
  bool expectPunct(llvm::StringRef S) {
    if (consumePunct(S))
      return true;
    fail(Lex.peek().Offset, ("expected '" + S + "'").str());
    return false;
  }

  void fail(size_t Offset, const llvm::Twine &Msg) {
    if (Failed)
      return;
    Failed = true;
    Error = Msg.str();
    ErrorOffset = Offset;
  }

  SymContext &Ctx;
  Lexer Lex;
  uint32_t Width;
  unsigned Depth = 0;
  bool Failed = false;
  std::string Error;
  size_t ErrorOffset = 0;
};

SymParseResult Parser::run() {
  SymParseResult Result;
  SymRef Root = parseExpr();
  if (!Failed && Lex.peek().K != Token::End)
    fail(Lex.peek().Offset,
         "unexpected '" + Lex.peek().Text + "' after the expression");
  if (Failed) {
    Result.Error = std::move(Error);
    Result.ErrorOffset = ErrorOffset;
    return Result;
  }
  Result.Root = Root;
  return Result;
}

SymRef Parser::parseTernary() {
  SymRef C = parseBinary(PrecLogOr);
  if (Failed || !isPunct("?"))
    return C;

  size_t Offset = Lex.peek().Offset;
  Lex.take();
  DepthGuard Guard(*this, Offset);
  if (Failed)
    return {};
  SymRef T = parseTernary();
  if (Failed || !expectPunct(":"))
    return {};
  SymRef E = parseTernary();
  if (Failed)
    return {};
  return makeIte(C, T, E);
}

int binaryPrec(llvm::StringRef S) {
  if (S == "||")
    return PrecLogOr;
  if (S == "&&")
    return PrecLogAnd;
  if (S == "|")
    return PrecBitOr;
  if (S == "^")
    return PrecBitXor;
  if (S == "&")
    return PrecBitAnd;
  if (S == "==" || S == "!=")
    return PrecEquality;
  if (S == "<" || S == "<=" || S == ">" || S == ">=")
    return PrecRelational;
  if (S == "<<" || S == ">>")
    return PrecShift;
  if (S == "+" || S == "-")
    return PrecAdditive;
  if (S == "*" || S == "/" || S == "%")
    return PrecMultiplicative;
  return 0;
}

SymRef Parser::parseBinary(int MinPrec) {
  SymRef L = parseUnary();
  if (Failed)
    return {};

  while (Lex.peek().K == Token::Punct) {
    llvm::StringRef S = Lex.peek().Text;
    int P = binaryPrec(S);
    if (P == 0 || P < MinPrec)
      break;
    Lex.take();
    // Every infix operator here is left-associative, so the right side stops
    // at the next operator of equal strength.
    SymRef R = parseBinary(P + 1);
    if (Failed)
      return {};
    L = applyBinary(S, L, R);
    if (Failed)
      return {};
  }
  return L;
}

SymRef Parser::applyBinary(llvm::StringRef S, SymRef A, SymRef B) {
  // The short-circuit forms are the only ones that read their operands as
  // conditions rather than as words.
  if (S == "&&")
    return Ctx.mkAnd(toBool(A), toBool(B));
  if (S == "||")
    return Ctx.mkOr(toBool(A), toBool(B));

  unify(A, B);
  if (S == "|")
    return Ctx.mkOr(A, B);
  if (S == "^")
    return Ctx.mkXor(A, B);
  if (S == "&")
    return Ctx.mkAnd(A, B);
  if (S == "==")
    return Ctx.mkEq(A, B);
  if (S == "!=")
    return Ctx.mkNe(A, B);
  if (S == "<")
    return Ctx.mkUlt(A, B);
  if (S == "<=")
    return Ctx.mkUle(A, B);
  if (S == ">")
    return Ctx.mkUgt(A, B);
  if (S == ">=")
    return Ctx.mkUge(A, B);
  if (S == "<<")
    return Ctx.mkShl(A, B);
  if (S == ">>")
    return Ctx.mkLShr(A, B);
  if (S == "+")
    return Ctx.mkAdd(A, B);
  if (S == "-")
    return Ctx.mkSub(A, B);
  if (S == "*")
    return Ctx.mkMul(A, B);
  if (S == "/")
    return Ctx.mkUDiv(A, B);
  if (S == "%")
    return Ctx.mkURem(A, B);
  llvm_unreachable("binaryPrec accepted an operator applyBinary does not know");
}

SymRef Parser::parseUnary() {
  const Token &T = Lex.peek();
  if (T.K == Token::Punct &&
      (T.Text == "~" || T.Text == "-" || T.Text == "!" || T.Text == "+")) {
    llvm::StringRef S = T.Text;
    size_t Offset = T.Offset;
    Lex.take();
    DepthGuard Guard(*this, Offset);
    if (Failed)
      return {};
    SymRef A = parseUnary();
    if (Failed)
      return {};
    if (S == "+")
      return A;
    if (S == "-")
      return Ctx.mkNeg(A);
    if (S == "~")
      return Ctx.mkNot(A);
    // `!` is logical, so at any width above one it asks whether the value is
    // zero rather than complementing its bits.
    uint32_t W = Ctx.width(A);
    return W == 1 ? Ctx.mkNot(A) : Ctx.mkEq(A, Ctx.mkZero(W));
  }
  return parsePrimary();
}

SymRef Parser::parsePrimary() {
  const Token &T = Lex.peek();
  switch (T.K) {
  case Token::End:
    fail(T.Offset, "expected an expression");
    return {};

  case Token::Number:
    return makeLiteral(Lex.take());

  case Token::Ident: {
    Token Id = Lex.take();
    if (isPunct("("))
      return parseCall(Id);

    uint32_t W = parseWidthSuffix(Width);
    if (Failed)
      return {};
    // mkVar asserts on a width change, and text from a user is exactly where
    // that would fire, so the clash becomes a diagnostic here instead.
    if (std::optional<uint32_t> Existing = Ctx.findVar(Id.Text)) {
      uint32_t Have = Ctx.varInfo(*Existing).Width;
      if (Have != W) {
        fail(Id.Offset, "'" + Id.Text + "' was already declared at width " +
                            llvm::Twine(Have));
        return {};
      }
    }
    return Ctx.mkVar(Id.Text, W);
  }

  case Token::Punct: {
    if (T.Text == "(") {
      size_t Offset = T.Offset;
      Lex.take();
      DepthGuard Guard(*this, Offset);
      if (Failed)
        return {};
      SymRef E = parseExpr();
      if (Failed || !expectPunct(")"))
        return {};
      return E;
    }
    fail(T.Offset, "unexpected '" + T.Text + "'");
    return {};
  }
  }
  llvm_unreachable("unhandled token kind");
}

SymRef Parser::makeLiteral(const Token &N) {
  if (N.Digits.empty()) {
    fail(N.Offset, "malformed numeric literal");
    return {};
  }
  uint32_t W = parseWidthSuffix(Width);
  if (Failed)
    return {};

  unsigned Needed = llvm::APInt::getBitsNeeded(N.Digits, N.Radix);
  llvm::APInt V(std::max(Needed, W), N.Digits, N.Radix);
  return Ctx.mkConst(V.zextOrTrunc(W));
}

uint32_t Parser::parseWidthSuffix(uint32_t Default) {
  if (!isPunct("#"))
    return Default;
  size_t Offset = Lex.peek().Offset;
  Lex.take();
  uint64_t W = 0;
  if (!parsePlainInt(W))
    return Default;
  if (W == 0 || W > kMaxWidth) {
    fail(Offset, "a width must be between 1 and " + llvm::Twine(kMaxWidth));
    return Default;
  }
  return static_cast<uint32_t>(W);
}

bool Parser::parsePlainInt(uint64_t &Out) {
  const Token &T = Lex.peek();
  if (T.K != Token::Number || T.Digits.empty()) {
    fail(T.Offset, "expected a bit count");
    return false;
  }
  Token N = Lex.take();
  if (N.Digits.getAsInteger(N.Radix, Out)) {
    fail(N.Offset, "bit count is out of range");
    return false;
  }
  return true;
}

/// True for the forms whose trailing arguments are bit positions rather than
/// expressions.  They are parsed as plain integers because a bit count is not
/// a value in the expression's word size — `zext(x, 256)` has to mean 256 even
/// when the surrounding width is 8.
bool takesBitCounts(llvm::StringRef F) {
  return F == "zext" || F == "sext" || F == "trunc" || F == "extract";
}

SymRef Parser::parseCall(const Token &Name) {
  DepthGuard Guard(*this, Name.Offset);
  if (Failed)
    return {};
  Lex.take(); // '('
  return takesBitCounts(Name.Text) ? parseWidthCall(Name)
                                   : parseVariadicCall(Name);
}

SymRef Parser::parseWidthCall(const Token &Name) {
  llvm::StringRef F = Name.Text;
  SymRef A = parseExpr();
  if (Failed)
    return {};

  unsigned NumCounts = F == "extract" ? 2 : 1;
  uint64_t Counts[2] = {0, 0};
  size_t Offsets[2] = {0, 0};
  for (unsigned I = 0; I < NumCounts; ++I) {
    if (!expectPunct(","))
      return {};
    Offsets[I] = Lex.peek().Offset;
    if (!parsePlainInt(Counts[I]))
      return {};
  }
  if (!expectPunct(")"))
    return {};

  uint32_t AW = Ctx.width(A);
  if (F == "extract") {
    uint64_t Low = Counts[0], W = Counts[1];
    if (W == 0 || W > kMaxWidth || Low > kMaxWidth || Low + W > AW) {
      fail(Offsets[0], "extract window does not fit in a " + llvm::Twine(AW) +
                           "-bit operand");
      return {};
    }
    return Ctx.mkExtract(A, static_cast<uint32_t>(Low),
                         static_cast<uint32_t>(W));
  }

  uint64_t W = Counts[0];
  if (W == 0 || W > kMaxWidth) {
    fail(Offsets[0], "a width must be between 1 and " + llvm::Twine(kMaxWidth));
    return {};
  }
  if (F == "trunc") {
    if (W > AW) {
      fail(Offsets[0], "trunc must not widen a " + llvm::Twine(AW) +
                           "-bit operand");
      return {};
    }
    return Ctx.mkExtract(A, 0, static_cast<uint32_t>(W));
  }
  if (W < AW) {
    fail(Offsets[0],
         F + " must not narrow a " + llvm::Twine(AW) + "-bit operand");
    return {};
  }
  return F == "zext" ? Ctx.mkZExt(A, static_cast<uint32_t>(W))
                     : Ctx.mkSExt(A, static_cast<uint32_t>(W));
}

SymRef Parser::parseVariadicCall(const Token &Name) {
  llvm::SmallVector<SymRef, 4> Args;
  if (!isPunct(")")) {
    do {
      SymRef A = parseExpr();
      if (Failed)
        return {};
      Args.push_back(A);
    } while (consumePunct(","));
  }
  if (!expectPunct(")"))
    return {};
  return applyVariadic(Name, Args);
}

void Parser::unifyAll(llvm::MutableArrayRef<SymRef> Args) {
  uint32_t W = 0;
  for (SymRef A : Args)
    W = std::max(W, Ctx.width(A));
  for (SymRef &A : Args)
    if (Ctx.width(A) != W)
      A = Ctx.mkZExt(A, W);
}

SymRef Parser::applyVariadic(const Token &Name,
                             llvm::MutableArrayRef<SymRef> Args) {
  llvm::StringRef F = Name.Text;
  size_t N = Args.size();

  auto arity = [&](size_t Want) {
    if (N == Want)
      return true;
    fail(Name.Offset, F + " takes " + llvm::Twine(Want) + " argument" +
                          (Want == 1 ? "" : "s") + ", got " + llvm::Twine(N));
    return false;
  };

  if (F == "add" || F == "mul" || F == "and" || F == "or" || F == "xor" ||
      F == "concat") {
    if (N == 0) {
      fail(Name.Offset, F + " needs at least one argument");
      return {};
    }
    // Concatenation is the one variadic that means to keep its operands at
    // different widths.
    if (F == "concat")
      return Ctx.mkConcat(Args);
    unifyAll(Args);
    if (F == "add")
      return Ctx.mkAdd(Args);
    if (F == "mul")
      return Ctx.mkMul(Args);
    if (F == "and")
      return Ctx.mkAnd(Args);
    if (F == "or")
      return Ctx.mkOr(Args);
    return Ctx.mkXor(Args);
  }

  if (F == "not" || F == "neg") {
    if (!arity(1))
      return {};
    return F == "not" ? Ctx.mkNot(Args[0]) : Ctx.mkNeg(Args[0]);
  }

  if (F == "ite") {
    if (!arity(3))
      return {};
    return makeIte(Args[0], Args[1], Args[2]);
  }

  if (F == "sub" || F == "udiv" || F == "sdiv" || F == "urem" || F == "srem" ||
      F == "shl" || F == "lshr" || F == "ashr" || F == "rol" || F == "ror" ||
      F == "eq" || F == "ne" || F == "ult" || F == "ule" || F == "ugt" ||
      F == "uge" || F == "slt" || F == "sle" || F == "sgt" || F == "sge") {
    if (!arity(2))
      return {};
    SymRef A = Args[0], B = Args[1];
    unify(A, B);
    if (F == "sub")
      return Ctx.mkSub(A, B);
    if (F == "udiv")
      return Ctx.mkUDiv(A, B);
    if (F == "sdiv")
      return Ctx.mkSDiv(A, B);
    if (F == "urem")
      return Ctx.mkURem(A, B);
    if (F == "srem")
      return Ctx.mkSRem(A, B);
    if (F == "shl")
      return Ctx.mkShl(A, B);
    if (F == "lshr")
      return Ctx.mkLShr(A, B);
    if (F == "ashr")
      return Ctx.mkAShr(A, B);
    if (F == "rol")
      return Ctx.mkRol(A, B);
    if (F == "ror")
      return Ctx.mkRor(A, B);
    if (F == "eq")
      return Ctx.mkEq(A, B);
    if (F == "ne")
      return Ctx.mkNe(A, B);
    if (F == "ult")
      return Ctx.mkUlt(A, B);
    if (F == "ule")
      return Ctx.mkUle(A, B);
    if (F == "ugt")
      return Ctx.mkUgt(A, B);
    if (F == "uge")
      return Ctx.mkUge(A, B);
    if (F == "slt")
      return Ctx.mkSlt(A, B);
    if (F == "sle")
      return Ctx.mkSle(A, B);
    if (F == "sgt")
      return Ctx.mkSgt(A, B);
    return Ctx.mkSge(A, B);
  }

  fail(Name.Offset, "unknown function '" + F + "'");
  return {};
}

} // namespace

std::string SymContext::toString(SymRef R) const {
  if (!R.isValid())
    return "<invalid>";
  return Printer(*this, width(R)).run(R);
}

SymParseResult parseSymExpr(SymContext &Ctx, llvm::StringRef Text,
                            uint32_t Width) {
  if (Width == 0 || Width > kMaxWidth) {
    SymParseResult R;
    R.Error = ("a width of " + llvm::Twine(Width) + " is out of range").str();
    return R;
  }
  return Parser(Ctx, Text, Width).run();
}

} // namespace neverd::symbolic
