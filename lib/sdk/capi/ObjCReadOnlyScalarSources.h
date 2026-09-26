#ifndef NEVERD_SDK_CAPI_OBJC_READONLYSCALARSOURCES_H
#define NEVERD_SDK_CAPI_OBJC_READONLYSCALARSOURCES_H

#include "BorrowedByteSources.h"

#include "neverd/ir/high/HighSourceFlow.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace neverd::sdk {
namespace readonly_scalar_detail {
struct LoadPlan {
  va_t Base = 0;
  uint32_t Extent = 0;
  uint64_t Stride = 1;
  uint64_t SignLimit = UINT64_MAX;
  ExprPtr Index;
  ExprPtr Offset;
  ExprPtr BaseExpression;
};
inline bool ordinary(const ExprPtr &E) {
  return E && E->Type && E->IntrinsicId == Intrinsic::None &&
         E->IntrinsicOutputs.empty() &&
         E->MemoryOrdering == NdMemoryOrdering::None &&
         E->MemoryAddressSpace == NdMemoryAddressSpace::Default;
}
inline std::optional<LoadPlan> loadPlan(const ExprPtr &E, bool Bound,
                                        bool ObjectPointers) {
  if (!ordinary(E) || E->Kind != ExprKind::Load || E->Operands.size() != 1 ||
      (ObjectPointers
           ? (E->Type->Kind != NdTypeKind::Ptr &&
              E->Type->Kind != NdTypeKind::Int) ||
                 E->Type->Size != 8
           : !((E->Type->Kind == NdTypeKind::Int &&
                (E->Type->Size == 1 || E->Type->Size == 2 ||
                 E->Type->Size == 4 || E->Type->Size == 8)) ||
               (E->Type->Kind == NdTypeKind::Float &&
                (E->Type->Size == 4 || E->Type->Size == 8)))))
    return std::nullopt;
  const auto &Address = E->Operands[0];
  if (!ordinary(Address) || Address->Type->Size != 8 ||
      Address->Kind != ExprKind::BinOp || Address->Op != NdOp::INT_ADD ||
      Address->Operands.size() != 2)
    return std::nullopt;
  LoadPlan P;
  for (unsigned Side = 0; Side < 2; ++Side) {
    const auto &Base = Address->Operands[Side];
    if (!ordinary(Base) || Base->Type->Size != 8 || !Base->Operands.empty())
      continue;
    if (!Bound && Base->Kind == ExprKind::Const &&
        Base->Type->Kind == NdTypeKind::Int) {
      P.Base = Base->ConstVal;
    } else if (Bound && Base->Kind == ExprKind::Call && Base->SourceCallHint &&
               Base->SourceCallHint->CallKind ==
                   (ObjectPointers
                        ? SourceCallTypeHint::Kind::RuntimeConstantObjectTable
                        : SourceCallTypeHint::Kind::RuntimeReadOnlyBytes)) {
      P.Base = Base->SourceCallHint->TargetAddress;
      P.Extent = Base->SourceCallHint->ByteCount;
    } else {
      continue;
    }
    P.BaseExpression = Base;
    P.Offset = Address->Operands[1 - Side];
    break;
  }
  if (!P.Base || !ordinary(P.Offset) || P.Offset->Type->Size != 8 ||
      P.Offset->Type->Kind != NdTypeKind::Int)
    return std::nullopt;
  // Fold a small fixed bias into the helper base. Keep the indexed part as
  // the emitted offset so the copied byte range starts at the first load,
  // including a load a few bytes before the original image constant.
  if (!ObjectPointers && P.Offset->Kind == ExprKind::BinOp &&
      (P.Offset->Op == NdOp::INT_ADD ||
       P.Offset->Op == NdOp::INT_SUB) &&
      P.Offset->Operands.size() == 2) {
    ExprPtr Indexed;
    uint64_t Bias = 0;
    bool Subtract = P.Offset->Op == NdOp::INT_SUB;
    for (unsigned Side = 0; Side < (Subtract ? 1U : 2U); ++Side) {
      const auto &Candidate = P.Offset->Operands[Side];
      const auto &Constant = P.Offset->Operands[1 - Side];
      if (!ordinary(Candidate) || !ordinary(Constant) ||
          Candidate->Type->Kind != NdTypeKind::Int ||
          Candidate->Type->Size != 8 ||
          Constant->Kind != ExprKind::Const ||
          !Constant->Operands.empty() ||
          Constant->Type->Kind != NdTypeKind::Int ||
          Constant->Type->Size != 8 || Constant->ConstVal > 65536)
        continue;
      Indexed = Candidate;
      Bias = Constant->ConstVal;
      break;
    }
    if (Indexed) {
      // A published helper already owns its exact base and byte count. Do
      // not reinterpret an edited offset as a second, shifted helper range.
      if (Bound)
        return std::nullopt;
      if ((Subtract && P.Base < Bias) ||
          (!Subtract && P.Base > UINT64_MAX - Bias))
        return std::nullopt;
      P.Base = Subtract ? P.Base - Bias : P.Base + Bias;
      P.Offset = Indexed;
    }
  }
  P.Index = P.Offset;
  if (P.Index->Kind == ExprKind::BinOp &&
      (P.Index->Op == NdOp::INT_LEFT || P.Index->Op == NdOp::INT_MULT) &&
      P.Index->Operands.size() == 2) {
    const auto &Scale = P.Index->Operands[1];
    if (!ordinary(Scale) || Scale->Kind != ExprKind::Const ||
        !Scale->Operands.empty() || Scale->Type->Kind != NdTypeKind::Int ||
        !Scale->Type->Size || Scale->Type->Size > 8 ||
        (Scale->Type->Size < 8 &&
         Scale->ConstVal >= (uint64_t{1} << (Scale->Type->Size * 8))))
      return std::nullopt;
    if (P.Index->Op == NdOp::INT_LEFT) {
      if (Scale->ConstVal > 16)
        return std::nullopt;
      P.Stride = uint64_t{1} << Scale->ConstVal;
    } else {
      if (Scale->Type->Size != 8 || !Scale->ConstVal || Scale->ConstVal > 65536)
        return std::nullopt;
      P.Stride = Scale->ConstVal;
    }
    P.Index = P.Index->Operands[0];
    if (!ordinary(P.Index) || P.Index->Type->Kind != NdTypeKind::Int ||
        P.Index->Type->Size != 8)
      return std::nullopt;
  }
  if (P.Index->Kind == ExprKind::UnaryOp && P.Index->Operands.size() == 1 &&
      (P.Index->Op == NdOp::INT_ZEXT || P.Index->Op == NdOp::INT_SEXT)) {
    const auto &Narrow = P.Index->Operands[0];
    if (!ordinary(Narrow) || Narrow->Type->Kind != NdTypeKind::Int ||
        !Narrow->Type->Size || Narrow->Type->Size > 8)
      return std::nullopt;
    if (P.Index->Op == NdOp::INT_SEXT)
      P.SignLimit = uint64_t{1} << (Narrow->Type->Size * 8 - 1);
    P.Index = Narrow;
  }
  return P;
}
} // namespace readonly_scalar_detail

/// Every occurrence of a shared load must fit the same immutable table prefix.
/// Pointer tables additionally require full-width slots; their object targets
/// are validated by the Objective-C binder and publication gate.
inline std::map<const HighExpr *, readonly_scalar_detail::LoadPlan>
readOnlyLoadPlans(const HighFunc &Function, const BinaryImage &Image, bool Bound,
                  bool ObjectPointers) {
  using namespace readonly_scalar_detail;
  struct Occurrence {
    ExprPtr Load;
    LoadPlan Plan;
  };
  std::vector<Occurrence> Occurrences;
  std::vector<HighSourceUnsignedRangeQuery> Queries;
  size_t Budget = 100000;
  walkStmts(Function.Body, [&](const HighStmt &S) {
    if (!Budget)
      return;
    --Budget;
    std::set<const HighExpr *> Seen;
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      while (!Pending.empty() && Budget) {
        --Budget;
        auto E = Pending.back();
        Pending.pop_back();
        if (!E || !Seen.insert(E.get()).second)
          continue;
        if (auto P = loadPlan(E, Bound, ObjectPointers)) {
          if (Queries.size() == 128) {
            Budget = 0;
            return;
          }
          Queries.push_back({&S, P->Index});
          Occurrences.push_back({E, std::move(*P)});
        }
        Pending.insert(Pending.end(), E->Operands.begin(), E->Operands.end());
      }
    });
  });
  std::map<const HighExpr *, LoadPlan> Plans;
  if (!Budget || Queries.empty())
    return Plans;
  const auto Bounds = highSourceUnsignedUpperBounds(Function, Queries);
  std::set<const HighExpr *> Rejected;
  for (size_t I = 0; I < Occurrences.size(); ++I) {
    auto &[Load, P] = Occurrences[I];
    const auto Upper = Bounds[I];
    // Bound both the scalar inventory and the byte copy; sparse strides do
    // not permit unbounded reads or source helper growth.
    if (!Upper || *Upper >= 4096 || *Upper >= P.SignLimit ||
        *Upper > (65536 - Load->Type->Size) / P.Stride) {
      Rejected.insert(Load.get());
      continue;
    }
    const auto Extent = uint32_t(*Upper * P.Stride + Load->Type->Size);
    if (Bound && (P.Extent < Extent || P.Extent > 65536)) {
      Rejected.insert(Load.get());
      continue;
    }
    if (!Bound)
      P.Extent = Extent;
    bool Valid = true;
    if (ObjectPointers) {
      if (P.Stride != 8)
        Valid = false;
      for (uint64_t Entry = 0; Entry <= *Upper && Valid; ++Entry) {
        const va_t Slot = P.Base + Entry * 8;
        if (readImmutableImagePointer(Image, Slot))
          continue;
        const auto Bytes = readImmutableImageBytes(Image, Slot, 8);
        Valid = Bytes && std::all_of(Bytes->begin(), Bytes->end(),
                                    [](uint8_t Byte) { return Byte == 0; });
      }
    } else {
      const auto Bytes = readImmutableImageBytes(Image, P.Base, P.Extent);
      Valid = bool(Bytes);
      // Scalar copies do not relocate pointers, including unmarked values that
      // happen to point into this image. The existing pointer reader owns them.
      for (uint64_t Entry = 0; Entry <= *Upper && Valid; ++Entry) {
        uint64_t Bits = 0;
        for (unsigned B = 0; B < Load->Type->Size; ++B)
          Bits |= uint64_t((*Bytes)[Entry * P.Stride + B]) << (B * 8);
        Valid = !isImagePointerBitPattern(Image, Bits, Load->Type->Size);
      }
    }
    if (!Valid) {
      Rejected.insert(Load.get());
      continue;
    }
    auto [It, Fresh] = Plans.emplace(Load.get(), P);
    if (!Fresh)
      It->second.Extent = std::max(It->second.Extent, P.Extent);
  }
  for (const auto *E : Rejected)
    Plans.erase(E);
  return Plans;
}

inline std::map<const HighExpr *, readonly_scalar_detail::LoadPlan>
readOnlyScalarLoadPlans(const HighFunc &Function, const BinaryImage &Image,
                        bool Bound = false) {
  return readOnlyLoadPlans(Function, Image, Bound, false);
}

struct ReadOnlyLoopBytePlans {
  std::map<const HighExpr *, readonly_scalar_detail::LoadPlan> Loads;
  // Rebase the private induction pointer to a byte offset. Its only uses are
  // the certified loads and the increment in the same bounded loop.
  std::map<const HighExpr *, uint64_t> Initializers;
};

/// Recognize a closed three-byte table walk controlled by a finite countdown.
/// A pointer increment alone has no range proof: the exit must precede the
/// increment on the last iteration, and neither induction local may escape or
/// acquire another definition. The publication pass repeats this proof on the
/// rebound source with the helper address in place of the image address.
inline ReadOnlyLoopBytePlans readOnlyLoopBytePlans(const HighFunc &Function,
                                                   const BinaryImage &Image,
                                                   bool Bound = false) {
  using Identity = HighSourceLocalIdentity;
  using Plan = readonly_scalar_detail::LoadPlan;
  ReadOnlyLoopBytePlans Result;
  const auto Local = [](const ExprPtr &E) -> std::optional<Identity> {
    if (!E || (E->Kind != ExprKind::Var && E->Kind != ExprKind::Phi) ||
        !E->Operands.empty() || !E->Type || E->Type->Kind != NdTypeKind::Int ||
        !E->Type->Size || E->Type->Size > 8)
      return std::nullopt;
    return highSourceLocalIdentity(E->Var);
  };
  const auto Constant = [](const ExprPtr &Original) -> std::optional<uint64_t> {
    auto E = Original;
    uint64_t SignLimit = UINT64_MAX;
    for (unsigned Depth = 0;
         Depth < 4 && E &&
         (E->Kind == ExprKind::Cast ||
          (E->Kind == ExprKind::UnaryOp && E->Op == NdOp::INT_ZEXT)) &&
         E->Operands.size() == 1;
         ++Depth) {
      if (!readonly_scalar_detail::ordinary(E) ||
          E->Type->Kind != NdTypeKind::Int || !E->Operands[0] ||
          !E->Operands[0]->Type ||
          E->Operands[0]->Type->Kind != NdTypeKind::Int ||
          E->Type->Size < E->Operands[0]->Type->Size)
        return std::nullopt;
      if (E->Kind == ExprKind::Cast &&
          E->Type->Size > E->Operands[0]->Type->Size &&
          E->Operands[0]->Type->IsSigned && E->Operands[0]->Type->Size < 8)
        SignLimit = std::min(
            SignLimit, uint64_t{1} << (E->Operands[0]->Type->Size * 8 - 1));
      E = E->Operands[0];
    }
    if (!readonly_scalar_detail::ordinary(E) || E->Kind != ExprKind::Const ||
        !E->Operands.empty() || E->Type->Kind != NdTypeKind::Int ||
        !E->Type->Size || E->Type->Size > 8 || E->ConstVal >= SignLimit ||
        (E->Type->IsSigned && E->Type->Size < 8 &&
         E->ConstVal >= (uint64_t{1} << (E->Type->Size * 8 - 1))) ||
        (E->Type->Size < 8 &&
         E->ConstVal >= (uint64_t{1} << (E->Type->Size * 8))))
      return std::nullopt;
    return E->ConstVal;
  };
  const auto Assignment = [&](const HighStmt &S,
                              const Identity &Dst) -> ExprPtr {
    return S.Kind == StmtKind::Assign && Local(S.Dst) == Dst ? S.Val : nullptr;
  };
  const auto Binary = [&](const ExprPtr &E, NdOp Op, const Identity &L,
                          uint64_t R) {
    return E && E->Kind == ExprKind::BinOp && E->Op == Op && E->Type &&
           (Op == NdOp::INT_NOTEQUAL || E->Type->Size == 8) &&
           E->Operands.size() == 2 && Local(E->Operands[0]) == L &&
           E->Operands[0]->Type->Size == 8 && Constant(E->Operands[1]) == R;
  };
  size_t Budget = 100000;
  std::function<void(const std::vector<HighStmt> &, unsigned)> Inspect;
  Inspect = [&](const std::vector<HighStmt> &Body, unsigned Depth) {
    if (!Budget || Depth > 200 || Body.size() > 100000)
      return;
    for (size_t I = 2; I < Body.size() && Budget; ++I) {
      --Budget;
      const auto &Loop = Body[I];
      if (Loop.Kind != StmtKind::While || !Loop.Cond ||
          Constant(Loop.Cond) != 1 || Loop.Body.size() < 9)
        continue;
      const auto &CountInit = Body[I - 2], &PointerInit = Body[I - 1];
      const auto Count = Local(CountInit.Dst), Pointer = Local(PointerInit.Dst);
      if (CountInit.Kind != StmtKind::Assign ||
          PointerInit.Kind != StmtKind::Assign || !Count || !Pointer ||
          *Count == *Pointer || !CountInit.Val || !PointerInit.Val ||
          CountInit.Dst->Type->Size != 8 || PointerInit.Dst->Type->Size != 8)
        continue;
      const auto Iterations = Constant(CountInit.Val);
      const auto Start = Constant(PointerInit.Val);
      if (!Iterations || *Iterations == 0 || *Iterations > 1365 || !Start)
        continue;
      const auto &Steps = Loop.Body;
      const size_t End = Steps.size();
      // Compilers may test the final iteration before computing the next
      // countdown value. Both orders exit before advancing the pointer after
      // the last read, provided the tail has no other control-flow edges.
      const bool TestFirst =
          Steps[End - 6].Kind == StmtKind::Assign &&
          Steps[End - 6].Val &&
          Steps[End - 6].Val->Kind == ExprKind::BinOp &&
          Steps[End - 6].Val->Op == NdOp::INT_NOTEQUAL;
      const size_t DecrementAt = TestFirst ? End - 4 : End - 6;
      const size_t TestAt = TestFirst ? End - 6 : End - 5;
      const size_t ExitAt = TestFirst ? End - 5 : End - 4;
      const auto Decrement = Local(Steps[DecrementAt].Dst);
      const auto Test = Local(Steps[TestAt].Dst);
      const auto Increment = Local(Steps[End - 3].Dst);
      if (!Decrement || !Test || !Increment || *Decrement == *Count ||
          *Decrement == *Pointer || *Test == *Count || *Test == *Pointer ||
          *Increment == *Count || *Increment == *Pointer ||
          Assignment(Steps[DecrementAt], *Decrement) !=
              Steps[DecrementAt].Val ||
          !Binary(Steps[DecrementAt].Val, NdOp::INT_SUB, *Count, 1) ||
          Assignment(Steps[TestAt], *Test) != Steps[TestAt].Val ||
          !Binary(Steps[TestAt].Val, NdOp::INT_NOTEQUAL, *Count, 1) ||
          Steps[ExitAt].Kind != StmtKind::If ||
          !Steps[ExitAt].ElseBody.empty() ||
          Steps[ExitAt].Body.size() != 1 ||
          Steps[ExitAt].Body[0].Kind != StmtKind::Break ||
          !Steps[ExitAt].Cond ||
          Steps[ExitAt].Cond->Kind != ExprKind::UnaryOp ||
          Steps[ExitAt].Cond->Op != NdOp::BOOL_NOT ||
          Steps[ExitAt].Cond->Operands.size() != 1 ||
          Local(Steps[ExitAt].Cond->Operands[0]) != *Test ||
          Assignment(Steps[End - 3], *Increment) != Steps[End - 3].Val ||
          !Binary(Steps[End - 3].Val, NdOp::INT_ADD, *Pointer, 3) ||
          Local(Steps[End - 2].Dst) != *Count ||
          Local(Steps[End - 2].Val) != *Decrement ||
          Local(Steps[End - 1].Dst) != *Pointer ||
          Local(Steps[End - 1].Val) != *Increment)
        continue;
      bool Straight = true;
      std::map<unsigned, std::pair<const HighExpr *, ExprPtr>> Loads;
      va_t Base = Bound ? 0 : *Start >= 2 ? *Start - 2 : 0;
      const uint32_t Extent = uint32_t(*Iterations * 3);
      for (size_t J = 0; J < End - 6 && Straight; ++J) {
        const auto &S = Steps[J];
        if (S.Kind != StmtKind::Assign && S.Kind != StmtKind::Store &&
            S.Kind != StmtKind::Call && S.Kind != StmtKind::ExprStmt) {
          Straight = false;
          break;
        }
        if (S.Kind != StmtKind::Assign || !S.Val ||
            S.Val->Kind != ExprKind::Load)
          continue;
        const auto &Load = S.Val;
        if (!readonly_scalar_detail::ordinary(Load) ||
            Load->Type->Kind != NdTypeKind::Int || Load->Type->Size != 1 ||
            Load->Operands.size() != 1) {
          continue;
        }
        ExprPtr Address = Load->Operands[0];
        ExprPtr Helper;
        if (Bound) {
          if (!readonly_scalar_detail::ordinary(Address) ||
              Address->Type->Size != 8 || Address->Kind != ExprKind::BinOp ||
              Address->Op != NdOp::INT_ADD || Address->Operands.size() != 2)
            continue;
          Helper = Address->Operands[0];
          Address = Address->Operands[1];
          if (!readonly_scalar_detail::ordinary(Helper) ||
              Helper->Type->Size != 8 || Helper->Kind != ExprKind::Call ||
              !Helper->Operands.empty() || !Helper->SourceCallHint ||
              Helper->SourceCallHint->CallKind !=
                  SourceCallTypeHint::Kind::RuntimeReadOnlyBytes ||
              Helper->SourceCallHint->ByteCount != Extent ||
              (Base && Base != Helper->SourceCallHint->TargetAddress)) {
            Straight = false;
            break;
          }
          Base = Helper->SourceCallHint->TargetAddress;
        }
        if (!readonly_scalar_detail::ordinary(Address) ||
            Address->Type->Size != 8)
          continue;
        unsigned Bias = 0;
        if (Local(Address) != *Pointer) {
          if (!Address || Address->Kind != ExprKind::BinOp ||
              Address->Op != NdOp::INT_SUB || Address->Operands.size() != 2 ||
              Local(Address->Operands[0]) != *Pointer) {
            continue;
          }
          const auto Value = Constant(Address->Operands[1]);
          if (!Value || *Value > 2)
            continue;
          Bias = unsigned(*Value);
        }
        if (!Loads.emplace(Bias, std::pair{Load.get(), Helper}).second)
          Straight = false;
      }
      if (!Straight || Loads.size() != 3 || !Loads.count(0) ||
          !Loads.count(1) || !Loads.count(2) || !Base ||
          (Bound ? *Start != 2 : *Start != Base + 2) ||
          !readImmutableImageBytes(Image, Base, Extent))
        continue;

      std::map<Identity, unsigned> Reads, Writes;
      unsigned InitializerOccurrences = 0;
      bool HiddenWrite = false, NarrowView = false;
      walkStmts(Function.Body, [&](const HighStmt &S) {
        if (!Budget)
          return;
        --Budget;
        if (S.Kind == StmtKind::Assign)
          if (const auto Id = Local(S.Dst)) {
            ++Writes[*Id];
            NarrowView |= (*Id == *Count || *Id == *Pointer ||
                           *Id == *Decrement || *Id == *Increment) &&
                          S.Dst->Type->Size != 8;
          }
        forEachRhsExpr(S, [&](const ExprPtr &Root) {
          std::vector<ExprPtr> Pending{Root};
          while (!Pending.empty() && Budget) {
            --Budget;
            const auto E = Pending.back();
            Pending.pop_back();
            if (!E)
              continue;
            InitializerOccurrences += E.get() == PointerInit.Val.get();
            if (const auto Id = Local(E)) {
              ++Reads[*Id];
              NarrowView |= (*Id == *Count || *Id == *Pointer ||
                             *Id == *Decrement || *Id == *Increment) &&
                            E->Type->Size != 8;
            }
            for (const auto &Output : E->IntrinsicOutputs) {
              const auto Id = highSourceLocalIdentity(Output);
              HiddenWrite |= Id == *Count || Id == *Pointer ||
                             Id == *Decrement || Id == *Test ||
                             Id == *Increment;
            }
            Pending.insert(Pending.end(), E->Operands.begin(),
                           E->Operands.end());
          }
        });
      });
      const auto Flow = analyzeHighSourceFlow(Function, false);
      if (!Budget || HiddenWrite || NarrowView || InitializerOccurrences != 1 ||
          !Flow.Complete || !Flow.Items.empty() || Writes[*Count] != 2 ||
          Writes[*Pointer] != 2 || Writes[*Decrement] != 1 ||
          Writes[*Test] != 1 || Writes[*Increment] != 1 || Reads[*Count] != 2 ||
          Reads[*Pointer] != 4 || Reads[*Decrement] != 1 || Reads[*Test] != 1 ||
          Reads[*Increment] != 1)
        continue;
      // The only pointer reads are the three byte loads and one increment;
      // replacing its private initial value therefore cannot change pointer
      // identity, comparisons, stores, calls, or any other visible effect.
      for (const auto &[Bias, Entry] : Loads) {
        const HighExpr *Load = Entry.first;
        Plan P;
        P.Base = Base;
        P.Extent = Extent;
        P.Offset = Load->Operands[0];
        P.BaseExpression = Entry.second;
        if (Bound)
          P.Offset = P.Offset->Operands[1];
        Result.Loads.emplace(Load, std::move(P));
      }
      if (!Bound)
        Result.Initializers.emplace(PointerInit.Val.get(), 2);
    }
    for (const auto &S : Body) {
      Inspect(S.Body, Depth + 1);
      Inspect(S.ElseBody, Depth + 1);
      Inspect(S.DefaultBody, Depth + 1);
      for (const auto &Case : S.Cases)
        Inspect(Case.Body, Depth + 1);
    }
  };
  Inspect(Function.Body, 0);
  if (!Budget)
    return {};
  return Result;
}

inline std::map<const HighExpr *, readonly_scalar_detail::LoadPlan>
readOnlyObjectPointerLoadPlans(const HighFunc &Function,
                               const BinaryImage &Image, bool Bound = false) {
  return readOnlyLoadPlans(Function, Image, Bound, true);
}

/// Prove that an object-table load is transported only through direct local
/// copies to pointer parameters or a pointer return. Integer carriers acquire
/// pointer meaning from those consumers; arithmetic, conditions, stores and
/// indirect wrappers keep the load unbound.
inline std::set<const HighExpr *> readOnlyObjectPointerLoadConsumers(
    const HighFunc &Function,
    const std::map<const HighExpr *, readonly_scalar_detail::LoadPlan> &Plans) {
  using Identity = HighSourceLocalIdentity;
  auto Local = [](const ExprPtr &E) -> std::optional<Identity> {
    if (!E || (E->Kind != ExprKind::Var && E->Kind != ExprKind::Phi) ||
        !E->Operands.empty())
      return std::nullopt;
    return highSourceLocalIdentity(E->Var);
  };
  std::set<const HighExpr *> Result;
  for (const auto &[Candidate, Unused] : Plans) {
    (void)Unused;
    std::set<Identity> Values;
    bool Changed = true;
    size_t Budget = 100000;
    while (Changed && Budget) {
      Changed = false;
      walkStmts(Function.Body, [&](const HighStmt &S) {
        if (!Budget || S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
          return;
        --Budget;
        const auto Dst = Local(S.Dst);
        const auto Src = Local(S.Val);
        if (Dst && (S.Val.get() == Candidate ||
                    (Src && Values.count(*Src))))
          Changed |= Values.insert(*Dst).second;
      });
    }
    if (!Budget)
      continue;

    bool Seen = false, Consumed = false, Valid = true;
    walkStmts(Function.Body, [&](const HighStmt &S) {
      if (!Valid || !Budget)
        return;
      forEachExpr(S, [&](const ExprPtr &Root) {
        if (!Valid || !Budget || !Root || Root == S.Dst)
          return;
        const bool DirectCopy =
            S.Kind == StmtKind::Assign && Root == S.Val && Local(S.Dst);
        const bool DirectReturn =
            Root == S.RetVal && Function.ReturnType &&
            Function.ReturnType->Kind == NdTypeKind::Ptr;
        std::function<void(const ExprPtr &, const HighExpr *, size_t)> Visit;
        Visit = [&](const ExprPtr &E, const HighExpr *Parent, size_t Index) {
          if (!Valid || !E || !Budget)
            return;
          --Budget;
          const auto L = Local(E);
          if (E.get() == Candidate || (L && Values.count(*L))) {
            Seen = true;
            bool PointerArgument = false;
            if (Parent && Parent->Kind == ExprKind::Call &&
                Parent->SourceCallHint &&
                Index < Parent->SourceCallHint->Signature.Parameters.size()) {
              const auto &Type =
                  Parent->SourceCallHint->Signature.Parameters[Index].Type;
              PointerArgument = Type && Type->Kind == NdTypeKind::Ptr;
            }
            Consumed |= (DirectReturn && E == Root) || PointerArgument;
            if (!(DirectCopy && E == Root) &&
                !(DirectReturn && E == Root) && !PointerArgument)
              Valid = false;
            return;
          }
          for (size_t I = 0; I < E->Operands.size(); ++I)
            Visit(E->Operands[I], E.get(), I);
        };
        Visit(Root, nullptr, 0);
      });
    });
    if (Budget && Seen && Consumed && Valid)
      Result.insert(Candidate);
  }
  return Result;
}

/// Revalidate current bounds, bytes and every occurrence of each helper.
/// A table address has no authority outside its proven loads.
inline std::set<const HighExpr *>
readOnlyScalarSourceHelpers(const HighFunc &Function,
                            const BinaryImage &Image) {
  auto Plans = readOnlyScalarLoadPlans(Function, Image, true);
  for (auto &[Load, Plan] : readOnlyLoopBytePlans(Function, Image, true).Loads)
    Plans.emplace(Load, std::move(Plan));
  std::set<const HighExpr *> Allowed, Escaped;
  size_t Budget = 100000;
  walkStmts(Function.Body, [&](const HighStmt &S) {
    if (!Budget)
      return;
    --Budget;
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      std::set<const HighExpr *> Seen;
      while (!Pending.empty() && Budget) {
        --Budget;
        auto E = Pending.back();
        Pending.pop_back();
        if (!E || !Seen.insert(E.get()).second)
          continue;
        if (const auto P = Plans.find(E.get()); P != Plans.end()) {
          Allowed.insert(P->second.BaseExpression.get());
          Pending.push_back(P->second.Offset);
          continue;
        }
        if (E->SourceCallHint &&
            E->SourceCallHint->CallKind ==
                SourceCallTypeHint::Kind::RuntimeReadOnlyBytes)
          Escaped.insert(E.get());
        Pending.insert(Pending.end(), E->Operands.begin(), E->Operands.end());
      }
    });
  });
  if (!Budget)
    return {};
  for (const auto *E : Escaped)
    Allowed.erase(E);
  return Allowed;
}

inline std::set<const HighExpr *>
readOnlyObjectPointerSourceHelpers(const HighFunc &Function,
                                   const BinaryImage &Image) {
  const auto Plans = readOnlyObjectPointerLoadPlans(Function, Image, true);
  const auto Consumers =
      readOnlyObjectPointerLoadConsumers(Function, Plans);
  std::set<const HighExpr *> Allowed, Escaped;
  size_t Budget = 100000;
  walkStmts(Function.Body, [&](const HighStmt &S) {
    if (!Budget)
      return;
    --Budget;
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      std::set<const HighExpr *> Seen;
      while (!Pending.empty() && Budget) {
        --Budget;
        auto E = Pending.back();
        Pending.pop_back();
        if (!E || !Seen.insert(E.get()).second)
          continue;
        if (const auto P = Plans.find(E.get());
            P != Plans.end() && Consumers.count(E.get())) {
          Allowed.insert(P->second.BaseExpression.get());
          Pending.push_back(P->second.Offset);
          continue;
        }
        if (E->SourceCallHint &&
            E->SourceCallHint->CallKind ==
                SourceCallTypeHint::Kind::RuntimeConstantObjectTable)
          Escaped.insert(E.get());
        Pending.insert(Pending.end(), E->Operands.begin(), E->Operands.end());
      }
    });
  });
  if (!Budget)
    return {};
  for (const auto *E : Escaped)
    Allowed.erase(E);
  return Allowed;
}
} // namespace neverd::sdk
#endif
