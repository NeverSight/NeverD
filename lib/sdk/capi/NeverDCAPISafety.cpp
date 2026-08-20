//===- NeverDCAPISafety.cpp - C API memory-safety audit & hunt -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/safety/Safety.h"
#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/Support/JSON.h"

#include <cstddef>
#include <set>

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
  return B;
}

const char *readPath(const neverd_safety_options *In, size_t End,
                     const char *Value) {
  if (!In || !reaches(In->struct_size, End))
    return nullptr;
  return (Value && Value[0]) ? Value : nullptr;
}

std::string errorReport(const std::string &Message) {
  llvm::json::Object O{{"schema_version", 1},
                       {"ok", false},
                       {"verdict", "UNKNOWN"},
                       {"confidence", "LOW"},
                       {"error", Message}};
  return jsonToString(llvm::json::Value(std::move(O)));
}

std::string runSafety(Session *S, const neverd_safety_options *Options,
                      safety::Track Track) {
  if (!S->Loaded)
    return errorReport("no binary loaded");
  if (S->Img.Arch == Arch::EVM || S->Img.Arch == Arch::SBF)
    return errorReport("safety analysis supports native binaries only");

  safety::SinkCatalog Cat = safety::SinkCatalog::defaults();
  if (const char *P =
          readPath(Options, FIELD_END(neverd_safety_options, sinks_path),
                   Options ? Options->sinks_path : nullptr))
    if (llvm::Error E = Cat.mergeSinksFromFile(P))
      return errorReport(llvm::toString(std::move(E)));
  if (const char *P =
          readPath(Options, FIELD_END(neverd_safety_options, sources_path),
                   Options ? Options->sources_path : nullptr))
    if (llvm::Error E = Cat.mergeSourcesFromFile(P))
      return errorReport(llvm::toString(std::move(E)));

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
    return errorReport(Res.Error.empty() ? "MedIR verification failed"
                                         : Res.Error);
  if (Res.MedFuncs.empty())
    return errorReport(Res.Error.empty() ? "lifting failed for this binary"
                                         : Res.Error);

  safety::AnalysisInput In;
  In.Img = &S->Img;
  In.MedFuncs = &Res.MedFuncs;
  In.LowFuncs = &Res.LowFuncs;
  In.Dbg = S->Dbg.get();
  In.DebugKind = S->DbgKind;
  In.Renames = &S->Renames;
  std::set<va_t> SigNamed;
  for (const auto &M : S->SigDB.matches())
    SigNamed.insert(M.Address);
  In.SignatureNamed = &SigNamed;
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

} // namespace

extern "C" {

const char *neverd_session_audit_json(neverd_session_t Sess,
                                      const neverd_safety_options *Options) {
  auto *S = toSession(Sess);
  if (!S)
    return dupStr(errorReport("invalid session"));
  S->clearError();
  return dupStr(runSafety(S, Options, safety::Track::Audit));
}

const char *neverd_session_hunt_json(neverd_session_t Sess,
                                     const neverd_safety_options *Options) {
  auto *S = toSession(Sess);
  if (!S)
    return dupStr(errorReport("invalid session"));
  S->clearError();
  return dupStr(runSafety(S, Options, safety::Track::Hunt));
}

} // extern "C"
