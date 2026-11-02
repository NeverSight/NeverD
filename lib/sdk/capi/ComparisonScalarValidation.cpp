//===- ComparisonScalarValidation.cpp - Fresh scalar lift admission -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Uses the authoritative strict lift pipeline, retaining audit and model
/// digests. Lifting stops between bounded functions; it is not a hard deadline.
//===----------------------------------------------------------------------===//

#include "ComparisonScalarValidation.h"

#include "SessionImpl.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/diff/DataFlow.h"
#include "neverd/loader/ExceptionFunction.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

#include <optional>

namespace neverd::sdk {
namespace {
using namespace diff;
struct Subject {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module;
  const llvm::Function *Function = nullptr;
  std::optional<ExceptionFunction> Exceptions;
};
void lift(Session &S, const Program &P, uint32_t Index, Subject &Out,
          ScalarLiftSummary &Summary, const ScalarValidationOptions &Settings,
          const ComparisonControl &Control) {
  const auto &F = P.Functions.at(Index);
  if (P.instructionCount(F) > Settings.MaxNativeInstructions)
    throw DataFlowInterrupted(ComparisonState::ResourceLimited);
  if (auto Gap = scalarValidationNativeGap(P, Index))
    throw std::runtime_error(*Gap);
  if (auto Stop = Control.stop())
    throw DataFlowInterrupted(*Stop);
  PipelineOptions Options;
  S.applyAnalysisOptions(Options);
  Options.OnlyFunctionEntries = {F.Address};
  Options.LiftMode = true;
  Options.NoOpt = true;
  Options.EmitDumpOutput = false;
  auto R = Pipeline().run(S.Img, Out.Context, Options, S.decompileDebug());
  if (auto Stop = Control.stop())
    throw DataFlowInterrupted(*Stop);
  const auto Audit = llvm::find_if(
      R.FunctionAudits, [&](const auto &A) { return A.Entry == F.Address; });
  if (Audit != R.FunctionAudits.end()) {
    Summary.Decoded = Audit->DecodedInstructions;
    Summary.Lifted = Audit->LiftedInstructions;
  }
  if (!R.Success || R.SourceImage != &S.Img || !R.LlvmModule ||
      Audit == R.FunctionAudits.end() || !Audit->MedIRVerified ||
      !Audit->HasLLVMDefinition || !Audit->DecodeFailures.empty() ||
      !Audit->UnsupportedInstructions.empty() ||
      !Audit->TruncatedPaths.empty() || !Audit->UnprovenReturns.empty() ||
      Summary.Decoded != Summary.Lifted ||
      Summary.Decoded != P.instructionCount(F))
    throw std::runtime_error(
        R.Error.empty()
            ? "pipeline did not verify the complete selected native body"
            : R.Error);
  for (const auto &Definition : *R.LlvmModule) {
    if (Definition.isDeclaration())
      continue;
    auto Original = rewrite_source::getOriginalVA(Definition);
    if (!Original)
      throw std::runtime_error(llvm::toString(Original.takeError()));
    if (*Original != F.Address)
      continue;
    if (Out.Function)
      throw std::runtime_error(
          "multiple LLVM definitions claim the native entry");
    Out.Function = &Definition;
  }
  if (!Out.Function)
    throw std::runtime_error("no LLVM definition owns the native entry");
  auto Med = llvm::find_if(R.MedFuncs,
                           [&](const auto &M) { return M.Entry == F.Address; });
  if (Med == R.MedFuncs.end() || llvm::count_if(R.MedFuncs, [&](const auto &M) {
                                   return M.Entry == F.Address;
                                 }) != 1)
    throw std::runtime_error("no unique source MedIR owns the native entry");
  Out.Exceptions = std::move(Med->ExceptionMetadata);
  Out.Module = std::move(R.LlvmModule);
  // A diagnostic content identity, not a signed or persistent proof receipt.
  std::string IR;
  llvm::raw_string_ostream Stream(IR);
  Out.Function->print(Stream);
  Summary.SHA256 =
      llvm::toHex(llvm::SHA256::hash(llvm::arrayRefFromStringRef(IR)), true);
  Summary.Verified = true;
}
} // namespace

ComparisonScalarValidation
validateComparisonScalars(Session &AS, const diff::Program &A, uint32_t AI,
                          Session &BS, const diff::Program &B, uint32_t BI,
                          const diff::ScalarValidationOptions &Options,
                          const diff::ComparisonControl &Control) {
  using Status = analysis::LLVMScalarEquivalenceStatus;
  ComparisonScalarValidation R;
  R.Result.Proof.Status = Status::Unsupported;
  R.Result.Proof.InputDomain =
      analysis::LLVMScalarInputDomain::DefinedBitvectors;
  try {
    Subject Left, Right;
    if (Control.Progress)
      Control.Progress("primary_lift", 0, 2);
    lift(AS, A, AI, Left, R.Primary, Options, Control);
    if (Control.Progress)
      Control.Progress("secondary_lift", 1, 2);
    lift(BS, B, BI, Right, R.Secondary, Options, Control);
    R.Result = diff::validateScalarFunctions(
        A, AI,
        {*Left.Function, Left.Exceptions ? &*Left.Exceptions : nullptr,
         AS.Img.abiFormat()},
        B, BI,
        {*Right.Function, Right.Exceptions ? &*Right.Exceptions : nullptr,
         BS.Img.abiFormat()},
        Options, Control);
  } catch (const diff::DataFlowInterrupted &Stop) {
    R.Result.State = Stop.State;
    R.Result.Proof.Status = Stop.State == ComparisonState::ResourceLimited
                                ? Status::BudgetExceeded
                                : Status::Cancelled;
    R.Result.Proof.Diagnostic = Stop.what();
  } catch (const std::exception &Error) {
    R.Result.Proof.Diagnostic = Error.what();
  }
  if (auto Stop = Control.stop()) {
    R.Result.State = *Stop;
    R.Result.Proof.Status = Status::Cancelled;
    R.Result.Proof.Diagnostic = "scalar validation stopped";
  }
  auto &Reason = R.Result.Proof.Diagnostic;
  Reason.resize(
      std::min<size_t>(Reason.size(), limits::diff::kMaxFlowReasonBytes));
  return R;
}
} // namespace neverd::sdk
