//===- NeverDCAPISafety.cpp - C API memory-safety audit & hunt -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/safety/Safety.h"
#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <exception>
#include <new>
#include <string>
using namespace neverd;
using namespace neverd::sdk;

namespace {

#define FIELD_END(Type, Field)                                                 \
  (offsetof(Type, Field) + sizeof(static_cast<Type *>(nullptr)->Field))

bool reaches(size_t Size, size_t End) { return Size >= End; }

safety::SafetyBudgets readBudgets(const neverd_safety_options *In) {
  safety::SafetyBudgets B;
  if (!In)
    return B;
  const size_t Size = In->struct_size;
  if (reaches(Size, FIELD_END(neverd_safety_options, max_paths)) &&
      In->max_paths)
    B.MaxPaths = In->max_paths;
  if (reaches(Size, FIELD_END(neverd_safety_options, max_steps)) &&
      In->max_steps)
    B.MaxSteps = In->max_steps;
  if (reaches(Size, FIELD_END(neverd_safety_options, max_loop)) && In->max_loop)
    B.MaxLoop = In->max_loop;
  if (reaches(Size, FIELD_END(neverd_safety_options, solver_conflicts)) &&
      In->solver_conflicts)
    B.SolverConflicts = In->solver_conflicts;
  if (reaches(Size, FIELD_END(neverd_safety_options, max_call_depth)) &&
      In->max_call_depth)
    B.MaxCallDepth = In->max_call_depth;
  if (reaches(Size, FIELD_END(neverd_safety_options, max_summary_iterations)) &&
      In->max_summary_iterations)
    B.MaxSummaryIterations = In->max_summary_iterations;
  return B;
}

using PathMember = const char *neverd_safety_options::*;

const char *readPath(const neverd_safety_options *In, size_t End,
                     PathMember Field) {
  if (!In || !reaches(In->struct_size, End))
    return nullptr;
  const char *Value = In->*Field;
  return (Value && Value[0]) ? Value : nullptr;
}

std::string errorReport(safety::Track Track, const std::string &Message) {
  safety::SafetyReport Report;
  Report.Origin = Track;
  Report.Error = Message;
  return safety::toJson(Report);
}

std::string runSafety(Session *S, const neverd_safety_options *Options,
                      safety::Track Track) {
  if (!S->Loaded)
    return errorReport(Track, "no binary loaded");
  if (S->Img.Arch == Arch::EVM || S->Img.Arch == Arch::SBF)
    return errorReport(Track, "safety analysis supports native binaries only");

  safety::SinkCatalog Cat = safety::SinkCatalog::defaults();
  if (const char *P =
          readPath(Options, FIELD_END(neverd_safety_options, sinks_path),
                   &neverd_safety_options::sinks_path))
    if (llvm::Error E = Cat.mergeSinksFromFile(P))
      return errorReport(Track, llvm::toString(std::move(E)));
  if (const char *P =
          readPath(Options, FIELD_END(neverd_safety_options, sources_path),
                   &neverd_safety_options::sources_path))
    if (llvm::Error E = Cat.mergeSourcesFromFile(P))
      return errorReport(Track, llvm::toString(std::move(E)));

  // The analyses read recovered call arguments, which only the lift pipeline
  // fills, so run one here regardless of the session's default mode.
  llvm::LLVMContext Ctx;
  PipelineOptions PO;
  PO.LiftMode = true;
  S->applyAnalysisOptions(PO);
  Pipeline Pipe;
  PipelineResult Res = Pipe.run(S->Img, Ctx, PO, S->Dbg.get());
  // Call-ABI recovery runs before LLVM emission.  Hunt and audit read
  // MedIR/LowIR, so a CRT emission failure must not hide recovered program
  // functions.  MedIR verification, or no functions at all, still fail closed.
  if (Res.MedIRVerifierFailures != 0)
    return errorReport(Track, Res.Error.empty() ? "MedIR verification failed"
                                                : Res.Error);
  if (Res.MedFuncs.empty())
    return errorReport(Track, Res.Error.empty()
                                  ? "lifting failed for this binary"
                                  : Res.Error);
  if (std::optional<std::string> CoverageError =
          safety::validatePipelineCoverage(Res, &S->Img))
    return errorReport(Track, *CoverageError);

  safety::AnalysisInput In;
  In.Img = &S->Img;
  In.MedFuncs = &Res.MedFuncs;
  In.LowFuncs = &Res.LowFuncs;
  In.ValidatedPipeline = &Res;
  In.Dbg = S->Dbg.get();
  In.DebugKind = S->DbgKind;
  In.Renames = &S->Renames;
  const auto SignatureNames = S->SigDB.buildNameMap();
  In.SignatureNames = &SignatureNames;
  const TargetRegInfo &TRI = getTargetRegInfo(S->Img.Arch);
  In.StackPointerReg = TRI.StackPointer;
  In.FramePointerReg = TRI.FramePointer;
  In.StackRegsKnown = true;

  safety::SafetyBudgets Budgets = readBudgets(Options);
  safety::SafetyReport Report = Track == safety::Track::Hunt
                                    ? safety::runHunt(In, Cat, Budgets)
                                    : safety::runAudit(In, Cat, Budgets);
  return safety::toJson(Report);
}

void setSafetyBoundaryErrorNoThrow(Session *S,
                                   llvm::StringRef Message) noexcept {
  if (!S)
    return;
  try {
    S->clearError();
    S->setError(Message.str());
  } catch (...) {
  }
}

const char *internalSafetyErrorReportNoThrow(Session *S, safety::Track Track,
                                             llvm::StringRef Message) noexcept {
  try {
    std::string Detail = "internal_error: unexpected native ";
    Detail += safety::toString(Track);
    Detail += " failure";
    if (!Message.empty()) {
      Detail += ": ";
      Detail += jsonSafeText(Message);
    }
    setSafetyBoundaryErrorNoThrow(S, Detail);
    const char *Owned = dupStr(errorReport(Track, Detail));
    if (!Owned)
      setSafetyBoundaryErrorNoThrow(
          S, "safety allocation failed while reporting an internal error");
    return Owned;
  } catch (const std::bad_alloc &) {
    setSafetyBoundaryErrorNoThrow(
        S, "safety allocation failed while reporting an internal error");
  } catch (...) {
    setSafetyBoundaryErrorNoThrow(S, "safety internal error reporting failed");
  }
  return nullptr;
}

const char *runSafetyJSONImpl(neverd_session_t Sess,
                              const neverd_safety_options *Options,
                              safety::Track Track) {
  auto *S = toSession(Sess);
  if (!S)
    return dupStr(errorReport(Track, "invalid session"));
  S->clearError();
  if (S->SafetyBeforeRunForTesting)
    S->SafetyBeforeRunForTesting();
  return dupStr(runSafety(S, Options, Track));
}

const char *runSafetyJSONBoundary(neverd_session_t Sess,
                                  const neverd_safety_options *Options,
                                  safety::Track Track) noexcept {
  Session *S = toSession(Sess);
  try {
    const char *Owned = runSafetyJSONImpl(Sess, Options, Track);
    if (!Owned)
      setSafetyBoundaryErrorNoThrow(S, "safety report allocation failed");
    return Owned;
  } catch (const std::bad_alloc &) {
    setSafetyBoundaryErrorNoThrow(S, "safety analysis allocation failed");
    return nullptr;
  } catch (const std::exception &Exception) {
    return internalSafetyErrorReportNoThrow(S, Track, Exception.what());
  } catch (...) {
    return internalSafetyErrorReportNoThrow(S, Track,
                                            "non-standard native exception");
  }
}

} // namespace

extern "C" {

const char *neverd_session_audit_json(neverd_session_t Sess,
                                      const neverd_safety_options *Options) {
  return runSafetyJSONBoundary(Sess, Options, safety::Track::Audit);
}

const char *neverd_session_hunt_json(neverd_session_t Sess,
                                     const neverd_safety_options *Options) {
  return runSafetyJSONBoundary(Sess, Options, safety::Track::Hunt);
}

} // extern "C"
