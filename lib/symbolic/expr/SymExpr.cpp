//===- SymExpr.cpp - Building symbolic bitvector expressions --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the interning arena and the leaf builders.  The canonicalizing
/// operator builders are split by family across SymExprArith.cpp,
/// SymExprLogic.cpp and SymExprStruct.cpp; traversal and substitution live in
/// SymExprWalk.cpp and evaluation in SymExprEval.cpp.
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

#include "llvm/ADT/Twine.h"

#include <cassert>

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
  SymRef Result = mkVar(Name, Width);
  Vars[varId(Result)].Fresh = true;
  return Result;
}

} // namespace neverd::symbolic
