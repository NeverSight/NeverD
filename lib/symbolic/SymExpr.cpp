//===- SymExpr.cpp - Building symbolic bitvector expressions --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the interning arena, canonicalizing builders, inspection and
/// substitution.
///
/// Division and shift behaviour follows SMT-LIB QF_BV rather than any one
/// machine's: a shift by at least the operand width yields zero (or the sign
/// for an arithmetic right shift), `bvudiv x 0` is all-ones, `bvsdiv x 0` is
/// -1 or 1 by the sign of x, and the remainders by zero return x.  Matching
/// the theory keeps a future solver bridge free of correction terms; lifters
/// that need a machine's trapping or undefined behaviour must model it in the
/// expression they build rather than rely on these totalized results.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <map>

namespace neverd::symbolic {

namespace {

/// Mix one word into a running hash.  The constant is the golden-ratio odd
/// integer used by boost::hash_combine and llvm::hash_combine alike.
inline uint64_t mixHash(uint64_t H, uint64_t V) {
  H ^= V + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2);
  return H;
}

/// Key an arbitrary-width literal by width and raw words so equal literals
/// share one pool slot.
std::string wideConstKey(const llvm::APInt &V) {
  std::string Key;
  Key.reserve(16 + V.getNumWords() * 20);
  Key += std::to_string(V.getBitWidth());
  for (unsigned I = 0, E = V.getNumWords(); I < E; ++I) {
    Key += ':';
    Key += std::to_string(V.getRawData()[I]);
  }
  return Key;
}

} // namespace

const char *symOpName(SymOp Op) {
  switch (Op) {
  case SymOp::Const:
    return "const";
  case SymOp::Var:
    return "var";
  case SymOp::Add:
    return "add";
  case SymOp::Mul:
    return "mul";
  case SymOp::And:
    return "and";
  case SymOp::Or:
    return "or";
  case SymOp::Xor:
    return "xor";
  case SymOp::Not:
    return "not";
  case SymOp::Shl:
    return "shl";
  case SymOp::LShr:
    return "lshr";
  case SymOp::AShr:
    return "ashr";
  case SymOp::UDiv:
    return "udiv";
  case SymOp::SDiv:
    return "sdiv";
  case SymOp::URem:
    return "urem";
  case SymOp::SRem:
    return "srem";
  case SymOp::Rol:
    return "rol";
  case SymOp::Ror:
    return "ror";
  case SymOp::Extract:
    return "extract";
  case SymOp::Concat:
    return "concat";
  case SymOp::ZExt:
    return "zext";
  case SymOp::SExt:
    return "sext";
  case SymOp::Ite:
    return "ite";
  case SymOp::Eq:
    return "eq";
  case SymOp::Ult:
    return "ult";
  case SymOp::Ule:
    return "ule";
  case SymOp::Slt:
    return "slt";
  case SymOp::Sle:
    return "sle";
  }
  return "?";
}

SymContext::SymContext() {
  Nodes.reserve(1024);
  OperandPool.reserve(2048);
}

//===----------------------------------------------------------------------===//
// Interning
//===----------------------------------------------------------------------===//

SymRef SymContext::intern(SymOp Op, uint32_t Width, llvm::ArrayRef<SymRef> Ops,
                          uint64_t Aux) {
  assert(Width > 0 && "a bitvector node must have a nonzero width");

  uint64_t H = mixHash(0x1234567u, static_cast<uint64_t>(Op));
  H = mixHash(H, Width);
  H = mixHash(H, Aux);
  for (SymRef R : Ops)
    H = mixHash(H, R.index());

  auto &Bucket = InternTable[H];
  for (uint32_t Cand : Bucket) {
    const SymNode &N = Nodes[Cand];
    if (N.Op != Op || N.Width != Width || N.Aux != Aux ||
        N.NumOperands != Ops.size())
      continue;
    bool Same = true;
    for (uint32_t I = 0; I < N.NumOperands; ++I) {
      if (OperandPool[N.FirstOperand + I] != Ops[I]) {
        Same = false;
        break;
      }
    }
    if (Same)
      return SymRef(Cand);
  }

  SymNode N;
  N.Op = Op;
  N.Width = Width;
  N.Aux = Aux;
  N.FirstOperand = static_cast<uint32_t>(OperandPool.size());
  N.NumOperands = static_cast<uint32_t>(Ops.size());
  OperandPool.insert(OperandPool.end(), Ops.begin(), Ops.end());

  auto Index = static_cast<uint32_t>(Nodes.size());
  Nodes.push_back(N);
  Bucket.push_back(Index);
  return SymRef(Index);
}

llvm::APInt SymContext::maskToWidth(const llvm::APInt &V,
                                    uint32_t Width) const {
  return V.getBitWidth() == Width ? V : V.zextOrTrunc(Width);
}

//===----------------------------------------------------------------------===//
// Leaves
//===----------------------------------------------------------------------===//

SymRef SymContext::mkConst(const llvm::APInt &Val) {
  uint32_t W = Val.getBitWidth();
  if (W <= 64)
    return intern(SymOp::Const, W, {}, Val.getZExtValue());

  std::string Key = wideConstKey(Val);
  auto It = WideConstByKey.find(Key);
  uint32_t Slot;
  if (It != WideConstByKey.end()) {
    Slot = It->second;
  } else {
    Slot = static_cast<uint32_t>(WideConsts.size());
    WideConsts.push_back(Val);
    WideConstByKey.emplace(std::move(Key), Slot);
  }
  return intern(SymOp::Const, W, {}, Slot);
}

SymRef SymContext::mkConst(uint32_t Width, uint64_t Val) {
  // Every operation here is modulo 2^Width and a literal is no exception, so a
  // value with bits above the width keeps only the ones that fit.  APInt's
  // constructor does not do that on its own: left to itself it asserts in a
  // debug build and stores the unreduced word in a release one, which would
  // put a node in the table whose value disagrees with its own width.
  return mkConst(
      llvm::APInt(Width, Val, /*isSigned=*/false, /*implicitTrunc=*/true));
}

SymRef SymContext::mkOnes(uint32_t Width) {
  return mkConst(llvm::APInt::getAllOnes(Width));
}

llvm::APInt SymContext::constValue(SymRef R) const {
  const SymNode &N = Nodes[R.index()];
  assert(N.Op == SymOp::Const && "constValue on a non-constant");
  if (N.Width <= 64)
    return llvm::APInt(N.Width, N.Aux);
  return WideConsts[N.Aux];
}

std::optional<llvm::APInt> SymContext::asConst(SymRef R) const {
  if (!isConst(R))
    return std::nullopt;
  return constValue(R);
}

bool SymContext::isConstZero(SymRef R) const {
  return isConst(R) && constValue(R).isZero();
}

bool SymContext::isConstOnes(SymRef R) const {
  return isConst(R) && constValue(R).isAllOnes();
}

SymRef SymContext::mkVar(llvm::StringRef Name, uint32_t Width) {
  std::string Key = Name.str();
  auto It = VarByName.find(Key);
  if (It != VarByName.end()) {
    assert(Vars[It->second].Width == Width &&
           "a variable was redeclared at a different width");
    return intern(SymOp::Var, Width, {}, It->second);
  }
  auto Id = static_cast<uint32_t>(Vars.size());
  Vars.push_back(SymVarInfo{Key, Width});
  VarByName.emplace(std::move(Key), Id);
  return intern(SymOp::Var, Width, {}, Id);
}

std::optional<uint32_t> SymContext::findVar(llvm::StringRef Name) const {
  auto It = VarByName.find(Name.str());
  if (It == VarByName.end())
    return std::nullopt;
  return It->second;
}

SymRef SymContext::mkFreshVar(uint32_t Width, llvm::StringRef Prefix) {
  std::string Name;
  do {
    Name = (Prefix + llvm::Twine(FreshCounter++)).str();
  } while (VarByName.count(Name));
  return mkVar(Name, Width);
}

//===----------------------------------------------------------------------===//
// Arithmetic
//===----------------------------------------------------------------------===//

void SymContext::splitCoefficient(SymRef R, llvm::APInt &Coeff,
                                  SymRef &Base) const {
  uint32_t W = width(R);
  if (op(R) == SymOp::Mul) {
    llvm::ArrayRef<SymRef> Ops = operands(R);
    if (!Ops.empty() && isConst(Ops[0])) {
      Coeff = constValue(Ops[0]);
      if (Ops.size() == 2) {
        Base = Ops[1];
        return;
      }
      // A product of three or more factors keeps its non-constant tail as the
      // base.  Rebuilding it here would intern, which the caller is not
      // prepared for, so the tail is reconstructed by the caller instead.
      Base = SymRef();
      return;
    }
  }
  Coeff = llvm::APInt(W, 1);
  Base = R;
}

SymRef SymContext::mkAdd(llvm::ArrayRef<SymRef> Ops) {
  assert(!Ops.empty() && "mkAdd needs at least one operand");
  uint32_t W = width(Ops[0]);

  llvm::APInt ConstTerm(W, 0);
  // Ordered by base node index so the rebuilt operand list is canonical.
  std::map<uint32_t, llvm::APInt> Terms;

  llvm::SmallVector<SymRef, 8> Work(Ops.begin(), Ops.end());
  while (!Work.empty()) {
    SymRef R = Work.pop_back_val();
    assert(width(R) == W && "mkAdd operands must share a width");

    if (op(R) == SymOp::Add) {
      llvm::ArrayRef<SymRef> Sub = operands(R);
      Work.append(Sub.begin(), Sub.end());
      continue;
    }
    if (isConst(R)) {
      ConstTerm += constValue(R);
      continue;
    }

    llvm::APInt Coeff(W, 1);
    SymRef Base;
    splitCoefficient(R, Coeff, Base);
    if (!Base.isValid()) {
      // Product with three or more factors: rebuild the non-constant tail.
      llvm::SmallVector<SymRef, 4> Tail(operands(R).begin() + 1,
                                        operands(R).end());
      Base = mkMul(Tail);
    }

    auto It = Terms.find(Base.index());
    if (It == Terms.end())
      Terms.emplace(Base.index(), Coeff);
    else
      It->second += Coeff;
  }

  llvm::SmallVector<SymRef, 8> Final;
  if (!ConstTerm.isZero())
    Final.push_back(mkConst(ConstTerm));
  for (const auto &[BaseIdx, Coeff] : Terms) {
    if (Coeff.isZero())
      continue;
    SymRef Base(BaseIdx);
    Final.push_back(Coeff.isOne() ? Base : mkMul(mkConst(Coeff), Base));
  }

  if (Final.empty())
    return mkZero(W);
  if (Final.size() == 1)
    return Final[0];
  return intern(SymOp::Add, W, Final, 0);
}

SymRef SymContext::mkSub(SymRef A, SymRef B) { return mkAdd(A, mkNeg(B)); }

SymRef SymContext::mkNeg(SymRef A) {
  uint32_t W = width(A);
  return mkMul(mkConst(llvm::APInt::getAllOnes(W)), A);
}

SymRef SymContext::mkMul(llvm::ArrayRef<SymRef> Ops) {
  assert(!Ops.empty() && "mkMul needs at least one operand");
  uint32_t W = width(Ops[0]);

  llvm::APInt ConstFactor(W, 1);
  llvm::SmallVector<SymRef, 8> Rest;

  llvm::SmallVector<SymRef, 8> Work(Ops.begin(), Ops.end());
  while (!Work.empty()) {
    SymRef R = Work.pop_back_val();
    assert(width(R) == W && "mkMul operands must share a width");
    if (op(R) == SymOp::Mul) {
      llvm::ArrayRef<SymRef> Sub = operands(R);
      Work.append(Sub.begin(), Sub.end());
      continue;
    }
    if (isConst(R)) {
      ConstFactor *= constValue(R);
      continue;
    }
    Rest.push_back(R);
  }

  if (ConstFactor.isZero())
    return mkZero(W);
  if (Rest.empty())
    return mkConst(ConstFactor);

  std::sort(Rest.begin(), Rest.end());

  if (ConstFactor.isOne()) {
    if (Rest.size() == 1)
      return Rest[0];
    return intern(SymOp::Mul, W, Rest, 0);
  }

  llvm::SmallVector<SymRef, 8> Final;
  Final.push_back(mkConst(ConstFactor));
  Final.append(Rest.begin(), Rest.end());
  return intern(SymOp::Mul, W, Final, 0);
}

SymRef SymContext::mkUDiv(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(A) && isConst(B)) {
    llvm::APInt D = constValue(B);
    return mkConst(D.isZero() ? llvm::APInt::getAllOnes(W)
                              : constValue(A).udiv(D));
  }
  if (isConst(B) && constValue(B).isOne())
    return A;
  return intern(SymOp::UDiv, W, {A, B}, 0);
}

SymRef SymContext::mkSDiv(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(A) && isConst(B)) {
    llvm::APInt N = constValue(A), D = constValue(B);
    if (D.isZero())
      return mkConst(N.isNonNegative() ? llvm::APInt::getAllOnes(W)
                                       : llvm::APInt(W, 1));
    // The single overflowing case, INT_MIN / -1, wraps back to INT_MIN.
    if (N.isMinSignedValue() && D.isAllOnes())
      return mkConst(N);
    return mkConst(N.sdiv(D));
  }
  if (isConst(B) && constValue(B).isOne())
    return A;
  return intern(SymOp::SDiv, W, {A, B}, 0);
}

SymRef SymContext::mkURem(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(A) && isConst(B)) {
    llvm::APInt D = constValue(B);
    return mkConst(D.isZero() ? constValue(A) : constValue(A).urem(D));
  }
  if (isConst(B) && constValue(B).isOne())
    return mkZero(W);
  return intern(SymOp::URem, W, {A, B}, 0);
}

SymRef SymContext::mkSRem(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(A) && isConst(B)) {
    llvm::APInt N = constValue(A), D = constValue(B);
    if (D.isZero())
      return mkConst(N);
    if (N.isMinSignedValue() && D.isAllOnes())
      return mkZero(W);
    return mkConst(N.srem(D));
  }
  if (isConst(B) && constValue(B).isOne())
    return mkZero(W);
  return intern(SymOp::SRem, W, {A, B}, 0);
}

//===----------------------------------------------------------------------===//
// Bitwise
//===----------------------------------------------------------------------===//

/// Shared shape for And/Or/Xor: flatten the nested same-operator operands,
/// fold the constants into one, then let the caller apply the operator's own
/// absorbing, idempotent and complement laws.
SymRef SymContext::mkAnd(llvm::ArrayRef<SymRef> Ops) {
  assert(!Ops.empty());
  uint32_t W = width(Ops[0]);

  llvm::APInt Acc = llvm::APInt::getAllOnes(W);
  llvm::SmallVector<SymRef, 8> Rest;
  llvm::SmallVector<SymRef, 8> Work(Ops.begin(), Ops.end());
  while (!Work.empty()) {
    SymRef R = Work.pop_back_val();
    assert(width(R) == W && "mkAnd operands must share a width");
    if (op(R) == SymOp::And) {
      llvm::ArrayRef<SymRef> Sub = operands(R);
      Work.append(Sub.begin(), Sub.end());
      continue;
    }
    if (isConst(R)) {
      Acc &= constValue(R);
      continue;
    }
    Rest.push_back(R);
  }

  if (Acc.isZero())
    return mkZero(W);

  std::sort(Rest.begin(), Rest.end());
  Rest.erase(std::unique(Rest.begin(), Rest.end()), Rest.end());

  // x & ~x == 0.  Testing the Not operands against the set avoids interning a
  // complement just to look it up.
  for (SymRef R : Rest) {
    if (op(R) != SymOp::Not)
      continue;
    if (std::binary_search(Rest.begin(), Rest.end(), operand(R, 0)))
      return mkZero(W);
  }

  if (Rest.empty())
    return mkConst(Acc);

  if (Acc.isAllOnes()) {
    if (Rest.size() == 1)
      return Rest[0];
    return intern(SymOp::And, W, Rest, 0);
  }

  llvm::SmallVector<SymRef, 8> Final;
  Final.push_back(mkConst(Acc));
  Final.append(Rest.begin(), Rest.end());
  return intern(SymOp::And, W, Final, 0);
}

SymRef SymContext::mkOr(llvm::ArrayRef<SymRef> Ops) {
  assert(!Ops.empty());
  uint32_t W = width(Ops[0]);

  llvm::APInt Acc(W, 0);
  llvm::SmallVector<SymRef, 8> Rest;
  llvm::SmallVector<SymRef, 8> Work(Ops.begin(), Ops.end());
  while (!Work.empty()) {
    SymRef R = Work.pop_back_val();
    assert(width(R) == W && "mkOr operands must share a width");
    if (op(R) == SymOp::Or) {
      llvm::ArrayRef<SymRef> Sub = operands(R);
      Work.append(Sub.begin(), Sub.end());
      continue;
    }
    if (isConst(R)) {
      Acc |= constValue(R);
      continue;
    }
    Rest.push_back(R);
  }

  if (Acc.isAllOnes())
    return mkConst(Acc);

  std::sort(Rest.begin(), Rest.end());
  Rest.erase(std::unique(Rest.begin(), Rest.end()), Rest.end());

  // x | ~x == -1.
  for (SymRef R : Rest) {
    if (op(R) != SymOp::Not)
      continue;
    if (std::binary_search(Rest.begin(), Rest.end(), operand(R, 0)))
      return mkOnes(W);
  }

  if (Rest.empty())
    return mkConst(Acc);

  if (Acc.isZero()) {
    if (Rest.size() == 1)
      return Rest[0];
    return intern(SymOp::Or, W, Rest, 0);
  }

  llvm::SmallVector<SymRef, 8> Final;
  Final.push_back(mkConst(Acc));
  Final.append(Rest.begin(), Rest.end());
  return intern(SymOp::Or, W, Final, 0);
}

SymRef SymContext::mkXor(llvm::ArrayRef<SymRef> Ops) {
  assert(!Ops.empty());
  uint32_t W = width(Ops[0]);

  llvm::APInt Acc(W, 0);
  llvm::SmallVector<SymRef, 8> Flat;
  llvm::SmallVector<SymRef, 8> Work(Ops.begin(), Ops.end());
  while (!Work.empty()) {
    SymRef R = Work.pop_back_val();
    assert(width(R) == W && "mkXor operands must share a width");
    if (op(R) == SymOp::Xor) {
      llvm::ArrayRef<SymRef> Sub = operands(R);
      Work.append(Sub.begin(), Sub.end());
      continue;
    }
    if (isConst(R)) {
      Acc ^= constValue(R);
      continue;
    }
    Flat.push_back(R);
  }

  // x ^ x == 0, so only the parity of each operand's multiplicity survives.
  std::sort(Flat.begin(), Flat.end());
  llvm::SmallVector<SymRef, 8> Rest;
  for (size_t I = 0; I < Flat.size();) {
    size_t J = I;
    while (J < Flat.size() && Flat[J] == Flat[I])
      ++J;
    if ((J - I) & 1)
      Rest.push_back(Flat[I]);
    I = J;
  }

  // x ^ ~x == -1.  Each such pair contributes all-ones to the constant and
  // drops both operands.
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (size_t I = 0; I < Rest.size(); ++I) {
      if (op(Rest[I]) != SymOp::Not)
        continue;
      SymRef Inner = operand(Rest[I], 0);
      auto It = std::find(Rest.begin(), Rest.end(), Inner);
      if (It == Rest.end())
        continue;
      Acc ^= llvm::APInt::getAllOnes(W);
      Rest.erase(Rest.begin() + I);
      Rest.erase(std::find(Rest.begin(), Rest.end(), Inner));
      Changed = true;
      break;
    }
  }

  if (Rest.empty())
    return mkConst(Acc);

  // x ^ -1 == ~x: prefer the complement, which the bitwise laws above and the
  // MBA solver's boolean domain both recognise.
  if (Acc.isAllOnes()) {
    SymRef Inner = Rest.size() == 1 ? Rest[0] : intern(SymOp::Xor, W, Rest, 0);
    return mkNot(Inner);
  }

  if (Acc.isZero()) {
    if (Rest.size() == 1)
      return Rest[0];
    return intern(SymOp::Xor, W, Rest, 0);
  }

  llvm::SmallVector<SymRef, 8> Final;
  Final.push_back(mkConst(Acc));
  Final.append(Rest.begin(), Rest.end());
  return intern(SymOp::Xor, W, Final, 0);
}

SymRef SymContext::mkNot(SymRef A) {
  uint32_t W = width(A);
  if (isConst(A))
    return mkConst(~constValue(A));
  if (op(A) == SymOp::Not)
    return operand(A, 0);
  return intern(SymOp::Not, W, {A}, 0);
}

SymRef SymContext::mkShl(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(B)) {
    llvm::APInt Amt = constValue(B);
    if (Amt.uge(W))
      return mkZero(W);
    if (Amt.isZero())
      return A;
    if (isConst(A))
      return mkConst(constValue(A).shl(Amt.getZExtValue()));
    // A left shift is a multiply by a power of two.  Normalising to Mul lets
    // the sum canonicalisation collect `x + (x << 1)` into `3*x`, which is a
    // very common obfuscation shape.
    return mkMul(mkConst(llvm::APInt(W, 1).shl(Amt.getZExtValue())), A);
  }
  if (isConstZero(A))
    return mkZero(W);
  return intern(SymOp::Shl, W, {A, B}, 0);
}

SymRef SymContext::mkLShr(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(B)) {
    llvm::APInt Amt = constValue(B);
    if (Amt.uge(W))
      return mkZero(W);
    if (Amt.isZero())
      return A;
    if (isConst(A))
      return mkConst(constValue(A).lshr(Amt.getZExtValue()));
  }
  if (isConstZero(A))
    return mkZero(W);
  return intern(SymOp::LShr, W, {A, B}, 0);
}

SymRef SymContext::mkAShr(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(B)) {
    llvm::APInt Amt = constValue(B);
    if (isConst(A)) {
      llvm::APInt V = constValue(A);
      return mkConst(Amt.uge(W) ? (V.isNegative() ? llvm::APInt::getAllOnes(W)
                                                  : llvm::APInt(W, 0))
                                : V.ashr(Amt.getZExtValue()));
    }
    if (Amt.isZero())
      return A;
  }
  if (isConstZero(A))
    return mkZero(W);
  return intern(SymOp::AShr, W, {A, B}, 0);
}

SymRef SymContext::mkRol(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(B)) {
    uint64_t Amt = constValue(B).urem(llvm::APInt(W, W)).getZExtValue();
    if (Amt == 0)
      return A;
    if (isConst(A))
      return mkConst(constValue(A).rotl(Amt));
  }
  return intern(SymOp::Rol, W, {A, B}, 0);
}

SymRef SymContext::mkRor(SymRef A, SymRef B) {
  uint32_t W = width(A);
  if (isConst(B)) {
    uint64_t Amt = constValue(B).urem(llvm::APInt(W, W)).getZExtValue();
    if (Amt == 0)
      return A;
    if (isConst(A))
      return mkConst(constValue(A).rotr(Amt));
  }
  return intern(SymOp::Ror, W, {A, B}, 0);
}

//===----------------------------------------------------------------------===//
// Structural
//===----------------------------------------------------------------------===//

SymRef SymContext::mkExtract(SymRef A, uint32_t Low, uint32_t Width) {
  assert(Width > 0 && Low + Width <= width(A) && "extract out of range");

  if (Low == 0 && Width == width(A))
    return A;
  if (isConst(A))
    return mkConst(constValue(A).extractBits(Width, Low));
  // extract(extract(x, l1), l2) == extract(x, l1 + l2)
  if (op(A) == SymOp::Extract)
    return mkExtract(operand(A, 0), static_cast<uint32_t>(node(A).Aux) + Low,
                     Width);
  // Taking the low bits of a widening cast reaches straight through to the
  // original value when the window stays inside it.
  if ((op(A) == SymOp::ZExt || op(A) == SymOp::SExt) && Low == 0 &&
      Width <= width(operand(A, 0)))
    return mkExtract(operand(A, 0), 0, Width);

  // A slice of a concatenation is a slice of whichever parts it reaches.  This
  // is the companion to the merging mkConcat does, and the pair of them is
  // what lets a byte-addressed machine state exist: without it, reading a wide
  // register and then using a narrow view of it — which is most of what x86
  // does — would leave the whole concatenation behind under an extract.
  if (op(A) == SymOp::Concat) {
    // Operands are stored most significant first; walking from the low end
    // makes the bit range of each one a running total.
    llvm::SmallVector<SymRef, 8> Parts(operands(A).begin(), operands(A).end());
    llvm::SmallVector<SymRef, 8> Kept;
    uint32_t PartLow = 0;
    for (auto It = Parts.rbegin(); It != Parts.rend(); ++It) {
      const uint32_t PartHigh = PartLow + width(*It);
      const uint32_t OverlapLow = std::max(Low, PartLow);
      const uint32_t OverlapHigh = std::min(Low + Width, PartHigh);
      if (OverlapLow < OverlapHigh)
        Kept.push_back(
            mkExtract(*It, OverlapLow - PartLow, OverlapHigh - OverlapLow));
      PartLow = PartHigh;
    }
    if (!Kept.empty()) {
      std::reverse(Kept.begin(), Kept.end());
      return mkConcat(Kept);
    }
  }

  return intern(SymOp::Extract, Width, {A}, Low);
}

SymRef SymContext::mkConcat(llvm::ArrayRef<SymRef> Ops) {
  assert(!Ops.empty());
  if (Ops.size() == 1)
    return Ops[0];

  // Concatenation is associative but not commutative, so flatten in order.
  llvm::SmallVector<SymRef, 8> Flat;
  for (SymRef R : Ops) {
    if (op(R) == SymOp::Concat) {
      llvm::ArrayRef<SymRef> Sub = operands(R);
      Flat.append(Sub.begin(), Sub.end());
    } else {
      Flat.push_back(R);
    }
  }

  llvm::SmallVector<SymRef, 8> Merged;
  for (SymRef R : Flat) {
    if (!Merged.empty()) {
      SymRef Prev = Merged.back();

      // Fold runs of adjacent literals, which is how a byte-wise memory
      // model's reads of a known region collapse back into one word.
      if (isConst(Prev) && isConst(R)) {
        llvm::APInt Hi = constValue(Prev);
        llvm::APInt Lo = constValue(R);
        uint32_t NW = Hi.getBitWidth() + Lo.getBitWidth();
        Merged.back() =
            mkConst(Hi.zext(NW).shl(Lo.getBitWidth()) | Lo.zext(NW));
        continue;
      }

      // Adjacent slices of one value are that value's wider slice.  This is
      // what makes a byte-addressed machine state usable: such a state takes a
      // word apart on every write and puts it back on every read, and without
      // this every expression it produced would carry eight extracts and a
      // concatenation around the value it actually means.  Operands run most
      // significant first, so the previous one is the upper slice.
      //
      // Anything can be read as a slice of itself, which is how a value that
      // needed no extract still lines up with one that did.
      auto sliceOf = [&](SymRef N, SymRef &Base, uint32_t &Low,
                         uint32_t &Bits) {
        if (op(N) == SymOp::Extract) {
          Base = operand(N, 0);
          Low = static_cast<uint32_t>(node(N).Aux);
        } else {
          Base = N;
          Low = 0;
        }
        Bits = width(N);
      };

      SymRef UpperBase, LowerBase;
      uint32_t UpperLow = 0, UpperBits = 0, LowerLow = 0, LowerBits = 0;
      sliceOf(Prev, UpperBase, UpperLow, UpperBits);
      sliceOf(R, LowerBase, LowerLow, LowerBits);

      // The two can be slices of one value and still not say so, because
      // taking the low bits of a widening cast is rewritten to take them from
      // what was cast — correct on its own, and enough to stop the halves of a
      // word from recognising each other.  Line them back up.
      if (UpperBase != LowerBase) {
        auto isWideningOf = [&](SymRef Wide, SymRef Narrow) {
          return (op(Wide) == SymOp::ZExt || op(Wide) == SymOp::SExt) &&
                 operand(Wide, 0) == Narrow;
        };
        if (isWideningOf(UpperBase, LowerBase) &&
            LowerLow + LowerBits <= width(LowerBase))
          LowerBase = UpperBase;
        else if ((op(UpperBase) == SymOp::ZExt ||
                  op(UpperBase) == SymOp::SExt) &&
                 UpperLow == LowerBits &&
                 LowerBits <= width(operand(UpperBase, 0)) &&
                 R == mkExtract(operand(UpperBase, 0), 0, LowerBits)) {
          // mkExtract intentionally looks through the low end of a widening
          // cast.  When that operand is itself a concatenation, the low slice
          // may canonicalise all the way to one of its leaves.  Recognise that
          // leaf as the missing low slice so splitting and rejoining the wide
          // value still reconstructs the widening node exactly.
          LowerBase = UpperBase;
          LowerLow = 0;
        }
        else if (isWideningOf(LowerBase, UpperBase) &&
                 UpperLow + UpperBits <= width(UpperBase))
          UpperBase = LowerBase;
      }

      if (UpperBase == LowerBase && UpperLow == LowerLow + LowerBits) {
        // mkExtract collapses a slice that covers the whole value back to the
        // value, so a full round trip leaves nothing behind at all.
        Merged.back() = mkExtract(UpperBase, LowerLow, LowerBits + UpperBits);
        continue;
      }
    }
    Merged.push_back(R);
  }

  if (Merged.size() == 1)
    return Merged[0];

  uint32_t W = 0;
  for (SymRef R : Merged)
    W += width(R);
  return intern(SymOp::Concat, W, Merged, 0);
}

SymRef SymContext::mkZExt(SymRef A, uint32_t Width) {
  assert(Width >= width(A) && "zext must not narrow");
  if (Width == width(A))
    return A;
  if (isConst(A))
    return mkConst(constValue(A).zext(Width));
  if (op(A) == SymOp::ZExt)
    return mkZExt(operand(A, 0), Width);
  return intern(SymOp::ZExt, Width, {A}, 0);
}

SymRef SymContext::mkSExt(SymRef A, uint32_t Width) {
  assert(Width >= width(A) && "sext must not narrow");
  if (Width == width(A))
    return A;
  if (isConst(A))
    return mkConst(constValue(A).sext(Width));
  if (op(A) == SymOp::SExt)
    return mkSExt(operand(A, 0), Width);
  return intern(SymOp::SExt, Width, {A}, 0);
}

SymRef SymContext::mkZExtOrTrunc(SymRef A, uint32_t Width) {
  if (Width == width(A))
    return A;
  if (Width < width(A))
    return mkExtract(A, 0, Width);
  return mkZExt(A, Width);
}

SymRef SymContext::mkIte(SymRef C, SymRef T, SymRef E) {
  assert(width(C) == 1 && "an ite condition must be a single bit");
  assert(width(T) == width(E) && "ite arms must share a width");
  if (isConst(C))
    return constValue(C).isZero() ? E : T;
  if (T == E)
    return T;
  // ite(c, 1, 0) is c itself, the shape a lifted setcc produces.
  if (width(T) == 1 && isConst(T) && isConst(E)) {
    if (!constValue(T).isZero() && constValue(E).isZero())
      return C;
    if (constValue(T).isZero() && !constValue(E).isZero())
      return mkNot(C);
  }
  return intern(SymOp::Ite, width(T), {C, T, E}, 0);
}

//===----------------------------------------------------------------------===//
// Predicates
//===----------------------------------------------------------------------===//

SymRef SymContext::mkEq(SymRef A, SymRef B) {
  assert(width(A) == width(B) && "comparison operands must share a width");
  if (A == B)
    return mkTrue();
  if (isConst(A) && isConst(B))
    return constValue(A) == constValue(B) ? mkTrue() : mkFalse();
  // Equality is commutative, so order the operands for canonicity.
  if (B < A)
    std::swap(A, B);
  return intern(SymOp::Eq, 1, {A, B}, 0);
}

SymRef SymContext::mkNe(SymRef A, SymRef B) { return mkNot(mkEq(A, B)); }

SymRef SymContext::mkUlt(SymRef A, SymRef B) {
  assert(width(A) == width(B));
  if (A == B)
    return mkFalse();
  if (isConst(A) && isConst(B))
    return constValue(A).ult(constValue(B)) ? mkTrue() : mkFalse();
  // Nothing is below zero, and nothing is at or above the maximum.
  if (isConstZero(B))
    return mkFalse();
  if (isConstOnes(B) && !isConst(A))
    return mkNe(A, B);
  return intern(SymOp::Ult, 1, {A, B}, 0);
}

SymRef SymContext::mkUle(SymRef A, SymRef B) {
  assert(width(A) == width(B));
  if (A == B)
    return mkTrue();
  if (isConst(A) && isConst(B))
    return constValue(A).ule(constValue(B)) ? mkTrue() : mkFalse();
  if (isConstZero(A))
    return mkTrue();
  if (isConstOnes(B))
    return mkTrue();
  return intern(SymOp::Ule, 1, {A, B}, 0);
}

SymRef SymContext::mkSlt(SymRef A, SymRef B) {
  assert(width(A) == width(B));
  if (A == B)
    return mkFalse();
  if (isConst(A) && isConst(B))
    return constValue(A).slt(constValue(B)) ? mkTrue() : mkFalse();
  return intern(SymOp::Slt, 1, {A, B}, 0);
}

SymRef SymContext::mkSle(SymRef A, SymRef B) {
  assert(width(A) == width(B));
  if (A == B)
    return mkTrue();
  if (isConst(A) && isConst(B))
    return constValue(A).sle(constValue(B)) ? mkTrue() : mkFalse();
  return intern(SymOp::Sle, 1, {A, B}, 0);
}

//===----------------------------------------------------------------------===//
// Inspection
//===----------------------------------------------------------------------===//

size_t SymContext::dagSize(SymRef R) const {
  llvm::DenseSet<uint32_t> Seen;
  llvm::SmallVector<SymRef, 32> Work{R};
  while (!Work.empty()) {
    SymRef Cur = Work.pop_back_val();
    if (!Seen.insert(Cur.index()).second)
      continue;
    for (SymRef C : operands(Cur))
      Work.push_back(C);
  }
  return Seen.size();
}

size_t SymContext::readabilityCost(SymRef R) const {
  constexpr size_t Ceiling = std::numeric_limits<size_t>::max();

  // Interning appends a node only after all of its operands exist.  Extending
  // one prefix cache therefore computes every new cost in a single pass, even
  // when a solver asks again after interning more candidates.
  ReadabilityCosts.reserve(Nodes.size());
  while (ReadabilityCosts.size() < Nodes.size()) {
    SymRef Current(static_cast<uint32_t>(ReadabilityCosts.size()));
    size_t Total = isConst(Current) && isConstOnes(Current) ? 0 : 1;
    for (SymRef Operand : operands(Current)) {
      assert(Operand.index() < ReadabilityCosts.size() &&
             "a symbolic node precedes one of its operands");
      const size_t OperandCost = ReadabilityCosts[Operand.index()];
      if (OperandCost > Ceiling - Total) {
        Total = Ceiling;
        break;
      }
      Total += OperandCost;
    }
    ReadabilityCosts.push_back(Total);
  }
  return ReadabilityCosts[R.index()];
}

void SymContext::collectVars(SymRef R,
                             llvm::SmallVectorImpl<uint32_t> &Out) const {
  llvm::DenseSet<uint32_t> Seen;
  llvm::SmallVector<SymRef, 32> Work{R};
  while (!Work.empty()) {
    SymRef Cur = Work.pop_back_val();
    if (!Seen.insert(Cur.index()).second)
      continue;
    if (op(Cur) == SymOp::Var) {
      Out.push_back(varId(Cur));
      continue;
    }
    for (SymRef C : operands(Cur))
      Work.push_back(C);
  }
  std::sort(Out.begin(), Out.end());
  Out.erase(std::unique(Out.begin(), Out.end()), Out.end());
}

//===----------------------------------------------------------------------===//
// Substitution
//===----------------------------------------------------------------------===//

SymRef SymContext::substitute(SymRef R,
                              const std::unordered_map<uint32_t, SymRef> &Map) {
  if (Map.empty())
    return R;

  // Rebuild bottom-up over a topological order.  Going through the builders
  // means the result is canonical, so a substitution that makes two subterms
  // equal has that equality recognised immediately.
  std::unordered_map<uint32_t, SymRef> Done;

  llvm::SmallVector<std::pair<SymRef, bool>, 32> Work{{R, false}};
  while (!Work.empty()) {
    auto [Cur, Expanded] = Work.pop_back_val();
    if (Done.count(Cur.index()))
      continue;

    auto Hit = Map.find(Cur.index());
    if (Hit != Map.end()) {
      Done.emplace(Cur.index(), Hit->second);
      continue;
    }

    if (!Expanded) {
      Work.emplace_back(Cur, true);
      for (SymRef C : operands(Cur))
        if (!Done.count(C.index()))
          Work.emplace_back(C, false);
      continue;
    }

    llvm::SmallVector<SymRef, 8> NewOps;
    bool Changed = false;
    for (SymRef C : operands(Cur)) {
      SymRef NC = Done.at(C.index());
      Changed |= (NC != C);
      NewOps.push_back(NC);
    }
    if (!Changed) {
      Done.emplace(Cur.index(), Cur);
      continue;
    }
    Done.emplace(Cur.index(), rebuild(Cur, NewOps));
  }

  return Done.at(R.index());
}

SymRef SymContext::substituteVar(SymRef R, uint32_t VarIdx, SymRef Val) {
  llvm::SmallVector<uint32_t, 8> Seen;
  collectVars(R, Seen);
  if (std::find(Seen.begin(), Seen.end(), VarIdx) == Seen.end())
    return R;

  SymRef VarRef = intern(SymOp::Var, Vars[VarIdx].Width, {}, VarIdx);
  std::unordered_map<uint32_t, SymRef> Map;
  Map.emplace(VarRef.index(), Val);
  return substitute(R, Map);
}

SymRef SymContext::rebuild(SymRef Orig, llvm::ArrayRef<SymRef> NewOps) {
  // Copied rather than referenced: every builder below can intern, which grows
  // the node vector and would invalidate a reference into it.
  const SymNode N = Nodes[Orig.index()];
  switch (N.Op) {
  case SymOp::Const:
  case SymOp::Var:
    return Orig;
  case SymOp::Add:
    return mkAdd(NewOps);
  case SymOp::Mul:
    return mkMul(NewOps);
  case SymOp::And:
    return mkAnd(NewOps);
  case SymOp::Or:
    return mkOr(NewOps);
  case SymOp::Xor:
    return mkXor(NewOps);
  case SymOp::Not:
    return mkNot(NewOps[0]);
  case SymOp::Shl:
    return mkShl(NewOps[0], NewOps[1]);
  case SymOp::LShr:
    return mkLShr(NewOps[0], NewOps[1]);
  case SymOp::AShr:
    return mkAShr(NewOps[0], NewOps[1]);
  case SymOp::UDiv:
    return mkUDiv(NewOps[0], NewOps[1]);
  case SymOp::SDiv:
    return mkSDiv(NewOps[0], NewOps[1]);
  case SymOp::URem:
    return mkURem(NewOps[0], NewOps[1]);
  case SymOp::SRem:
    return mkSRem(NewOps[0], NewOps[1]);
  case SymOp::Rol:
    return mkRol(NewOps[0], NewOps[1]);
  case SymOp::Ror:
    return mkRor(NewOps[0], NewOps[1]);
  case SymOp::Extract:
    return mkExtract(NewOps[0], static_cast<uint32_t>(N.Aux), N.Width);
  case SymOp::Concat:
    return mkConcat(NewOps);
  case SymOp::ZExt:
    return mkZExt(NewOps[0], N.Width);
  case SymOp::SExt:
    return mkSExt(NewOps[0], N.Width);
  case SymOp::Ite:
    return mkIte(NewOps[0], NewOps[1], NewOps[2]);
  case SymOp::Eq:
    return mkEq(NewOps[0], NewOps[1]);
  case SymOp::Ult:
    return mkUlt(NewOps[0], NewOps[1]);
  case SymOp::Ule:
    return mkUle(NewOps[0], NewOps[1]);
  case SymOp::Slt:
    return mkSlt(NewOps[0], NewOps[1]);
  case SymOp::Sle:
    return mkSle(NewOps[0], NewOps[1]);
  }
  llvm_unreachable("unhandled SymOp in rebuild");
}

} // namespace neverd::symbolic
