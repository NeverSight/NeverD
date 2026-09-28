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

#include "PipelineReturnModelingDetail.h"

#include "neverd/Common.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/NdTypes.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/MedABIPass.h"
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
  for (MedFunc &MF : Result.MedFuncs)
    inferMedTypes(MF, Img.Arch);
  modelWideIntReturns(Img, Result);

  auto AllFuncNames = buildFuncNameMap(Img, Result);
  if (Img.Arch == Arch::ARM) {
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
    propagateARMForwardedPointerParams(Result.MedFuncs);
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
}

} // namespace neverd
