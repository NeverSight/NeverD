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
#include "PipelineReturnModelingDetail.h"

#include "neverd/Common.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/NdTypes.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

namespace {
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
} // namespace

//===----------------------------------------------------------------------===//
// buildHighIR — Phase 3
//===----------------------------------------------------------------------===//

void Pipeline::buildHighIR(const BinaryImage &Img,
                           const PipelineOptions & /*Opts*/,
                           PipelineResult &Result, DebugContext *Dbg) {
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
  modelWideIntReturns(Img, Result);
  // Calls remodeled to return a packed register pair must agree with the
  // corresponding definition on the HighIR route as well as the LLVM route.
  recoverStructReturnFromCallers(Img, Result);
  propagateStructReturnForwarderShapes(Img, Result);
  materializeKnownStructReturnCallSites(Img, Result);
  bindFloatCallResults(Img, Result);
  settleReturnContracts(Img, Result, Dbg);

  auto AllFuncNames = buildFuncNameMap(Img, Result);
  // Without callee summaries, a convention's call arguments, and the
  // parameters a forwarder passes straight through, are the setup each caller
  // writes, as the LLVM route recovers them.
  const CallArgumentConvention *Convention =
      callArgumentConvention(Img.Arch, Img.abiFormat());
  if (Convention && Convention->ArgumentsFromCallSetup) {
    recoverModuleCallAbi(Img, Result, AllFuncNames);
    matchCallsToCalleeSignatures(Img, Result);
    propagateForwardedPointerParams(Result.MedFuncs, Img.Arch);
  } else if (Img.Arch == Arch::ARM) {
    const auto &TRI = getTargetRegInfo(Img.Arch);
    std::map<va_t, int> RegisterArity;
    std::map<va_t, int> TotalArity;
    for (const MedFunc &MF : Result.MedFuncs) {
      int MaxRegister = -1;
      for (const MedVar &Param : MF.Params)
        MaxRegister = std::max(
            MaxRegister,
            TRI.integerArgumentLayout(false).registerIndex(Param.RegOff));
      RegisterArity[MF.Entry] = MaxRegister + 1;
      TotalArity[MF.Entry] = static_cast<int>(MF.Params.size());
    }
    // A wrapper may only forward its live-in registers to another wrapper.
    // Probe calls to a fixed point before mutating the real MedIR so an
    // outer -> middle -> leaf chain is independent of function order.
    std::map<va_t, std::vector<size_t>> DirectCallers;
    std::set<va_t> Entries;
    for (const MedFunc &MF : Result.MedFuncs)
      Entries.insert(MF.Entry);
    for (size_t I = 0; I < Result.MedFuncs.size(); ++I)
      for (const MedBlock &Block : Result.MedFuncs[I].Blocks)
        for (const MedOp &Op : Block.Ops)
          if (Op.Opcode == NdOp::CALL && Op.NumInputs > 0 &&
              Op.Inputs[0].isConst() &&
              Entries.count(Op.Inputs[0].ConstVal) != 0)
            DirectCallers[Op.Inputs[0].ConstVal].push_back(I);
    std::queue<size_t> Work;
    std::vector<bool> Queued(Result.MedFuncs.size(), true);
    for (size_t I = 0; I < Result.MedFuncs.size(); ++I)
      Work.push(I);
    while (!Work.empty()) {
      const size_t I = Work.front();
      Work.pop();
      Queued[I] = false;
      MedFunc Probe = Result.MedFuncs[I];
      recoverCallAbi(Probe, Img.Arch, AllFuncNames, &Img, &RegisterArity,
                     &TotalArity);
      int MaxRegister = -1;
      int MaxIndex = -1;
      for (const MedVar &Param : Probe.Params) {
        if (Param.RegOff != kNoParamReg) {
          const int Index =
              TRI.integerArgumentLayout(false).registerIndex(Param.RegOff);
          MaxRegister = std::max(MaxRegister, Index);
          MaxIndex = std::max(MaxIndex, Index);
        } else if (Param.Kind == MedVar::Param) {
          MaxIndex = std::max(MaxIndex, Param.Id);
        }
      }
      const bool Grew = MaxRegister + 1 > RegisterArity[Probe.Entry] ||
                        MaxIndex + 1 > TotalArity[Probe.Entry];
      RegisterArity[Probe.Entry] =
          std::max(RegisterArity[Probe.Entry], MaxRegister + 1);
      TotalArity[Probe.Entry] = std::max(TotalArity[Probe.Entry], MaxIndex + 1);
      if (!Grew)
        continue;
      for (size_t Caller : DirectCallers[Probe.Entry])
        if (!Queued[Caller]) {
          Queued[Caller] = true;
          Work.push(Caller);
        }
    }
    for (MedFunc &MF : Result.MedFuncs)
      recoverCallAbi(MF, Img.Arch, AllFuncNames, &Img, &RegisterArity,
                     &TotalArity);
    propagateForwardedPointerParams(Result.MedFuncs, Img.Arch);
  }
  if (Dbg && Dbg->hasInfo()) {
    for (const MedFunc &MF : Result.MedFuncs) {
      if (auto DF = Dbg->functionName(MF.Entry); DF && !DF->empty()) {
        auto It = AllFuncNames.find(MF.Entry);
        if (It == AllFuncNames.end() || It->second.empty() ||
            isSynthesizedFuncName(It->second))
          AllFuncNames[MF.Entry] = *DF;
      }
      for (const MedBlock &B : MF.Blocks) {
        for (const MedOp &Op : B.Ops) {
          if (Op.Opcode != NdOp::CALL || Op.NumInputs < 1 ||
              !Op.Inputs[0].isConst())
            continue;
          const va_t Target = static_cast<va_t>(Op.Inputs[0].ConstVal);
          if (!Target)
            continue;
          auto It = AllFuncNames.find(Target);
          if (It != AllFuncNames.end() && !It->second.empty() &&
              !isSynthesizedFuncName(It->second))
            continue;
          if (auto DF = Dbg->functionName(Target); DF && !DF->empty())
            AllFuncNames[Target] = *DF;
        }
      }
    }
  }

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
  NameResolver.resolveCalleeNames(CallTargets, ResolvedCalleeNames);

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
    Local.setBinaryImage(&Img);
    Local.setFuncNames(&AllFuncNames);
    Local.setResolvedCalleeNames(&ResolvedCalleeNames);
    for (size_t FI; (FI = Claim()) < N;) {
      const MedFunc &MF = Result.MedFuncs[FI];
      Local.setExpressionObserver({});
      Local.setExpressionCloneObserver({});
      Local.setStatementObserver({});
      std::map<const HighExpr *, std::vector<size_t>> SourceIndex;
      bool SourceOverflow = false;
      size_t SourceWork = 250000;
      std::vector<const sigs::LibraryRecognition *> SourceRegions;
      for (const auto &R : Result.LibraryRecognitions)
        if (R.Function == MF.Entry && R.Isolated &&
            R.Scope != sigs::LibraryFeatureScope::WholeFunction)
          SourceRegions.push_back(&R);
      auto Append = [&](sigs::LibraryOccurrence Occurrence, const ExprPtr &Expr,
                        HighSourceKind Kind = HighSourceKind::Expression,
                        std::optional<StmtKind> StatementKind = std::nullopt) {
        if (SourceOverflow)
          return;
        if (Sources[FI].size() >= 100000) {
          Sources[FI].clear();
          SourceIndex.clear();
          SourceOverflow = true;
          return;
        }
        if (Expr)
          SourceIndex[Expr.get()].push_back(Sources[FI].size());
        Sources[FI].push_back(
            {MF.Entry, Occurrence, Expr, Kind, StatementKind});
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
        Append({Op.Addr, Op.OriginSeq}, {},
               S.Kind == StmtKind::Store ? HighSourceKind::Store
                                         : HighSourceKind::Statement,
               S.Kind);
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
        if (FI < Result.LowFuncs.size())
          Local.setJumpTables(Result.LowFuncs[FI].JumpTables);
        else
          Local.setJumpTables({});
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
  for (auto &Source : Sources)
    for (auto &Observation : Source)
      if (Observation.Kind != HighSourceKind::Expression ||
          !Observation.Expression.expired())
        Result.HighSources.push_back(std::move(Observation));
}

} // namespace neverd
