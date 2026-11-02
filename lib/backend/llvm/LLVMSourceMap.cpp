//===- LLVMSourceMap.cpp - Scalar expression emission ancestry -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/LLVMSourceMap.h"

#include "neverd/backend/Limits.h"

#include "llvm/IR/Instructions.h"

#include <set>

namespace neverd {
namespace {
bool scalarExpression(const llvm::Instruction &I) {
  return (llvm::isa<llvm::BinaryOperator, llvm::CastInst, llvm::CmpInst,
                    llvm::SelectInst, llvm::GetElementPtrInst>(I)) &&
         !I.mayReadOrWriteMemory() && !I.mayHaveSideEffects();
}
} // namespace

void LLVMSourceMap::preserveExpressionOrigins(
    llvm::ArrayRef<sigs::LibraryRecognition> Matches) {
  ExpressionRegions.clear();
  for (const auto &M : Matches)
    if (M.Isolated && M.Scope != sigs::LibraryFeatureScope::WholeFunction)
      ExpressionRegions[M.Function].push_back(M.Occurrences);
  refreshExpressionOrigins();
}

void LLVMSourceMap::refreshExpressionOrigins() {
  std::map<const llvm::Instruction *,
           std::vector<const LLVMSourceObservation *>>
      Index;
  for (const auto &O : Observations)
    if (auto *I = llvm::dyn_cast_or_null<llvm::Instruction>(O.Value))
      Index[I].push_back(&O);
  std::vector<LLVMSourceObservation> Added;
  size_t Budget = ::neverd::limits::backend::
      kLLVMSourceMapLLVMSourceMapRefreshExpressionOriginsBudget;
  for (const auto &O : Observations) {
    auto *Parent = llvm::dyn_cast_or_null<llvm::Instruction>(O.Value);
    if (!Parent ||
        (!scalarExpression(*Parent) && !llvm::isa<llvm::LoadInst>(Parent)))
      continue;
    auto At = ExpressionRegions.find(O.Function);
    if (At == ExpressionRegions.end())
      continue;
    for (const auto &Occurrences : At->second) {
      auto Contains = [&](sigs::LibraryOccurrence Origin) {
        return std::binary_search(Occurrences.begin(), Occurrences.end(),
                                  Origin);
      };
      if (!Contains(O.Occurrence))
        continue;
      std::set<const llvm::Instruction *> Seen;
      std::set<sigs::LibraryOccurrence> Origins;
      std::vector<const llvm::Instruction *> Pending;
      for (const auto &Input : Parent->operands())
        if (auto *I = llvm::dyn_cast<llvm::Instruction>(Input))
          Pending.push_back(I);
      while (!Pending.empty()) {
        if (!Budget)
          return; // Transactional: publish no partial ancestry.
        --Budget;
        const auto *I = Pending.back();
        Pending.pop_back();
        if (!Seen.insert(I).second || I->getParent() != Parent->getParent() ||
            !scalarExpression(*I))
          continue;
        if (auto Found = Index.find(I); Found != Index.end())
          for (const auto *Source : Found->second)
            if (Source->Function == O.Function && Contains(Source->Occurrence))
              Origins.insert(Source->Occurrence);
        for (const auto &Input : I->operands())
          if (auto *Child = llvm::dyn_cast<llvm::Instruction>(Input))
            Pending.push_back(Child);
      }
      for (const auto &Origin : Origins)
        if (Origin != O.Occurrence)
          Added.push_back({O.Function, Origin, Parent, true});
    }
  }
  std::set<std::tuple<va_t, sigs::LibraryOccurrence, llvm::Value *>> Seen;
  for (const auto &O : Observations)
    Seen.emplace(O.Function, O.Occurrence, O.Value);
  for (auto &O : Added)
    if (Seen.emplace(O.Function, O.Occurrence, O.Value).second)
      Observations.push_back(std::move(O));
}
} // namespace neverd
