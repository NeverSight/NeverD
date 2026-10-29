//===- PipelineHighIR.cpp - HighIR pipeline stage ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// MedIR-to-HighIR conversion for the decompilation pipeline.
///
//===----------------------------------------------------------------------===//

#include "PipelineCallAbiDetail.h"
#include "PipelineFunctionNames.h"
#include "PipelineHighIRDetail.h"
#include "PipelineLowIRDetail.h"
#include "PipelineReturnModelingDetail.h"
#include "PipelineSourceStackAccesses.h"

#include "neverd/Common.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/NdTypes.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/low/SourceFrameBytes.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/SymbolDecoration.h"
#include "neverd/pipeline/Limits.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

/// A call whose callee returns a scalar float in the vector return register
/// defines that register, not the integer one (promoteFloatCallResult). The
/// callees are the imports their C declarations describe and the functions
/// typed as returning one. Binding a call can make its caller return the
/// result, so a caller is typed again, and each round that finds another
/// such function binds the calls to it.
void bindFloatCallResults(const BinaryImage &Img, PipelineResult &Result) {
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  if (TRI.FPReturnReg == 0 || TRI.ReturnsFPInX87 ||
      !TRI.isVectorReg(TRI.fpReturnModelReg()))
    return;
  auto FloatReturn = [](const MedFunc &MF) -> uint16_t {
    return MF.ReturnType && MF.ReturnType->Kind == NdTypeKind::Float &&
                   MF.ReturnType->Size <= sizeof(double) &&
                   MF.MultiReturn.empty() && !MF.FPReturnViaX87
               ? MF.ReturnType->Size
               : 0;
  };
  std::map<va_t, uint16_t> FloatReturns = Result.CallFloatReturns;
  for (const MedFunc &MF : Result.MedFuncs)
    if (const uint16_t Bytes = FloatReturn(MF))
      FloatReturns.try_emplace(MF.Entry, Bytes);
  for (size_t Round = 0; Round <= Result.MedFuncs.size(); ++Round) {
    bool Found = false;
    for (MedFunc &MF : Result.MedFuncs) {
      bool Bound = false;
      for (MedBlock &Blk : MF.Blocks)
        for (size_t OI = 0; OI < Blk.Ops.size(); ++OI) {
          const MedOp &Op = Blk.Ops[OI];
          if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
              Op.NumInputs == 0 || !Op.Inputs[0].isConst() || Op.SourceCallHint)
            continue;
          if (const auto Callee = FloatReturns.find(Op.Inputs[0].ConstVal);
              Callee != FloatReturns.end())
            Bound |=
                promoteFloatCallResult(MF, Blk, OI, Callee->second, Img.Arch);
        }
      if (!Bound || MF.SourceParametersBound)
        continue;
      inferMedTypes(MF, Img.Arch, &FloatReturns);
      if (const uint16_t Bytes = FloatReturn(MF))
        Found |= FloatReturns.try_emplace(MF.Entry, Bytes).second;
    }
    if (!Found)
      break;
  }
}

namespace {
/// The import-address names of one run, plus the names of the functions it
/// lifts. When those imports come from the session cache, every name this run
/// adds is removed afterwards: the next run must still see imports only. A
/// fresh map already belongs to this run and is left as it is.
struct ImportNameOverlay {
  std::map<va_t, std::string> *Names = nullptr;
  std::map<va_t, std::string> Owned;
  std::vector<std::pair<va_t, std::optional<std::string>>> Undo;
  std::set<va_t> Recorded;
  bool Rollback = false;

  ~ImportNameOverlay() {
    if (!Rollback || !Names)
      return;
    for (auto It = Undo.rbegin(); It != Undo.rend(); ++It) {
      if (!It->second)
        Names->erase(It->first);
      else
        (*Names)[It->first] = std::move(*It->second);
    }
  }

  ImportNameOverlay() = default;
  ImportNameOverlay(const ImportNameOverlay &) = delete;
  ImportNameOverlay &operator=(const ImportNameOverlay &) = delete;

  std::map<va_t, std::string> &names() { return *Names; }

  void adoptOwned(std::map<va_t, std::string> Map) {
    Owned = std::move(Map);
    Names = &Owned;
    Rollback = false;
  }

  /// Imports already in \p Shared win, as buildFuncNameMap writes them last.
  void adoptShared(std::map<va_t, std::string> *Shared,
                   const std::vector<LowFunc> &LowFuncs) {
    Names = Shared;
    Rollback = true;
    for (const LowFunc &LF : LowFuncs)
      if (!Names->contains(LF.Entry))
        assign(LF.Entry, LF.Name);
  }

  /// Remember \p Key's current value once, so a later direct write (a thunk
  /// stub's assignment) still restores the import map.
  void remember(va_t Key) {
    if (!Rollback || !Recorded.insert(Key).second)
      return;
    const auto It = Names->find(Key);
    if (It == Names->end())
      Undo.emplace_back(Key, std::nullopt);
    else
      Undo.emplace_back(Key, It->second);
  }

  void assign(va_t Key, const std::string &Value) {
    remember(Key);
    (*Names)[Key] = Value;
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// buildHighIR — Phase 3
//===----------------------------------------------------------------------===//

void Pipeline::prepareSourceMedIR(const BinaryImage &Img,
                                  PipelineResult &Result, DebugContext *Dbg,
                                  PipelineAnalysisCache *Cache) {
  // The HighIR route needs the same call-site evidence for register-pair
  // integer returns as direct MedIR -> LLVM emission.  In particular, a
  // 32-bit callee may look low-word-only until another function reads both
  // return registers.  Recover the module-wide return contract before call
  // ABI recovery and source return lowering inspect its functions.
  // Source-bound entries already have authoritative parameters and return
  // types. Re-inferring them can reinterpret preserved register seeds as
  // additional source parameters and discard the binding.
  for (MedFunc &MF : Result.MedFuncs)
    if (!MF.SourceParametersBound)
      inferMedTypes(MF, Img.Arch);
  modelWideIntReturns(Img, Result, MedStackProjection::Source);
  // Calls remodeled to return a packed register pair must agree with the
  // corresponding definition on the HighIR route as well as the LLVM route.
  recoverStructReturnFromCallers(Img, Result);
  propagateStructReturnForwarderShapes(Img, Result);
  recoverStructReturnFromBody(Img, Result, StructReturnEvidence::Callers);
  recoverVectorReturnFromCallers(Img, Result);
  materializeKnownStructReturnCallSites(Img, Result);
  bindFloatCallResults(Img, Result);
  settleReturnContracts(Img, Result, Dbg);

  // Imports name every selection the same way. The cache keeps that map;
  // this run's function names sit on it only until Names is destroyed.
  ImportNameOverlay Names;
  if (std::map<va_t, std::string> *Shared = importAddressNames(Img, Cache))
    Names.adoptShared(Shared, Result.LowFuncs);
  else
    Names.adoptOwned(buildFuncNameMap(Img, Result));
  std::map<va_t, std::string> &AllFuncNames = Names.names();
  for (const auto &[Address, Name] :
       pipeline_detail::debugFunctionNames(Result, AllFuncNames, Dbg))
    Names.assign(Address, Name);
  // A positional register/stack ABI needs one final argument prefix on both
  // source routes. Independent register banks keep their existing source
  // recovery, including floating return veneers and variadic format joins.
  const CallArgumentConvention *Convention =
      callArgumentConvention(Img.Arch, Img.abiFormat());
  if (Convention && (Convention->ArgumentsFromCallSetup ||
                     (Convention->StackArgumentSummary &&
                      Convention->PositionalArgumentSlots))) {
    recoverModuleCallAbi(Img, Result, AllFuncNames,
                         /*SourceDeclarations=*/false,
                         /*SourceParameterReads=*/true);
    propagateForwardedPointerParams(Result.MedFuncs, Img.Arch);
    // A variadic callee takes its call sites' overflow arguments as trailing
    // stack parameters, which MedToHigh seeds where its va_arg walk reads
    // them, as lift mode does for the LLVM emitter.
    finalizeVariadicCallees(Result.MedFuncs, Img.Arch, Img.abiFormat());
  } else {
    // A convention whose calls take their arguments from callee summaries
    // recovers no call arguments here: a System V variadic callee's overflow
    // is the stack its calls store.
    sizeVariadicOverflowFromStores(Result.MedFuncs, Img.Arch, Img.abiFormat());
  }
  // A tail call that forwards a multi-register return (`return g(x);`, an
  // import's stub) returns every register its callee returns, as in lift
  // mode; without the remodel the RETURN finds the trailing registers only
  // before the call, which may change them.
  remodelStructReturnForwarderCalls(Img, Result);
  // A va_start forwarder passes C's own va_list only to a parameter that
  // takes one: an import's or a function's named as a routine's va_list form,
  // or one of a function in this image the analysis follows.
  if (llvm::any_of(Result.MedFuncs, [](const MedFunc &MF) {
        return MF.VaListDisplacement.has_value();
      })) {
    const libc::LibCVariant Variant = libc::libcVariantOf(Img);
    markVaListCalls(
        Result.MedFuncs, Img.Arch, Img.abiFormat(), [&](va_t Target) {
          std::string Name = importCalleeCName(Img, Target);
          if (const auto It = AllFuncNames.find(Target);
              Name.empty() && It != AllFuncNames.end() &&
              !isSynthesizedFuncName(It->second))
            Name = cNameOfSymbol(It->second, Img.Format, Img.Arch).str();
          NamedCallee Callee;
          if (const auto Arity = libc::libcArityForSymbol(Name, Variant))
            Callee.IntegerParams = static_cast<unsigned>(Arity->IntArgs);
          Callee.VaList = libc::vaListParameter(Name);
          return Callee;
        });
  }
  refineSourcePointerParameters(Img, Result, Dbg);
  prepareRecoveredSourceStackAccesses(Img, Result, Result.LowFuncs);
  // Both C projections consume these original-machine observations after
  // source ABI remodeling. They never participate in type or call recovery.
  std::map<va_t, const LowFunc *> OriginalFunctions;
  for (const auto &Function : Result.LowFuncs) {
    auto [Found, Inserted] =
        OriginalFunctions.emplace(Function.Entry, &Function);
    if (!Inserted)
      Found->second = nullptr;
  }
  for (auto &Function : Result.MedFuncs) {
    Function.FrameBytes.clear();
    if (const auto Original = OriginalFunctions.find(Function.Entry);
        !Function.AnalystStackFrame && Original != OriginalFunctions.end() &&
        Original->second)
      Function.FrameBytes = observeSourceFrameBytes(*Original->second, Img);
  }
}

void Pipeline::buildHighIR(const BinaryImage &Img, const PipelineOptions &Opts,
                           PipelineResult &Result, DebugContext *Dbg) {
  prepareSourceMedIR(Img, Result, Dbg, sharedAnalysisCache(Img, Opts));
  if (!refuseObservedCallReturnReads(Img, Result, refusalFailsRun(Opts), Dbg))
    throw std::runtime_error(Result.Error);
  rebuildHighIR(Img, Opts, Result, Dbg);
}

bool Pipeline::resumeHighIR(const BinaryImage &Img, const PipelineOptions &Opts,
                            PipelineResult &Result, DebugContext *Dbg) {
  if (!Result.Success || !Result.HighIRDeferred)
    return Result.Success;
  if (Result.SourceImage != &Img) {
    Result.Success = false;
    Result.Error = "deferred HighIR belongs to a different image";
    return false;
  }
  Result.Success = pipeline_detail::runHighIRStage(
      Result, [&] { rebuildHighIR(Img, Opts, Result, Dbg); });
  if (Result.Success)
    Result.HighIRDeferred = false;
  return Result.Success;
}

void Pipeline::rebuildHighIR(const BinaryImage &Img,
                             const PipelineOptions &Opts,
                             PipelineResult &Result, DebugContext *Dbg) {
  for (const MedFunc &Function : Result.MedFuncs)
    MedToHighConverter::validateCallContracts(Function);
  PipelineAnalysisCache *Cache = sharedAnalysisCache(Img, Opts);
  // Same import map as prepareSourceMedIR. Debug and thunk names are this
  // run's overlay; Names removes them after the workers below have joined.
  ImportNameOverlay Names;
  if (std::map<va_t, std::string> *Shared = importAddressNames(Img, Cache))
    Names.adoptShared(Shared, Result.LowFuncs);
  else
    Names.adoptOwned(buildFuncNameMap(Img, Result));
  std::map<va_t, std::string> &AllFuncNames = Names.names();
  for (const auto &[Address, Name] :
       pipeline_detail::debugFunctionNames(Result, AllFuncNames, Dbg))
    Names.assign(Address, Name);

  // detectThunkStubs writes LowFunc entries itself. Remember those values
  // first so an overwrite of an import name is undone with the rest.
  for (const LowFunc &LF : Result.LowFuncs)
    Names.remember(LF.Entry);
  detectThunkStubs(Result.LowFuncs, AllFuncNames);

  // The same direct and indirect targets recur across MedIR operations.
  // Resolve each distinct address before starting workers so absent names do
  // not repeatedly scan the image's full symbol table (including address 0
  // for unresolved indirect calls). Keep the existing naming precedence.
  std::set<va_t> CallTargets;
  for (const MedFunc &MF : Result.MedFuncs)
    for (const MedBlock &B : MF.Blocks)
      for (const MedOp &Op : B.Ops)
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
          CallTargets.insert(Op.NumInputs && Op.Inputs[0].isConst()
                                 ? static_cast<va_t>(Op.Inputs[0].ConstVal)
                                 : 0);
  std::map<va_t, std::string> ResolvedCalleeNames;
  MedToHighConverter NameResolver;
  NameResolver.setBinaryImage(&Img);
  NameResolver.setFuncNames(&AllFuncNames);
  // A run that shares the analysis cache names them from its symbol
  // inventory, read once for every run.
  std::shared_ptr<const PipelineAnalysisCache::SymbolInventory> Inventory;
  if (Cache) {
    Inventory = PipelineAnalysisCache::SymbolInventory::of(Img, *Cache);
    NameResolver.setImageFunctionNames(&Inventory->functionNames(Img));
  }
  NameResolver.resolveCalleeNames(CallTargets, ResolvedCalleeNames);
  std::map<va_t, VariadicVectorCallee> VariadicVectorCallees;
  for (const MedFunc &MF : Result.MedFuncs)
    if (MF.takesVectorArgumentRegisters())
      VariadicVectorCallees[MF.Entry] = {MF.VariadicOverflowCount,
                                         MF.VaNamedIntegerRegisters};

  const size_t Total = Result.MedFuncs.size();
  // A worker failure invalidates the whole stage. Keep output private until
  // every worker has joined so callers never receive a partially built batch.
  std::vector<HighFunc> Pending(Total);

  // Weight each function by its MedIR op count so the heaviest structurings
  // start first and the tail stays balanced (see parallelForEachWeighted).
  std::vector<uint64_t> Weight(Total, 1);
  std::vector<HighSourceMap> Sources(Total);
  std::set<va_t> RecognizedFunctions;
  for (const auto &Match : Result.LibraryRecognitions)
    RecognizedFunctions.insert(Match.Function);
  for (size_t I = 0; I < Total; ++I) {
    uint64_t W = 1;
    for (const auto &B : Result.MedFuncs[I].Blocks)
      W += B.Ops.size() + B.Phis.size();
    Weight[I] = W;
  }

  parallelForEachWeighted(Weight, [&](auto Claim, size_t N) {
    MedToHighConverter Local;
    Local.setSymbolicSimplification(Opts.SymbolicHighC);
    // The outer helper runs small batches on the caller. Only those batches
    // can lend their unused worker allowance to a large function's independent
    // semantic segments; parallel function workers must not nest another pool.
    Local.setSemanticThreadLimit(
        Total < limits::kMinParallelIRWorkItems ? workerThreadCount() : 1);
    Local.setBinaryImage(&Img);
    Local.setFuncNames(&AllFuncNames);
    Local.setResolvedCalleeNames(&ResolvedCalleeNames);
    Local.setVariadicVectorCallees(&VariadicVectorCallees);
    Local.setPrototypeArgumentCallees(&Result.CallPrototypeArguments);
    for (size_t FI; (FI = Claim()) < N;) {
      const MedFunc &MF = Result.MedFuncs[FI];
      Local.setExpressionObserver({});
      Local.setExpressionCloneObserver({});
      Local.setStatementObserver({});
      Local.setObservationReset({});
      std::map<const HighExpr *, std::vector<size_t>> SourceIndex;
      bool SourceOverflow = false;
      size_t SourceWork =
          ::neverd::limits::pipeline::kPipelineHighIRSourceWorkBudget;
      std::vector<const sigs::LibraryRecognition *> SourceRegions;
      for (const auto &R : Result.LibraryRecognitions)
        if (R.Function == MF.Entry && R.Isolated &&
            R.Scope != sigs::LibraryFeatureScope::WholeFunction)
          SourceRegions.push_back(&R);
      auto Append = [&](sigs::LibraryOccurrence Occurrence, const ExprPtr &Expr,
                        HighSourceKind Kind = HighSourceKind::Expression,
                        std::optional<StmtKind> StatementKind = std::nullopt,
                        std::optional<uint8_t> ExpressionKind = std::nullopt) {
        if (SourceOverflow)
          return;
        if (Sources[FI].size() >=
            ::neverd::limits::pipeline::
                kPipelineHighIRPipelineRebuildHighIRAppendSourcesFISizeLimit) {
          Sources[FI].clear();
          SourceIndex.clear();
          SourceOverflow = true;
          return;
        }
        if (Expr && Kind == HighSourceKind::Expression)
          SourceIndex[Expr.get()].push_back(Sources[FI].size());
        Sources[FI].push_back(
            {MF.Entry, Occurrence, Expr, Kind, StatementKind, ExpressionKind});
      };
      auto Observe = [&](sigs::LibraryOccurrence Occurrence,
                         const ExprPtr &Expr) {
        Append(Occurrence, Expr);
        for (const auto *R : SourceRegions) {
          auto Contains = [&](const auto &O) {
            return std::binary_search(R->Occurrences.begin(),
                                      R->Occurrences.end(), O);
          };
          if (!Contains(Occurrence))
            continue;
          std::set<sigs::LibraryOccurrence> Origins;
          std::set<const HighExpr *> Seen;
          std::vector<ExprPtr> Pending;
          Expr->forEachChildExpr([&](const auto &E) { Pending.push_back(E); });
          while (!Pending.empty()) {
            if (!SourceWork || SourceOverflow) {
              Sources[FI].clear();
              SourceIndex.clear();
              SourceOverflow = true;
              return;
            }
            --SourceWork;
            auto E = Pending.back();
            Pending.pop_back();
            if (!E || !Seen.insert(E.get()).second ||
                (E->Kind != ExprKind::BinOp && E->Kind != ExprKind::UnaryOp &&
                 E->Kind != ExprKind::Cast && E->Kind != ExprKind::BitCast &&
                 E->Kind != ExprKind::Addr))
              continue;
            if (auto At = SourceIndex.find(E.get()); At != SourceIndex.end())
              for (size_t I : At->second) {
                const auto &S = Sources[FI][I];
                if (S.Expression.lock() == E && Contains(S.Occurrence))
                  Origins.insert(S.Occurrence);
              }
            Pending.insert(Pending.end(), E->Operands.begin(),
                           E->Operands.end());
          }
          for (auto O : Origins)
            if (O != Occurrence)
              Append(O, Expr);
        }
      };
      Local.setStatementObserver([&](const MedOp &Op, const HighStmt &S) {
        const auto Source =
            observeHighStatement(MF.Entry, {Op.Addr, Op.OriginSeq}, S);
        const ExprPtr Call = S.Kind == StmtKind::Assign ? S.Val : S.CallExpr;
        Append(Source.Occurrence,
               Source.Kind == HighSourceKind::StatementExpression ? Call
                                                                  : ExprPtr(),
               Source.Kind, Source.StatementKind, Source.ExpressionKind);
      });
      Local.setObservationReset([&] {
        Sources[FI].clear();
        SourceIndex.clear();
        SourceOverflow = false;
        SourceWork =
            ::neverd::limits::pipeline::kPipelineHighIRSourceWorkBudget;
      });
      if (RecognizedFunctions.contains(MF.Entry)) {
        Local.setExpressionObserver(
            [&, FI](const MedOp &Op, const ExprPtr &Expr) {
              Observe({Op.Addr, Op.OriginSeq}, Expr);
            });
        Local.setExpressionCloneObserver(
            [&](const ExprPtr &From, const ExprPtr &To) {
              auto At = SourceIndex.find(From.get());
              if (At == SourceIndex.end())
                return;
              const auto Indices = At->second;
              for (size_t I : Indices) {
                if (SourceOverflow)
                  break;
                const auto &S = Sources[FI][I];
                if (S.Expression.lock() == From)
                  Observe(S.Occurrence, To);
              }
            });
      }
      auto keepIdentity = [&] {
        HighFunc &HF = Pending[FI];
        HF.Name = MF.Name;
        HF.Entry = MF.Entry;
        HF.OriginalSize = MF.OriginalSize;
        HF.DebugName = MF.DebugName;
        HF.SourceFile = MF.SourceFile;
        HF.SourceLine = MF.SourceLine;
        HF.ExceptionMetadata = MF.ExceptionMetadata;
        HF.HasUnresolvedSEHFrame = MF.HasUnresolvedSEHFrame;
        HF.ReturnType = MF.ReturnType ? MF.ReturnType : NdType::makeInt(4);
        HF.ReturnsNoValue = MF.ReturnsNoValue;
        HF.SourceTypeHint = MF.SourceTypeHint;
        if (!MF.Blocks.empty()) {
          fillUnstructuredGotoSkeleton(HF, MF);
          return;
        }
        if (FI >= Result.LowFuncs.size() || Result.LowFuncs[FI].Blocks.empty())
          return;
        const LowFunc &LF = Result.LowFuncs[FI];
        MedFunc FromLow;
        FromLow.Name = LF.Name;
        FromLow.Entry = LF.Entry;
        FromLow.ExceptionMetadata = LF.ExceptionMetadata;
        for (const auto &LB : LF.Blocks) {
          MedBlock MB;
          MB.Id = LB.Id;
          MB.StartAddr = LB.StartAddr;
          MB.EndAddr = LB.EndAddr;
          MB.Succs = LB.Succs;
          MB.Preds = LB.Preds;
          FromLow.Blocks.push_back(std::move(MB));
        }
        if (HF.Name.empty())
          HF.Name = LF.Name;
        if (!HF.ExceptionMetadata)
          HF.ExceptionMetadata = LF.ExceptionMetadata;
        fillUnstructuredGotoSkeleton(HF, FromLow);
      };
      if (MF.Blocks.empty()) {
        keepIdentity();
        continue;
      }
      try {
        Pending[FI] = Local.convert(MF, Img.Arch);
        auto &HF = Pending[FI];
        // Structuring may invert or simplify an If condition. Its statement
        // retains the original branch address even when the expression root
        // changes; bind only an unambiguous original branch at that address.
        if (RecognizedFunctions.contains(MF.Entry)) {
          std::map<va_t, std::vector<sigs::LibraryOccurrence>> Branches;
          for (const auto &B : MF.Blocks)
            for (const auto &Op : B.Ops)
              if (!Op.Dead && Op.Opcode == NdOp::COND_BR &&
                  Op.Addr != InvalidVA && Op.OriginSeq >= 0)
                Branches[Op.Addr].push_back({Op.Addr, Op.OriginSeq});
          walkStmts(HF.Body, [&](const HighStmt &S) {
            if (!S.Cond)
              return;
            auto At = Branches.find(S.Addr);
            if (At != Branches.end() && At->second.size() == 1)
              Observe(At->second.front(), S.Cond);
          });
        }
        HF.OriginalSize = MF.OriginalSize;
        HF.DebugName = MF.DebugName;
        HF.SourceFile = MF.SourceFile;
        HF.SourceLine = MF.SourceLine;
        if (!HF.ExceptionMetadata)
          HF.ExceptionMetadata = MF.ExceptionMetadata;
        // Structuring can yield an empty body even when MedIR/LowIR exist.
        // Identity-only HighFuncs become trap stubs in HighC; emit a goto
        // skeleton instead so coverage cannot treat them as silent success.
        if (HF.Body.empty())
          keepIdentity();
      } catch (const std::exception &Error) {
        keepIdentity();
        llvm::errs() << "pipeline: highir threw on " << MF.Name << " at 0x"
                     << llvm::utohexstr(MF.Entry) << ": " << Error.what()
                     << "\n";
      } catch (...) {
        keepIdentity();
        llvm::errs() << "pipeline: highir threw on " << MF.Name << " at 0x"
                     << llvm::utohexstr(MF.Entry) << "\n";
      }
    }
  });
  Result.HighFuncs = std::move(Pending);
  Result.HighSources.clear();
  for (auto &Source : Sources)
    for (auto &Observation : Source)
      if (Observation.Kind != HighSourceKind::Expression ||
          !Observation.Expression.expired())
        Result.HighSources.push_back(std::move(Observation));
}

} // namespace neverd
