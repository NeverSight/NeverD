//===- PipelineStructReturn.cpp - Aggregate return recovery --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Whole-module recovery of multi-register struct, HFA, and wide-vector
/// returns.
///
//===----------------------------------------------------------------------===//

#include "PipelineReturnModelingDetail.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/med/MedReturnArguments.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/SymbolDecoration.h"
#include "neverd/pipeline/Limits.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {

// A caller's field reads propose a shape. Each finite path to a return must
// pass a complete, defined write, or preserve an observed argument. A loop
// backedge alone supplies no initial value, and calls cut off older writes.
// A caller that reads only the second word observed no argument passed
// through: an untouched register is then what the caller itself left.
bool returnsStructFields(const MedFunc &Function,
                         llvm::ArrayRef<MedReturnReg> Shape, Arch Architecture,
                         BinaryFormat Format, bool PreservedArguments) {
  if (Function.SkippedSSA || Function.Blocks.empty())
    return false;
  const auto &TRI = getTargetRegInfo(Architecture);
  using Key = std::tuple<MedVar::VarKind, int, int>;
  const auto Identity = [](const MedVar &V) -> Key {
    return {V.Kind, V.Id, V.SSAVer};
  };
  std::map<int, const MedBlock *> Blocks;
  std::set<Key> Definitions, Unknown;
  std::map<Key, std::vector<Key>> Users;
  for (const MedBlock &Block : Function.Blocks) {
    Blocks.emplace(Block.Id, &Block);
    for (const PhiNode &Phi : Block.Phis)
      Definitions.insert(Identity(Phi.Output));
    for (const MedOp &Op : Block.Ops) {
      if (Op.Output.Size)
        Definitions.insert(Identity(Op.Output));
      for (const MedVar &Output : Op.IntrinsicOutputs)
        Definitions.insert(Identity(Output));
    }
  }
  for (const auto &Clobber : Function.CallClobbers)
    Unknown.insert(Identity(Clobber.Value));
  const auto Depend = [&](const MedVar &Output, const MedVar &Input) {
    if (!Output.Size || Input.isConst() || Input.Kind == MedVar::Param)
      return;
    const Key Out = Identity(Output), In = Identity(Input);
    if (Input.Kind == MedVar::Unspecified || !Definitions.count(In))
      Unknown.insert(Out);
    else
      Users[In].push_back(Out);
  };
  for (const MedBlock &Block : Function.Blocks) {
    for (const PhiNode &Phi : Block.Phis) {
      if (Phi.Args.empty())
        Unknown.insert(Identity(Phi.Output));
      for (const auto &[Pred, Input] : Phi.Args)
        Depend(Phi.Output, Input);
      if (Phi.ExceptionalEntry)
        Depend(Phi.Output, *Phi.ExceptionalEntry);
    }
    for (const MedOp &Op : Block.Ops) {
      const bool Seed = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                        Op.Output == Op.Inputs[0];
      if (Seed) {
        const bool Parameter =
            Op.Output.Kind == MedVar::Reg &&
            ((Op.Output.RegOff == TRI.StackPointer &&
              Op.Output.Size == TRI.PointerSize) ||
             llvm::any_of(Function.Params, [&](const auto &P) {
               return P.RegOff == Op.Output.RegOff && P.Size >= Op.Output.Size;
             }));
        if (!Parameter)
          Unknown.insert(Identity(Op.Output));
        continue;
      }
      for (unsigned I = 0; I != Op.NumInputs; ++I) {
        Depend(Op.Output, Op.Inputs[I]);
        for (const MedVar &Output : Op.IntrinsicOutputs)
          Depend(Output, Op.Inputs[I]);
      }
    }
  }
  // Propagate missing inputs once through the SSA use graph, including PHIs
  // and arithmetic. A copy cannot turn an implicit call clobber into a field.
  std::vector<Key> Pending(Unknown.begin(), Unknown.end());
  for (size_t I = 0; I < Pending.size(); ++I)
    if (const auto Found = Users.find(Pending[I]); Found != Users.end())
      for (const Key &User : Found->second)
        if (Unknown.insert(User).second)
          Pending.push_back(User);

  bool SawReturn = false;
  for (const auto &Field : Shape) {
    if (PreservedArguments &&
        returnsArgumentUnchanged(Function, Field, TRI, Format))
      continue;
    // A block end is checked once per field even when many returns share
    // predecessors. The first visit enqueues every path before marking it.
    std::set<int> SeenEnds;
    std::vector<std::pair<const MedBlock *, size_t>> Work;
    for (const MedBlock &Block : Function.Blocks)
      for (size_t I = 0; I < Block.Ops.size(); ++I)
        if (Block.Ops[I].Opcode == NdOp::RETURN) {
          SawReturn = true;
          Work.emplace_back(&Block, I);
        }
    while (!Work.empty()) {
      const auto [Block, Before] = Work.back();
      Work.pop_back();
      if (Before == Block->Ops.size() && !SeenEnds.insert(Block->Id).second)
        continue;
      bool Written = false;
      for (size_t I = Before; I-- > 0;) {
        const MedOp &Op = Block->Ops[I];
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
          return false;
        const MedVar &Out = Op.Output;
        if (Out.Kind != MedVar::Reg || !Out.Size ||
            Out.RegOff >= Field.RegOff + Field.Size ||
            Field.RegOff >= Out.RegOff + Out.Size)
          continue;
        // A synthetic low-byte view doesn't overwrite the full register.
        if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
            isSameRegisterLowSlice(Out.RegOff, Out.Size, Op.Inputs[0].RegOff,
                                   Op.Inputs[0].Size, Op.Inputs[1].ConstVal) &&
            Op.Inputs[0].Kind == MedVar::Reg && Op.Inputs[1].isConst())
          continue;
        const bool SelfCopy = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                              Op.Inputs[0].Kind == MedVar::Reg &&
                              Op.Inputs[0].RegOff == Out.RegOff &&
                              Op.Inputs[0].Size == Out.Size;
        if (SelfCopy)
          continue;
        if (Out.RegOff != Field.RegOff || Out.Size < Field.Size ||
            Unknown.count(Identity(Out)))
          return false;
        Written = true;
        break;
      }
      if (Written)
        continue;
      if (Block->Preds.empty() || !Block->ExceptionalPreds.empty())
        return false;
      for (int Pred : Block->Preds) {
        const auto Found = Blocks.find(Pred);
        if (Found == Blocks.end())
          return false;
        Work.emplace_back(Found->second, Found->second->Ops.size());
      }
    }
  }
  // All fields may be unchanged parameters, with no walk needed above.
  if (!SawReturn)
    for (const MedBlock &Block : Function.Blocks)
      for (const MedOp &Op : Block.Ops)
        SawReturn |= Op.Opcode == NdOp::RETURN;
  return SawReturn;
}

// A small struct returned by value across multiple registers (x86-64 SysV
// eightbytes / AArch64 HFA): the caller-side remodel (modelCallStructReturn,
// run during low->med) rewrote each such direct call to produce a flat
// aggregate temp it SUBBYTES into the field return registers.  Read that
// remodeling back to learn each callee's multi-register return shape, then
// re-type the callee so its RETURN emits the matching LLVM aggregate and the
// backend's ABI lowering places the fields in the right registers.
void recoverStructReturnFromCallers(const BinaryImage &Img,
                                    PipelineResult &Result) {
  const auto &TRI = getTargetRegInfo(Img.Arch);
  if (TRI.PointerSize == 8) {
    std::map<va_t, std::vector<MedReturnReg>> StructRetCallees;
    for (const auto &MF : Result.MedFuncs)
      for (const auto &Blk : MF.Blocks)
        for (size_t I = 0; I < Blk.Ops.size(); ++I) {
          const auto &Op = Blk.Ops[I];
          // Source ABI components are projection metadata; they must not
          // become caller-derived evidence for the binary rewrite ABI.
          if (Op.SourceCallHint || Op.Opcode != NdOp::CALL ||
              Op.Output.Kind != MedVar::Temp || Op.NumInputs < 1 ||
              !Op.Inputs[0].isConst())
            continue;
          std::vector<MedReturnReg> Desc;
          for (size_t J = I + 1; J < Blk.Ops.size(); ++J) {
            const auto &Ex = Blk.Ops[J];
            if (Ex.Opcode != NdOp::SUBBYTES || Ex.NumInputs < 1 ||
                Ex.Inputs[0].Kind != MedVar::Temp ||
                Ex.Inputs[0].Id != Op.Output.Id ||
                Ex.Output.Kind != MedVar::Reg)
              break;
            MedReturnReg RR;
            RR.RegOff = Ex.Output.RegOff;
            RR.Size = Ex.Output.Size;
            RR.IsFP = TRI.isVectorReg(Ex.Output.RegOff);
            Desc.push_back(RR);
          }
          if (Desc.size() >= 2)
            StructRetCallees[Op.Inputs[0].ConstVal] = Desc;
        }
    if (!StructRetCallees.empty())
      for (auto &MF : Result.MedFuncs)
        if (auto It = StructRetCallees.find(MF.Entry);
            It != StructRetCallees.end())
          MF.MultiReturn = It->second;
    // A compiler runtime routine declared to return a double word returns
    // the integer pair from its definition too (CompilerRuntimeDoubleWord.def):
    // libgcc_s defines the __fixunsdfti its calls reach through its PLT entry.
    if (TRI.IntReturnReg2)
      for (auto &MF : Result.MedFuncs)
        if (MF.MultiReturn.empty() && !MF.SourceParametersBound &&
            libc::returnsDoubleWord(
                cNameOfSymbol(MF.Name, Img.Format, Img.Arch), TRI.PointerSize))
          MF.MultiReturn = {{TRI.IntReturnReg, TRI.PointerSize, false},
                            {TRI.IntReturnReg2, TRI.PointerSize, false}};
  }
}

MedFunc *forwardingCallee(const BinaryImage &Img, const PipelineResult &Result,
                          const std::map<va_t, MedFunc *> &ByEntry,
                          const MedOp &Call, bool *ThroughImport) {
  if (ThroughImport)
    *ThroughImport = false;
  if (Call.NumInputs < 1 || !Call.Inputs[0].isConst())
    return nullptr;
  const va_t Target = Call.Inputs[0].ConstVal;
  if (const std::optional<va_t> Own =
          importOwnRoutine(Img, &Result.CallArgumentSources, Target))
    if (const auto It = ByEntry.find(*Own); It != ByEntry.end()) {
      if (ThroughImport)
        *ThroughImport = true;
      return It->second;
    }
  // Another module's routine keeps the ABI its declaration gives it, which
  // its stub's callers do not override: the stub is no function of this
  // module whose return the forwarder decides.
  if (Call.Opcode != NdOp::CALL || Img.findImportStubAt(Target) ||
      !importCalleeName(Img, Target).empty())
    return nullptr;
  const auto It = ByEntry.find(Target);
  return It == ByEntry.end() ? nullptr : It->second;
}

// Struct-return tail-call forwarder shape propagation: `struct S
// f(args){return g(args);}` lowers at -O2 to a lone `b g` (a tail call), so
// f's RETURN forwards exactly g's return value -- f and g have the SAME
// return shape.  The caller-side remodel above learns f's shape from f's own
// direct callers (`main` extracts f's fields), but g -- reached only through
// f's tail call -- is never the target of a struct-return call site, so its
// shape is still unknown here. Propagate f's proven multi-register shape
// FORWARD into such a callee.  Done BEFORE the body-based recovery below and
// only into a callee whose shape is still unknown, so (a) a directly-called
// callee keeps its own caller-proven shape, and (b) a forwarder-reached
// callee takes its forwarder's exact shape rather than the heuristic body
// scan (which can miss a field -- e.g. recover only 2 of a 3-double HFA, or 0
// of a `{x, 2x}` pair whose first field is the passed-through argument).
// Iterated to a fixpoint so a forwarder chain (f -> mid -> g) propagates all
// the way down, through an import's PLT entry to this image's own routine
// too (forwardingCallee).  64-bit register struct returns only (the
// caller-side remodel is gated to PointerSize==8).
void propagateStructReturnForwarderShapes(const BinaryImage &Img,
                                          PipelineResult &Result) {
  const auto &TRI = getTargetRegInfo(Img.Arch);
  if (TRI.PointerSize == 8) {
    auto IsCompatibleSubset = [](const std::vector<MedReturnReg> &Subset,
                                 const std::vector<MedReturnReg> &Shape) {
      if (Subset.size() >= Shape.size())
        return false;
      size_t ShapeIdx = 0;
      for (const MedReturnReg &Field : Subset) {
        while (ShapeIdx < Shape.size() &&
               (Field.RegOff != Shape[ShapeIdx].RegOff ||
                Field.Size != Shape[ShapeIdx].Size ||
                Field.IsFP != Shape[ShapeIdx].IsFP))
          ++ShapeIdx;
        if (ShapeIdx == Shape.size())
          return false;
        ++ShapeIdx;
      }
      return true;
    };
    std::map<va_t, MedFunc *> ByEntry;
    for (auto &MF : Result.MedFuncs)
      ByEntry[MF.Entry] = &MF;
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (auto &MF : Result.MedFuncs) {
        if (MF.MultiReturn.size() < 2)
          continue;
        for (const auto &Blk : MF.Blocks) {
          bool Found = false;
          for (size_t I = 0; I + 1 < Blk.Ops.size(); ++I) {
            const auto &Op = Blk.Ops[I];
            if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
                Op.NumInputs < 1 || !Op.Inputs[0].isConst() ||
                Op.Output.Kind != MedVar::Reg)
              continue;
            const auto &Ret = Blk.Ops[I + 1];
            if (Ret.Opcode != NdOp::RETURN || Ret.NumInputs < 1 ||
                Ret.Inputs[0].Kind != MedVar::Reg ||
                Ret.Inputs[0].RegOff != Op.Output.RegOff)
              continue;
            bool ThroughImport = false;
            MedFunc *Callee =
                forwardingCallee(Img, Result, ByEntry, Op, &ThroughImport);
            // Through an import's entry the shape comes from the entry's
            // callers, which read what the routine left in the integer return
            // registers: no call keeps them for its caller, so a read there
            // is of a result.  A shape with a floating field is no such
            // evidence: libgcc's __divtf3 returns a float128 in XMM0, yet its
            // entry's callers gave it {RAX, XMM0}.
            const bool Confirmed =
                !ThroughImport ||
                llvm::none_of(MF.MultiReturn,
                              [](const MedReturnReg &F) { return F.IsFP; });
            if (Callee && Confirmed &&
                (Callee->MultiReturn.empty() ||
                 IsCompatibleSubset(Callee->MultiReturn, MF.MultiReturn))) {
              Callee->MultiReturn = MF.MultiReturn;
              Changed = true;
            }
            Found = true;
            break;
          }
          if (Found)
            break;
        }
      }
    }
  }
}

/// Whether \p MF returns a whole vector in its FP return register: a RETURN
/// whose closest write to the register since the last call is a genuine
/// 16-byte value.  With \p NoIntegerResult, that RETURN's block also neither
/// writes an integer return register nor leaves a call's integer result there
/// (see the body-only use below).
static bool returnsWholeVector(const MedFunc &MF, const TargetRegInfo &TRI,
                               bool NoIntegerResult) {
  if (TRI.FPReturnRegs.empty())
    return false;
  const uint64_t V0 = TRI.FPReturnRegs.front();
  std::map<std::pair<int, int>, const MedOp *> Defs;
  for (const auto &Blk : MF.Blocks)
    for (const auto &O : Blk.Ops)
      if (O.Output.Id >= 0)
        Defs[{O.Output.Id, O.Output.SSAVer}] = &O;
  // True iff \p V (traced through COPY chains) is a genuine >=16-byte
  // value -- a real vector -- rather than a zero-extended <=8-byte
  // scalar (an `INT_ZEXT Qn <- Dn` HFA element / scalar FP).
  auto isWideVec = [&](MedVar V) -> bool {
    for (int Depth = 0;
         Depth < ::neverd::limits::pipeline::
                     kPipelineStructReturnsWholeVectorIsWideVecDepthLimit;
         ++Depth) {
      if (V.Kind == MedVar::Const)
        return false;
      auto It = Defs.find({V.Id, V.SSAVer});
      if (It == Defs.end())
        return V.Size >= 16;
      const MedOp *D = It->second;
      if (D->Opcode == NdOp::INT_ZEXT)
        return false; // zero-extended scalar/HFA element, high 64 = 0
      if (D->Opcode == NdOp::COPY && D->NumInputs >= 1) {
        V = D->Inputs[0];
        continue;
      }
      return D->Output.Size >= 16; // CONCAT / LOAD / NEON real 16 bytes
    }
    return false;
  };
  bool WideVecRet = false;
  for (const auto &Blk : MF.Blocks) {
    int RetIdx = -1;
    for (size_t I = 0; I < Blk.Ops.size(); ++I)
      if (Blk.Ops[I].Opcode == NdOp::RETURN) {
        RetIdx = static_cast<int>(I);
        break;
      }
    if (RetIdx < 0)
      continue;
    const MedOp *V0Write = nullptr;
    bool GPWritten = false;
    bool GPLiveFromCall = false;
    for (int J = RetIdx - 1; J >= 0; --J) {
      const auto &O = Blk.Ops[J];
      if (O.Opcode == NdOp::CALL || O.Opcode == NdOp::INDIR_CALL) {
        // The function's live return may be THIS call's own GP result,
        // left in the GP return register and not overwritten before the
        // RETURN -- a `p = malloc(n); /* fill *p with NEON q-register
        // stores */ return p;` tail.  The V0 writes after the call are
        // then dead memcpy scratch (the loaded init data on its way to
        // memory), not a vector return; without this the pointer return
        // is mistyped <2 x i64> and the caller dereferences garbage.
        if (!GPWritten && O.Output.Kind == MedVar::Reg)
          for (uint64_t IR : TRI.IntReturnRegs)
            if (O.Output.RegOff == IR)
              GPLiveFromCall = true;
        break;
      }
      if (O.Output.Kind != MedVar::Reg || O.Output.Size == 0)
        continue;
      bool SelfCopy = O.Opcode == NdOp::COPY && O.NumInputs >= 1 &&
                      O.Inputs[0].Kind == MedVar::Reg &&
                      O.Inputs[0].RegOff == O.Output.RegOff;
      if (SelfCopy)
        continue;
      if (O.Output.RegOff == V0 && !V0Write)
        V0Write = &O;
      for (uint64_t IR : TRI.IntReturnRegs)
        if (O.Output.RegOff == IR)
          GPWritten = true;
    }
    if (V0Write && (!NoIntegerResult || (!GPWritten && !GPLiveFromCall)) &&
        isWideVec(V0Write->Output)) {
      WideVecRet = true;
      break;
    }
  }
  return WideVecRet;
}

// A whole vector returned in the FP return register: an x86-64 `__m128` or
// `{float, float}` packed in xmm0, a NEON vector in V0.  The type pass types
// only a scalar float's return, and a body alone cannot tell a returned
// vector from a vector operation's leftover, so a direct caller must read the
// register after the call (modelCallFPReturn bound the call's result to it)
// while none reads an integer result from it.  An integer register the
// callee writes before such a RETURN is then its scratch.
void recoverVectorReturnFromCallers(const BinaryImage &Img,
                                    PipelineResult &Result) {
  const auto &TRI = getTargetRegInfo(Img.Arch);
  if (!TRI.hasFPReturnReg() || !TRI.isVectorReg(TRI.FPReturnReg) ||
      TRI.ReturnsFPInX87)
    return;
  std::set<va_t> ReadAsVector, ReadAsInteger;
  for (const auto &Caller : Result.MedFuncs) {
    std::set<std::pair<int, int>> Read;
    for (const auto &Blk : Caller.Blocks) {
      for (const auto &Op : Blk.Ops)
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          Read.insert({Op.Inputs[I].Id, Op.Inputs[I].SSAVer});
      for (const auto &Phi : Blk.Phis)
        for (const auto &Arg : Phi.Args)
          Read.insert({Arg.second.Id, Arg.second.SSAVer});
    }
    for (const auto &Blk : Caller.Blocks)
      for (const auto &Op : Blk.Ops) {
        if (Op.SourceCallHint || Op.Opcode != NdOp::CALL || Op.NumInputs < 1 ||
            !Op.Inputs[0].isConst() || Op.Output.Kind != MedVar::Reg ||
            !Read.count({Op.Output.Id, Op.Output.SSAVer}))
          continue;
        if (Op.Output.RegOff == TRI.FPReturnReg)
          ReadAsVector.insert(Op.Inputs[0].ConstVal);
        else if (llvm::is_contained(TRI.IntReturnRegs, Op.Output.RegOff))
          ReadAsInteger.insert(Op.Inputs[0].ConstVal);
      }
  }
  for (auto &MF : Result.MedFuncs)
    if (!MF.SourceTypeHint && !MF.SourceParametersBound &&
        MF.MultiReturn.empty() && !MF.FPReturnViaX87 &&
        !(MF.ReturnType && MF.ReturnType->Kind == NdTypeKind::Float) &&
        ReadAsVector.count(MF.Entry) && !ReadAsInteger.count(MF.Entry) &&
        returnsWholeVector(MF, TRI, /*NoIntegerResult=*/false)) {
      MF.ReturnType = NdType::makeFloat(16);
      MF.ReturnsVectorRegister = true;
    }
}

// A function reached ONLY through a function pointer is never the target of a
// direct CALL, so the caller-side struct-return remodel above never learns
// its multi-register return shape (an indirect call site cannot name its
// callee). Recover it from the callee's own body instead: clang's by-value
// small-struct return loads each field into its return register straight-line
// before RETURN
// (`ldr x0,[..]; ldr x1,[..]; ret` for a GP struct; `ldr d0; ldr d1; ret` for
// a 2-double HFA).  A candidate return register (x0:x1, or the HFA v0..v3) is
// a genuine return field when, before a RETURN with no intervening call, it
// is written by a value op (not a live-in self-copy) whose result is LIVE-OUT
// -- not read again before the RETURN.  The live-out test is what separates a
// real field from an FP scratch register: a `double f(double a,double
// b){return a*b;}` reloads b into d1 and then CONSUMES it in the multiply (d1
// is read after its last write), so d1 is not a field; a 2-double HFA leaves
// d0 and d1 untouched after loading them, so both are fields.  AArch64
// returns a small aggregate either all-GP or all-HFA, never mixed (a mixed
// live-out set is a scalar FP return plus the lifter's dead integer
// placeholder), so a mixed set is rejected.  declareFunc then emits the
// aggregate return ({i64,i64} / {double,double} / {float,float}) and the
// indirect call site (modeled by modelCallStructReturn for INDIR_CALL) reads
// every field.  Gated to AArch64 (the x86-64 RAX/RDX overlap with div/mul
// byproducts is handled on the direct-call path's redefine tracking) and to
// functions WITHOUT an already recovered MultiReturn (a directly-called
// struct returner keeps the proven Pipeline- propagated shape).
void recoverStructReturnFromBody(const BinaryImage &Img, PipelineResult &Result,
                                 StructReturnEvidence Evidence) {
  const auto &TRI = getTargetRegInfo(Img.Arch);
  struct RetCand {
    uint64_t RegOff;
    bool IsFP;
  };
  std::vector<RetCand> Cands;
  for (uint64_t R : TRI.IntReturnRegs)
    Cands.push_back({R, false});
  for (uint64_t R : TRI.FPReturnRegs)
    Cands.push_back({R, true});
  if (Img.Arch == Arch::AArch64 && Cands.size() >= 2) {
    std::map<va_t, const LowFunc *> LowByEntry;
    for (const auto &LF : Result.LowFuncs)
      LowByEntry.emplace(LF.Entry, &LF);

    // A direct caller that reads V1.. after the call reads registers that only
    // the callee can have defined, so it has observed a homogeneous
    // floating-point aggregate returned in V0..Vn, not one vector in V0.
    // modelCallStructReturn left that evidence as a candidate because a field
    // is an implicit call clobber.  Keep a shape only when every such caller
    // agrees on it and it names consecutive FP return registers of one width.
    std::map<va_t, std::vector<MedReturnReg>> CallerHFAShapes;
    std::set<va_t> ConflictingCallerShapes;
    // Targets with a caller that read the first word as well.
    std::set<va_t> FirstWordObserved;
    for (const auto &Caller : Result.MedFuncs)
      for (const MedStructReturnCandidate &Candidate :
           Caller.StructReturnCandidates) {
        const MedOp *Call = nullptr;
        for (const auto &Blk : Caller.Blocks)
          for (const auto &Op : Blk.Ops)
            if (Op.CallSiteId == Candidate.CallSiteId &&
                (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL))
              Call = &Op;
        if (!Call || Call->Opcode != NdOp::CALL || Call->NumInputs < 1 ||
            !Call->Inputs[0].isConst())
          continue;
        std::vector<MedReturnReg> Shape;
        bool Complete = Candidate.Fields.size() >= 2 &&
                        Candidate.Fields.size() <= TRI.FPReturnRegs.size();
        for (size_t I = 0; Complete && I < Candidate.Fields.size(); ++I) {
          const MedVar &Field = Candidate.Fields[I];
          Complete = Field.RegOff == TRI.FPReturnRegs[I] &&
                     (Field.Size == 4 || Field.Size == 8) &&
                     Field.Size == Candidate.Fields.front().Size;
          Shape.push_back({Field.RegOff, Field.Size, /*IsFP=*/true});
        }
        // The integer return registers a caller reads in order are the
        // fields of a non-HFA record.  Each holds eight of its bytes, as if
        // loaded by LDP, though the caller may read only the low word.
        if (!Complete) {
          Shape.clear();
          Complete = Candidate.Fields.size() >= 2 &&
                     Candidate.Fields.size() <= TRI.IntReturnRegs.size();
          for (size_t I = 0; Complete && I < Candidate.Fields.size(); ++I) {
            const MedVar &Field = Candidate.Fields[I];
            Complete = Field.RegOff == TRI.IntReturnRegs[I] &&
                       (Field.Size == 4 || Field.Size == 8);
            Shape.push_back({Field.RegOff, TRI.PointerSize, /*IsFP=*/false});
          }
        }
        if (!Complete)
          continue;
        const va_t Target = Call->Inputs[0].ConstVal;
        if (!Candidate.SecondWordOnly)
          FirstWordObserved.insert(Target);
        auto [It, Inserted] = CallerHFAShapes.emplace(Target, Shape);
        if (!Inserted &&
            !std::equal(It->second.begin(), It->second.end(), Shape.begin(),
                        Shape.end(),
                        [](const MedReturnReg &A, const MedReturnReg &B) {
                          return A.RegOff == B.RegOff && A.Size == B.Size &&
                                 A.IsFP == B.IsFP;
                        }))
          ConflictingCallerShapes.insert(Target);
      }
    for (va_t Target : ConflictingCallerShapes)
      CallerHFAShapes.erase(Target);

    for (auto &MF : Result.MedFuncs) {
      if (!MF.MultiReturn.empty())
        continue;
      if (auto Shape = CallerHFAShapes.find(MF.Entry);
          Shape != CallerHFAShapes.end() &&
          returnsStructFields(MF, Shape->second, Img.Arch, Img.abiFormat(),
                              FirstWordObserved.count(MF.Entry) != 0)) {
        MF.MultiReturn = Shape->second;
        continue;
      }
      if (Evidence == StructReturnEvidence::Callers)
        continue;
      const LowFunc *LF = nullptr;
      if (auto It = LowByEntry.find(MF.Entry); It != LowByEntry.end())
        LF = It->second;

      // A single 128-bit vector returned by value in V0 (a NEON `int32x4` /
      // `float __attribute__((vector_size(16)))`): the callee assembles the
      // whole 16-byte value into V0 with REAL data in the high 64 bits and
      // returns it, unlike a 2..4 element HFA which puts each element in its
      // own D/S register.  The HFA recovery below would pair the wide V0 with
      // a stale V1 (often a reloaded argument register) into a bogus
      // {double,double}, silently corrupting lanes 2-3.  The discriminator is
      // the high-64 content, NOT the bare write width: the lifter models
      // every `ldr d0` (an 8-byte HFA element) as `INT_ZEXT Q0 <- d` -- also
      // a 16-byte V0 write, but a zero-extension whose high 64 bits are 0.  A
      // genuine vector instead reaches V0 through a CONCAT lane-assembly /
      // 16-byte LOAD / NEON op (real high lanes).  Detect the latter before a
      // RETURN with no genuine GP-return-register write (a pure FP/vector
      // return, so not an __int128 returned in X0:X1 that clang stages
      // through V0 at the call site) and type the function as a 16-byte
      // vector return; the emitter's fpAbiType lowers it to a <2 x i64>
      // return carrying all 16 bytes (V0 read whole).
      if (returnsWholeVector(MF, TRI, /*NoIntegerResult=*/true)) {
        MF.ReturnType = NdType::makeFloat(16);
        MF.ReturnsVectorRegister = true;
        continue; // single vector return, not a multi-register HFA
      }

      std::vector<MedReturnReg> Fields;
      for (const auto &Blk : MF.Blocks) {
        const LowBlock *LowBlk = nullptr;
        if (LF)
          for (const auto &B : LF->Blocks)
            if (B.Id == Blk.Id && B.StartAddr == Blk.StartAddr) {
              LowBlk = &B;
              break;
            }
        int RetIdx = -1;
        for (size_t I = 0; I < Blk.Ops.size(); ++I)
          if (Blk.Ops[I].Opcode == NdOp::RETURN) {
            RetIdx = static_cast<int>(I);
            break;
          }
        if (RetIdx < 0)
          continue;
        std::vector<MedReturnReg> BlkFields;
        for (const auto &C : Cands) {
          // Last genuine (non-self-copy) write to this return register before
          // the RETURN, plus its natural element width.  A Q-register zero
          // extension (size 16) is the lifter's normalization of a D/S write,
          // not the field width, so the element size takes the widest write
          // that still fits a single field (<= 8 bytes).
          int WIdx = -1;
          uint16_t ElemSz = 0;
          for (int J = RetIdx - 1; J >= 0; --J) {
            const auto &O = Blk.Ops[J];
            if (O.Opcode == NdOp::CALL || O.Opcode == NdOp::INDIR_CALL)
              break;
            if (O.Output.Kind != MedVar::Reg || O.Output.Size == 0 ||
                O.Output.RegOff != C.RegOff)
              continue;
            bool SelfCopy = O.Opcode == NdOp::COPY && O.NumInputs >= 1 &&
                            O.Inputs[0].Kind == MedVar::Reg &&
                            O.Inputs[0].RegOff == O.Output.RegOff;
            if (SelfCopy)
              continue;
            if (WIdx < 0)
              WIdx = J;
            if (O.Output.Size <= 8)
              ElemSz = std::max(ElemSz, O.Output.Size);
          }
          if (WIdx < 0)
            continue;
          // Live-out test: the last write's value must not be read again
          // before the RETURN (an FP scratch multiplicand is read by its
          // consumer).
          bool Consumed = false;
          for (int J = WIdx + 1; J < RetIdx && !Consumed; ++J) {
            const auto &O = Blk.Ops[J];
            // The lifter emits architectural flag calculations after the
            // value-producing instruction.  Those reads describe side effects
            // of the write; they do not consume the register's returned value.
            if (O.Output.Kind == MedVar::Flag && O.Addr == Blk.Ops[WIdx].Addr)
              continue;
            for (uint8_t K = 0; K < O.NumInputs; ++K)
              if (O.Inputs[K] == Blk.Ops[WIdx].Output) {
                Consumed = true;
                break;
              }
          }
          // MedIR eliminates a compare whose flags are dead.  The machine
          // instruction still read the register after its last write, so it
          // must count for the body-only return-shape heuristic.  Read the
          // pre-DCE LowIR for distinct later instructions; flag micro-ops at
          // the producing instruction's own address are not consumers.
          if (!Consumed && LowBlk) {
            const va_t WriteAddr = Blk.Ops[WIdx].Addr;
            const va_t ReturnAddr = Blk.Ops[RetIdx].Addr;
            // AArch64's Q-register normalization may write 16 bytes for an
            // S/D field; only the recovered field's low bytes are relevant.
            const uint16_t FieldSize =
                ElemSz ? ElemSz
                       : std::min<uint16_t>(Blk.Ops[WIdx].Output.Size, 8);
            const uint64_t RegEnd = C.RegOff + FieldSize;
            for (const LowOp &O : LowBlk->Ops) {
              if (O.Addr <= WriteAddr || O.Addr >= ReturnAddr)
                continue;
              for (uint8_t K = 0; K < O.NumInputs; ++K) {
                const NdVar &In = O.Inputs[K];
                if (In.isReg() && In.Offset < RegEnd &&
                    C.RegOff < In.Offset + In.Size) {
                  Consumed = true;
                  break;
                }
              }
              if (Consumed)
                break;
            }
          }
          if (Consumed)
            continue;
          MedReturnReg RR;
          RR.RegOff = C.RegOff;
          RR.IsFP = C.IsFP;
          RR.Size = ElemSz ? (ElemSz >= 8 ? 8 : (ElemSz >= 4 ? 4 : ElemSz)) : 8;
          BlkFields.push_back(RR);
        }
        if (BlkFields.size() >= 2) {
          Fields = std::move(BlkFields);
          break;
        }
      }
      if (Fields.size() < 2)
        continue;
      bool AnyInt = false, AnyFP = false;
      for (const auto &F : Fields)
        (F.IsFP ? AnyFP : AnyInt) = true;
      if (AnyInt && AnyFP)
        continue; // AArch64 never mixes GP and FP fields in a register return
      MF.MultiReturn = std::move(Fields);
    }
  }
}

} // namespace neverd
