//===- SymDispatch.cpp - Reading the shape of a computed branch -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the match described in SymDispatch.h.
///
/// Everything here rests on the expression builders having already put the
/// target into a normal form.  A sum is flattened with its constant first and
/// its like terms collected, a shift by a constant has become a multiply, and
/// a scaled index is one product however the code spelled it — so recognising
/// `base + stride * index` is reading two constants off a sum rather than
/// walking a tree of the shapes an address computation can take.  Without that
/// normalisation this file would be a pattern list; with it, it is a
/// decomposition.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymDispatch.h"

#include "neverd/symbolic/SymExec.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

namespace neverd::symbolic {

namespace {

/// A value read as `Offset + Scale * Index`.
struct Linear {
  uint64_t Offset = 0;
  uint64_t Scale = 1;
  SymRef Index;
};

/// Decompose \p E into an offset, a scale and one remaining unknown.
///
/// Fails when the expression is not of that shape, which for this purpose
/// includes anything mentioning two unknowns: a table is indexed by one thing.
std::optional<Linear> matchLinear(const SymContext &Ctx, SymRef E) {
  Linear Out;

  // The builders put a sum's constant term first and leave at most one of it.
  if (Ctx.op(E) == SymOp::Add) {
    llvm::ArrayRef<SymRef> Terms = Ctx.operands(E);
    if (Terms.size() != 2 || !Ctx.isConst(Terms[0]))
      return std::nullopt;
    Out.Offset = Ctx.constValue(Terms[0]).getZExtValue();
    E = Terms[1];
  }

  // A scaled index is one product with the constant first, whether the code
  // wrote a multiply or a shift.
  if (Ctx.op(E) == SymOp::Mul) {
    llvm::ArrayRef<SymRef> Factors = Ctx.operands(E);
    if (Factors.size() != 2 || !Ctx.isConst(Factors[0]))
      return std::nullopt;
    Out.Scale = Ctx.constValue(Factors[0]).getZExtValue();
    E = Factors[1];
  }

  if (Ctx.isConst(E))
    return std::nullopt;
  Out.Index = E;
  return Out;
}

/// Look through the widening a table entry gets before it is used, reporting
/// whether it was signed.
SymRef stripExtension(const SymContext &Ctx, SymRef E, bool &Signed) {
  for (;;) {
    SymOp Op = Ctx.op(E);
    if (Op != SymOp::ZExt && Op != SymOp::SExt)
      return E;
    Signed = Signed || Op == SymOp::SExt;
    E = Ctx.operand(E, 0);
  }
}

/// Fill in the table half of the shape from the address an entry was read at.
bool describeTable(const SymContext &Ctx, const SymState &State, SymRef Entry,
                   SymRef Index, uint16_t EntrySize, DispatchShape &Shape) {
  std::optional<Linear> Address;
  if (const SymState::LoadOrigin *From = State.loadOrigin(Entry))
    Address = matchLinear(Ctx, From->Address);
  if (!Address || Address->Scale == 0)
    return false;

  // Linear in the index, not merely linear.  Any unknown reads as `0 + 1 * x`,
  // so without this an address that is itself something loaded — a table of
  // pointers to tables, say — would be described as a table at address zero
  // with entries one byte apart.  Entries would then be read out of whatever
  // happens to be mapped at the bottom of the image, and every one of them
  // would be believed.
  if (Address->Index != Index)
    return false;

  Shape.TableBase = Address->Offset;
  Shape.EntryStride = Address->Scale;
  Shape.EntrySize = EntrySize;
  return true;
}

/// The name given to the register left unknown, and the width it was given at.
constexpr const char *kIndexName = "dispatch$index";

/// Seed the index register with a single name and run to the branch.
///
/// One name for the whole register rather than one per byte, so the index
/// stays recognisable as a single unknown through the arithmetic.
bool runToBranch(SymContext &Ctx, SymState &State, llvm::ArrayRef<LowOp> Ops,
                 uint64_t IndexRegOffset, uint16_t IndexRegSize, SymRef &Target,
                 unsigned *Unmodelled = nullptr) {
  State.write(SymSpace::Register, IndexRegOffset,
              Ctx.mkVar(kIndexName, uint32_t(IndexRegSize) * 8));

  SymExec Exec(Ctx, State);
  if (Exec.run(Ops) == 0)
    return false;
  if (Unmodelled)
    *Unmodelled = Exec.unmodelledCount();
  Target = Exec.branchTarget();
  return Target.isValid();
}

} // namespace

bool dispatchVariesWithIndex(SymContext &Ctx, llvm::ArrayRef<LowOp> Ops,
                             uint64_t IndexRegOffset, uint16_t IndexRegSize) {
  SymState State(Ctx);
  SymRef Target;
  unsigned Unmodelled = 0;
  if (!runToBranch(Ctx, State, Ops, IndexRegOffset, IndexRegSize, Target,
                   &Unmodelled))
    return true;

  // An operation that was not carried out severs whatever dependence ran
  // through it, and what is left looks exactly like independence.  Since a
  // wrong "no" here discards a real dispatch and a wrong "yes" only means
  // carrying on as before, say yes whenever the run was incomplete.
  if (Unmodelled != 0)
    return true;

  // Interning is idempotent, so this names the unknown the run already made
  // rather than making another.
  const SymRef Index = Ctx.mkVar(kIndexName, uint32_t(IndexRegSize) * 8);

  llvm::SmallVector<SymRef, 16> Worklist{Target};
  llvm::DenseSet<uint32_t> Seen;
  while (!Worklist.empty()) {
    SymRef E = Worklist.pop_back_val();
    if (!Seen.insert(E.index()).second)
      continue;
    if (E == Index)
      return true;
    // Stepping from a loaded value to the address it was loaded from is the
    // whole of this.  A table dispatch ends in a value that mentions nothing,
    // because it is the contents of somewhere; everything that makes it a
    // dispatch is in the address.
    if (const SymState::LoadOrigin *From = State.loadOrigin(E))
      Worklist.push_back(From->Address);
    for (SymRef Operand : Ctx.operands(E))
      Worklist.push_back(Operand);
  }
  return false;
}

std::optional<DispatchShape> analyzeDispatch(SymContext &Ctx,
                                             llvm::ArrayRef<LowOp> Ops,
                                             uint64_t IndexRegOffset,
                                             uint16_t IndexRegSize) {
  SymState State(Ctx);
  SymRef Target;
  if (!runToBranch(Ctx, State, Ops, IndexRegOffset, IndexRegSize, Target) ||
      Ctx.isConst(Target))
    return std::nullopt;

  const SymRef Index = Ctx.mkVar(kIndexName, uint32_t(IndexRegSize) * 8);
  DispatchShape Shape;

  // The target is the entry itself: `load(base + stride * index)`.
  if (const SymState::LoadOrigin *From = State.loadOrigin(Target)) {
    Shape.Kind = DispatchKind::Absolute;
    return describeTable(Ctx, State, Target, Index, From->Bytes, Shape)
               ? std::optional<DispatchShape>(Shape)
               : std::nullopt;
  }

  // Otherwise the entry is an offset: `relbase + scale * entry`, where the
  // entry has usually been widened on the way and may have been signed.
  std::optional<Linear> Outer = matchLinear(Ctx, Target);
  if (!Outer)
    return std::nullopt;

  bool Signed = false;
  SymRef Entry = stripExtension(Ctx, Outer->Index, Signed);
  const SymState::LoadOrigin *From = State.loadOrigin(Entry);
  if (!From)
    return std::nullopt;

  Shape.Kind = DispatchKind::Relative;
  Shape.RelativeBase = Outer->Offset;
  Shape.EntryScale = Outer->Scale;
  Shape.EntryIsSigned = Signed;
  return describeTable(Ctx, State, Entry, Index, From->Bytes, Shape)
             ? std::optional<DispatchShape>(Shape)
             : std::nullopt;
}

} // namespace neverd::symbolic
