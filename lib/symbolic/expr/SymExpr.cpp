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

#include <algorithm>
#include <cassert>
#include <limits>

namespace neverd::symbolic {

namespace {

constexpr uint32_t kAmbiguousVariable = std::numeric_limits<uint32_t>::max();

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

void SymContext::recordVariableName(llvm::StringRef Name, uint32_t Id) {
  auto [It, Inserted] = VarByName.try_emplace(Name.str(), Id);
  if (!Inserted && It->second != Id)
    It->second = kAmbiguousVariable;
}

SymRef SymContext::mkVar(llvm::StringRef Name, uint32_t Width) {
  std::string Key = Name.str();
  auto Identity = std::make_pair(Key, Width);
  auto It = VarByNameAndWidth.find(Identity);
  if (It != VarByNameAndWidth.end())
    return intern(SymOp::Var, Width, {}, It->second);
  auto Id = static_cast<uint32_t>(Vars.size());
  Vars.push_back(SymVarInfo{Key, Width});
  recordVariableName(Key, Id);
  VarByNameAndWidth.emplace(std::move(Identity), Id);
  return intern(SymOp::Var, Width, {}, Id);
}

SymRef SymContext::mkInputVar(llvm::StringRef Name, uint32_t Width,
                              SymInputOrigin Origin, llvm::endianness Order) {
  constexpr uint32_t ByteBits = 8;
  if (Width == 0)
    return {};
  auto DeclareInput = [&](llvm::StringRef SegmentName, uint32_t SegmentWidth,
                          SymInputOrigin Segment) {
    const std::string Key = SegmentName.str();
    InputVariableKey Identity{Key, SegmentWidth, Segment};
    auto It = InputVarByIdentity.find(Identity);
    if (It != InputVarByIdentity.end())
      return intern(SymOp::Var, SegmentWidth, {}, It->second);

    const auto Id = static_cast<uint32_t>(Vars.size());
    Vars.push_back(SymVarInfo{Key, SegmentWidth, false, Segment});
    recordVariableName(Key, Id);
    InputVarByIdentity.emplace(std::move(Identity), Id);
    return intern(SymOp::Var, SegmentWidth, {}, Id);
  };

  const bool ValidKind = Origin.Kind == SymInputKind::Register ||
                         Origin.Kind == SymInputKind::Temporary ||
                         Origin.Kind == SymInputKind::AbsoluteMemory;
  const bool RangeWraps =
      Origin.Bytes > 1 &&
      Origin.Offset > std::numeric_limits<uint64_t>::max() - (Origin.Bytes - 1);
  if (!ValidKind || Origin.Bytes == 0 ||
      Width != uint32_t(Origin.Bytes) * ByteBits || RangeWraps) {
    // Preserve malformed producer metadata as a standalone leaf so defensive
    // consumers can reject it without terminating the process.  It must not
    // enter the canonical byte store: doing so could let an invalid range
    // contaminate otherwise valid sibling-state inputs.
    return DeclareInput(Name, Width, Origin);
  }

  auto DeclareSegment = [&](llvm::StringRef SegmentName,
                            SymInputOrigin Segment) {
    return DeclareInput(SegmentName, uint32_t(Segment.Bytes) * ByteBits,
                        Segment);
  };

  auto &Bytes = InputBytes[InputSpaceKey{Origin.Kind, Origin.Epoch}];
  bool HasMaterialisedByte = false;
  for (uint16_t I = 0; I < Origin.Bytes; ++I)
    HasMaterialisedByte |= Bytes.count(Origin.Offset + I) != 0;

  auto RecordSegment = [&](SymRef Value, SymInputOrigin Segment) {
    const uint32_t SegmentWidth = uint32_t(Segment.Bytes) * ByteBits;
    for (uint16_t I = 0; I < Segment.Bytes; ++I) {
      const uint32_t Low = Order == llvm::endianness::little
                               ? uint32_t(I) * ByteBits
                               : SegmentWidth - (uint32_t(I) + 1) * ByteBits;
      Bytes.emplace(Segment.Offset + I, mkExtract(Value, Low, ByteBits));
    }
  };

  // The overwhelmingly common case remains one leaf: an untouched complete
  // register (or memory word) read before any sibling looked at its bytes.
  if (!HasMaterialisedByte) {
    SymRef Result = DeclareSegment(Name, Origin);
    RecordSegment(Result, Origin);
    return Result;
  }

  auto SegmentName = [](SymInputOrigin Segment) {
    const char *Prefix = nullptr;
    switch (Segment.Kind) {
    case SymInputKind::Register:
      Prefix = "reg";
      break;
    case SymInputKind::Temporary:
      Prefix = "tmp";
      break;
    case SymInputKind::AbsoluteMemory:
      Prefix = "mem";
      break;
    }
    std::string Result =
        (llvm::Twine(Prefix) + "$" + llvm::Twine(Segment.Offset)).str();
    if (Segment.Epoch != 0)
      Result += (llvm::Twine("$epoch") + llvm::Twine(Segment.Epoch)).str();
    return Result;
  };

  // Fill each unseen run as one variable.  Besides keeping expressions small,
  // this ensures a narrow-first read followed by a wide one adds one compact
  // high half instead of four or eight unrelated byte leaves.
  uint16_t I = 0;
  while (I < Origin.Bytes) {
    if (Bytes.count(Origin.Offset + I) != 0) {
      ++I;
      continue;
    }
    const uint16_t RunStart = I;
    do {
      ++I;
    } while (I < Origin.Bytes && Bytes.count(Origin.Offset + I) == 0);
    SymInputOrigin Segment{Origin.Kind, Origin.Offset + RunStart,
                           uint16_t(I - RunStart), Origin.Epoch};
    SymRef Value = DeclareSegment(SegmentName(Segment), Segment);
    RecordSegment(Value, Segment);
  }

  llvm::SmallVector<SymRef, 8> Parts;
  Parts.reserve(Origin.Bytes);
  for (uint16_t Byte = 0; Byte < Origin.Bytes; ++Byte)
    Parts.push_back(Bytes.at(Origin.Offset + Byte));
  if (Order == llvm::endianness::little)
    std::reverse(Parts.begin(), Parts.end());
  return mkConcat(Parts);
}

SymRef SymContext::varRef(uint32_t Id) {
  if (Id >= Vars.size())
    return {};
  return intern(SymOp::Var, Vars[Id].Width, {}, Id);
}

bool SymContext::hasVarName(llvm::StringRef Name) const {
  return VarByName.count(Name.str()) != 0;
}

std::optional<uint32_t> SymContext::findVar(llvm::StringRef Name) const {
  auto It = VarByName.find(Name.str());
  if (It == VarByName.end() || It->second == kAmbiguousVariable)
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
