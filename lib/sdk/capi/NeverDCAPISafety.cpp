//===- NeverDCAPISafety.cpp - C API native memory-safety scan ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "InstructionFlow.h"
#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/safety/SafetyScan.h"
#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <new>
#include <optional>
#include <string>
#include <vector>
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
  safety::ScanSelection Selection = safety::ScanSelection::Both;

  neverd_cancel_probe_fn Cancel = nullptr;
  void *Context = nullptr;

  bool canceled() const { return Cancel && Cancel(Context); }

  std::string error(const std::string &Message,
                    safety::ScanState State = safety::ScanState::Failed,
                    const BinaryImage *Img = nullptr) const {
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

  // Both engines consume the native lift's function inventory and recovered
  // call arguments. Their last input is the machine MedIR ready for emission.
  llvm::LLVMContext Ctx;
  PipelineOptions PO;
  PO.LiftMode = true;
  PO.EmitLiftedLLVM = false;
  if (Request.Selection != safety::ScanSelection::Hunt &&
      isMIPSN32(S->Img.Arch) && S->Img.MIPS &&
      !S->Img.MIPS->RequiredImportVersions.empty() &&
      llvm::is_contained(S->Img.DynInfo.NeededLibs, "libc.so.6")) {
    const auto &Versions = S->Img.MIPS->RequiredImportVersions;
    for (const auto &Import : S->Img.Imports) {
      if (Import.Name != "malloc" && Import.Name != "free")
        continue;
      const auto V = Versions.find(Import.IATAddr);
      if (V == Versions.end() || V->second.Library != "libc.so.6" ||
          V->second.Version != "GLIBC_2.0")
        continue;
      const auto *Sink = Cat.matchSink(Import.Name);
      if (!Sink)
        continue;
      if (Import.Name == "malloc" && Sink->Kind == safety::SinkKind::Alloc &&
          Sink->HandleArg == -1)
        PO.StaticAllocatorSlots.insert(Import.IATAddr);
      else if (Import.Name == "free" && Sink->Kind == safety::SinkKind::Free &&
               Sink->HandleArg == 0 && !Sink->ReleaseMayFail)
        PO.StaticReleaseSlots.insert(Import.IATAddr);
    }
  }
  S->applyAnalysisOptions(PO);
  Pipeline Pipe;
  PipelineResult Res = Pipe.run(S->Img, Ctx, PO, S->Dbg.get());
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

  return safety::toJson(
      safety::runScan(In, Cat, readBudgets(Options), Request.Selection,
                      [&](safety::Track) { return Request.canceled(); }));
}

// Only full-width, initialized slots known to the loader are pointers.
// Two slots containing the same address are two distinct initializers.
uint64_t dataPointerCount(const BinaryImage &Img, const Section &Data) {
  const uint64_t Backed = std::min(Data.Size, Data.FileSz);
  const unsigned Width = Img.getPointerSize();
  uint64_t Count = 0;
  for (auto It = Img.DataPtrRelocSlots.lower_bound(Data.VA);
       It != Img.DataPtrRelocSlots.end(); ++It) {
    const va_t Offset = *It - Data.VA;
    if (Offset >= Backed || Width > Backed - Offset)
      break;
    const auto Target = relocatedPointer(Img, *It);
    const Section *Dest = Target ? Img.getSectionFor(*Target) : nullptr;
    if (!Dest || Dest->IsImageHeader || !Dest->isReadable() ||
        Dest->isExecutable() || Img.isCodeAddress(*Target))
      continue;
    // A one-past pointer may have the same value as the next section's
    // first byte. Its relocation still belongs to the original section.
    const auto Owner = Img.DataPtrRelocTargetOwners.find(*It);
    if (Owner == Img.DataPtrRelocTargetOwners.end() ||
        Dest->contains(Owner->second))
      ++Count;
  }
  return Count;
}

const char *runDataPtrScan(Session &S, neverd_cancel_probe_fn Cancel,
                           void *Context) {
  S.clearError();
  if (!S.Loaded) {
    S.setError("no binary loaded");
    return nullptr;
  }
  const BinaryImage &Img = S.Img;
  if (!archLiftSupported(Img.Arch) ||
      !(Img.isCOFF() || Img.isELF() || Img.isMachO())) {
    S.setError("data-ptr scan requires a native PE/COFF, ELF or Mach-O image");
    return nullptr;
  }
  const Section *Data = Img.getDataSection();
  if (!Data || !Data->isReadable() || Data->isExecutable()) {
    S.setError(
        "data-ptr scan requires a readable, non-executable data section");
    return nullptr;
  }
  if (neverd_session_discover_functions_cancellable(&S, Cancel, Context) < 0 ||
      !S.synchronizeFunctions())
    return nullptr;

  // Overlapping function extents and read/modify/write operands can publish
  // an instruction more than once. Count its original address once.
  llvm::DenseSet<va_t> Instructions;
  size_t FunctionsScanned = 0;
  size_t IncompleteFunctions = 0;
  for (std::optional<va_t> Cursor = 0; Cursor;) {
    if (Cancel && Cancel(Context)) {
      S.setError("data-ptr scan canceled");
      return nullptr;
    }
    const FunctionPage Page =
        functionPage(S, *Cursor, FunctionPage::MaxFunctionsPerPage);
    FunctionsScanned += Page.Functions.size();
    struct FunctionMatches {
      std::vector<va_t> Addresses;
      bool Unlifted = false;
    };
    std::vector<FunctionMatches> Matches(Page.Functions.size());
    std::vector<FunctionDecodeStatus> Coverage;
    const bool Decoded = decodeFunctionReferences(
        S, Page,
        [&](size_t Index, va_t Address, llvm::StringRef,
            const InstructionFlow &Flow, unsigned, InstructionMode) {
          Matches[Index].Unlifted |= Flow.Kind == UnliftedFlow;
          if (llvm::any_of(Flow.Refs, [&](const InstructionRef &Ref) {
                return Data->contains(Ref.To);
              }))
            Matches[Index].Addresses.push_back(Address);
        },
        &Coverage);
    if (!Decoded) {
      S.setError("failed to initialize the native reference decoder");
      return nullptr;
    }
    for (size_t Index = 0; Index < Matches.size(); ++Index) {
      const auto &Batch = Matches[Index];
      Instructions.insert(Batch.Addresses.begin(), Batch.Addresses.end());
      IncompleteFunctions += Coverage[Index].Incomplete || Batch.Unlifted;
    }
    Cursor = Page.NextEntry;
  }
  const uint64_t Pointers = dataPointerCount(Img, *Data);
  if (Cancel && Cancel(Context)) {
    S.setError("data-ptr scan canceled");
    return nullptr;
  }
  return dupStr(jsonToString(
      llvm::json::Object{{"schema", "neverd.data-ptr-scan"},
                         {"schema_version", 1},
                         {"format", Img.getFormatName()},
                         {"arch", getArchName(Img.Arch)},
                         {"section", Data->Name},
                         {"functions_scanned", FunctionsScanned},
                         {"incomplete_functions", IncompleteFunctions},
                         {"code_references", Instructions.size()},
                         {"data_pointers", Pointers}}));
}

void setScanBoundaryErrorNoThrow(Session *S, llvm::StringRef Message) noexcept {
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
    std::string Detail = "internal_error: unexpected native scan failure";
    if (!Message.empty()) {
      Detail += ": ";
      Detail += jsonSafeText(Message);
    }
    setScanBoundaryErrorNoThrow(S, Detail);
    const char *Owned = dupStr(Request.error(Detail));
    if (!Owned)
      setScanBoundaryErrorNoThrow(
          S, "safety allocation failed while reporting an internal error");
    return Owned;
  } catch (const std::bad_alloc &) {
    setScanBoundaryErrorNoThrow(
        S, "safety allocation failed while reporting an internal error");
  } catch (...) {
    setScanBoundaryErrorNoThrow(S, "safety internal error reporting failed");
  }
  return nullptr;
}

} // namespace

extern "C" const char *
neverd_session_scan_json(neverd_session_t Sess,
                         const neverd_scan_options *Scan) {
  Session *S = toSession(Sess);
  SafetyRequest Request;
  try {
    const neverd_safety_options *Options = nullptr;
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
      setScanBoundaryErrorNoThrow(S, "safety report allocation failed");
    return Owned;
  } catch (const std::bad_alloc &) {
    setScanBoundaryErrorNoThrow(S, "safety analysis allocation failed");
    return nullptr;
  } catch (const std::exception &Exception) {
    return internalSafetyErrorReportNoThrow(S, Request, Exception.what());
  } catch (...) {
    return internalSafetyErrorReportNoThrow(S, Request,
                                            "non-standard native exception");
  }
}

extern "C" const char *neverd_session_data_ptr_scan_json(
    neverd_session_t Sess, neverd_cancel_probe_fn Cancel, void *Context) {
  Session *S = toSession(Sess);
  if (!S)
    return nullptr;
  try {
    const char *Report = runDataPtrScan(*S, Cancel, Context);
    if (!Report && S->LastError.empty())
      setScanBoundaryErrorNoThrow(S, "data-ptr report allocation failed");
    return Report;
  } catch (const std::exception &E) {
    setScanBoundaryErrorNoThrow(S, E.what());
  } catch (...) {
    setScanBoundaryErrorNoThrow(S, "unexpected data-ptr scan failure");
  }
  return nullptr;
}
