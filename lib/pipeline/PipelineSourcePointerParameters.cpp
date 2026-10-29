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
#include "neverd/libc/LibCFixedIntegerArguments.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/DenseSet.h"

#include <algorithm>
#include <map>
#include <vector>

namespace neverd {
void refineSourcePointerParameters(const BinaryImage &Image,
                                   PipelineResult &Result,
                                   const DebugContext *Debug) {
  if (!Debug || (Image.Arch != Arch::X64 && Image.Arch != Arch::AArch64))
    return;
  std::map<va_t, std::vector<TypeRef>> Parameters;
  llvm::DenseSet<va_t> Examined;
  struct Candidate {
    size_t Index;
    bool HasDeclaredImport;
  };
  std::vector<Candidate> Candidates;
  std::vector<uint64_t> Weights;
  Candidates.reserve(Result.MedFuncs.size());
  Weights.reserve(Result.MedFuncs.size());
  const auto PointerBytes = getTargetRegInfo(Image.Arch).PointerSize;
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
    const auto &Function = Result.MedFuncs[I];
    if (Function.SourceTypeHint || Function.SourceParametersBound ||
        Function.Params.empty() || Function.IsVariadic ||
        Function.Params.size() != Function.TypedParams.size() ||
        !std::any_of(Function.TypedParams.begin(), Function.TypedParams.end(),
                     [&](const MedTypedParam &Parameter) {
                       return Parameter.Type &&
                              Parameter.Type->Kind == NdTypeKind::Int &&
                              Parameter.Type->Size == PointerBytes;
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
    Candidates.push_back({I, HasDeclaredImport});
    uint64_t Weight = 0;
    for (const auto &Block : Function.Blocks)
      Weight += Block.Ops.size() + Block.Phis.size();
    Weights.push_back(Weight);
  }
  // HighC may retain explicit source-call operands without CallInfos on ABIs
  // with independent register banks. Check those existing bindings on the
  // weighted workers before building any use graph. Import declarations above
  // are shared immutable facts.
  parallelForEachWeighted(Weights, [&](auto Claim, size_t Count) {
    for (size_t I; (I = Claim()) < Count;) {
      auto &Function = Result.MedFuncs[Candidates[I].Index];
      if (!Candidates[I].HasDeclaredImport &&
          !std::any_of(
              Function.Blocks.begin(), Function.Blocks.end(),
              [&](const MedBlock &Block) {
                return std::any_of(
                    Block.Ops.begin(), Block.Ops.end(), [&](const MedOp &Op) {
                      return Op.SourceCallHint ||
                             ((Op.Opcode == NdOp::CALL ||
                               Op.Opcode == NdOp::INDIR_CALL) &&
                              Op.NumInputs > 1 && Op.Inputs[0].isConst() &&
                              Parameters.count(Op.Inputs[0].ConstVal));
                    });
              }))
        continue;
      const auto Pointers =
          inferMedSourcePointerParameters(Function, Image.Arch, Parameters);
      for (size_t P = 0; P < Pointers.size(); ++P)
        if (Pointers[P] && Function.TypedParams[P].Type &&
            Function.TypedParams[P].Type->Kind == NdTypeKind::Int) {
          auto Pointer = NdType::makePtr(NdType::makeVoid());
          Pointer->Size = PointerBytes;
          Function.TypedParams[P].Type = std::move(Pointer);
        }
    }
  });
}
} // namespace neverd
