//===- PipelineSourcePointerParameters.cpp - Forwarded pointer roles -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Refine source parameter carriers from admitted import declarations.
///
//===----------------------------------------------------------------------===//

#include "PipelineCallAbiDetail.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/med/MedSourceParameterUses.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/libc/LibCFixedIntegerArguments.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/DenseSet.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace neverd {
void refineSourcePointerParameters(const BinaryImage &Image,
                                   PipelineResult &Result,
                                   const DebugContext *Debug) {
  if (!Debug || (Image.Arch != Arch::X64 && Image.Arch != Arch::AArch64))
    return;
  std::map<va_t, std::vector<TypeRef>> Parameters;
  std::map<va_t, std::vector<TypeRef>> RegisterParameters;
  llvm::DenseSet<va_t> Examined;
  struct Candidate {
    size_t Index;
    bool HasDeclaredImport;
    std::vector<size_t> Callees;
  };
  std::vector<Candidate> Candidates;
  std::vector<uint64_t> Weights;
  Candidates.reserve(Result.MedFuncs.size());
  Weights.reserve(Result.MedFuncs.size());
  const auto PointerBytes = getTargetRegInfo(Image.Arch).PointerSize;
  const auto NeedsRefinement = [&](const TypeRef &Type) {
    return Type && Type->Size == PointerBytes &&
           (Type->Kind == NdTypeKind::Int ||
            (Type->Kind == NdTypeKind::Ptr && Type->Pointee &&
             Type->Pointee->Kind == NdTypeKind::Void &&
             Type->TypedefName.empty() && Type->SourceName.empty() &&
             Type->Pointee->TypedefName.empty()));
  };
  auto Admit = [&](va_t Target) {
    if (!Examined.insert(Target).second)
      return;
    const auto Name = importCalleeCName(Image, Target);
    const auto Declared =
        Name.empty() ? std::nullopt : Debug->resolvePrototype(Name);
    // Source placement, rather than a name or argument count alone,
    // establishes a dense fixed scalar argument list for this ABI.
    if (Declared && libc::fixedIntegerCallArguments(*Declared, Image.Arch,
                                                    Image.abiFormat())) {
      std::vector<TypeRef> Types;
      Types.reserve(Declared->Params.size());
      bool HasPointer = false;
      for (const auto &[Name, Type] : Declared->Params) {
        Types.push_back(Type);
        HasPointer |= Type->Kind == NdTypeKind::Ptr;
      }
      if (HasPointer)
        Parameters.emplace(Target, std::move(Types));
    }
  };
  // These targets already supplied explicit call operands before SSA. HighC
  // does not need a second call-argument recovery on independent register
  // banks.
  for (const auto &[Target, Arguments] : Result.CallEntryIntegerArguments)
    Admit(Target);
  for (size_t I = 0; I < Result.MedFuncs.size(); ++I) {
    auto &Function = Result.MedFuncs[I];
    if (Function.SourceTypeHint || Function.SourceParametersBound ||
        Function.Params.empty() || Function.IsVariadic)
      continue;
    // A forward-only parameter can appear after the initial type pass. Use
    // the parameter type owner without changing the settled return contract.
    if (Function.Params.size() != Function.TypedParams.size())
      refreshInferredMedParameters(Function, Image.Arch);
    if (!std::any_of(Function.TypedParams.begin(), Function.TypedParams.end(),
                     [&](const MedTypedParam &Parameter) {
                       return NeedsRefinement(Parameter.Type);
                     }))
      continue;
    bool HasDeclaredImport = false;
    for (const auto &Call : Function.CallInfos) {
      Admit(Call.TargetAddr);
      HasDeclaredImport |= Parameters.count(Call.TargetAddr) != 0;
    }
    // An explicit entry declaration owns its parameter types, including an
    // integer deliberately converted to a pointer inside the function.
    if (const auto Declared = Debug->resolveFunction(Function.Entry);
        Declared && !Declared->Params.empty())
      continue;
    Candidates.push_back({I, HasDeclaredImport, {}});
    uint64_t Weight = 0;
    for (const auto &Block : Function.Blocks)
      Weight += Block.Ops.size() + Block.Phis.size();
    Weights.push_back(Weight);
  }
  std::map<va_t, size_t> ByEntry;
  for (size_t I = 0; I < Candidates.size(); ++I)
    ByEntry.emplace(Result.MedFuncs[Candidates[I].Index].Entry, I);
  // Discover local dependencies once. The existing whole-value proof still
  // owns call identity, source argument order and every conflicting use. This
  // graph only schedules those proofs; it supplies no pointer facts itself.
  parallelForEachWeighted(Weights, [&](auto Claim, size_t Count) {
    for (size_t I; (I = Claim()) < Count;) {
      auto &Candidate = Candidates[I];
      const auto &Function = Result.MedFuncs[Candidate.Index];
      const auto ObserveTarget = [&](va_t Target) {
        Candidate.HasDeclaredImport |= Parameters.count(Target) != 0;
        if (const auto Found = ByEntry.find(Target); Found != ByEntry.end())
          Candidate.Callees.push_back(Found->second);
      };
      for (const auto &Call : Function.CallInfos)
        ObserveTarget(Call.TargetAddr);
      for (const auto &Block : Function.Blocks)
        for (const auto &Op : Block.Ops) {
          Candidate.HasDeclaredImport |= bool(Op.SourceCallHint);
          if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
              Op.NumInputs && Op.Inputs[0].isConst())
            ObserveTarget(Op.Inputs[0].ConstVal);
        }
      auto &Callees = Candidate.Callees;
      std::sort(Callees.begin(), Callees.end());
      Callees.erase(std::unique(Callees.begin(), Callees.end()), Callees.end());
    }
  });

  std::vector<std::vector<size_t>> Callers(Candidates.size());
  std::vector<bool> Relevant(Candidates.size());
  std::vector<size_t> Reachable;
  for (size_t I = 0; I < Candidates.size(); ++I) {
    for (auto Callee : Candidates[I].Callees)
      Callers[Callee].push_back(I);
    if (Candidates[I].HasDeclaredImport) {
      Relevant[I] = true;
      Reachable.push_back(I);
    }
  }
  for (size_t Cursor = 0; Cursor < Reachable.size(); ++Cursor)
    for (auto Caller : Callers[Reachable[Cursor]])
      if (!Relevant[Caller]) {
        Relevant[Caller] = true;
        Reachable.push_back(Caller);
      }
  // Only callers connected to declared evidence need a use graph. Evaluate
  // each at most once, after its relevant local callees. Independent functions
  // in a wave retain the weighted parallel path; publication is deterministic
  // and happens only after all readers of the previous facts have finished.
  std::vector<size_t> Pending(Candidates.size()), Ready;
  std::vector<bool> Done(Candidates.size());
  for (auto I : Reachable) {
    for (auto Callee : Candidates[I].Callees)
      Pending[I] += Relevant[Callee];
    if (!Pending[I])
      Ready.push_back(I);
  }
  const auto Refine = [&](const std::vector<size_t> &Batch, bool Publish) {
    std::vector<uint64_t> BatchWeights;
    BatchWeights.reserve(Batch.size());
    for (auto I : Batch)
      BatchWeights.push_back(Weights[I]);
    std::vector<std::vector<TypeRef>> Inferred(Batch.size());
    parallelForEachWeighted(BatchWeights, [&](auto Claim, size_t Count) {
      for (size_t I; (I = Claim()) < Count;) {
        auto &Function = Result.MedFuncs[Candidates[Batch[I]].Index];
        auto &Pointers = Inferred[I];
        Pointers = inferMedSourcePointerParameterTypes(
            Function, Image.Arch, Parameters, RegisterParameters);
        for (size_t P = 0; P < Pointers.size(); ++P)
          if (Pointers[P] && NeedsRefinement(Function.TypedParams[P].Type))
            Function.TypedParams[P].Type = Pointers[P];
          else
            Pointers[P].reset();
      }
    });
    if (Publish)
      for (size_t I = 0; I < Batch.size(); ++I) {
        const auto &Function = Result.MedFuncs[Candidates[Batch[I]].Index];
        if (std::any_of(Inferred[I].begin(), Inferred[I].end(),
                        [](const TypeRef &Type) { return bool(Type); })) {
          // Local call recovery can use a callee's custom register order.
          // Admit that order only when it matches every inferred parameter:
          // no stack, vector, aggregate or reordered source slots are assumed.
          bool RegisterOrder = Function.Params.size() ==
                               Function.IntegerArgumentRegisters.size();
          for (size_t P = 0; RegisterOrder && P < Function.Params.size(); ++P) {
            const auto &Type = Function.TypedParams[P].Type;
            RegisterOrder = Function.Params[P].RegOff ==
                                Function.IntegerArgumentRegisters[P] &&
                            Type &&
                            (Type->Kind == NdTypeKind::Int ||
                             Type->Kind == NdTypeKind::Ptr) &&
                            Type->Size == Function.Params[P].Size &&
                            Type->Size <= PointerBytes;
          }
          if (RegisterOrder)
            RegisterParameters.emplace(Function.Entry, Inferred[I]);
          Parameters.emplace(Function.Entry, std::move(Inferred[I]));
        }
      }
  };
  while (!Ready.empty()) {
    Refine(Ready, true);
    std::vector<size_t> Next;
    for (auto I : Ready) {
      Done[I] = true;
      for (auto Caller : Callers[I])
        if (Relevant[Caller] && !--Pending[Caller])
          Next.push_back(Caller);
    }
    Ready = std::move(Next);
  }
  // A recursive component must not prove its own types by feedback. Preserve
  // the existing intrafunction proof for it and its dependent callers, without
  // publishing provisional results to any other remaining function.
  for (auto I : Reachable)
    if (!Done[I])
      Ready.push_back(I);
  if (!Ready.empty())
    Refine(Ready, false);
}
} // namespace neverd
