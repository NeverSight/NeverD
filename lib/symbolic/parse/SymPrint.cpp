//===- SymPrint.cpp - Rendering symbolic expressions ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements \c SymContext::toString.  The precedence ladder shared with the
/// parser lives in SymParseDetail.h so the two halves of the textual syntax
/// cannot drift after living in separate translation units.
///
//===----------------------------------------------------------------------===//

#include "../SymPrintShape.h"
#include "SymParseDetail.h"

#include "neverd/symbolic/Limits.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace neverd::symbolic {

namespace {

using namespace detail;

bool isTextIdentifier(llvm::StringRef Name) {
  return !Name.empty() && isIdentStart(Name.front()) &&
         llvm::all_of(Name.drop_front(), isIdentBody);
}

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

// Append directly to the parent's buffer. Materializing a wrapped copy for
// every child allocates and copies long MBA subexpressions twice.
void appendWrapped(std::string &Out, const Fragment &F, int Need) {
  if (F.Prec < Need)
    Out += '(';
  Out += F.Text;
  if (F.Prec < Need)
    Out += ')';
}

void appendDigits(std::string &Out, const llvm::APInt &V) {
  // The decimal spelling is bounded to four digits. All larger values use
  // the same limb emitter, including a single native word.
  if (V.ule(9999)) {
    char Digits[4];
    const auto End =
        std::to_chars(Digits, Digits + sizeof(Digits), V.getRawData()[0]).ptr;
    Out.append(Digits, End);
    return;
  }
  Out += "0x";
  const size_t Count = llvm::divideCeil(V.getActiveBits(), 4u);
  const size_t Start = Out.size();
  Out.resize(Start + Count);
  size_t End = Count;
  for (unsigned I = 0; End; ++I) {
    uint64_t Word = V.getRawData()[I];
    const size_t Begin = End - std::min<size_t>(End, 16);
    while (End > Begin) {
      Out[Start + --End] = llvm::hexdigit(Word & 15);
      Word >>= 4;
    }
  }
}

class Printer {
public:
  Printer(const SymContext &Ctx, uint32_t Ambient)
      : Ctx(Ctx), Ambient(Ambient) {}

  std::string run(SymRef Root);
  std::pair<std::string, std::string> runPair(SymRef Before, SymRef After);

private:
  void prepare(llvm::ArrayRef<SymRef> Roots);

  uint32_t slot(SymRef R) const {
    return Direct.empty() ? Memo.find(R.index())->second : Direct[R.index()];
  }
  const Fragment &frag(SymRef R) const { return Fragments[slot(R)]; }
  /// A child in a comma-separated argument list, which never needs brackets.
  const std::string &arg(SymRef R) const { return frag(R).Text; }
  std::string sub(SymRef R, int Need) const { return wrap(frag(R), Need); }

  void appendJoined(std::string &Text, llvm::ArrayRef<SymRef> Ops,
                    std::string_view Sep, int Need) const;
  std::string join(llvm::ArrayRef<SymRef> Ops, std::string_view Sep,
                   int Need) const;
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

  std::optional<Fragment> negatedLiteral(const llvm::APInt &V) const;
  std::string_view subtractedText(SymRef R) const;

  Fragment render(SymRef R) const;

  const SymContext &Ctx;
  uint32_t Ambient;
  // The discovery table also indexes fragment slots. Keeping strings out of
  // empty buckets reduces the table's footprint. Slots are sized once before
  // postorder rendering; completed child references remain stable.
  // Wide keys keep every valid node handle distinct from the sentinels.
  llvm::SmallDenseMap<uint64_t, uint32_t,
                      limits::symbolic::kSymExprTraversalInlineBuckets>
      Memo;
  llvm::SmallVector<uint32_t, 0> Direct;
  llvm::SmallVector<Fragment, 32> Fragments;
};

std::string Printer::join(llvm::ArrayRef<SymRef> Ops, std::string_view Sep,
                          int Need) const {
  std::string T;
  appendJoined(T, Ops, Sep, Need);
  return T;
}

void Printer::appendJoined(std::string &Text, llvm::ArrayRef<SymRef> Ops,
                           std::string_view Sep, int Need) const {
  // Long joins benefit from reserving once. Ordinary binary fragments
  // often fit in the inline string and only need one operand traversal.
  if (Ops.size() > 2) {
    size_t Length = Text.size() + (Ops.size() - 1) * Sep.size();
    for (auto Operand : Ops) {
      const auto &Child = frag(Operand);
      Length += Child.Text.size() + 2 * (Child.Prec < Need);
    }
    Text.reserve(Length);
  }
  for (size_t I = 0; I < Ops.size(); ++I) {
    if (I)
      Text += Sep;
    appendWrapped(Text, frag(Ops[I]), Need);
  }
}

std::string Printer::call(const char *Name, llvm::ArrayRef<SymRef> Ops) const {
  const std::string_view Function = Name;
  size_t Size = Function.size() + 2 * Ops.size();
  for (auto Operand : Ops)
    Size += arg(Operand).size();
  std::string T;
  T.reserve(Size);
  T += Function;
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
  if (!print_detail::hasSignedMagnitude(V))
    return std::nullopt;
  llvm::APInt Mag = -V;
  return literal(Mag, V.getBitWidth());
}

/// The already rendered magnitude when a sum can spell this child as a
/// subtraction. Only negation of a whole product adds an outer pair of
/// parentheses that is unnecessary after the binary minus.
std::string_view Printer::subtractedText(SymRef R) const {
  const std::string_view Text = arg(R);
  if (Ctx.isConst(R))
    return frag(R).Prec == PrecUnary ? Text.substr(1) : std::string_view{};
  if (Ctx.op(R) != SymOp::Mul)
    return {};
  const auto Coeff = Ctx.operand(R, 0);
  if (!Ctx.isConst(Coeff) || frag(Coeff).Prec != PrecUnary)
    return {};
  if (Ctx.numOperands(R) > 2 && Ctx.isConstOnes(Coeff))
    return Text.substr(2, Text.size() - 3);
  return Text.substr(1);
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
  case SymOp::Var: {
    const uint32_t Id = Ctx.varId(R);
    const SymVarInfo &Info = Ctx.varInfo(Id);
    if (Ctx.findVarRef(Info.Name) == R && isTextIdentifier(Info.Name))
      return leaf(Info.Name, N.Width);
    return {"varid(" + std::to_string(Id) + ")", PrecPrimary};
  }

  case SymOp::Add: {
    unsigned Lead = print_detail::leadingAddOperand(Ctx, Ops);
    std::string T;
    if (Ops.size() > 2) {
      size_t Size = 3 * (Ops.size() - 1);
      for (auto Operand : Ops)
        Size += arg(Operand).size();
      T.reserve(Size);
    }
    appendWrapped(T, frag(Ops[Lead]), PrecAdditive);
    for (unsigned I = 0, E = Ops.size(); I < E; ++I) {
      if (I == Lead)
        continue;
      if (const auto Magnitude = subtractedText(Ops[I]); !Magnitude.empty()) {
        T += " - ";
        T += Magnitude;
      } else {
        T += " + ";
        appendWrapped(T, frag(Ops[I]), PrecAdditive + 1);
      }
    }
    return {std::move(T), PrecAdditive};
  }
  case SymOp::Mul:
    if (print_detail::hasUnaryMinus(Ctx, R)) {
      // A unit negative coefficient negates the whole remaining product.
      // A single quotient or sum already gets brackets from the join.
      std::string Text = Ops.size() == 2 ? "-" : "-(";
      appendJoined(Text, Ops.drop_front(), " * ", PrecMultiplicative + 1);
      if (Ops.size() > 2)
        Text += ')';
      return {std::move(Text), PrecUnary};
    }
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
      return {sub(Cmp[0], PrecEquality) +
                  " != " + sub(Cmp[1], PrecEquality + 1),
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
    return {sub(Ops[0], PrecRelational) +
                " <= " + sub(Ops[1], PrecRelational + 1),
            PrecRelational};
  case SymOp::Slt:
    return {call("slt", Ops), PrecPrimary};
  case SymOp::Sle:
    return {call("sle", Ops), PrecPrimary};
  }
  llvm_unreachable("unhandled SymOp in render");
}

void Printer::prepare(llvm::ArrayRef<SymRef> Roots) {
  // Visit operands before completing their parent. Interned nodes form a DAG,
  // so an already visited operand is complete, never an active ancestor.
  // Iterative postorder avoids both recursion on deep expressions and sorting
  // the reachable set, while rendering each shared node only once.
  llvm::SmallVector<uint32_t, 64> Order;
  struct Frame {
    SymRef Ref;
    bool Expanded;
  };
  llvm::SmallVector<Frame, 64> Work;
  for (SymRef R : llvm::reverse(Roots))
    Work.push_back({R, false});
  const size_t Span = size_t(llvm::max_element(Roots)->index()) + 1;
  constexpr size_t SlotsPerEntry =
      sizeof(decltype(Memo)::value_type) / sizeof(uint32_t);
  const size_t DenseAfter =
      std::max<size_t>(limits::symbolic::kSymExprTraversalInlineBuckets + 1,
                       llvm::divideCeil(Span, SlotsPerEntry));
  constexpr auto Unseen = std::numeric_limits<uint32_t>::max();
  uint32_t Seen = 0;
  while (!Work.empty()) {
    auto [R, Expanded] = Work.pop_back_val();
    if (Expanded) {
      Order.push_back(R.index());
      continue;
    }
    if (!Direct.empty()) {
      auto &Position = Direct[R.index()];
      if (Position != Unseen)
        continue;
      Position = Seen++;
    } else {
      if (!Memo.try_emplace(R.index(), Seen).second)
        continue;
      if (++Seen == DenseAfter) {
        // Once the actually visited set pays for this index, the rest of
        // discovery and all fragment lookups can bypass the hash table.
        Direct.assign(Span, Unseen);
        for (const auto &[Id, Position] : Memo)
          Direct[Id] = Position;
      }
    }
    Work.push_back({R, true});
    for (SymRef C : Ctx.operands(R))
      Work.push_back({C, false});
  }

  Fragments.resize(Order.size());
  for (auto I : Order)
    Fragments[slot(SymRef(I))] = render(SymRef(I));
}

std::pair<std::string, std::string> Printer::runPair(SymRef Before,
                                                     SymRef After) {
  if (Before == After) {
    auto Text = run(Before);
    return {Text, std::move(Text)};
  }
  prepare({Before, After});
  return {std::move(Fragments.front().Text),
          std::move(Fragments[slot(After)].Text)};
}

std::string Printer::run(SymRef Root) {
  if (Ctx.operands(Root).empty())
    return render(Root).Text;
  prepare({Root});
  return std::move(Fragments.front().Text);
}

} // namespace

std::pair<std::string, std::string>
printSymExprPair(const SymContext &Ctx, SymRef Before, SymRef After) {
  if (!Before || !After || Ctx.width(Before) != Ctx.width(After))
    return {Ctx.toString(Before), Ctx.toString(After)};
  return Printer(Ctx, Ctx.width(Before)).runPair(Before, After);
}

std::string SymContext::toString(SymRef R) const {
  if (!R.isValid())
    return "<invalid>";
  return Printer(*this, width(R)).run(R);
}

} // namespace neverd::symbolic
