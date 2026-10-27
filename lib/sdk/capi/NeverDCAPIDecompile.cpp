//===- NeverDCAPIDecompile.cpp - C API: decompilation and IR
//---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Single-function decompilation, multi-stage IR dump (Low/Med/High/LLVM),
/// and high-level pipeline operations for lift and decompile-all.
///
//===----------------------------------------------------------------------===//

#include "FunctionAuditReason.h"
#include "InstructionFlow.h"
#include "LLVMFunctionView.h"
#include "LibraryPresentation.h"
#include "SessionImpl.h"
#include "SourceEdits.h"

#include "neverd/Limits.h"
#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/dialect/SourceDialect.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/evm/analysis/EVMAnalyzer.h"
#include "neverd/evm/emit/EVMCEmitter.h"
#include "neverd/evm/emit/EVMSolidityEmitter.h"
#include "neverd/ir/NdOps.h"
#include "neverd/ir/high/HighIRPrint.h"
#include "neverd/ir/high/HighVariableEdits.h"
#include "neverd/loader/ARM/ARMModeCLIStrings.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/emit/SBFCEmitter.h"
#include "neverd/sbf/emit/SBFRustEmitter.h"
#include "neverd/sdk/Limits.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/ScopeExit.h"

#include <limits>
#include <set>
#include <stdexcept>

using namespace neverd;
using namespace neverd::sdk;

namespace {
struct IRRowOrigin {
  std::string ObjectId;
  const char *Kind = "decoration";
  const char *Status = "unmapped";
  std::optional<va_t> Address;
  int Sequence = -1;
};
using IRRowSink = std::function<void(llvm::StringRef, const IRRowOrigin &)>;

std::set<std::pair<va_t, int>> instructionOrigins(const LowFunc &F) {
  std::set<std::pair<va_t, int>> Origins;
  for (const auto &B : F.Blocks)
    for (const auto &Boundary : B.InstructionBoundaries) {
      if (Boundary.FirstOp > B.Ops.size() ||
          Boundary.OpCount > B.Ops.size() - Boundary.FirstOp)
        continue;
      for (uint64_t I = Boundary.FirstOp;
           I < Boundary.FirstOp + Boundary.OpCount; ++I) {
        const auto &Op = B.Ops[I];
        if (Op.Seq >= 0 && Op.Addr == Boundary.Address && Boundary.Size > 0)
          Origins.emplace(Op.Addr, Op.Seq);
      }
    }
  return Origins;
}

std::string rowId(llvm::StringRef Stage, va_t Entry, int Block,
                  llvm::StringRef Kind, size_t Index = 0) {
  return (Stage + ":" + vaHex(Entry) + ":block:" + std::to_string(Block) + ":" +
          Kind + ":" + std::to_string(Index))
      .str();
}

// Both legacy text and mapped pages use these exact emission points. A name or
// operand containing a newline is split into physical rows by the page sink,
// rather than guessed from a parallel line-count model.
void emitLowView(const LowFunc &F, const IRRowSink &Sink,
                 bool CollectAnchors = false) {
  const auto Origins =
      CollectAnchors ? instructionOrigins(F) : std::set<std::pair<va_t, int>>();
  auto Emit = [&](const IRRowOrigin &Origin, auto Write) {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    Write(OS);
    Sink(Text, Origin);
  };
  Emit({"low:" + vaHex(F.Entry) + ":header", "header"}, [&](auto &OS) {
    OS << "; LowIR: " << F.Name << " @ " << vaHex(F.Entry) << "\n";
  });
  for (const auto &B : F.Blocks) {
    Emit({rowId("low", F.Entry, B.Id, "block"), "block"}, [&](auto &OS) {
      OS << "block_" << B.Id << ":  ; [" << vaHex(B.StartAddr) << " - "
         << vaHex(B.EndAddr) << ")\n";
    });
    for (size_t Index = 0; Index < B.Ops.size(); ++Index) {
      const auto &Op = B.Ops[Index];
      IRRowOrigin Origin{rowId("low", F.Entry, B.Id, "op", Index) + ":" +
                             vaHex(Op.Addr) + ":seq:" + std::to_string(Op.Seq),
                         "operation"};
      if (Origins.count({Op.Addr, Op.Seq})) {
        Origin.Address = Op.Addr;
        Origin.Sequence = Op.Seq;
        Origin.Status = "instruction_anchor";
      }
      Emit(Origin, [&](auto &OS) {
        OS << "  " << ndOpName(Op.Opcode);
        if (Op.Output.Size > 0)
          OS << " -> (" << static_cast<int>(Op.Output.Space) << ":"
             << Op.Output.Offset << ":" << Op.Output.Size << ")";
        for (int I = 0; I < Op.NumInputs; ++I)
          OS << " (" << static_cast<int>(Op.Inputs[I].Space) << ":"
             << Op.Inputs[I].Offset << ":" << Op.Inputs[I].Size << ")";
        OS << "\n";
      });
    }
    Emit({rowId("low", F.Entry, B.Id, "successors"), "successors"},
         [&](auto &OS) {
           OS << "  succs: [";
           for (size_t I = 0; I < B.Succs.size(); ++I) {
             if (I)
               OS << ", ";
             OS << B.Succs[I];
           }
           OS << "]\n";
         });
  }
}

void emitHighView(const HighFunc &F, const LowFunc *Low,
                  const HighSourceMap &Sources, const IRRowSink &Sink) {
  const auto Canonical =
      Low ? instructionOrigins(*Low) : std::set<std::pair<va_t, int>>();
  std::optional<HighStatementSourceIndex> Index;
  if (Low)
    Index.emplace(Sources);
  const std::string Prefix = "high:" + vaHex(F.Entry);
  Sink("; HighIR: " + F.Name + " @ " + vaHex(F.Entry) + "\n",
       {Prefix + ":header", "header"});
  Sink("; Params: " + std::to_string(F.Params.size()) +
           ", Locals: " + std::to_string(F.Locals.size()) + "\n",
       {Prefix + ":frame", "header"});
  if (F.ReturnProtection)
    Sink(std::string("; Return protection: PAC, key=") +
             (F.ReturnProtection->BKey ? "IB" : "IA") +
             ", modifier=entry SP, " +
             (F.ReturnProtection->Leaf ? "leaf" : "non-leaf") + "\n",
         {Prefix + ":return-protection", "header"});
  Sink("\n", {Prefix + ":blank"});
  size_t Row = 0;
  for (const auto &S : F.Body)
    printHighStatementRows(
        S, 0, [&](llvm::StringRef Text, const HighStmt *Statement) {
          IRRowOrigin Origin{Prefix + ":row:" + std::to_string(Row++),
                             Statement ? "operation" : "decoration"};
          if (Statement && Index)
            if (const auto O = Index->lookup(F.Entry, *Statement);
                O && Canonical.contains({O->Address, O->Sequence})) {
              Origin.Status = "instruction_anchor";
              Origin.Address = O->Address;
              Origin.Sequence = O->Sequence;
            }
          Sink(Text, Origin);
        });
}

void emitMedView(const MedFunc &F, const LowFunc *Low, const IRRowSink &Sink) {
  const auto LowOrigins =
      Low ? instructionOrigins(*Low) : std::set<std::pair<va_t, int>>();
  auto Emit = [&](const IRRowOrigin &Origin, auto Write) {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    Write(OS);
    Sink(Text, Origin);
  };
  Emit({"med:" + vaHex(F.Entry) + ":header", "header"}, [&](auto &OS) {
    OS << "; MedIR: " << F.Name << " @ " << vaHex(F.Entry) << "\n";
  });
  Emit({"med:" + vaHex(F.Entry) + ":frame", "header"}, [&](auto &OS) {
    OS << "; CC: " << static_cast<int>(F.CC) << " FrameSize: " << F.FrameSize
       << "\n";
  });
  for (const auto &B : F.Blocks) {
    Emit({rowId("med", F.Entry, B.Id, "block"), "block"},
         [&](auto &OS) { OS << "block_" << B.Id << ":\n"; });
    for (size_t Index = 0; Index < B.Phis.size(); ++Index) {
      const auto &Phi = B.Phis[Index];
      Emit({rowId("med", F.Entry, B.Id, "phi", Index), "phi", "synthetic"},
           [&](auto &OS) {
             OS << "  PHI " << Phi.Output.display() << " = [";
             for (size_t I = 0; I < Phi.Args.size(); ++I) {
               if (I)
                 OS << ", ";
               OS << "b" << Phi.Args[I].first << ":"
                  << Phi.Args[I].second.display();
             }
             OS << "]\n";
           });
    }
    for (size_t Index = 0; Index < B.Ops.size(); ++Index) {
      const auto &Op = B.Ops[Index];
      IRRowOrigin Origin{
          rowId("med", F.Entry, B.Id, "op", Index) + ":" + vaHex(Op.Addr) +
              ":seq:" + std::to_string(Op.OriginSeq),
          "operation", Op.OriginSeq < 0 ? "synthetic" : "unmapped"};
      if (Op.OriginSeq >= 0 && LowOrigins.count({Op.Addr, Op.OriginSeq})) {
        Origin.Address = Op.Addr;
        Origin.Sequence = Op.OriginSeq;
        Origin.Status = "instruction_anchor";
      }
      Emit(Origin, [&](auto &OS) {
        OS << "  " << ndOpName(Op.Opcode);
        if (Op.Output.Id >= 0)
          OS << " " << Op.Output.display() << " =";
        for (int I = 0; I < Op.NumInputs; ++I)
          OS << " " << Op.Inputs[I].display();
        // Source views leave this store out: say so where the IR shows it.
        if (Op.PrologueSave)
          OS << " ; prologue save";
        if (Op.ReturnAddressRead)
          OS << " ; return address";
        OS << "\n";
      });
    }
    Emit({rowId("med", F.Entry, B.Id, "successors"), "successors"},
         [&](auto &OS) {
           OS << "  succs: [";
           for (size_t I = 0; I < B.Succs.size(); ++I) {
             if (I)
               OS << ", ";
             OS << B.Succs[I];
           }
           OS << "]\n";
         });
  }
}
} // namespace

// ===--------------------------------------------------------------------===//
// Single-function decompilation
// ===--------------------------------------------------------------------===//

namespace {
std::optional<std::string> missingARMModeReason(const BinaryImage &Img,
                                                va_t Entry) {
  if (Img.Arch != Arch::ARM || !Img.isCodeAddress(Entry) ||
      Img.instructionModeAt(Entry))
    return std::nullopt;
  return "ARM/Thumb mode cannot be established for function at 0x" +
         llvm::utohexstr(Entry) + "; supply --" + arm_mode_cli::Option +
         arm_mode_cli::HintEntryPrefix + arm_mode_cli::ARM + "|" +
         arm_mode_cli::Thumb;
}

/// Why \p Entry has no HighIR, naming the pipeline's audit disposition when
/// it recorded one (for example a rejected or absorbed candidate).
std::string missingHighFunctionReason(const PipelineResult &Result,
                                      neverd_va_t Entry) {
  return missingPipelineFunctionReason(Result, Entry,
                                       "function not found in HighIR");
}

/// A note for each function the pipeline refused, with why, which a whole
/// unit begins with: a refused function is otherwise missing from it with
/// no word, where asking for it alone names the reason.
std::string refusedFunctionNotes(const PipelineResult &Result) {
  std::vector<const PipelineFunctionAudit *> Refused;
  for (const PipelineFunctionAudit &Audit : Result.FunctionAudits)
    switch (Audit.Disposition) {
    case PipelineFunctionDisposition::RejectedLowIR:
    case PipelineFunctionDisposition::RejectedIncomplete:
    case PipelineFunctionDisposition::RejectedUnwindlessNonLeaf:
    case PipelineFunctionDisposition::MedIRFailed:
      Refused.push_back(&Audit);
      break;
    default:
      break;
    }
  llvm::sort(Refused,
             [](const auto *A, const auto *B) { return A->Entry < B->Entry; });
  std::string Notes;
  for (const PipelineFunctionAudit *Audit : Refused) {
    std::string Name = Audit->Name.empty()
                           ? "sub_" + llvm::utohexstr(Audit->Entry)
                           : Audit->Name;
    // A name cannot end the comment it stands in.
    for (size_t At = Name.find("*/"); At != std::string::npos;
         At = Name.find("*/", At))
      Name.replace(At, 2, "* /");
    Notes += "/* neverd.not-decompiled: " + Name + " at 0x" +
             llvm::utohexstr(Audit->Entry) + ": " +
             functionAuditReason(*Audit) + " */\n";
  }
  return Notes.empty() ? Notes : Notes + "\n";
}
} // namespace

/// Whether the program has local types, whose records a unit defines: a
/// context made for the user's declarations alone holds none.
static bool holdsLocalTypes(const LocalTypesDebugContext &Local) {
  return Local.library().size() != 0;
}

/// The source \p Route emitted for \p Entry under the current pipeline, if it
/// holds what the caller needs: a map when \p SourceMap asks for one.
static const char *cachedSource(Session &S, va_t Entry,
                                Session::SourceRoute Route,
                                CSourceMap *SourceMap) {
  const Session::FunctionSource *Source = S.findFunctionSource(Entry, Route);
  if (!Source || (SourceMap && !Source->Map))
    return nullptr;
  if (SourceMap) {
    *SourceMap = *Source->Map;
    SourceMap->Recognitions = &S.PipeResult.LibraryRecognitions;
  }
  return dupStr(Source->Text);
}

/// Keep what an emission of \p Entry's source produced and return a copy for
/// the caller.  Empty output is a refusal and is not kept.
static const char *keptSource(Session &S, va_t Entry,
                              Session::SourceRoute Route, std::string Text,
                              const CSourceMap *SourceMap) {
  const char *Result = dupStr(Text);
  if (!Text.empty())
    S.rememberFunctionSource(Entry, Route, std::move(Text), SourceMap);
  return Result;
}

/// Why C emission refused: the C type layer throws for a type C cannot
/// spell, which ends the decompile with that reason, not the process.
static std::string emissionFailure(const std::exception &Error) {
  return std::string("C emission failed: ") + Error.what();
}

static int prepareFunction(neverd_session_t Sess, neverd_va_t FuncEntry,
                           Session::PipelineDepth Depth) {
  auto *S = toSession(Sess);
  if (!S)
    return 0;
  S->clearError();
  try {
    if (!S->Loaded) {
      S->setError("no binary loaded");
      return 0;
    }
    if (auto Reason = missingARMModeReason(S->Img, FuncEntry)) {
      S->setError(*Reason);
      return 0;
    }
    if (S->Img.Format == BinaryFormat::COFF && !S->ExceptionHandlersSettled) {
      // Handler discovery can grow metadata that the borrowed CFG facts
      // read.  When it adds anything, even if it then throws, they are
      // dropped before any reads them again; nothing reads them during it.
      // Each call scans every record, so once discovery can add nothing for
      // any function, preparing one skips it.
      const auto Before = coff_loader::exceptionHandlerState(S->Img);
      {
        const llvm::scope_exit Retire([&] {
          if (coff_loader::exceptionHandlerState(S->Img) != Before)
            S->AnalysisCache.reset();
        });
        coff_loader::ensureExceptionHandlers(S->Img, {FuncEntry});
      }
      S->ExceptionHandlersSettled =
          coff_loader::exceptionHandlersSettled(S->Img);
    }

    if (!S->PipeRan)
      S->OnlyFunctionEntries.insert(FuncEntry);
    else if (!S->OnlyFunctionEntries.empty() &&
             !S->OnlyFunctionEntries.count(FuncEntry)) {
      S->selectFunction(FuncEntry);
    }
    return S->ensurePipeline(Depth) ? 1 : 0;
  } catch (const std::exception &Error) {
    S->setError(std::string("function analysis failed: ") + Error.what());
  } catch (...) {
    S->setError("unexpected function analysis failure");
  }
  return 0;
}

static int prepareFunction(neverd_session_t Sess, neverd_va_t FuncEntry,
                           bool NeedHighIR) {
  return prepareFunction(Sess, FuncEntry,
                         NeedHighIR ? Session::PipelineDepth::HighIR
                                    : Session::PipelineDepth::Analysis);
}

const char *neverd_warm_functions_json(neverd_session_t Sess,
                                       neverd_va_t FirstEntry,
                                       int MaxFunctions) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  try {
    if (!S->Loaded) {
      S->setError("no binary loaded");
      return nullptr;
    }
    std::optional<AnalysisCacheWarmup> Warmup;
    std::optional<va_t> Next;
    // A PE function's preparation keeps this state unless its handler
    // discovery adds metadata, which after a full load it does not.  Bytecode
    // images have no C view to prepare.
    if (S->Img.Arch != Arch::EVM && S->Img.Arch != Arch::SBF &&
        S->synchronizeFunctions()) {
      // ensurePipeline drops analysis state from before the current
      // signature feature generation.  The state built here belongs to the
      // current one, so record it as ensurePipeline would after a run.
      if (S->PipelineFeatureGeneration != S->SigDB.featureGeneration()) {
        S->clearPipeline();
        S->PipelineFeatureGeneration = S->SigDB.featureGeneration();
      }
      // The function list as the first selection reads it, built here, where
      // nothing waits, rather than in that selection.
      S->refreshFunctionList();
      // So is the index its C view's names are resolved in.
      (void)S->functionIndexNamed({});
      // The C view of the first function opened reads the widths of the
      // image's data accesses, which scanning every function finds.  Scan
      // them here, where nothing waits, as that view would; the session
      // keeps them while the functions' extents stay.
      S->dataAccessWidths();
      // Selections read bodies and graphs (x86-64), or the signature bodies
      // of the callees they leave out (where each call's setup gives its
      // arguments), that warming builds a page of functions at a time.
      {
        const FunctionPage Page = functionPage(*S, FirstEntry, MaxFunctions);
        std::vector<va_t> Entries;
        Entries.reserve(Page.Functions.size());
        for (const FuncInfo *F : Page.Functions)
          Entries.push_back(F->Entry);
        PipelineOptions Opts;
        S->applyAnalysisOptions(Opts);
        Opts.AnalysisCache = &S->AnalysisCache;
        Warmup = Pipeline().warmAnalysisCache(S->Img, Opts, S->decompileDebug(),
                                              Entries);
        Next = Page.NextEntry;
      }
      // The prototypes of callees are read by the names the image states
      // for them; read those here too.
      if (const LocalTypesDebugContext *Local = S->LocalTypesDbg.get()) {
        Local->indexStatedNames();
        Local->prepareDefinitions();
      }
      // So is what the C emitter reads of the image for every unit.
      (void)S->cEmitter();
    }
    llvm::json::Object Result;
    Result["available"] = Warmup.has_value();
    Result["prepared"] = static_cast<int64_t>(Warmup ? Warmup->Prepared : 0);
    Result["full"] = Warmup && Warmup->Full;
    Result["next_entry"] =
        Warmup && Next ? llvm::json::Value(vaHex(*Next)) : nullptr;
    return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
  } catch (const std::exception &Error) {
    S->setError(std::string("function preparation failed: ") + Error.what());
  } catch (...) {
    S->setError("unexpected function preparation failure");
  }
  return nullptr;
}

int neverd_prepare_function(neverd_session_t Sess, neverd_va_t FuncEntry) {
  return prepareFunction(Sess, FuncEntry, /*NeedHighIR=*/true);
}

int neverd_prepare_function_view(neverd_session_t Sess, neverd_va_t FuncEntry,
                                 const char *Representation) {
  auto *S = toSession(Sess);
  if (!S)
    return 0;
  const llvm::StringRef Stage = Representation ? Representation : "";
  if (Stage != "cfg" && Stage != "low" && Stage != "med" && Stage != "high" &&
      Stage != "llvm" && Stage != "llvmc" && Stage != "source" &&
      !sourceDialectFromKey(Stage)) {
    S->setError("invalid function view");
    return 0;
  }
  // The control-flow graph reads LowIR alone; the other IR views share the
  // analysis the source views take before HighIR.
  return prepareFunction(Sess, FuncEntry,
                         Stage == "cfg" ? Session::PipelineDepth::LowIR
                         : Stage == "low" || Stage == "med" ||
                                 Stage == "llvm" || Stage == "llvmc"
                             ? Session::PipelineDepth::Analysis
                             : Session::PipelineDepth::HighIR);
}

static const char *decompileHighC(neverd_session_t Sess, neverd_va_t FuncEntry,
                                  CSourceMap *SourceMap, bool PlainC = false) {
  if (!neverd_prepare_function(Sess, FuncEntry))
    return dupStr(std::string());
  auto *S = toSession(Sess);

  if (S->PipeResult.EVM) {
    auto Output = evm::emitC(*S->PipeResult.EVM);
    if (!Output) {
      S->setError(llvm::toString(Output.takeError()));
      return dupStr(std::string());
    }
    return dupStr(*Output);
  }

  if (S->PipeResult.SBF) {
    auto Output = sbf::emitC(*S->PipeResult.SBF);
    if (!Output) {
      S->setError(llvm::toString(Output.takeError()));
      return dupStr(std::string());
    }
    return dupStr(*Output);
  }

  const auto Route =
      PlainC ? Session::SourceRoute::PlainC : Session::SourceRoute::HighC;
  if (const char *Cached = cachedSource(*S, FuncEntry, Route, SourceMap))
    return Cached;

  const HighFunc *HF = S->findHighFunc(FuncEntry);
  if (!HF) {
    S->setError(missingHighFunctionReason(S->PipeResult, FuncEntry));
    return dupStr(std::string());
  }

  // A single-function view needs its native EH handlers, not every function
  // from an earlier whole-image analysis. Follow only authenticated clauses;
  // include nested handlers once and retain their original ABI.
  std::map<va_t, const HighFunc *> ByEntry;
  for (const HighFunc &Func : S->PipeResult.HighFuncs)
    ByEntry[Func.Entry] = &Func;
  std::set<va_t> Included = {HF->Entry};
  std::vector<const HighFunc *> Needed = {HF};
  for (size_t I = 0; I < Needed.size(); ++I)
    walkStmts(Needed[I]->Body, [&](const HighStmt &Stmt) {
      if (Stmt.Kind != StmtKind::CxxTry)
        return;
      for (const auto &Clause : Stmt.EHClauses) {
        const va_t Entry = Clause.Kind == HighEHClauseKind::CxxCleanup
                               ? Clause.FilterOrActionVA
                               : Clause.HandlerVA;
        if (auto It = ByEntry.find(Entry);
            It != ByEntry.end() && Included.insert(Entry).second)
          Needed.push_back(It->second);
      }
    });
  std::vector<HighFunc> Related;
  Related.reserve(Needed.size());
  for (const HighFunc *Func : Needed)
    Related.push_back(*Func);
  if (!PlainC)
    attachCxxFuncletBodies(Related);
  const HighFunc *Attached = nullptr;
  for (const HighFunc &Func : Related)
    if (Func.Entry == FuncEntry) {
      Attached = &Func;
      break;
    }
  if (!Attached)
    Attached = HF;

  std::vector<HighFunc> Single = {*Attached};
  if (PlainC)
    Single = std::move(Related);
  std::string Out;
  llvm::raw_string_ostream OS(Out);

  CEmitterOptions Opts;
  std::string VariableSnapshot =
      SourceMap || !S->SourceOptions.VariableTypes.empty() ||
              !S->SourceOptions.VariableEdits.empty()
          ? variableCallProfile(
                highVariableSnapshot(*Attached, S->SourceOptions.Symbolic),
                S->SourceOptions)
          : std::string();
  HighSourceMap EditedSources = S->PipeResult.HighSources;
  bool EditFailed = false;
  for (const auto &Edit : S->SourceOptions.VariableEdits) {
    SourceVariableEditResult Status;
    if (EditFailed || VariableSnapshot.empty() ||
        Edit.Snapshot != VariableSnapshot) {
      Status.Refusal = "The source analysis or an earlier association changed; "
                       "reset or reapply the association";
    } else {
      auto Current =
          std::find_if(Single.begin(), Single.end(),
                       [&](const HighFunc &F) { return F.Entry == FuncEntry; });
      auto Changed = editHighVariable(
          *Current, EditedSources,
          {Edit.Kind == "map" ? HighVariableEdit::Map : HighVariableEdit::Split,
           Edit.From, Edit.To});
      Status.Refusal = Changed.Refusal;
      if (Changed) {
        *Current = std::move(Changed.Function);
        for (const auto &Observation : EditedSources)
          if (Observation.Function != FuncEntry)
            Changed.Sources.push_back(Observation);
        EditedSources = std::move(Changed.Sources);
        Status.Applied = true;
        Status.Outputs = std::move(Changed.Outputs);
        VariableSnapshot = editedVariableSnapshot(VariableSnapshot, Edit);
      }
    }
    EditFailed |= !Status.Applied;
    if (SourceMap)
      SourceMap->VariableEditResults.push_back(std::move(Status));
  }
  if (SourceMap)
    SourceMap->VariableSnapshot = VariableSnapshot;
  const SourceVariableTypes VariableTypes =
      sourceVariableTypes(*S, FuncEntry, VariableSnapshot,
                          SourceMap ? &SourceMap->StaleVariableTypes : nullptr);
  Opts.VariableTypes = &VariableTypes;
  std::map<int64_t, CEmitterOptions::FrameVariable> FrameVariables;
  for (const auto &V : S->SourceOptions.StackFrame.Variables) {
    std::string Error;
    auto Type = V.Type.empty()
                    ? NdType::makeInt(V.Bytes)
                    : parseSourceVariableType(*S, V.Type, V.Bytes, Error);
    if (!Type)
      throw std::invalid_argument(Error);
    FrameVariables[V.Offset] = {V.Name, Type};
  }
  Opts.FrameVariables = &FrameVariables;
  Opts.StructuredExceptionSyntax = !PlainC;
  Opts.TheArch = S->Img.Arch;
  Opts.Format = S->Img.abiFormat();
  Opts.Image = &S->Img;
  Opts.UserNames = &S->Renames;
  Opts.AccessWidths = S->dataAccessWidths();
  if (SourceMap) {
    SourceMap->Recognitions = &S->PipeResult.LibraryRecognitions;
    SourceMap->HighSources = &EditedSources;
    Opts.SourceMap = SourceMap;
  }
  DebugContext *Debug = S->decompileDebug();
  // The program's records, as its local types define them, which the
  // source map keeps for another language's spelling of the unit.
  if (SourceMap)
    SourceMap->LocalTypes.reset();
  if (const LocalTypesDebugContext *Local = S->LocalTypesDbg.get();
      Local && Debug == Local && holdsLocalTypes(*Local))
    Opts.RecordDefinitions = [Local,
                              SourceMap](const std::vector<TypeRef> &Records) {
      CRecordDefinitions Definitions = Local->definitions(Records);
      if (SourceMap && !Definitions.Text.empty())
        SourceMap->LocalTypes = CSourceLocalTypes{
            Definitions.Text, Definitions.Types, Definitions.DeclaredTypes,
            Definitions.HeaderTypes};
      return Definitions;
    };
  try {
    S->cEmitter().emit(Single, OS, Opts, Debug);
    if (SourceMap)
      validateSourceStackBindings(S->SourceOptions.StackFrame, *SourceMap,
                                  FuncEntry, "HighC");
    if (SourceMap)
      SourceMap->HighSources = nullptr;
    if (SourceMap)
      noteUnboundVariableTypes(*SourceMap, VariableTypes, FuncEntry);
  } catch (const std::exception &Error) {
    S->setError(emissionFailure(Error));
    return dupStr(std::string());
  }

  return keptSource(*S, FuncEntry, Route, std::move(Out), SourceMap);
}

const char *neverd_decompile(neverd_session_t Sess, neverd_va_t FuncEntry) {
  return decompileHighC(Sess, FuncEntry, nullptr, true);
}

const char *neverd_decompile_llvm(neverd_session_t Sess,
                                  neverd_va_t FuncEntry) {
  return neverd_decompile_llvm_ex(Sess, FuncEntry, 0);
}

static const char *decompileLlvmC(neverd_session_t Sess, neverd_va_t FuncEntry,
                                  int NoOpt, CSourceMap *SourceMap) {
  if (!prepareFunction(Sess, FuncEntry, /*NeedHighIR=*/false))
    return dupStr(std::string());
  auto *S = toSession(Sess);

  if (S->PipeResult.EVM) {
    S->setError("LLVM-to-C route is not supported for EVM; use the "
                "dedicated C backend");
    return dupStr(std::string());
  }
  if (S->PipeResult.SBF) {
    S->setError("LLVM-to-C route is not supported for SBF; use the "
                "dedicated C or Rust backend");
    return dupStr(std::string());
  }

  const auto Route =
      NoOpt ? Session::SourceRoute::LLVMCNoOpt : Session::SourceRoute::LLVMC;
  if (const char *Cached = cachedSource(*S, FuncEntry, Route, SourceMap))
    return Cached;

  const auto *Native = S->ensureFunctionLlvmModule(
      FuncEntry, NoOpt != 0, /*OriginalImageStorage=*/true);
  if (!Native)
    return dupStr(std::string());
  llvm::Function *LF = S->findNativeLlvmFunction(*Native->Module, FuncEntry);
  if (!LF)
    return dupStr(std::string());

  std::string Out;
  llvm::raw_string_ostream OS(Out);
  CEmitterOptions Opts;
  std::string VariableIR;
  llvm::raw_string_ostream VariableOS(VariableIR);
  std::string VariableSnapshot;
  if (SourceMap || !S->SourceOptions.VariableTypes.empty() ||
      !S->SourceOptions.VariableEdits.empty()) {
    LF->print(VariableOS);
    VariableOS << FuncEntry << ':' << S->SourceOptions.Symbolic << ':'
               << static_cast<unsigned>(S->SourceOptions.LLVM) << ':'
               << S->SourceOptions.Passes << ':'
               << S->SourceOptions.RecoverControlFlow;
    VariableSnapshot = variableCallProfile(sourceVariableSnapshot(VariableIR),
                                           S->SourceOptions);
  }
  Opts.VariableSnapshot = VariableSnapshot;
  std::vector<SourceVariableEdit> VariableEdits =
      S->SourceOptions.VariableEdits;
  for (auto &Edit : VariableEdits) {
    Edit.AfterSnapshot = editedVariableSnapshot(VariableSnapshot, Edit);
    VariableSnapshot = Edit.AfterSnapshot;
  }
  Opts.VariableEdits = &VariableEdits;
  if (S->UsingAnalystCallContracts && S->CallVariant)
    Opts.CallTypes = &S->CallVariant->Hints;
  if (SourceMap)
    SourceMap->VariableSnapshot = Opts.VariableSnapshot;
  const SourceVariableTypes VariableTypes =
      sourceVariableTypes(*S, FuncEntry, VariableSnapshot,
                          SourceMap ? &SourceMap->StaleVariableTypes : nullptr);
  Opts.VariableTypes = &VariableTypes;
  std::map<int64_t, CEmitterOptions::FrameVariable> FrameVariables;
  for (const auto &V : S->SourceOptions.StackFrame.Variables) {
    std::string Error;
    auto Type = V.Type.empty()
                    ? NdType::makeInt(V.Bytes)
                    : parseSourceVariableType(*S, V.Type, V.Bytes, Error);
    if (!Type)
      throw std::invalid_argument(Error);
    FrameVariables[V.Offset] = {V.Name, Type};
  }
  Opts.FrameVariables = &FrameVariables;
  Opts.TheArch = S->Img.Arch;
  Opts.Format = S->Img.abiFormat();
  Opts.UserNames = &S->Renames;
  Opts.AccessWidths = S->dataAccessWidths();
  if (SourceMap) {
    SourceMap->Recognitions = &S->PipeResult.LibraryRecognitions;
    SourceMap->LLVMSources = Native->Sources.get();
    Opts.SourceMap = SourceMap;
  }
  DebugContext *Debug = S->decompileDebug();
  // The program's records, as its local types define them.
  if (const LocalTypesDebugContext *Local = S->LocalTypesDbg.get();
      Local && Debug == Local && holdsLocalTypes(*Local))
    Opts.RecordDefinitions = [Local,
                              SourceMap](const std::vector<TypeRef> &Records) {
      CRecordDefinitions Definitions = Local->definitions(Records);
      if (SourceMap && !Definitions.Text.empty())
        SourceMap->LocalTypes = CSourceLocalTypes{
            Definitions.Text, Definitions.Types, Definitions.DeclaredTypes,
            Definitions.HeaderTypes};
      return Definitions;
    };
  try {
    LLVMCEmitter Emitter;
    Emitter.emit(*Native->Module, OS, Opts, Debug, &S->Img, LF);
    if (SourceMap)
      validateSourceStackBindings(S->SourceOptions.StackFrame, *SourceMap,
                                  FuncEntry, "LLVM C");
    if (SourceMap)
      noteUnboundVariableTypes(*SourceMap, VariableTypes, FuncEntry);
  } catch (const std::exception &Error) {
    S->setError(emissionFailure(Error));
    return dupStr(std::string());
  }
  return keptSource(*S, FuncEntry, Route, std::move(Out), SourceMap);
}

const char *neverd_decompile_llvm_ex(neverd_session_t Sess,
                                     neverd_va_t FuncEntry, int NoOpt) {
  return decompileLlvmC(Sess, FuncEntry, NoOpt, nullptr);
}

namespace {

/// The symbol naming the function at \p Entry, or empty for a function the
/// image does not name.
llvm::StringRef functionSymbol(const BinaryImage &Img, va_t Entry) {
  for (const Symbol &Sym : Img.Symbols)
    if (Sym.Addr == Entry && Sym.IsFunc && !Sym.Name.empty() &&
        !llvm::StringRef(Sym.Name).starts_with(kAutoFuncPrefix))
      return Sym.Name;
  return {};
}

/// The language a source view of \p Entry reads in: the one \p Stage
/// names, or for `source` the function's own; none for a language the image
/// is not offered in.
std::optional<SourceDialect> viewDialect(const Session &S, va_t Entry,
                                         llvm::StringRef Stage) {
  const LanguageRuntimeInfo &Language = S.Img.ExceptionMetadata.Runtime;
  // The runtime whose personality dispatches this function's handlers.
  SourceLanguageRuntime Handler = SourceLanguageRuntime::Unknown;
  if (const ExceptionFunction *EH =
          S.Img.ExceptionMetadata.findFunctionEntry(Entry))
    Handler = getPersonalityRuntime(EH->Personality);
  if (Stage == "source") {
    const llvm::StringRef Symbol = functionSymbol(S.Img, Entry);
    return sourceDialectOfFunction(Symbol, !Symbol.empty(), Language, Handler);
  }
  const SourceDialect Dialect =
      sourceDialectFromKey(Stage).value_or(SourceDialect::C);
  if (!llvm::is_contained(offeredSourceDialects(Language), Dialect) &&
      dialectOfRuntime(Handler) != Dialect)
    return std::nullopt;
  return Dialect;
}

/// Why \p Dialect is not offered for an image: the languages it reads in.
std::string unofferedDialectReason(SourceDialect Dialect,
                                   const LanguageRuntimeInfo &Language) {
  std::string Offered;
  const std::vector<SourceDialect> Dialects = offeredSourceDialects(Language);
  for (size_t I = 0; I < Dialects.size(); ++I)
    Offered += (I == 0                     ? ""
                : I + 1 == Dialects.size() ? " and "
                                           : ", ") +
               sourceDialectDisplayName(Dialects[I]).str();
  return (sourceDialectDisplayName(Dialect) + " pseudocode is offered for " +
          sourceDialectDisplayName(Dialect) +
          " programs; this program's reads in " + Offered)
      .str();
}

/// The local types \p Original, a HighC unit, defines (\p Held, as
/// CSourceMap::LocalTypes keeps them) as \p Dialect, Rust or Go, defines
/// them, from the library of \p Local they were rendered from, and how its
/// code reaches their members; none when the text holds no definitions, or
/// not once.
std::optional<SourceDialectLocalTypes>
dialectLocalTypes(const LocalTypesDebugContext *Local, llvm::StringRef Original,
                  const std::optional<CSourceLocalTypes> &Held,
                  SourceDialect Dialect) {
  if (!Held || !Local)
    return std::nullopt;
  const size_t At = Original.find(Held->Text);
  if (At == llvm::StringRef::npos ||
      Original.find(Held->Text, At + 1) != llvm::StringRef::npos)
    return std::nullopt;
  const types::TypeLibrary &Library = Local->library();
  const auto found = [&](const std::vector<std::string> &Names) {
    std::vector<const types::LocalType *> Types;
    for (const std::string &Name : Names)
      if (const types::LocalType *Type = Library.find(Name))
        Types.push_back(Type);
    return Types;
  };
  types::RenderOptions Render;
  Render.Lang = Dialect == SourceDialect::Rust ? types::Language::Rust
                                               : types::Language::Go;
  // As the C does: exact widths, with the checks of every layout.
  Render.Spelling = types::BuiltinSpelling::Portable;
  Render.Asserts = true;
  const types::EmbeddedSource Embedded = types::renderEmbeddedSource(
      Library, found(Held->Types), found(Held->DeclaredTypes), Render);
  SourceDialectLocalTypes Result;
  Result.CBegin = At;
  Result.CEnd = At + Held->Text.size();
  Result.Text = Embedded.Text;
  for (const auto &[Record, Members] : Embedded.Members) {
    auto &Paths = Result.Members[Record];
    for (const auto &[Name, Path] : Members) {
      SourceDialectLocalTypes::Path &To = Paths[Name];
      for (const types::MemberPath::Hop &Hop : Path.Hops)
        To.Hops.push_back({Hop.Name, Hop.Accessor});
      To.Getter = Path.Getter;
      To.Setter = Path.Setter;
    }
  }
  for (const std::string &Name : Held->DeclaredTypes)
    Result.Opaque.insert(Name);
  return Result;
}

/// \p Entry's HighC source spelled in \p Dialect, with its library regions
/// and definition moved to the spelled text, kept with the C it spells.
const Session::FunctionSource &dialectSource(neverd_session_t Sess, va_t Entry,
                                             SourceDialect Dialect) {
  auto &S = *toSession(Sess);
  const auto Route = Dialect == SourceDialect::Cpp ? Session::SourceRoute::Cpp
                     : Dialect == SourceDialect::Rust
                         ? Session::SourceRoute::Rust
                         : Session::SourceRoute::Go;
  if (const auto *Cached = S.findFunctionSource(Entry, Route);
      Cached && Cached->Map)
    return *Cached;
  CSourceMap Map;
  const char *Raw = decompileHighC(Sess, Entry, &Map);
  std::unique_ptr<const char, decltype(&neverd_free_string)> Owned(
      Raw, neverd_free_string);
  if (!Raw || !S.LastError.empty())
    throw std::runtime_error(S.LastError.empty() ? "source emission failed"
                                                 : S.LastError);
  SourceDialectOptions Options;
  Options.Dialect = Dialect;
  Options.TheArch = S.Img.Arch;
  Options.Format = S.Img.abiFormat();
  Options.Names = Map.Names;
  if (S.LocalTypesDbg)
    Options.Types = &S.LocalTypesDbg->library();
  if (Map.LocalTypes)
    Options.HeaderTypes = Map.LocalTypes->HeaderTypes;
  const llvm::StringRef Original(Raw);
  if (Dialect == SourceDialect::Rust || Dialect == SourceDialect::Go)
    Options.LocalTypes = dialectLocalTypes(S.LocalTypesDbg.get(), Original,
                                           Map.LocalTypes, Dialect);
  SourceDialectText Spelled = spellInDialect(Original, Options);
  for (CSourceRegion &Region : Map.Regions) {
    std::vector<CSourceSpan> Spans;
    for (const CSourceSpan &Span : Region.Spans)
      if (auto Moved = Spelled.map(Span.Begin, Span.End))
        Spans.push_back({Moved->first, Moved->second});
    // A region the spelling does not keep whole is not folded.
    if (Spans.size() != Region.Spans.size())
      Region.Mapped = false;
    Region.Spans = std::move(Spans);
  }
  std::vector<CSourceDefinition> Definitions;
  for (const CSourceDefinition &Definition : Map.Definitions)
    if (auto Moved = Spelled.mapOffset(Definition.Begin))
      Definitions.push_back({Definition.Entry, *Moved});
  Map.Definitions = std::move(Definitions);
  std::vector<CSourceAnchor> Anchors;
  for (const auto &Anchor : Map.Anchors)
    if (auto Moved =
            Spelled.mapExact(Original, Anchor.Span.Begin, Anchor.Span.End))
      Anchors.push_back(
          {Anchor.Function, {Moved->first, Moved->second}, Anchor.Occurrences});
  Map.Anchors = std::move(Anchors);
  S.rememberFunctionSource(Entry, Route, std::move(Spelled.Text), &Map);
  Session::FunctionSource &Kept = *S.findFunctionSource(Entry, Route);
  Kept.Unread = std::move(Spelled.Unread);
  Kept.Names = std::move(Spelled.Names);
  return Kept;
}

/// Writes the page of \p Entry's view \p Stage, from line \p Offset and of at
/// most \p Limit lines, to \p OS: the JSON object llvm::json prints for it,
/// keys in its order, written once the page is known rather than built as a
/// value first.  A large function's page holds a row for each of its
/// thousands of lines.
void writeSourcePage(llvm::raw_ostream &OS, neverd_session_t Sess, va_t Entry,
                     llvm::StringRef Stage, size_t Offset, size_t Limit) {
  auto &S = *toSession(Sess);
  CSourceMap Map;
  std::optional<SourceDialect> Dialect;
  if (Stage != "llvmc")
    Dialect = viewDialect(S, Entry, Stage);
  assert((Stage == "llvmc" || Dialect) && "an unoffered view reached a page");
  std::string Spelled;
  std::vector<std::string> Unread;
  std::vector<SourceDialectName> Names;
  std::unique_ptr<const char, decltype(&neverd_free_string)> Owned(
      nullptr, neverd_free_string);
  if (Dialect && *Dialect != SourceDialect::C) {
    const Session::FunctionSource &Source =
        dialectSource(Sess, Entry, *Dialect);
    Spelled = Source.Text;
    Map = *Source.Map;
    Map.Recognitions = &S.PipeResult.LibraryRecognitions;
    Unread = Source.Unread;
    Names = Source.Names;
  } else {
    const char *Raw = Stage == "llvmc"
                          ? decompileLlvmC(Sess, Entry, 0, &Map)
                          : decompileHighC(Sess, Entry, &Map, true);
    Owned.reset(Raw);
    if (!Raw || !S.LastError.empty())
      throw std::runtime_error(S.LastError.empty() ? "source emission failed"
                                                   : S.LastError);
    Spelled = Raw;
  }
  (void)S.synchronizeFunctions();
  llvm::StringRef Full(Spelled);
  if (Full.size() >
          ::neverd::sdk::kNeverDCAPIDecompileWriteSourcePageFullSizeLimit ||
      !llvm::json::isUTF8(Full))
    throw std::length_error("source view exceeds the UTF-8/32 MiB budget");
  // Use the same canonical LowIR instruction/sequence gate as Low/Med pages.
  // Neither a HighStmt address nor a surviving LLVM handle alone proves that
  // the address is an original instruction boundary in this function.
  const LowFunc *Low = S.findLowFunc(Entry);
  const auto Canonical =
      Low ? instructionOrigins(*Low) : std::set<std::pair<va_t, int>>();
  std::vector<const CSourceAnchor *> Anchors;
  for (const auto &Anchor : Map.Anchors)
    if (Anchor.Function == Entry && Anchor.Span.Begin < Anchor.Span.End &&
        Anchor.Span.End <= Full.size() && !Anchor.Occurrences.empty() &&
        llvm::all_of(
            Anchor.Occurrences,
            [&](const auto &O) {
              return Canonical.contains({O.Address, O.Sequence});
            }))
      Anchors.push_back(&Anchor);
  std::stable_sort(Anchors.begin(), Anchors.end(),
                   [](const auto *A, const auto *B) {
                     return A->Span.Begin < B->Span.Begin;
                   });
  size_t NextAnchor = 0;
  std::vector<const CSourceAnchor *> ActiveAnchors;
  llvm::json::Array Regions;
  for (const auto &Region : Map.Regions) {
    const auto &Match = (*Map.Recognitions)[Region.Recognition];
    if (Match.Function != Entry)
      continue;
    std::string ID = vaHex(Entry) + ':' + Match.Pack + ':' + Match.Rule;
    if (Match.ResultOccurrence)
      ID += ':' + vaHex(Match.ResultOccurrence->Address) + '.' +
            std::to_string(Match.ResultOccurrence->Sequence);
    auto Item = libraryRecognitionJSON(Match);
    Item["id"] = ID;
    Item["function"] = vaHex(Entry);
    Item["foldable"] = Region.Mapped && Match.Isolated;
    Item["mapping_status"] = Region.Mapped ? "mapped" : "unknown";
    if (Match.Callee)
      Item["callee_identity"] = S.functionIdentity(*Match.Callee);
    llvm::json::Array Spans;
    for (const auto &Span : Region.Spans)
      Spans.push_back(
          llvm::json::Object{{"begin_byte", static_cast<int64_t>(Span.Begin)},
                             {"end_byte", static_cast<int64_t>(Span.End)}});
    Item["spans"] = std::move(Spans);
    Regions.push_back(std::move(Item));
  }
  std::string Text;
  // What each row of the page says: its line, how it maps and the
  // instructions it shows.
  struct SourceRow {
    size_t Line;
    llvm::StringRef MappingStatus;
    std::vector<va_t> Addresses;
  };
  std::vector<SourceRow> Rows;
  size_t Total = 0, Start = 0, ByteOffset = Full.size();
  bool PageFull = false;
  while (Start < Full.size()) {
    const size_t Newline = Full.find('\n', Start);
    const size_t End =
        Newline == llvm::StringRef::npos ? Full.size() : Newline + 1;
    // Sweep spans once, retaining only events intersecting this physical row.
    // A large function must not scan every instruction span for every line.
    while (NextAnchor < Anchors.size() && Anchors[NextAnchor]->Span.Begin < End)
      ActiveAnchors.push_back(Anchors[NextAnchor++]);
    std::erase_if(ActiveAnchors, [Start](const auto *Anchor) {
      return Anchor->Span.End <= Start;
    });
    // A page ends before the line that would pass its byte budget; the
    // next page starts there.
    if (!PageFull && Total >= Offset && Total - Offset < Limit &&
        End - Start > limits::kMaxViewPageBytes - Text.size()) {
      if (Text.empty())
        throw std::length_error("a source line exceeds the 2 MiB page budget");
      PageFull = true;
    }
    if (!PageFull && Total >= Offset && Total - Offset < Limit) {
      if (Text.empty())
        ByteOffset = Start;
      Text.append(Full.data() + Start, End - Start);
      std::set<va_t> Origins;
      std::optional<va_t> Primary;
      size_t SmallestSpan = std::numeric_limits<size_t>::max();
      if (!Full.slice(Start, End).trim().empty())
        for (const auto *Anchor : ActiveAnchors)
          if (Anchor->Span.Begin < End && Start < Anchor->Span.End) {
            for (const auto &Origin : Anchor->Occurrences)
              Origins.insert(Origin.Address);
            const size_t Width = Anchor->Span.End - Anchor->Span.Begin;
            if (Width < SmallestSpan) {
              SmallestSpan = Width;
              Primary = Anchor->Occurrences.front().Address;
            }
          }
      const bool InstructionAnchor = Primary.has_value();
      for (const auto &Region : Map.Regions) {
        const auto &Match = (*Map.Recognitions)[Region.Recognition];
        if (Match.Function != Entry || !Region.Mapped)
          continue;
        for (const auto &Span : Region.Spans)
          if (Span.Begin < End && Start < Span.End)
            for (const auto &Origin : Match.Occurrences)
              Origins.insert(Origin.Address);
      }
      std::vector<va_t> Addresses;
      if (Primary) {
        Addresses.push_back(*Primary);
        Origins.erase(*Primary);
      }
      Addresses.insert(Addresses.end(), Origins.begin(), Origins.end());
      Rows.push_back({Total,
                      InstructionAnchor ? "instruction_anchor"
                      : Origins.empty() ? "unmapped"
                                        : "library_region",
                      std::move(Addresses)});
    }
    ++Total;
    Start = End;
  }
  const size_t End = std::min(Offset, Total) + Rows.size();
  const size_t PageEnd = ByteOffset + Text.size();
  llvm::json::Object Identity = S.functionIdentity(Entry);
  // The functions the C names by another identifier than their symbol, as a
  // start-up function the C runtime defines (`fini` for `_fini`) or a symbol
  // C cannot spell: each with the symbol and the entry a reader finds it by.
  llvm::json::Array Renamed;
  for (const CSourceName &Name : Map.Names)
    if (Name.TheKind == CSourceName::Kind::Function && Name.Address &&
        !Name.Symbol.empty() && Name.Symbol != Name.Identifier)
      Renamed.push_back(
          llvm::json::Object{{"identifier", Name.Identifier},
                             {"symbol", jsonSafeText(Name.Symbol)},
                             {"address", vaHex(*Name.Address)}});
  // What the page reads in, which a `source` request chose; the
  // declarations it shows as C; and the source names on the page, which
  // splitting the text into identifiers would not find whole.
  llvm::json::Array Reasons;
  llvm::json::Array PageNames;
  if (Dialect) {
    for (const std::string &Reason : Unread)
      Reasons.push_back(Reason);
    for (const SourceDialectName &Name : Names) {
      if (Name.End <= ByteOffset || Name.Begin >= PageEnd)
        continue;
      llvm::json::Object Item{{"begin_byte", static_cast<int64_t>(Name.Begin)},
                              {"end_byte", static_cast<int64_t>(Name.End)},
                              {"identifier", Name.Identifier},
                              {"symbol", Name.Symbol}};
      if (Name.Address)
        Item["address"] = vaHex(*Name.Address);
      PageNames.push_back(std::move(Item));
    }
  }
  // The lines before the function's definition: includes, support types and
  // declarations. Published only where the emitter recorded the definition
  // at the start of a line.
  std::optional<llvm::json::Object> Prelude;
  if (Map.Definitions.size() == 1) {
    const size_t Begin = Map.Definitions.front().Begin;
    if (Begin > 0 && Begin < Full.size() && Full[Begin - 1] == '\n')
      Prelude = llvm::json::Object{
          {"lines", static_cast<int64_t>(Full.take_front(Begin).count('\n'))},
          {"end_byte", static_cast<int64_t>(Begin)}};
  }
  const std::string Prefix = (Stage + ":" + vaHex(Entry) + ":line:").str();
  llvm::json::OStream J(OS);
  J.object([&] {
    J.attribute("address", vaHex(Entry));
    J.attribute("byte_offset", static_cast<int64_t>(ByteOffset));
    J.attribute("complete", End == Total);
    if (Dialect)
      J.attribute("dialect", sourceDialectKey(*Dialect));
    J.attribute("function_identity", std::move(Identity));
    J.attribute("variable_snapshot", Map.VariableSnapshot);
    const auto *Low = toSession(Sess)->findLowFunc(Entry);
    J.attribute("call_snapshot",
                Low ? sourceCallSnapshot(*Low) : std::string());
    J.attributeArray("source_calls", [&] {
      if (!Low)
        return;
      size_t Count = 0;
      for (const auto &Block : Low->Blocks)
        for (const auto &Op : Block.Ops)
          if (const auto Site = sourceCallOccurrenceKey(Op);
              Site &&
              ++Count <=
                  ::neverd::sdk::kNeverDCAPIDecompileWriteSourcePageCountLimit)
            J.object([&] {
              J.attribute("id", sourceCallId(*Site));
              J.attribute("instruction", vaHex(Site->Instruction));
              J.attribute("sequence", Site->Sequence);
              J.attribute("indirect", Site->Opcode == NdOp::INDIR_CALL);
              if (Site->StaticTarget)
                J.attribute("target", vaHex(*Site->StaticTarget));
            });
    });
    J.attributeArray("call_type_results", [&] {
      for (const auto &Status : toSession(Sess)->CurrentCallResults)
        J.object([&] {
          J.attribute("id", Status.Id);
          J.attribute("applied", Status.Applied);
          J.attribute("refusal", Status.Refusal);
        });
    });
    J.attributeArray("variable_edit_results", [&] {
      for (const auto &Result : Map.VariableEditResults) {
        J.object([&] {
          J.attribute("applied", Result.Applied);
          J.attribute("refusal", Result.Refusal);
          J.attributeArray("outputs", [&] {
            for (const auto &Id : Result.Outputs)
              J.value(Id);
          });
        });
      }
    });
    J.attributeArray("source_variables", [&] {
      for (const auto &[Key, Variable] : Map.Variables) {
        if (Variable.Function != Entry)
          continue;
        J.object([&] {
          J.attribute("id", Variable.Id);
          J.attribute("name", Variable.Name);
          J.attribute("bytes", Variable.Bytes);
          if (SourceVariableTypes::compatible(Variable.Type, Variable.Bytes))
            J.attribute("type", declarationToC(Variable.Type, ""));
          if (Variable.StackOffset)
            J.attribute("stack_offset", *Variable.StackOffset);
          J.attribute("parameter", Variable.Parameter);
          J.attribute("ambiguous", Variable.Ambiguous ||
                                       (Variable.Type &&
                                        !SourceVariableTypes::compatible(
                                            Variable.Type, Variable.Bytes)));
          J.attribute("floating", Variable.Type &&
                                      Variable.Type->Kind == NdTypeKind::Float);
          if (Variable.Address)
            J.attribute("address", vaHex(Variable.Address));
        });
      }
    });
    J.attributeArray("stale_variable_types", [&] {
      for (const auto &Reason : Map.StaleVariableTypes)
        J.value(Reason);
    });
    J.attribute("identifier_symbols", std::move(Renamed));
    J.attribute("library_regions", std::move(Regions));
    J.attribute("mapping_status",
                Anchors.empty() ? "library_regions" : "instruction_anchors");
    J.attribute("next_offset",
                End == Total ? llvm::json::Value(nullptr)
                             : llvm::json::Value(static_cast<int64_t>(End)));
    J.attribute("offset", static_cast<int64_t>(Offset));
    if (Prelude)
      J.attribute("prelude", std::move(*Prelude));
    J.attribute("provenance_complete", false);
    J.attribute("recognition_budget_exhausted",
                S.PipeResult.LibraryRecognitionBudgetExhausted.contains(Entry));
    J.attribute("representation", Stage);
    J.attributeArray("rows", [&] {
      for (const SourceRow &Row : Rows)
        J.object([&] {
          J.attributeArray("addresses", [&] {
            for (va_t Address : Row.Addresses)
              J.value(vaHex(Address));
          });
          J.attribute("kind", "source");
          J.attribute("line", static_cast<int64_t>(Row.Line));
          J.attribute("mapping_status", Row.MappingStatus);
          J.attribute("object_id", Prefix + std::to_string(Row.Line));
        });
    });
    J.attribute("schema_version", 1);
    if (Dialect)
      J.attribute("source_names", std::move(PageNames));
    J.attribute("text", llvm::StringRef(Text));
    J.attribute("total_lines", static_cast<int64_t>(Total));
    if (Dialect)
      J.attribute("unread", std::move(Reasons));
  });
}

} // namespace

// ===--------------------------------------------------------------------===//
// Multi-stage IR
// ===--------------------------------------------------------------------===//

const char *neverd_ir_low(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  S->clearError();

  if (!S->ensurePipeline(/*NeedHighIR=*/false))
    return dupStr(std::string());

  if (S->PipeResult.EVM)
    return dupStr(evm::dumpLowIR(S->PipeResult.EVM->Low));
  if (S->PipeResult.SBF)
    return dupStr(sbf::dumpLowIR(S->PipeResult.SBF->Low));

  const LowFunc *F = S->findLowFunc(FuncEntry);
  if (!F) {
    S->setError("function not found in LowIR");
    return dupStr(std::string());
  }

  std::string Out;
  emitLowView(*F, [&](llvm::StringRef Text, const IRRowOrigin &) {
    Out.append(Text.data(), Text.size());
  });

  return dupStr(Out);
}

const char *neverd_ir_med(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  S->clearError();

  if (!S->ensurePipeline(/*NeedHighIR=*/false))
    return dupStr(std::string());

  if (S->PipeResult.EVM)
    return dupStr(evm::dumpMedIR(S->PipeResult.EVM->Med));
  if (S->PipeResult.SBF)
    return dupStr(sbf::dumpMedIR(S->PipeResult.SBF->Med));

  for (const auto &F : S->PipeResult.MedFuncs) {
    if (F.Entry != FuncEntry)
      continue;

    std::string Out;
    emitMedView(F, nullptr, [&](llvm::StringRef Text, const IRRowOrigin &) {
      Out.append(Text.data(), Text.size());
    });
    return dupStr(Out);
  }

  S->setError("function not found in MedIR");
  return dupStr(std::string());
}

namespace {

SourceViewOptions readSourceOptions(llvm::StringRef Text,
                                    llvm::StringRef Stage) {
  if (Text.size() > kMaxSourceOptionsBytes)
    throw std::invalid_argument("source options exceed 256 KiB");
  auto Parsed = llvm::json::parse(Text);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    throw std::invalid_argument("source options must be a JSON object");
  }
  const auto *Object = Parsed->getAsObject();
  if (!Object)
    throw std::invalid_argument("source options must be a JSON object");
  SourceViewOptions Options;
  for (const auto &Field : *Object) {
    const llvm::StringRef Key = Field.first;
    if (Key != "symbolic" && Key != "llvm_optimization" &&
        Key != "llvm_passes" && Key != "llvm_control_flow" &&
        Key != "variable_types" && Key != "variable_edits" &&
        Key != "call_types" && Key != "stack_frame")
      throw std::invalid_argument("unknown source option: " + Key.str());
    if (Key != "symbolic" && Key != "variable_types" &&
        Key != "variable_edits" && Key != "call_types" &&
        Key != "stack_frame" && Stage != "llvmc")
      throw std::invalid_argument("LLVM options require llvmc");
  }
  if (const auto *Frame = Object->getObject("stack_frame"))
    Options.StackFrame = readSourceStackFrame(*Frame);
  else if (Object->get("stack_frame"))
    throw std::invalid_argument("stack_frame must be an object");
  if (const auto *Calls = Object->getArray("call_types")) {
    if (Calls->size() > kMaxSourceCallEdits)
      throw std::invalid_argument(
          "at most 32 call-site declarations are supported per function");
    std::set<std::string> Seen;
    for (const auto &Value : *Calls) {
      const auto *Call = Value.getAsObject();
      if (!Call)
        throw std::invalid_argument(
            "a call-site declaration must be an object");
      SourceCallEdit Edit;
      Edit.Id = Call->getString("id").value_or("").str();
      Edit.Snapshot = Call->getString("snapshot").value_or("").str();
      Edit.Declaration = Call->getString("declaration").value_or("").str();
      if (Edit.Id.empty() || Edit.Id.size() > kMaxSourceEditIdBytes ||
          !Seen.insert(Edit.Id).second || Edit.Snapshot.empty() ||
          Edit.Snapshot.size() > kMaxSourceSnapshotBytes ||
          Edit.Declaration.empty() ||
          Edit.Declaration.size() > kMaxFunctionDeclarationBytes)
        throw std::invalid_argument("invalid call-site declaration identity");
      if (const auto *Actuals = Call->getArray("variadic_types")) {
        if (Actuals->size() > kMaxCallVariadicTypes)
          throw std::invalid_argument(
              "at most 32 actual variadic types are supported");
        for (const auto &Actual : *Actuals) {
          const auto Text = Actual.getAsString();
          if (!Text || Text->empty() ||
              Text->size() > kMaxFunctionDeclarationBytes)
            throw std::invalid_argument(
                "an actual variadic argument needs a type");
          Edit.VariadicTypes.push_back(Text->str());
        }
      } else if (Call->get("variadic_types"))
        throw std::invalid_argument("variadic_types must be an array");
      Options.CallTypes.push_back(std::move(Edit));
    }
  } else if (Object->get("call_types"))
    throw std::invalid_argument("call_types must be an array");
  if (const auto *Edits = Object->getArray("variable_edits")) {
    if (Edits->size() > kMaxSourceVariableAssociations)
      throw std::invalid_argument(
          "at most 64 variable associations are supported");
    for (const auto &Value : *Edits) {
      const auto *Item = Value.getAsObject();
      if (!Item)
        throw std::invalid_argument("a variable association must be an object");
      SourceVariableEdit Edit;
      Edit.Kind = Item->getString("kind").value_or("").str();
      Edit.From = Item->getString("from").value_or("").str();
      Edit.To = Item->getString("to").value_or("").str();
      Edit.Snapshot = Item->getString("snapshot").value_or("").str();
      if ((Edit.Kind != "map" && Edit.Kind != "split") || Edit.From.empty() ||
          Edit.From.size() > kMaxSourceEditIdBytes ||
          Edit.To.size() > kMaxSourceEditIdBytes ||
          (Edit.Kind == "map" && Edit.To.empty()) || Edit.Snapshot.empty() ||
          Edit.Snapshot.size() > kMaxSourceSnapshotBytes)
        throw std::invalid_argument("invalid variable association identity");
      Options.VariableEdits.push_back(std::move(Edit));
    }
  } else if (Object->get("variable_edits"))
    throw std::invalid_argument("variable_edits must be an array");
  if (const auto *Types = Object->getArray("variable_types")) {
    if (Types->size() > kMaxSourceVariableTypes)
      throw std::invalid_argument(
          "at most 256 source variable types are supported per view");
    for (const auto &Value : *Types) {
      const auto *Type = Value.getAsObject();
      if (!Type)
        throw std::invalid_argument("a source variable type must be an object");
      SourceViewOptions::VariableType Edit;
      auto ReadAddress = [&](llvm::StringRef Key) -> uint64_t {
        const auto Text = Type->getString(Key);
        uint64_t Address = 0;
        if (!Text || Text->getAsInteger(0, Address))
          throw std::invalid_argument(
              "a source variable type has an invalid owner address");
        return Address;
      };
      Edit.Function = ReadAddress("function");
      const auto Bytes = Type->getInteger("bytes");
      const auto Floating = Type->getBoolean("floating");
      if (!Bytes || *Bytes < 1 || *Bytes > 16 || !Floating)
        throw std::invalid_argument("a source variable type needs its original "
                                    "byte width and scalar class");
      Edit.Bytes = *Bytes;
      Edit.Floating = *Floating;
      if (Type->get("address"))
        Edit.Address = ReadAddress("address");
      Edit.Id = Type->getString("id").value_or("").str();
      Edit.Snapshot = Type->getString("snapshot").value_or("").str();
      Edit.Type = Type->getString("type").value_or("").str();
      if (Edit.Id.empty() || Edit.Id.size() > kMaxSourceEditIdBytes ||
          Edit.Snapshot.size() > kMaxSourceSnapshotBytes || Edit.Type.empty() ||
          Edit.Type.size() > kMaxFunctionDeclarationBytes)
        throw std::invalid_argument("invalid source variable type binding");
      Options.VariableTypes.push_back(std::move(Edit));
    }
  } else if (Object->get("variable_types")) {
    throw std::invalid_argument("variable_types must be an array");
  }
  if (Object->get("symbolic")) {
    auto Enabled = Object->getBoolean("symbolic");
    if (!Enabled)
      throw std::invalid_argument("symbolic must be a boolean");
    Options.Symbolic = *Enabled;
  }
  if (Object->get("llvm_optimization")) {
    auto Level = Object->getString("llvm_optimization");
    if (!Level)
      throw std::invalid_argument("llvm_optimization must be a string");
    using LevelKind = SourceViewOptions::LLVMOptimization;
    if (*Level == "basic")
      Options.LLVM = LevelKind::Basic;
    else if (*Level == "O0")
      Options.LLVM = LevelKind::O0;
    else if (*Level == "O1")
      Options.LLVM = LevelKind::O1;
    else if (*Level == "O2")
      Options.LLVM = LevelKind::O2;
    else if (*Level == "O3")
      Options.LLVM = LevelKind::O3;
    else if (*Level == "custom")
      Options.LLVM = LevelKind::Custom;
    else
      throw std::invalid_argument("unsupported LLVM source optimization");
  }
  if (Object->get("llvm_control_flow")) {
    auto Enabled = Object->getBoolean("llvm_control_flow");
    if (!Enabled)
      throw std::invalid_argument("llvm_control_flow must be a boolean");
    Options.RecoverControlFlow = *Enabled;
  }
  const auto *Passes = Object->getArray("llvm_passes");
  if (Options.LLVM == SourceViewOptions::LLVMOptimization::Custom) {
    if (!Passes ||
        Passes->size() >
            ::neverd::sdk::kNeverDCAPIDecompileReadSourceOptionsPassesSizeLimit)
      throw std::invalid_argument("custom requires at most four llvm_passes");
    Options.Passes = 0;
    for (const auto &Value : *Passes) {
      const auto Name = Value.getAsString();
      uint32_t Bit = 0;
      if (Name == "instcombine")
        Bit = SourceViewOptions::InstCombine;
      else if (Name == "early-cse")
        Bit = SourceViewOptions::EarlyCSE;
      else if (Name == "simplifycfg")
        Bit = SourceViewOptions::SimplifyCFG;
      else if (Name == "dce")
        Bit = SourceViewOptions::DCE;
      else
        throw std::invalid_argument("unsupported LLVM source pass");
      if (Options.Passes & Bit)
        throw std::invalid_argument("duplicate LLVM source pass");
      Options.Passes |= Bit;
    }
  } else if (Object->get("llvm_passes")) {
    throw std::invalid_argument("llvm_passes requires custom optimization");
  }
  return Options;
}

/// A source request borrows one variant; every exit restores ordinary views.
struct ScopedSourceView {
  Session &S;
  SourceViewOptions Previous;
  bool Swapped = false;
  bool CallsSwapped = false;
  std::vector<SourceCallEditResult> PreviousCallResults;
  ScopedSourceView(Session &S, va_t Entry, const SourceViewOptions &Options,
                   bool High)
      : S(S), Previous(S.SourceOptions) {
    if (!Options.CallTypes.empty() || !Options.StackFrame.empty()) {
      prepareSourceCallVariant(S, Entry, Options);
      PreviousCallResults = std::move(S.CurrentCallResults);
      S.CurrentCallResults = S.CallVariant->Results;
      if (S.CallVariant->Result) {
        if (!High && S.CallVariant->TargetSignatures.empty()) {
          // Preserve the ordinary callee declarations. A call-site contract
          // changes only its own CallBase type, never its siblings' prototype.
          if (const auto *Base = S.ensureFunctionLlvmModule(
                  Entry, false, /*OriginalImageStorage=*/true))
            for (const auto &Function : *Base->Module)
              S.CallVariant->TargetSignatures.emplace(
                  Function.getName().str(),
                  std::pair{Function.getFunctionType(),
                            unsigned(Function.getCallingConv())});
          S.clearError();
        }
        std::swap(S.PipeResult, *S.CallVariant->Result);
        std::swap(S.NativeMedFuncs, S.CallVariant->Native);
        S.UsingAnalystCallContracts = CallsSwapped = true;
      }
    }
    if (!CallsSwapped && High && Options.Symbolic) {
      if (!S.EnhancedHighIR) {
        auto Variant = std::make_unique<PipelineResult>();
        Variant->LowFuncs = S.PipeResult.LowFuncs;
        Variant->MedFuncs = S.PipeResult.MedFuncs;
        Variant->LibraryRecognitions = S.PipeResult.LibraryRecognitions;
        PipelineOptions Policy;
        Policy.SymbolicHighC = true;
        Pipeline().rebuildHighIR(S.Img, Policy, *Variant, S.decompileDebug());
        S.EnhancedHighIR = std::make_unique<Session::HighIRVariant>(
            Session::HighIRVariant{std::move(Variant->HighFuncs),
                                   std::move(Variant->HighSources)});
      }
      S.PipeResult.HighFuncs.swap(S.EnhancedHighIR->HighFuncs);
      S.PipeResult.HighSources.swap(S.EnhancedHighIR->HighSources);
      Swapped = true;
    }
    S.SourceOptions = Options;
  }
  ~ScopedSourceView() {
    S.SourceOptions = Previous;
    if (Swapped) {
      S.PipeResult.HighFuncs.swap(S.EnhancedHighIR->HighFuncs);
      S.PipeResult.HighSources.swap(S.EnhancedHighIR->HighSources);
    }
    if (CallsSwapped) {
      std::swap(S.PipeResult, *S.CallVariant->Result);
      std::swap(S.NativeMedFuncs, S.CallVariant->Native);
      S.UsingAnalystCallContracts = false;
    }
    S.CurrentCallResults = std::move(PreviousCallResults);
  }
};

/// A page of \p Entry's LLVM IR view: its lines from \p Offset, where the
/// declarations before the function's own lines end and where the attribute
/// groups and metadata after them begin, and the image address each global
/// name on the page stands for.  No row maps to an instruction.
llvm::json::Object llvmViewPage(const LLVMFunctionView &View, va_t Entry,
                                size_t Offset, size_t Limit) {
  const llvm::StringRef Full(View.Text);
  std::string Text;
  size_t Total = 0, Shown = 0, ByteOffset = Full.size();
  bool PageFull = false;
  for (size_t Start = 0; Start < Full.size();) {
    const size_t Newline = Full.find('\n', Start);
    const size_t End =
        Newline == llvm::StringRef::npos ? Full.size() : Newline + 1;
    // A page ends before the line that would pass its byte budget; the next
    // page starts there.
    if (!PageFull && Total >= Offset && Total - Offset < Limit &&
        End - Start > limits::kMaxViewPageBytes - Text.size()) {
      if (Text.empty())
        throw std::length_error("an IR line exceeds the 2 MiB page budget");
      PageFull = true;
    }
    if (!PageFull && Total >= Offset && Total - Offset < Limit) {
      if (Text.empty())
        ByteOffset = Start;
      Text.append(Full.data() + Start, End - Start);
      ++Shown;
    }
    ++Total;
    Start = End;
  }
  const size_t First = std::min(Offset, Total);
  const size_t Last = First + Shown;
  llvm::json::Array References;
  for (const LLVMViewReference &Reference : View.References)
    if (Reference.Line >= First && Reference.Line < Last)
      References.push_back(
          llvm::json::Object{{"line", static_cast<int64_t>(Reference.Line)},
                             {"name", jsonSafeText(Reference.Name)},
                             {"address", vaHex(Reference.Address)}});
  llvm::json::Object Page{{"schema_version", 1},
                          {"address", vaHex(Entry)},
                          {"representation", "llvm"},
                          {"mapping_status", "unsupported_representation"},
                          {"provenance_complete", false},
                          {"text", std::move(Text)},
                          {"rows", llvm::json::Array()},
                          {"library_regions", llvm::json::Array()},
                          {"references", std::move(References)},
                          {"offset", static_cast<int64_t>(Offset)},
                          {"byte_offset", static_cast<int64_t>(ByteOffset)},
                          {"total_lines", static_cast<int64_t>(Total)},
                          {"complete", Last == Total}};
  Page["next_offset"] = Last == Total
                            ? llvm::json::Value(nullptr)
                            : llvm::json::Value(static_cast<int64_t>(Last));
  // The declarations of what the function refers to, before its own lines.
  if (View.DefinitionBegin > 0 && View.DefinitionBegin <= Full.size() &&
      Full[View.DefinitionBegin - 1] == '\n')
    Page["prelude"] = llvm::json::Object{
        {"lines", static_cast<int64_t>(
                      Full.take_front(View.DefinitionBegin).count('\n'))},
        {"end_byte", static_cast<int64_t>(View.DefinitionBegin)}};
  // The attribute groups and metadata it uses, after them.
  if (View.DefinitionEnd < Full.size())
    Page["epilogue"] = llvm::json::Object{
        {"lines",
         static_cast<int64_t>(Full.drop_front(View.DefinitionEnd).count('\n'))},
        {"begin_byte", static_cast<int64_t>(View.DefinitionEnd)}};
  return Page;
}

} // namespace

const char *neverd_ir_view_ex_json(neverd_session_t Sess, neverd_va_t FuncEntry,
                                   const char *Representation, size_t Offset,
                                   size_t Limit, const char *OptionsJSON) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  try {
    const llvm::StringRef Stage = Representation ? Representation : "";
    if ((Stage != "c" && Stage != "llvmc" && Stage != "source" &&
         !sourceDialectFromKey(Stage)) ||
        Limit == 0 ||
        Limit > ::neverd::sdk::kNeverDCAPIDecompileNeverdIrViewExJsonLimit ||
        Offset > static_cast<size_t>(std::numeric_limits<int64_t>::max()))
      throw std::invalid_argument("invalid source view or paging bounds");
    const auto Options =
        readSourceOptions(OptionsJSON ? OptionsJSON : "{}", Stage);
    if (!prepareFunction(Sess, FuncEntry, /*NeedHighIR=*/Stage != "llvmc"))
      return nullptr;
    if (S->PipeResult.EVM || S->PipeResult.SBF)
      throw std::invalid_argument("source transformations require native code");
    ScopedSourceView View(*S, FuncEntry, Options, Stage != "llvmc");
    return neverd_ir_view_json(Sess, FuncEntry, Representation, Offset, Limit);
  } catch (const std::exception &Error) {
    S->setError(Error.what());
  } catch (...) {
    S->setError("unexpected source transformation failure");
  }
  return nullptr;
}

const char *neverd_ir_view_json(neverd_session_t Sess, neverd_va_t FuncEntry,
                                const char *Representation, size_t Offset,
                                size_t Limit) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  try {
    if (!Representation || Limit == 0 ||
        Limit > ::neverd::sdk::kNeverDCAPIDecompileNeverdIrViewJsonLimit ||
        Offset > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
      S->setError("invalid IR view representation or paging bounds");
      return nullptr;
    }
    const llvm::StringRef Stage(Representation);
    if (!llvm::json::isUTF8(Stage)) {
      S->setError("IR view representation is not valid UTF-8");
      return nullptr;
    }
    llvm::json::Object Result;
    Result["schema_version"] = 1;
    Result["address"] = vaHex(FuncEntry);
    Result["representation"] = Stage;
    Result["rows"] = llvm::json::Array();
    if (Stage == "c" || Stage == "llvmc" || Stage == "source" ||
        sourceDialectFromKey(Stage)) {
      if (S->Img.Arch == Arch::EVM || S->Img.Arch == Arch::SBF) {
        Result["mapping_status"] = "unsupported_architecture";
        return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
      }
      // Rust and Go are offered for programs written in them.
      if (Stage != "llvmc" && !viewDialect(*S, FuncEntry, Stage)) {
        Result["mapping_status"] = "unsupported_representation";
        Result["reason"] = unofferedDialectReason(
            sourceDialectFromKey(Stage).value_or(SourceDialect::C),
            S->Img.ExceptionMetadata.Runtime);
        return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
      }
      std::string Page;
      llvm::raw_string_ostream PageOS(Page);
      writeSourcePage(PageOS, Sess, FuncEntry, Stage, Offset, Limit);
      PageOS.flush();
      return dupStr(Page);
    }
    if (Stage == "llvm") {
      if (!S->ensurePipeline(/*NeedHighIR=*/false))
        return nullptr;
      // Bytecode keeps its whole module as its text (neverd_ir_llvm).
      if (S->PipeResult.EVM || S->PipeResult.SBF) {
        Result["mapping_status"] = "unsupported_architecture";
        return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
      }
      const LLVMFunctionView *View = functionLlvmView(*S, FuncEntry);
      if (!View)
        return nullptr;
      auto Page = llvmViewPage(*View, FuncEntry, Offset, Limit);
      return dupStr(jsonToString(llvm::json::Value(std::move(Page))));
    }
    if (Stage != "low" && Stage != "med" && Stage != "high") {
      Result["mapping_status"] = "unsupported_representation";
      return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
    }
    if (!S->ensurePipeline(/*NeedHighIR=*/Stage == "high"))
      return nullptr;
    if (S->PipeResult.EVM || S->PipeResult.SBF) {
      Result["mapping_status"] = "unsupported_architecture";
      return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
    }
    const LowFunc *Low = S->findLowFunc(FuncEntry);
    const MedFunc *Med = nullptr;
    if (Stage == "med")
      for (const auto &F : S->PipeResult.MedFuncs)
        if (F.Entry == FuncEntry) {
          Med = &F;
          break;
        }
    const HighFunc *High =
        Stage == "high" ? S->findHighFunc(FuncEntry) : nullptr;
    if ((Stage == "low" && !Low) || (Stage == "med" && !Med) ||
        (Stage == "high" && !High)) {
      S->setError("function not found in requested IR stage");
      return nullptr;
    }
    size_t Total = 0;
    std::string Text;
    llvm::json::Array Rows;
    bool PageFull = false;
    const IRRowSink Sink = [&](llvm::StringRef Chunk,
                               const IRRowOrigin &Origin) {
      size_t Start = 0;
      while (Start < Chunk.size()) {
        const auto Newline = Chunk.find('\n', Start);
        const auto End =
            Newline == llvm::StringRef::npos ? Chunk.size() : Newline + 1;
        // A page ends before the line that would pass its byte budget; the
        // next page starts there.
        if (!PageFull && Total >= Offset && Total - Offset < Limit &&
            End - Start > limits::kMaxViewPageBytes - Text.size()) {
          if (Text.empty())
            throw std::length_error("an IR line exceeds the 2 MiB page budget");
          PageFull = true;
        }
        if (!PageFull && Total >= Offset && Total - Offset < Limit) {
          Text.append(Chunk.data() + Start, End - Start);
          llvm::json::Object Row;
          Row["line"] = static_cast<int64_t>(Total);
          Row["object_id"] = Origin.ObjectId;
          Row["kind"] = Origin.Kind;
          Row["mapping_status"] = Origin.Status;
          llvm::json::Array Addresses;
          if (Origin.Address)
            Addresses.push_back(vaHex(*Origin.Address));
          Row["addresses"] = std::move(Addresses);
          if (Origin.Sequence >= 0)
            Row["origin_seq"] = Origin.Sequence;
          Rows.push_back(std::move(Row));
        }
        ++Total;
        Start = End;
      }
    };
    if (Stage == "low")
      emitLowView(*Low, Sink, true);
    else if (Stage == "med")
      emitMedView(*Med, Low, Sink);
    else
      emitHighView(*High, Low, S->PipeResult.HighSources, Sink);
    if (!llvm::json::isUTF8(Text)) {
      S->setError("IR view page text is not valid UTF-8");
      return nullptr;
    }
    const size_t End = std::min(Offset, Total) + Rows.size();
    Result["mapping_status"] = "instruction_anchors";
    Result["provenance_complete"] = false;
    Result["text"] = std::move(Text);
    Result["rows"] = std::move(Rows);
    Result["offset"] = static_cast<int64_t>(Offset);
    Result["total_lines"] = static_cast<int64_t>(Total);
    Result["complete"] = End == Total;
    if (End == Total)
      Result["next_offset"] = nullptr;
    else
      Result["next_offset"] = static_cast<int64_t>(End);
    return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
  } catch (const std::exception &Error) {
    S->setError(Error.what());
    return nullptr;
  } catch (...) {
    S->setError("unexpected IR view failure");
    return nullptr;
  }
}

const char *neverd_ir_high(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();

  if (!S->ensurePipeline())
    return dupStr(std::string());

  if (S->PipeResult.EVM)
    return dupStr(evm::dumpHighIR(S->PipeResult.EVM->High));
  if (S->PipeResult.SBF)
    return dupStr(sbf::dumpHighIR(S->PipeResult.SBF->High));

  const HighFunc *HF = S->findHighFunc(FuncEntry);
  if (!HF) {
    S->setError(missingHighFunctionReason(S->PipeResult, FuncEntry));
    return dupStr(std::string());
  }

  std::string Out;
  llvm::raw_string_ostream OS(Out);
  try {
    emitHighView(
        *HF, nullptr, S->PipeResult.HighSources,
        [&](llvm::StringRef Text, const IRRowOrigin &) { OS << Text; });
  } catch (const std::exception &Error) {
    S->setError(Error.what());
    return nullptr;
  }

  return dupStr(Out);
}

const char *neverd_ir_llvm(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  S->clearError();

  if (!S->ensurePipeline(/*NeedHighIR=*/false))
    return dupStr(std::string());
  if (S->PipeResult.EVM || S->PipeResult.SBF) {
    if (!S->ensureLlvmModule()) {
      if (S->LastError.empty())
        S->setError("failed to generate LLVM module");
      return dupStr(std::string());
    }
    return dupStr(S->PipeResult.EVM
                      ? evm::emitLLVMText(*S->PipeResult.LlvmModule)
                      : sbf::emitLLVMText(*S->PipeResult.LlvmModule));
  }

  // The function in a module of its own, with what it refers to.
  const LLVMFunctionView *View = functionLlvmView(*S, FuncEntry);
  return dupStr(View ? View->Text : std::string());
}

// ===--------------------------------------------------------------------===//
// High-level pipeline: lift module
// ===--------------------------------------------------------------------===//

const char *neverd_lift_module(neverd_session_t Sess, const char *InputPath,
                               int NoOpt, int MaxFunctions) {
  auto *S = static_cast<Session *>(Sess);
  PipelineRunner R;
  std::string Err;
  if (!R.loadAnalysis(InputPath, Err, S)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  PipelineOptions Opts;
  Opts.LiftMode = true;
  Opts.NoOpt = NoOpt != 0;
  Opts.MaxFunctions = MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;
  if (S) {
    S->applyAnalysisOptions(Opts);
  }
  if (!R.run(Opts, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }
  if (!R.Result.LlvmModule)
    return nullptr;

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  R.Result.LlvmModule->print(OS, nullptr);
  return dupStr(Buf);
}

// ===--------------------------------------------------------------------===//
// High-level pipeline: lift dump (LowIR / MedIR / HighIR)
// ===--------------------------------------------------------------------===//

const char *neverd_lift_dump(neverd_session_t Sess, const char *InputPath,
                             int Level, int MaxFunctions) {
  auto *S = static_cast<Session *>(Sess);
  PipelineRunner R;
  std::string Err;
  if (!R.loadAnalysis(InputPath, Err, S)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  PipelineOptions Opts;
  Opts.MaxFunctions = MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;
  if (S) {
    S->applyAnalysisOptions(Opts);
  }
  if (Level == 0)
    Opts.DumpLow = true;
  else if (Level == 1)
    Opts.DumpMed = true;
  else
    Opts.DumpHigh = true;
  // The C API returns the dump as a string. Printing it from inside the shared
  // library as well gives Windows DLL and executable instances of llvm::outs()
  // independent buffers whose flush order is not deterministic.
  Opts.EmitDumpOutput = false;
  if (!R.run(Opts, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  if (R.Result.EVM) {
    if (Level == 0)
      OS << evm::dumpLowIR(R.Result.EVM->Low);
    else if (Level == 1)
      OS << evm::dumpMedIR(R.Result.EVM->Med);
    else
      OS << evm::dumpHighIR(R.Result.EVM->High);
  } else if (R.Result.SBF) {
    if (Level == 0)
      OS << sbf::dumpLowIR(R.Result.SBF->Low);
    else if (Level == 1)
      OS << sbf::dumpMedIR(R.Result.SBF->Med);
    else
      OS << sbf::dumpHighIR(R.Result.SBF->High);
  } else if (Level == 0)
    Pipeline::dumpLowIR(R.Result.LowFuncs, OS);
  else if (Level == 1)
    Pipeline::dumpMedIR(R.Result.MedFuncs, OS);
  else
    Pipeline::dumpHighIR(R.Result.HighFuncs, OS);
  return dupStr(Buf);
}

// ===--------------------------------------------------------------------===//
// High-level pipeline: decompile all
// ===--------------------------------------------------------------------===//

/// The functions of a whole-program run, as a session lists them: the
/// image's function symbols, then each function the pipeline found that no
/// symbol begins.
static std::vector<FuncInfo> runFunctions(const PipelineRunner &R) {
  std::vector<FuncInfo> Functions;
  llvm::DenseSet<va_t> Entries;
  for (const Symbol *Sym : R.Img.getFunctionSymbols())
    if (Entries.insert(Sym->Addr).second)
      Functions.push_back({Sym->Addr, Sym->Size, Sym->Name});
  for (const LowFunc &Function : R.Result.LowFuncs)
    if (Entries.insert(Function.Entry).second)
      Functions.push_back(
          {Function.Entry, Function.OriginalSize, Function.Name});
  return Functions;
}

/// The local types of the session's input, over \p R's own image and debug
/// information, when the session holds the file \p InputPath names, as the
/// command line loads it first: the prototypes of the routines the program
/// names and calls, the user's declarations and the records units define.
/// Null where the session holds another file or has nothing to add.
static std::unique_ptr<LocalTypesDebugContext>
localTypesOf(Session *S, const char *InputPath, PipelineRunner &R,
             const DebugContext &NoDebugInformation) {
  if (!S || !S->Loaded || S->FilePath.empty())
    return nullptr;
  std::error_code Error;
  if (!std::filesystem::equivalent(S->FilePath,
                                   std::filesystem::u8path(InputPath), Error))
    return nullptr;
  std::map<va_t, std::string> Declarations = S->functionDeclarations();
  // Types that do not read, as a PDB's may not, leave the output as the
  // program's debug information alone makes it, the session's error kept.
  const std::string LastError = S->LastError;
  const types::TypeLibrary *Library = S->readLocalTypes();
  S->LastError = LastError;
  if (!Library ||
      (!R.Dbg && Declarations.empty() && Library->prototypes().empty()))
    return nullptr;
  std::vector<types::HeaderLibrary> Headers;
  for (const Session::JoinedHeaders &Joined : S->LocalTypeHeaders)
    if (Joined.Error.empty())
      Headers.push_back(Joined.Library);
  return std::make_unique<LocalTypesDebugContext>(
      R.Dbg ? *R.Dbg : NoDebugInformation,
      std::make_shared<types::TypeLibrary>(*Library), std::move(Declarations),
      &R.Img, std::move(Headers), S->SigDB.buildNameMap(),
      S->LocalTypeEntryType, S->programMain());
}

static const char *decompileAllImpl(neverd_session_t Sess,
                                    const char *InputPath, int UseLlvmRoute,
                                    neverd_output_language_t Language,
                                    int NoOpt, int MaxFunctions,
                                    const std::set<va_t> *Only = nullptr) {
  auto *S = static_cast<Session *>(Sess);
  PipelineRunner R;
  std::string Err;
  if (!R.loadAnalysis(InputPath, Err, S)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }
  const NullDebugContext NoDebugInformation;
  const std::unique_ptr<LocalTypesDebugContext> LocalTypes =
      localTypesOf(S, InputPath, R, NoDebugInformation);
  DebugContext *Debug = LocalTypes ? LocalTypes.get() : R.Dbg.get();
  // The definitions of the records a unit names, which a Rust or Go
  // spelling of it defines as its own.
  std::optional<CSourceLocalTypes> Held;
  const auto recordDefinitions = [&](CEmitterOptions &COpts) {
    if (LocalTypes && holdsLocalTypes(*LocalTypes))
      COpts.RecordDefinitions = [&](const std::vector<TypeRef> &Records) {
        CRecordDefinitions Definitions = LocalTypes->definitions(Records);
        if (!Definitions.Text.empty())
          Held = CSourceLocalTypes{Definitions.Text, Definitions.Types,
                                   Definitions.DeclaredTypes,
                                   Definitions.HeaderTypes};
        return Definitions;
      };
  };

  PipelineOptions Opts;
  Opts.NoOpt = NoOpt != 0;
  Opts.MaxFunctions = MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;
  if (S) {
    S->applyAnalysisOptions(Opts);
  }
  if (Only)
    Opts.OnlyFunctionEntries = *Only;
  if (UseLlvmRoute)
    Opts.LiftMode = true;
  Opts.SourceProjection = true;
  // Symbolic source simplification is the caller's explicit choice for the
  // whole program, never a default.
  if (S && S->SourceSymbolic) {
    Opts.SymbolicHighC = true;
    Opts.SymbolicLLVMSource = true;
  }
  // A named ARM function with no instruction-state evidence is not a
  // candidate for either C route. Reject the whole requested output before
  // analysis so a partly marked image cannot silently omit that function.
  if (R.Img.Arch == Arch::ARM) {
    for (const Symbol &Sym : R.Img.Symbols) {
      if (!Sym.IsFunc || R.Img.isImportStubAt(Sym.Addr) ||
          (!Opts.OnlyFunctionEntries.empty() &&
           !Opts.OnlyFunctionEntries.count(Sym.Addr)))
        continue;
      if (auto Reason = missingARMModeReason(R.Img, Sym.Addr)) {
        if (S)
          S->setError(*Reason);
        return nullptr;
      }
    }
  }
  if (!R.run(Opts, Err, Debug)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);

  if (R.Result.EVM) {
    if (UseLlvmRoute) {
      if (S)
        S->setError("LLVM-to-C route is not supported for EVM; use the "
                    "dedicated C backend");
      return nullptr;
    }
    // A contract's own language is Solidity.
    if (Language == NEVERD_OUTPUT_SOLIDITY ||
        Language == NEVERD_OUTPUT_SOURCE) {
      auto Output = evm::emitSolidity(*R.Result.EVM);
      if (!Output) {
        if (S)
          S->setError(llvm::toString(Output.takeError()));
        return nullptr;
      }
      return dupStr(*Output);
    }
    if (Language == NEVERD_OUTPUT_RUST || Language == NEVERD_OUTPUT_GO ||
        Language == NEVERD_OUTPUT_CPP) {
      if (S)
        S->setError(Language == NEVERD_OUTPUT_RUST
                        ? "Rust output is not supported for EVM bytecode"
                    : Language == NEVERD_OUTPUT_GO
                        ? "Go output is not supported for EVM bytecode"
                        : "C++ output is not supported for EVM bytecode");
      return nullptr;
    }
    auto Output = evm::emitC(*R.Result.EVM);
    if (!Output) {
      if (S)
        S->setError(llvm::toString(Output.takeError()));
      return nullptr;
    }
    return dupStr(*Output);
  }

  if (R.Result.SBF) {
    if (UseLlvmRoute) {
      if (S)
        S->setError("LLVM-to-C route is not supported for SBF; use the "
                    "dedicated C or Rust backend");
      return nullptr;
    }
    if (Language == NEVERD_OUTPUT_SOLIDITY || Language == NEVERD_OUTPUT_GO ||
        Language == NEVERD_OUTPUT_CPP) {
      if (S)
        S->setError(Language == NEVERD_OUTPUT_SOLIDITY
                        ? "Solidity output is supported only for EVM bytecode"
                    : Language == NEVERD_OUTPUT_GO
                        ? "Go output is not supported for SBF programs"
                        : "C++ output is not supported for SBF programs");
      return nullptr;
    }
    // A Solana program's own language is Rust.
    if (Language == NEVERD_OUTPUT_RUST || Language == NEVERD_OUTPUT_SOURCE) {
      auto Output = sbf::emitRust(*R.Result.SBF);
      if (!Output) {
        if (S)
          S->setError(llvm::toString(Output.takeError()));
        return nullptr;
      }
      return dupStr(*Output);
    }
    auto Output = sbf::emitC(*R.Result.SBF);
    if (!Output) {
      if (S)
        S->setError(llvm::toString(Output.takeError()));
      return nullptr;
    }
    return dupStr(*Output);
  }

  if (Language == NEVERD_OUTPUT_SOLIDITY) {
    if (S)
      S->setError("Solidity output is supported only for EVM bytecode");
    return nullptr;
  }
  // Native code reads in the language a request names, or its own: the
  // HighC source, spelled in it.
  const LanguageRuntimeInfo &ImageLanguage = R.Img.ExceptionMetadata.Runtime;
  std::optional<SourceDialect> Dialect;
  if (Language == NEVERD_OUTPUT_RUST)
    Dialect = SourceDialect::Rust;
  else if (Language == NEVERD_OUTPUT_CPP)
    Dialect = SourceDialect::Cpp;
  else if (Language == NEVERD_OUTPUT_GO)
    Dialect = SourceDialect::Go;
  else if (Language == NEVERD_OUTPUT_SOURCE)
    Dialect = dialectOfRuntime(ImageLanguage.Runtime);
  if (Dialect &&
      !llvm::is_contained(offeredSourceDialects(ImageLanguage), *Dialect)) {
    if (Language != NEVERD_OUTPUT_SOURCE) {
      if (S)
        S->setError(unofferedDialectReason(*Dialect, ImageLanguage));
      return nullptr;
    }
    Dialect.reset();
  }
  if (Dialect == SourceDialect::C)
    Dialect.reset();
  if (Dialect && UseLlvmRoute) {
    if (S)
      S->setError("Source-language output spells the HighC route");
    return nullptr;
  }

  // A function the pipeline refused is named at the unit's head.
  OS << refusedFunctionNotes(R.Result);
  // Data takes the names a session's listing of the image gives it.
  const std::optional<DataAccessWidths> AccessWidths =
      collectDataAccessWidths(R.Img, runFunctions(R));
  if (UseLlvmRoute) {
    if (!R.Result.LlvmModule)
      return nullptr;
    CEmitterOptions COpts;
    COpts.TheArch = R.Img.Arch;
    COpts.Format = R.Img.abiFormat();
    COpts.UserNames = S ? &S->Renames : nullptr;
    COpts.AccessWidths = AccessWidths ? &*AccessWidths : nullptr;
    recordDefinitions(COpts);
    try {
      LLVMCEmitter Emitter;
      Emitter.emit(*R.Result.LlvmModule, OS, COpts, Debug, &R.Img);
    } catch (const std::exception &Error) {
      if (S)
        S->setError(emissionFailure(Error));
      return nullptr;
    }
  } else {
    CEmitterOptions COpts;
    COpts.TheArch = R.Img.Arch;
    COpts.Format = R.Img.abiFormat();
    COpts.Image = &R.Img;
    COpts.UserNames = S ? &S->Renames : nullptr;
    COpts.AccessWidths = AccessWidths ? &*AccessWidths : nullptr;
    std::vector<CSourceName> Names;
    COpts.StructuredExceptionSyntax = Dialect.has_value();
    if (Dialect)
      COpts.SourceNames = &Names;
    recordDefinitions(COpts);
    COpts.WholeProgram = true;
    try {
      HighCEmitter Emitter;
      Emitter.emit(R.Result.HighFuncs, OS, COpts, Debug);
    } catch (const std::exception &Error) {
      if (S)
        S->setError(emissionFailure(Error));
      return nullptr;
    }
    if (Dialect) {
      SourceDialectOptions Options;
      Options.Dialect = *Dialect;
      Options.TheArch = R.Img.Arch;
      Options.Format = R.Img.abiFormat();
      Options.Names = Names;
      if (LocalTypes)
        Options.Types = &LocalTypes->library();
      if (Held)
        Options.HeaderTypes = Held->HeaderTypes;
      if (*Dialect == SourceDialect::Rust || *Dialect == SourceDialect::Go)
        Options.LocalTypes =
            dialectLocalTypes(LocalTypes.get(), Buf, Held, *Dialect);
      return dupStr(spellInDialect(Buf, Options).Text);
    }
  }
  return dupStr(Buf);
}

const char *neverd_decompile_all(neverd_session_t Sess, const char *InputPath,
                                 int UseLlvmRoute, int NoOpt,
                                 int MaxFunctions) {
  return decompileAllImpl(Sess, InputPath, UseLlvmRoute, NEVERD_OUTPUT_C, NoOpt,
                          MaxFunctions);
}

const char *neverd_decompile_all_ex(neverd_session_t Sess,
                                    const char *InputPath,
                                    neverd_output_language_t Language,
                                    int NoOpt, int MaxFunctions) {
  const bool KnownLanguage = [&] {
    switch (Language) {
#define NEVERD_OUTPUT_LANGUAGE(NAME, VALUE, SPELLING, DISPLAY_NAME)            \
  case NEVERD_OUTPUT_##NAME:                                                   \
    return true;
#include "neverd/OutputLanguages.def"
    }
    return false;
  }();
  if (!KnownLanguage) {
    if (auto *S = static_cast<Session *>(Sess))
      S->setError("unknown output language");
    return nullptr;
  }
  return decompileAllImpl(Sess, InputPath, 0, Language, NoOpt, MaxFunctions);
}

const char *neverd_decompile_functions(neverd_session_t Sess,
                                       const char *InputPath,
                                       neverd_output_language_t Language,
                                       const neverd_va_t *Entries, int Count) {
  auto *S = static_cast<Session *>(Sess);
  if (!Entries || Count < 1 || Count > kMaxSourceEntriesPerQuery) {
    if (S)
      S->setError("entries must name 1 to " +
                  std::to_string(kMaxSourceEntriesPerQuery) + " functions");
    return nullptr;
  }
  const std::set<va_t> Only(Entries, Entries + Count);
  // The selection is checked against the image it is lifted from.
  if (S && S->Loaded && S->Img.Arch != Arch::EVM && S->Img.Arch != Arch::SBF)
    for (const va_t Entry : Only) {
      if (!S->Img.hasExecutableCodeOwnerAt(Entry)) {
        S->setError("no native code at " + vaHex(Entry));
        return nullptr;
      }
    }
  return decompileAllImpl(Sess, InputPath, 0, Language, 0, 0, &Only);
}

// ===--------------------------------------------------------------------===//
// Inject hello world pass
// ===--------------------------------------------------------------------===//

int neverd_inject_hello(neverd_session_t Sess) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !S->PipeResult.LlvmModule) {
    if (S)
      S->setError("no LLVM module available");
    return 1;
  }
  Pipeline::runHelloWorldPass(*S->PipeResult.LlvmModule);
  return 0;
}
