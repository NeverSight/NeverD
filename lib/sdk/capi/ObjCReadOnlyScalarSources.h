#ifndef NEVERD_SDK_CAPI_OBJC_READONLYSCALARSOURCES_H
#define NEVERD_SDK_CAPI_OBJC_READONLYSCALARSOURCES_H

#include "BorrowedByteSources.h"

#include "neverd/ir/high/HighSourceFlow.h"

#include <algorithm>
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
  const auto Plans = readOnlyScalarLoadPlans(Function, Image, true);
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
