//===- AArch64SVEVectorState.h - Local Z carrier closure ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// HighC's local scalable carrier is admitted by producer/consumer provenance,
/// not integer width. Whole copies and lowered PHI edge assignments carry
/// that representation across blocks. Other transport requires its own ABI.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64SVEVECTORSTATE_H
#define NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64SVEVECTORSTATE_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/AArch64/AArch64SVEVector.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace neverd {

inline bool isSVEVectorName(const HighExpr &E) {
  return (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) && E.Type &&
         E.Type->Size == aarch64_sve::kVectorStorageBytes &&
         (E.Var.Kind == MedVar::Reg || E.Var.Kind == MedVar::Temp) &&
         E.Var.Size == aarch64_sve::kVectorStorageBytes;
}

inline bool isSVEVectorProducer(const HighExpr &E) {
  const auto Operation = aarch64_sve::vectorOperation(E.IntrinsicId);
  return E.Kind == ExprKind::Call && Operation && Operation->ProducesVector;
}

inline bool aarch64VectorHighShapeIsValid(const HighExpr &E) {
  using namespace aarch64_sve;
  if (E.Kind != ExprKind::Call || E.Operands.size() > kVectorMaxOperands ||
      !E.IntrinsicOutputs.empty() || E.IsIndirectCall || E.IndirectTarget ||
      E.MemoryOrdering != NdMemoryOrdering::None ||
      E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  if (E.Type && E.Type->Size && E.Type->Kind != NdTypeKind::Int &&
      E.Type->Kind != NdTypeKind::Unknown)
    return false;
  VectorShape Shape;
  Shape.NumOperands = E.Operands.size();
  Shape.OutputBytes = E.Type ? E.Type->Size : 0;
  for (size_t I = 0; I != E.Operands.size(); ++I) {
    const auto &Operand = E.Operands[I];
    if (!Operand || !Operand->Type)
      return false;
    const bool Address = (E.IntrinsicId == Intrinsic::A64_SveSt1 ||
                          E.IntrinsicId == Intrinsic::A64_SveStnt1) &&
                         I == kVectorAddressInputIndex;
    if (Operand->Type->Kind != NdTypeKind::Int &&
        Operand->Type->Kind != NdTypeKind::Unknown &&
        !(Address && Operand->Type->Kind == NdTypeKind::Ptr))
      return false;
    Shape.Operands[I] = {Operand->Type->Size,
                         Operand->Kind == ExprKind::Const
                             ? std::optional<uint64_t>(Operand->ConstVal)
                             : std::nullopt};
  }
  return isValidVectorShape(E.IntrinsicId, Shape);
}

inline std::set<std::string> aarch64SVEVectorLocalNames(
    const HighFunc &Func, Arch TheArch,
    const std::function<std::string(const MedVar &)> &NameOf) {
  using namespace aarch64_sve;
  std::map<std::string, std::set<std::string>> Copies;
  std::set<std::string> Seeds, Producers, Defined, Invalid;
  std::function<void(const HighExpr &)> Collect = [&](const HighExpr &E) {
    if (const auto Op = vectorOperation(E.IntrinsicId);
        E.Kind == ExprKind::Call && Op) {
      if (TheArch != Arch::AArch64 || !aarch64VectorHighShapeIsValid(E))
        throw std::invalid_argument(kMalformedVector);
      for (unsigned I = 0; I != Op->OperandCount; ++I) {
        if (!Op->consumesVector(I))
          continue;
        const auto &Input = *E.Operands[I];
        if (isSVEVectorName(Input))
          Seeds.insert(NameOf(Input.Var));
        else if (!isSVEVectorProducer(Input))
          throw std::invalid_argument(kUnsupportedVectorTransport);
      }
    }
    E.forEachChildExpr([&](const ExprPtr &Child) { Collect(*Child); });
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (E)
        Collect(*E);
    });
    if (S.Kind != StmtKind::Assign || !S.Dst || !isSVEVectorName(*S.Dst))
      return;
    const auto Dest = NameOf(S.Dst->Var);
    Defined.insert(Dest);
    if (S.Val && isSVEVectorProducer(*S.Val)) {
      Seeds.insert(Dest);
      Producers.insert(Dest);
    } else if (S.Val && isSVEVectorName(*S.Val)) {
      const auto Source = NameOf(S.Val->Var);
      Copies[Dest].insert(Source);
      Copies[Source].insert(Dest);
    } else {
      Invalid.insert(Dest);
    }
  });

  std::set<std::string> Names;
  for (const auto &Seed : Seeds) {
    if (Names.count(Seed))
      continue;
    bool HasProducer = false;
    std::vector<std::string> Work{Seed};
    while (!Work.empty()) {
      const auto Name = Work.back();
      Work.pop_back();
      if (!Names.insert(Name).second)
        continue;
      if (!Defined.count(Name) || Invalid.count(Name))
        throw std::invalid_argument(kUnsupportedVectorTransport);
      HasProducer |= Producers.count(Name) != 0;
      for (const auto &Copy : Copies[Name])
        Work.push_back(Copy);
    }
    if (!HasProducer)
      throw std::invalid_argument(kUnsupportedVectorTransport);
  }

  // Reject scalable ABI escape, arithmetic on the fixed backing integer,
  // partial slices, and memory copies. These are not whole Z assignments.
  std::function<void(const HighExpr &, bool)> Check = [&](const HighExpr &E,
                                                          bool Allowed) {
    if (((isSVEVectorName(E) && Names.count(NameOf(E.Var))) ||
         isSVEVectorProducer(E)) &&
        !Allowed)
      throw std::invalid_argument(kUnsupportedVectorTransport);
    const auto Op = E.Kind == ExprKind::Call ? vectorOperation(E.IntrinsicId)
                                             : std::nullopt;
    for (size_t I = 0; I != E.Operands.size(); ++I)
      if (E.Operands[I])
        Check(*E.Operands[I], Op && Op->consumesVector(I));
    if (E.IndirectTarget)
      Check(*E.IndirectTarget, false);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Dst && !isSVEVectorName(*S.Dst))
      Check(*S.Dst, false);
    forEachRhsExpr(S, [&](const ExprPtr &E) {
      if (!E)
        return;
      const bool WholeAssignment = S.Kind == StmtKind::Assign && E == S.Val &&
                                   S.Dst && isSVEVectorName(*S.Dst) &&
                                   Names.count(NameOf(S.Dst->Var));
      // A validated checked-pointer operation must still execute when its
      // vector result is discarded. This is not a scalable ABI escape.
      const bool DiscardedCheck = S.Kind == StmtKind::ExprStmt && E == S.Val &&
                                  E->Kind == ExprKind::Call &&
                                  cpaOperation(E->IntrinsicId).has_value();
      Check(*E, WholeAssignment || DiscardedCheck);
    });
  });
  return Names;
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64SVEVECTORSTATE_H
