//===- NeverDCAPIBench.cpp - C API: benchmark support ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Structured pipeline benchmark: runs all IR stages and returns JSON
/// timing data.
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "llvm/Support/JSON.h"

#include <chrono>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::sdk;

const char *neverd_bench_run(neverd_session_t Sess, const char *InputPath,
                             int MaxFunctions) {
  auto *S = static_cast<Session *>(Sess);
  PipelineRunner R;
  std::string Err;
  if (!R.load(InputPath, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  using Clock = std::chrono::steady_clock;
  using MsT = std::chrono::milliseconds;
  auto T0Total = Clock::now();

  PipelineOptions LowOpts;
  LowOpts.DumpLow = true;
  LowOpts.EmitDumpOutput = false;
  LowOpts.MaxFunctions =
      MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;

  auto T0 = Clock::now();
  Pipeline LowPipe;
  R.Result = LowPipe.run(R.Img, R.LLVMCtx, LowOpts);
  long LowMs = std::chrono::duration_cast<MsT>(Clock::now() - T0).count();

  int FuncCount = static_cast<int>(R.Result.LowFuncs.size());
  int LowBlocks = 0, LowOps = 0;
  for (auto &LF : R.Result.LowFuncs) {
    LowBlocks += static_cast<int>(LF.Blocks.size());
    for (auto &B : LF.Blocks)
      LowOps += static_cast<int>(B.Ops.size());
  }

  PipelineOptions MedOpts;
  MedOpts.DumpMed = true;
  MedOpts.EmitDumpOutput = false;
  MedOpts.MaxFunctions =
      MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;

  T0 = Clock::now();
  Pipeline MedPipe;
  auto MedResult = MedPipe.run(R.Img, R.LLVMCtx, MedOpts);
  long MedMs = std::chrono::duration_cast<MsT>(Clock::now() - T0).count();

  int MedOpsTotal = 0;
  for (auto &MF : MedResult.MedFuncs)
    for (auto &B : MF.Blocks)
      MedOpsTotal += static_cast<int>(B.Ops.size());

  PipelineOptions HighOpts;
  HighOpts.DumpHigh = true;
  HighOpts.EmitDumpOutput = false;
  HighOpts.MaxFunctions =
      MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;

  T0 = Clock::now();
  Pipeline HighPipe;
  auto HighResult = HighPipe.run(R.Img, R.LLVMCtx, HighOpts);
  long HighMs = std::chrono::duration_cast<MsT>(Clock::now() - T0).count();

  int HighStmts = 0;
  for (auto &HF : HighResult.HighFuncs)
    HighStmts += static_cast<int>(HF.Body.size());

  PipelineOptions LiftOpts;
  LiftOpts.LiftMode = true;
  LiftOpts.MaxFunctions =
      MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;

  llvm::LLVMContext LlvmCtx;
  T0 = Clock::now();
  Pipeline LlvmPipe;
  auto LlvmResult = LlvmPipe.run(R.Img, LlvmCtx, LiftOpts);
  long LlvmMs = std::chrono::duration_cast<MsT>(Clock::now() - T0).count();

  int LlvmFuncs = 0;
  if (LlvmResult.LlvmModule)
    for (auto &F : *LlvmResult.LlvmModule)
      if (!F.isDeclaration())
        ++LlvmFuncs;

  long TotalMs =
      std::chrono::duration_cast<MsT>(Clock::now() - T0Total).count();

  llvm::json::Object Root;
  Root["func_count"] = FuncCount;
  Root["import_count"] = static_cast<int64_t>(R.Img.Imports.size());

  int64_t StringCount = 0;
  for (auto &Seg : R.Img.Segments) {
    if (Seg.isExecutable())
      continue;
    const uint8_t *Data = Seg.Data.data();
    size_t Len = Seg.Data.size();
    size_t RunLen = 0;
    for (size_t I = 0; I <= Len; ++I) {
      uint8_t B = (I < Len) ? Data[I] : 0;
      bool IsPrintable = (B >= 0x20 && B < 0x7F) || B == '\t' || B == '\n';
      if (IsPrintable) {
        ++RunLen;
      } else {
        if (RunLen >= 4 && B == 0)
          ++StringCount;
        RunLen = 0;
      }
    }
  }
  Root["string_count"] = StringCount;
  Root["low_time_ms"] = LowMs;
  Root["med_time_ms"] = MedMs;
  Root["high_time_ms"] = HighMs;
  Root["llvm_time_ms"] = LlvmMs;
  Root["total_time_ms"] = TotalMs;
  Root["low_blocks"] = LowBlocks;
  Root["low_ops"] = LowOps;
  Root["med_ops"] = MedOpsTotal;
  Root["high_stmts"] = HighStmts;
  Root["llvm_funcs"] = LlvmFuncs;

  int64_t CandidateFunctions = 0;
  int64_t AcceptedFunctions = 0;
  int64_t RejectedFunctions = 0;
  int64_t RemovedFunctions = 0;
  int64_t SkippedFunctions = 0;
  int64_t DecodedInstructions = 0;
  int64_t LiftedInstructions = 0;
  int64_t DecodeFailures = 0;
  int64_t UnsupportedInstructions = 0;
  int64_t TruncatedPaths = 0;
  int64_t MedFailures = 0;
  bool Complete = LlvmResult.Success && !LlvmResult.LLVMVerifierFailed &&
                  LlvmResult.MedIRVerifierFailures == 0 &&
                  LlvmResult.BackendUnhandledValueIntrinsics == 0;

  llvm::json::Array AuditFunctions;
  for (const auto &Audit : LlvmResult.FunctionAudits) {
    using D = PipelineFunctionDisposition;
    switch (Audit.Disposition) {
    case D::Candidate:
      ++CandidateFunctions;
      Complete = false;
      break;
    case D::SkippedImportStub:
    case D::SkippedRuntimeScaffold:
    case D::SkippedLimit:
      ++SkippedFunctions;
      break;
    case D::RejectedLowIR:
    case D::RejectedIncomplete:
      ++CandidateFunctions;
      ++RejectedFunctions;
      Complete = false;
      break;
    case D::RemovedJumpTableTarget:
      ++CandidateFunctions;
      ++RemovedFunctions;
      break;
    case D::MedIRFailed:
      ++CandidateFunctions;
      ++RejectedFunctions;
      ++MedFailures;
      Complete = false;
      break;
    case D::Accepted:
      ++CandidateFunctions;
      ++AcceptedFunctions;
      if (!Audit.HasLowIR || !Audit.HasMedIR || !Audit.MedIRVerified ||
          !Audit.HasLLVMDefinition)
        Complete = false;
      break;
    }

    DecodedInstructions += static_cast<int64_t>(Audit.DecodedInstructions);
    LiftedInstructions += static_cast<int64_t>(Audit.LiftedInstructions);
    DecodeFailures += static_cast<int64_t>(Audit.DecodeFailures.size());
    UnsupportedInstructions +=
        static_cast<int64_t>(Audit.UnsupportedInstructions.size());
    TruncatedPaths += static_cast<int64_t>(Audit.TruncatedPaths.size());
    if (Audit.DecodedInstructions != Audit.LiftedInstructions ||
        !Audit.DecodeFailures.empty() ||
        !Audit.UnsupportedInstructions.empty() || !Audit.TruncatedPaths.empty())
      Complete = false;

    auto AddressArray = [](const std::vector<va_t> &Addresses) {
      llvm::json::Array Values;
      for (va_t Address : Addresses)
        Values.push_back(vaHex(Address));
      return Values;
    };

    llvm::json::Object Function;
    Function["name"] = Audit.Name;
    Function["entry"] = vaHex(Audit.Entry);
    Function["disposition"] =
        pipelineFunctionDispositionName(Audit.Disposition);
    Function["decoded_instructions"] =
        static_cast<int64_t>(Audit.DecodedInstructions);
    Function["lifted_instructions"] =
        static_cast<int64_t>(Audit.LiftedInstructions);
    Function["decode_failures"] = AddressArray(Audit.DecodeFailures);
    Function["unsupported_instructions"] =
        AddressArray(Audit.UnsupportedInstructions);
    Function["truncated_paths"] = AddressArray(Audit.TruncatedPaths);
    Function["low_ir"] = Audit.HasLowIR;
    Function["med_ir"] = Audit.HasMedIR;
    Function["med_ir_verified"] = Audit.MedIRVerified;
    Function["llvm_definition"] = Audit.HasLLVMDefinition;
    AuditFunctions.push_back(std::move(Function));
  }

  llvm::json::Object Audit;
  Audit["complete"] = Complete;
  Audit["pipeline_success"] = LlvmResult.Success;
  Audit["detected_functions"] =
      static_cast<int64_t>(LlvmResult.FunctionAudits.size());
  Audit["candidate_functions"] = CandidateFunctions;
  Audit["accepted_functions"] = AcceptedFunctions;
  Audit["rejected_functions"] = RejectedFunctions;
  Audit["removed_functions"] = RemovedFunctions;
  Audit["skipped_functions"] = SkippedFunctions;
  Audit["decoded_instructions"] = DecodedInstructions;
  Audit["lifted_instructions"] = LiftedInstructions;
  Audit["decode_failures"] = DecodeFailures;
  Audit["unsupported_instructions"] = UnsupportedInstructions;
  Audit["truncated_paths"] = TruncatedPaths;
  Audit["med_failures"] = MedFailures;
  Audit["med_ir_verifier_failures"] =
      static_cast<int64_t>(LlvmResult.MedIRVerifierFailures);
  Audit["backend_unhandled_value_intrinsics"] =
      static_cast<int64_t>(LlvmResult.BackendUnhandledValueIntrinsics);
  Audit["llvm_verifier_failed"] = LlvmResult.LLVMVerifierFailed;
  Root["audit"] = std::move(Audit);
  Root["audit_functions"] = std::move(AuditFunctions);

  llvm::json::Array LLVMDefinitions;
  for (const auto &Name : LlvmResult.LLVMDefinitionNames)
    LLVMDefinitions.push_back(Name);
  Root["llvm_definitions"] = std::move(LLVMDefinitions);

  llvm::json::Array Funcs;
  for (auto &LF : R.Result.LowFuncs) {
    llvm::json::Object FObj;
    FObj["name"] = LF.Name;
    FObj["entry"] = vaHex(LF.Entry);
    FObj["blocks"] = static_cast<int64_t>(LF.Blocks.size());
    int Ops = 0;
    for (auto &B : LF.Blocks)
      Ops += static_cast<int>(B.Ops.size());
    FObj["ops"] = Ops;
    Funcs.push_back(std::move(FObj));
  }
  Root["functions"] = std::move(Funcs);

  return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
}
