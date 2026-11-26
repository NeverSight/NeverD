//===- NeverDCAPISafety.cpp - C API memory-safety audit & hunt -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/safety/SafetyScan.h"
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

struct SafetyRequest {
  std::optional<safety::Track> Legacy;
  safety::ScanSelection Selection = safety::ScanSelection::Both;

  neverd_cancel_probe_fn Cancel = nullptr;
  void *Context = nullptr;

  bool canceled() const { return Cancel && Cancel(Context); }

  const char *name() const {
    return Legacy ? safety::toString(*Legacy) : "scan";
  }

  std::string error(const std::string &Message,
                    safety::ScanState State = safety::ScanState::Failed,
                    const BinaryImage *Img = nullptr) const {
    if (Legacy) {
      safety::SafetyReport Report;
      Report.Origin = *Legacy;
      Report.Error = Message;
      return safety::toJson(Report);
    }
    safety::ScanReport Scan(Selection, State, Message);
    if (Img)
      for (auto &Result : Scan.Analyzers) {
        Result.Report.Format = Img->getFormatName();
        Result.Report.Arch = getArchName(Img->Arch);
      }
    return safety::toJson(Scan);
  }
};

std::string runSafety(Session *S, const neverd_safety_options *Options,
                      const SafetyRequest &Request) {
  if (!S->Loaded)
    return Request.error("no binary loaded");
  if (S->Img.Arch == Arch::EVM || S->Img.Arch == Arch::SBF)
    return Request.error("safety analysis supports native binaries only",
                         safety::ScanState::Unsupported, &S->Img);
  if (Request.canceled())
    return Request.error("scan canceled", safety::ScanState::Canceled, &S->Img);

  safety::SinkCatalog Cat = safety::SinkCatalog::defaults();
  if (const char *P =
          readPath(Options, FIELD_END(neverd_safety_options, sinks_path),
                   &neverd_safety_options::sinks_path))
    if (llvm::Error E = Cat.mergeSinksFromFile(P))
      return Request.error(llvm::toString(std::move(E)),
                           safety::ScanState::Failed, &S->Img);
  if (const char *P =
          readPath(Options, FIELD_END(neverd_safety_options, sources_path),
                   &neverd_safety_options::sources_path))
    if (llvm::Error E = Cat.mergeSourcesFromFile(P))
      return Request.error(llvm::toString(std::move(E)),
                           safety::ScanState::Failed, &S->Img);

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
  if (Request.canceled())
    return Request.error("scan canceled", safety::ScanState::Canceled, &S->Img);
  if (Res.MedIRVerifierFailures != 0 || Res.MedFuncs.empty()) {
    const char *Reason = Res.MedIRVerifierFailures
                             ? "MedIR verification failed"
                             : "lifting failed for this binary";
    return Request.error(Res.Error.empty() ? Reason : Res.Error,
                         safety::ScanState::Unsupported, &S->Img);
  }
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
  if (!Request.Legacy)
    return safety::toJson(
        safety::runScan(In, Cat, Budgets, Request.Selection,
                        [&](safety::Track) { return Request.canceled(); }));
  safety::SafetyReport Report = *Request.Legacy == safety::Track::Hunt
                                    ? safety::runHunt(In, Cat, Budgets)
                                    : safety::runAudit(In, Cat, Budgets);
  if (!Report.AnalysisComplete)
    return Request.error(Report.Error);
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

const char *internalSafetyErrorReportNoThrow(Session *S,
                                             const SafetyRequest &Request,
                                             llvm::StringRef Message) noexcept {
  try {
    std::string Detail = "internal_error: unexpected native ";
    Detail += Request.name();
    Detail += " failure";
    if (!Message.empty()) {
      Detail += ": ";
      Detail += jsonSafeText(Message);
    }
    setSafetyBoundaryErrorNoThrow(S, Detail);
    const char *Owned = dupStr(Request.error(Detail));
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

const char *
runSafetyJSONBoundary(neverd_session_t Sess,
                      const neverd_safety_options *Options,
                      std::optional<safety::Track> Legacy,
                      const neverd_scan_options *Scan = nullptr) noexcept {
  Session *S = toSession(Sess);
  SafetyRequest Request{Legacy};
  try {
    if (S)
      S->clearError();
    if (Scan &&
        reaches(Scan->struct_size, FIELD_END(neverd_scan_options, analysis)))
      Options = Scan->analysis;
    if (Scan &&
        reaches(Scan->struct_size, FIELD_END(neverd_scan_options, context))) {
      Request.Cancel = Scan->cancel;
      Request.Context = Scan->context;
    }
    if (Scan &&
        reaches(Scan->struct_size, FIELD_END(neverd_scan_options, analyzers))) {
      switch (Scan->analyzers) {
      case NEVERD_SCAN_BOTH:
        break;
      case NEVERD_SCAN_AUDIT:
        Request.Selection = safety::ScanSelection::Audit;
        break;
      case NEVERD_SCAN_HUNT:
        Request.Selection = safety::ScanSelection::Hunt;
        break;
      default:
        return dupStr(Request.error("invalid scan analyzer selection"));
      }
    }
    if (!S)
      return dupStr(Request.error("invalid session"));
    if (S->SafetyBeforeRunForTesting)
      S->SafetyBeforeRunForTesting();
    const char *Owned = dupStr(runSafety(S, Options, Request));
    if (!Owned)
      setSafetyBoundaryErrorNoThrow(S, "safety report allocation failed");
    return Owned;
  } catch (const std::bad_alloc &) {
    setSafetyBoundaryErrorNoThrow(S, "safety analysis allocation failed");
    return nullptr;
  } catch (const std::exception &Exception) {
    return internalSafetyErrorReportNoThrow(S, Request, Exception.what());
  } catch (...) {
    return internalSafetyErrorReportNoThrow(S, Request,
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

const char *neverd_session_scan_json(neverd_session_t Sess,
                                     const neverd_scan_options *Options) {
  return runSafetyJSONBoundary(Sess, nullptr, std::nullopt, Options);
}

} // extern "C"
