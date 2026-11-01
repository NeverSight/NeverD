//===- PipelineSourceInline.cpp - Source call-body specialization ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded expansion of actual source bodies at unavailable call operands.
//===----------------------------------------------------------------------===//

#include "PipelineSourceInline.h"

#include "PipelineCallAbiDetail.h"
#include "PipelineSourceCallObservations.h"
#include "PipelineSourceInlineHigh.h"
#include "PipelineSourceStackAccesses.h"

#include "neverd/backend/LLVMSourceUnknownValue.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Limits.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {
namespace {

std::optional<va_t> address(const llvm::Function &Function) {
  auto Address = rewrite_source::getOriginalVA(Function);
  if (!Address) {
    llvm::consumeError(Address.takeError());
    return std::nullopt;
  }
  return *Address;
}

bool unavailable(const llvm::Value *Value) {
  for (unsigned I = 0; I != limits::pipeline::kSourceInlineOperandDepth; ++I) {
    if (llvm_source_unknown::isValue(Value))
      return true;
    const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Value);
    if (!Cast)
      return false;
    Value = Cast->getOperand(0);
  }
  return false;
}

bool sameSourceCarriers(const llvm::Function &Before,
                        const llvm::Function &After) {
  if (Before.arg_size() != After.arg_size() ||
      Before.isVarArg() != After.isVarArg() ||
      Before.getCallingConv() != After.getCallingConv())
    return false;
  // Joint source recovery may recognize an integer carrier as a pointer.
  // It must not add inputs or alter their physical Win64 widths.
  const auto Same = [](const llvm::Type *A, const llvm::Type *B) {
    const auto Pointer = [](const llvm::Type *T) {
      return T->isPointerTy() && T->getPointerAddressSpace() == 0;
    };
    return A == B || (Pointer(A) && B->isIntegerTy(64)) ||
           (A->isIntegerTy(64) && Pointer(B));
  };
  if (!Same(Before.getReturnType(), After.getReturnType()))
    return false;
  for (unsigned P = 0; P < Before.arg_size(); ++P)
    if (!Same(Before.getArg(P)->getType(), After.getArg(P)->getType()))
      return false;
  return true;
}

/// This is a candidate filter, never an unused-native-parameter proof. A
/// pointer can still observe the physical slot. Only expanding the validated
/// body below lets the source optimizer discard an unused formal operand.
std::set<unsigned> absentStackFormals(const MedFunc &Function) {
  if (Function.CC != CallingConv::Win64 || Function.SkippedSSA ||
      Function.SourceParametersBound || Function.IsVariadic ||
      Function.AnalystStackFrame ||
      !Function.IntegerArgumentRegisters.empty() ||
      !Function.FPParamScalarBytes.empty() ||
      Function.EntryKind != StackEntryKind::Call ||
      (Function.ExceptionMetadata &&
       (Function.ExceptionMetadata->Personality != ExceptionPersonality::None ||
        Function.ExceptionMetadata->ParseStatus !=
            ExceptionParseStatus::Complete)))
    return {};
  const auto Layout =
      getTargetRegInfo(Arch::X64).integerArgumentLayout(BinaryFormat::COFF);
  std::set<unsigned> Candidates;
  for (unsigned P = Layout.Registers.size(); P < Function.Params.size(); ++P)
    Candidates.insert(P);
  size_t Remaining = limits::pipeline::kSourceInlineBodyOperations;
  const auto Observe = [&](const MedVar &Value) {
    if (Value.Kind == MedVar::Param && Value.Id >= 0)
      Candidates.erase(unsigned(Value.Id));
  };
  for (const auto &Block : Function.Blocks) {
    if (!Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty())
      return {};
    for (const auto &Phi : Block.Phis) {
      if (Phi.Args.size() >= Remaining || Phi.ExceptionalEntry)
        return {};
      Remaining -= Phi.Args.size() + 1;
      for (const auto &[Pred, Value] : Phi.Args)
        Observe(Value);
    }
    for (const auto &Op : Block.Ops) {
      if (!Remaining)
        return {};
      --Remaining;
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        Observe(Op.Inputs[I]);
    }
  }
  for (const auto &Call : Function.CallInfos) {
    if (Call.Args.size() > Remaining)
      return {};
    Remaining -= Call.Args.size();
    for (const auto &Arg : Call.Args)
      Observe(Arg);
  }
  return Candidates;
}

} // namespace

SourceInlineResult specializeUnavailableSourceCalls(
    const BinaryImage &Image, const PipelineResult &Original,
    const llvm::Module &Initial, DebugContext *Debug,
    const PipelineOptions &AnalysisOptions, const SourceViewOptions &View,
    const SourceInlineHighView *High) {
  if (Image.Arch != Arch::X64 || Image.abiFormat() != BinaryFormat::COFF ||
      !Original.OmittedCallees || !View.CallTypes.empty() ||
      !View.StackFrame.empty() || !View.VariableTypes.empty() ||
      !View.VariableEdits.empty())
    return {};

  std::set<va_t> Roots;
  std::map<va_t, const llvm::Function *> RootFunctions;
  std::map<va_t, std::set<unsigned>> Requests;
  for (const auto &Function : Initial) {
    if (Function.isDeclaration())
      continue;
    const auto Entry = address(Function);
    if (!Entry)
      return {};
    Roots.insert(*Entry);
    RootFunctions.emplace(*Entry, &Function);
    for (const auto &Instruction : llvm::instructions(Function)) {
      const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction);
      const auto *Callee = Call ? Call->getCalledFunction() : nullptr;
      if (!Callee || !Callee->isDeclaration() || Call->hasOperandBundles())
        continue;
      const auto Target = address(*Callee);
      if (!Target || Original.CallPrototypeArguments.count(*Target))
        continue;
      for (unsigned P = 0; P < Call->arg_size(); ++P)
        if (unavailable(Call->getArgOperand(P)))
          Requests[*Target].insert(P);
    }
  }
  if (Requests.empty() || Roots.empty() ||
      Roots.size() > limits::pipeline::kSourceInlineRoots)
    return {};

  std::set<va_t> Supports;
  for (const auto &Body : Original.OmittedCallees->Bodies) {
    const auto At = Requests.find(Body->Entry);
    if (At == Requests.end() || Roots.count(Body->Entry) ||
        !Original.OmittedCallees->SourceOriginalBlocks.count(Body->Entry))
      continue;
    const auto Absent = absentStackFormals(*Body);
    for (unsigned P : At->second)
      if (Absent.count(P)) {
        Supports.insert(Body->Entry);
        break;
      }
    if (Supports.size() == limits::pipeline::kSourceInlineCallees)
      break;
  }
  if (Supports.empty())
    return {};

  // Reuse the normal source admission path. A signature-reader body alone
  // does not authorize emission: frame, return, exception and ABI validation
  // must all run with the actual caller and callee selected together.
  PipelineOptions Options = AnalysisOptions;
  Options.EmitDumpOutput = false;
  Options.DeferHighIR = true;
  Options.OnlyFunctionEntries = Roots;
  Options.OnlyFunctionEntries.insert(Supports.begin(), Supports.end());
  auto Recovered = Pipeline().run(Image, Initial.getContext(), Options, Debug);
  if (!Recovered.Success ||
      Recovered.MedFuncs.size() != Options.OnlyFunctionEntries.size())
    return {};
  std::map<va_t, std::string> Names;
  for (const auto &Function : Recovered.MedFuncs)
    Names.emplace(Function.Entry, Function.Name);
  std::vector<std::pair<va_t, std::string>> Imports;
  for (const auto &[Entry, Name] : Image.getImportAddressNames()) {
    Names.insert_or_assign(Entry, Name);
    Imports.emplace_back(Entry, Name);
  }
  recoverModuleCallAbi(Image, Recovered, Names, true, true);
  prepareRecoveredSourceStackAccesses(Image, Recovered, Recovered.LowFuncs);

  SourceInlineResult Result;
  Result.Sources = std::make_shared<LLVMSourceMap>();
  MedLLVMEmitter Emitter;
  Emitter.setOriginalImageStorageForSource(true);
  Emitter.setPreserveUnknownSourceValues(true);
  Emitter.setDebugContext(Debug);
  Emitter.setSourceMap(Result.Sources.get());
  Emitter.setSourceCalleeDeclarations(Recovered.OmittedCalleeDeclarations);
  Emitter.setPrototypeArgumentCallees(&Recovered.CallPrototypeArguments);
  Result.Module = Emitter.emit(Recovered.MedFuncs, Initial.getContext(),
                               "neverd_source_calls", Image.Arch, Imports,
                               &Image, Image.abiFormat());
  if (!Result.Module || llvm::verifyModule(*Result.Module))
    return {};
  Result.Sources->preserveExpressionOrigins(Recovered.LibraryRecognitions);
  Pipeline::promoteScaffoldingAllocas(*Result.Module);

  size_t Remaining = limits::pipeline::kSourceInlineExpansionOperations;
  std::map<const llvm::Function *, size_t> Sizes;
  for (const auto &Function : *Result.Module) {
    const auto Entry = address(Function);
    if (!Entry)
      continue;
    if (auto Root = RootFunctions.find(*Entry); Root != RootFunctions.end()) {
      if (Function.isDeclaration() ||
          !sameSourceCarriers(*Root->second, Function))
        return {};
    } else if (Supports.count(*Entry)) {
      size_t Size = 0;
      for (const auto &Block : Function)
        Size += Block.size();
      // An unused outer argument must not be traded for missing machine
      // inputs inside the body. Retain the original call in that case.
      if (!Function.isDeclaration() &&
          Size <= limits::pipeline::kSourceInlineBodyOperations &&
          !source_inline::hasUnavailableObservations(Function))
        Sizes.emplace(&Function, Size);
    }
  }
  unsigned Expanded = 0;
  std::map<va_t, SourceInlineBody> ExpandedBodies;
  for (auto &Function : *Result.Module) {
    const auto Entry = address(Function);
    if (!Entry || !Roots.count(*Entry))
      continue;
    llvm::SmallVector<llvm::CallInst *, 16> Calls;
    for (auto &Instruction : llvm::instructions(Function)) {
      auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction);
      auto *Callee = Call ? Call->getCalledFunction() : nullptr;
      if (!Callee || Callee->isDeclaration() || Call->hasOperandBundles() ||
          Call->isMustTailCall() || !Sizes.count(Callee) ||
          Callee->arg_size() != Call->arg_size())
        continue;
      for (unsigned P = 0; P < Call->arg_size(); ++P)
        if (Callee->getArg(P)->use_empty() &&
            unavailable(Call->getArgOperand(P))) {
          Calls.push_back(Call);
          break;
        }
    }
    for (auto *Call : Calls) {
      const auto *Callee = Call->getCalledFunction();
      const size_t Size = Sizes.at(Callee);
      if (Size > limits::pipeline::kSourceInlineBodyOperations ||
          Size > Remaining)
        continue;
      Remaining -= Size;
      SourceInlineBody Body;
      Body.Parameters = Callee->arg_size();
      for (unsigned P = 0; P < Call->arg_size(); ++P)
        if (Callee->getArg(P)->use_empty() &&
            unavailable(Call->getArgOperand(P)))
          Body.Unused.insert(P);
      const auto Target = address(*Callee);
      llvm::InlineFunctionInfo Info;
      if (llvm::InlineFunction(*Call, Info).isSuccess()) {
        ++Expanded;
        if (High && Target) {
          auto &Saved = ExpandedBodies[*Target];
          Saved.Parameters = Body.Parameters;
          Saved.Unused.insert(Body.Unused.begin(), Body.Unused.end());
        }
      }
    }
  }
  if (!Expanded)
    return {};
  for (auto &Function : *Result.Module)
    if (const auto Entry = address(Function); Entry && Supports.count(*Entry)) {
      Function.deleteBody();
      Function.setSubprogram(nullptr);
    }
  if (llvm::verifyModule(*Result.Module))
    return {};
  const auto Optimized = Pipeline::optimizeOrPromoteModule(
      *Result.Module, Result.Sources.get(), View);
  if (Optimized.Stop == OptimizationStopReason::VerificationFailed ||
      llvm::verifyModule(*Result.Module))
    return {};
  Result.Sources->refreshExpressionOrigins();
  if (High)
    Result.High = specializeHighSourceBodies(Image, Recovered, Options, Debug,
                                             ExpandedBodies, *High);
  return Result;
}
} // namespace neverd
