//===- PipelineStructReturnCallSites.cpp - Struct return call sites ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Call-site rewriting for multi-register struct returns: materializing the
/// return fields at known call sites and remodeling tail-call forwarders.
///
//===----------------------------------------------------------------------===//

#include "PipelineCallAbiDetail.h"
#include "PipelineReturnModelingDetail.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace neverd {

// Materialize the multi-register return at EVERY call site to a known
// multi-register-return callee -- not only the sites whose result the caller
// reads directly (modelCallStructReturn / the StructRetCallees + body passes
// above handled those).  An unrolled loop `for (i) z = f(z);` over an
// HFA-returning f (-O2) chains each call's FP return registers (d0/d1)
// straight into the next call's FP argument registers with NO intervening
// move; the intermediate calls' results are read by no explicit op, so they
// were left as a single-register CALL whose d0/d1 fields were never
// materialized, and the following call's argument recovery (recoverCallAbi,
// below) then resolved the chained FP arguments to the stale pre-loop d0/d1
// (0) -- every iteration collapses to f(0) (cplxmul: a Julia z=z*z+c step
// degenerates to its constant c).  Re-model each such call into a flat
// aggregate temp + one SUBBYTES per field so the return registers are defined
// and the next call's argument recovery picks them up.  Run AFTER all the
// MultiReturn-setting passes and BEFORE recoverCallAbi.  AArch64 register
// returns only; only a call still producing a single register output (an
// already-remodeled call has a Temp output) is touched.  Skip tail-call
// forwarders (`CALL ret; RETURN ret` with no intervening ops): recoverCallAbi
// keys IsTailReturn off the CALL still writing the integer return register;
// remodeling them here breaks forwarded FP-arg live-in recovery and is
// redundant — Phase B (after recoverCallAbi) remodels struct-return
// forwarders to temp+SUBBYTES instead.
void materializeKnownStructReturnCallSites(const BinaryImage &Img,
                                           PipelineResult &Result) {
  const auto &TRI = getTargetRegInfo(Img.Arch);
  if (Img.Arch == Arch::AArch64 && TRI.PointerSize == 8) {
    // All candidate queries precede the insertions below. Index the original
    // occurrences once; no pointer is used after call-site materialization.
    llvm::DenseMap<std::pair<const MedFunc *, uint32_t>, MedOp *> Calls;
    for (auto &Function : Result.MedFuncs)
      if (!Function.StructReturnCandidates.empty())
        for (auto &Block : Function.Blocks)
          for (auto &Op : Block.Ops)
            if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
              Calls.try_emplace({&Function, Op.CallSiteId}, &Op);
    auto findCallSite = [&](MedFunc &MF, uint32_t SiteId) -> MedOp * {
      return Calls.lookup({&MF, SiteId});
    };
    auto isTailForwarder = [](const MedFunc &MF) {
      for (const auto &Blk : MF.Blocks)
        for (size_t I = 0; I + 1 < Blk.Ops.size(); ++I) {
          const MedOp &Call = Blk.Ops[I];
          const MedOp &Ret = Blk.Ops[I + 1];
          if (Call.Opcode == NdOp::CALL && Call.Output.Kind == MedVar::Reg &&
              Ret.Opcode == NdOp::RETURN && Ret.NumInputs >= 1 &&
              Ret.Inputs[0].Kind == MedVar::Reg &&
              Ret.Inputs[0].RegOff == Call.Output.RegOff)
            return true;
        }
      return false;
    };
    auto candidateShape = [&](const MedStructReturnCandidate &Candidate) {
      std::vector<MedReturnReg> Shape;
      for (const MedVar &Field : Candidate.Fields) {
        MedReturnReg RR;
        RR.RegOff = Field.RegOff;
        RR.Size = Field.Size;
        RR.IsFP = TRI.isVectorReg(Field.RegOff);
        Shape.push_back(RR);
      }
      return Shape;
    };
    auto isCompleteShape = [&](const std::vector<MedReturnReg> &Shape) {
      if (Shape.size() < 2)
        return false;
      bool IsFP = Shape.front().IsFP;
      llvm::ArrayRef<uint64_t> Regs =
          IsFP ? TRI.FPReturnRegs : TRI.IntReturnRegs;
      if (Shape.size() > Regs.size())
        return false;
      for (size_t I = 0; I < Shape.size(); ++I)
        if (Shape[I].IsFP != IsFP || Shape[I].RegOff != Regs[I])
          return false;
      return true;
    };

    // A tail forwarder has no return-body writes of its own.  Complete field
    // evidence from one of its direct callers seeds its shape, after which the
    // existing forwarder propagation proves the inner callee's shape too.
    std::map<va_t, MedFunc *> ByEntry;
    for (auto &MF : Result.MedFuncs)
      ByEntry[MF.Entry] = &MF;
    for (auto &Caller : Result.MedFuncs)
      for (const MedStructReturnCandidate &Candidate :
           Caller.StructReturnCandidates) {
        MedOp *Call = findCallSite(Caller, Candidate.CallSiteId);
        // A second word read alone is no complete field evidence.
        if (!Call || Call->Opcode != NdOp::CALL || Call->NumInputs < 1 ||
            !Call->Inputs[0].isConst() || Candidate.SecondWordOnly)
          continue;
        auto It = ByEntry.find(Call->Inputs[0].ConstVal);
        if (It == ByEntry.end() || !It->second->MultiReturn.empty() ||
            !isTailForwarder(*It->second))
          continue;
        std::vector<MedReturnReg> Shape = candidateShape(Candidate);
        if (isCompleteShape(Shape))
          It->second->MultiReturn = std::move(Shape);
      }
    propagateStructReturnForwarderShapes(Img, Result);

    std::map<va_t, std::vector<MedReturnReg>> MRByEntry;
    for (const auto &MF : Result.MedFuncs) {
      if (MF.MultiReturn.size() < 2)
        continue;
      MRByEntry[MF.Entry] = MF.MultiReturn;
    }
    // A selection omits callee definitions, not their return-register
    // effects. Read only caller-observed integer pairs through the same
    // bounded body reader used for their argument signatures. Floating records
    // need an omitted HFA declaration as well as their packed field values.
    std::map<va_t, std::vector<MedReturnReg>> OmittedShapes;
    std::set<va_t> ConflictingShapes, OmittedFirstWordObserved;
    for (auto &Caller : Result.MedFuncs)
      for (const auto &Candidate : Caller.StructReturnCandidates) {
        const MedOp *Call = findCallSite(Caller, Candidate.CallSiteId);
        if (!Call || Call->Opcode != NdOp::CALL || !Call->NumInputs ||
            !Call->Inputs[0].isConst() || Call->SourceCallHint ||
            Result.CallPrototypeArguments.count(Call->Inputs[0].ConstVal) ||
            Result.CallUserPrototypes.count(Call->Inputs[0].ConstVal) ||
            ByEntry.count(Call->Inputs[0].ConstVal))
          continue;
        auto Shape = candidateShape(Candidate);
        if (!isCompleteShape(Shape) || Shape.front().IsFP)
          continue;
        // Integer aggregate fields occupy whole ABI words even when one
        // consumer observes only their low bytes, like selected definitions.
        for (auto &Field : Shape)
          if (!Field.IsFP)
            Field.Size = TRI.PointerSize;
        const va_t Target = Call->Inputs[0].ConstVal;
        if (!Candidate.SecondWordOnly)
          OmittedFirstWordObserved.insert(Target);
        auto [It, Inserted] = OmittedShapes.emplace(Target, Shape);
        if (!Inserted &&
            !std::equal(It->second.begin(), It->second.end(), Shape.begin(),
                        Shape.end(), [](const auto &A, const auto &B) {
                          return A.RegOff == B.RegOff && A.Size == B.Size &&
                                 A.IsFP == B.IsFP;
                        }))
          ConflictingShapes.insert(Target);
      }
    for (va_t Target : ConflictingShapes)
      OmittedShapes.erase(Target);
    confirmOmittedStructReturns(Img, Result, OmittedShapes,
                                OmittedFirstWordObserved);
    MRByEntry.insert(OmittedShapes.begin(), OmittedShapes.end());
    for (auto &Caller : Result.MedFuncs) {
      for (const MedStructReturnCandidate &Candidate :
           Caller.StructReturnCandidates) {
        MedOp *Call = findCallSite(Caller, Candidate.CallSiteId);
        if (!Call || Call->Opcode != NdOp::CALL || Call->NumInputs < 1 ||
            !Call->Inputs[0].isConst())
          continue;
        const Import *Imp = Img.findImportAt(Call->Inputs[0].ConstVal);
        if (!Imp || Candidate.SecondWordOnly)
          continue;
        std::vector<MedReturnReg> Shape = candidateShape(Candidate);
        if (!isCompleteShape(Shape))
          continue;
        // A complete contiguous X0:X1 use is sufficient caller-side evidence
        // for an unresolved import's small integer aggregate return.  No
        // symbol whitelist can describe arbitrary dylib APIs, and leaving X1
        // as an unknown call clobber provably discards a field the caller
        // consumes.  FP aggregate imports remain restricted to the curated
        // complex-return signatures because incidental V-register reads are
        // much easier to confuse with independent scalar state.
        if (Shape.front().IsFP) {
          auto Sig =
              libc::libcArityForSymbol(Imp->Name, libc::libcVariantOf(Img));
          if (!Sig || !Sig->FpRetComplex || Shape.size() != 2)
            continue;
        }
        MRByEntry[Call->Inputs[0].ConstVal] = std::move(Shape);
      }
    }
    if (!MRByEntry.empty())
      for (auto &MF : Result.MedFuncs) {
        int NextId = 0, NextVer = 0;
        auto bump = [&](const MedVar &V) {
          NextVer = std::max(NextVer, V.SSAVer + 1);
          NextId = std::max(NextId, V.Id + 1);
        };
        for (const auto &B : MF.Blocks) {
          for (const auto &Op : B.Ops) {
            bump(Op.Output);
            for (uint8_t I = 0; I < Op.NumInputs; ++I)
              bump(Op.Inputs[I]);
          }
          for (const auto &Phi : B.Phis) {
            bump(Phi.Output);
            for (const auto &A : Phi.Args)
              bump(A.second);
          }
        }
        for (const MedCallClobber &Clobber : MF.CallClobbers) {
          bump(Clobber.Value);
          if (Clobber.PreservedPrefixSize > 0)
            bump(Clobber.PreservedInput);
        }
        for (const MedStructReturnCandidate &Candidate :
             MF.StructReturnCandidates)
          for (const MedVar &Field : Candidate.Fields)
            bump(Field);

        std::set<uint32_t> MaterializedSites;
        for (auto &Blk : MF.Blocks) {
          for (size_t OI = 0; OI < Blk.Ops.size(); ++OI) {
            auto &Op = Blk.Ops[OI];
            if (Op.Opcode != NdOp::CALL || Op.NumInputs < 1 ||
                !Op.Inputs[0].isConst())
              continue;
            // Tail-call forwarder: leave CALL on the integer return register
            // until recoverCallAbi + Phase B (see block comment above).
            if (OI + 1 < Blk.Ops.size() &&
                Blk.Ops[OI + 1].Opcode == NdOp::RETURN)
              continue;
            // A call already remodeled (FP/wide/struct) produces a
            // non-register output; only the default single-register CALL is a
            // candidate.
            if (Op.Output.Kind != MedVar::Reg)
              continue;
            auto It = MRByEntry.find(Op.Inputs[0].ConstVal);
            if (It == MRByEntry.end())
              continue;
            uint32_t SiteId = Op.CallSiteId;
            va_t CallAddr = Op.Addr;
            const auto &Fields = It->second;
            uint16_t Total = 0;
            for (const auto &F : Fields)
              Total += F.Size;
            MedVar OrigOut = Op.Output;
            MedVar Tv;
            Tv.Kind = MedVar::Temp;
            Tv.TheArch = Arch::AArch64;
            Tv.Id = NextId++;
            Tv.SSAVer = 0;
            Tv.Size = Total;
            Op.Output = Tv;
            std::vector<MedOp> Extracts;
            const MedStructReturnCandidate *Candidate = nullptr;
            for (const MedStructReturnCandidate &C : MF.StructReturnCandidates)
              if (C.CallSiteId == Op.CallSiteId) {
                Candidate = &C;
                break;
              }
            uint16_t Cum = 0;
            for (const auto &F : Fields) {
              MedVar Out;
              if (F.RegOff == OrigOut.RegOff) {
                Out = OrigOut;
              } else {
                const MedVar *Exact = nullptr;
                if (Candidate)
                  for (const MedVar &Field : Candidate->Fields)
                    if (Field.RegOff == F.RegOff) {
                      Exact = &Field;
                      break;
                    }
                if (!Exact)
                  for (const MedCallClobber &Clobber : MF.CallClobbers)
                    if (Clobber.CallSiteId == Op.CallSiteId &&
                        Clobber.Value.RegOff == F.RegOff &&
                        (!Exact || Clobber.Value.Size == F.Size)) {
                      Exact = &Clobber.Value;
                      if (Exact->Size == F.Size)
                        break;
                    }
                if (Exact)
                  Out = *Exact;
                else {
                  Out.Kind = MedVar::Reg;
                  Out.TheArch = Arch::AArch64;
                  Out.RegOff = F.RegOff;
                  Out.Id = NextId++;
                  Out.SSAVer = NextVer++;
                }
              }
              Out.Size = F.Size;
              MedOp Ex;
              Ex.Opcode = NdOp::SUBBYTES;
              Ex.Addr = Op.Addr;
              Ex.Output = Out;
              Ex.addInput(Tv);
              Ex.addInput(MedVar::makeConst(Cum, TRI.PointerSize));
              Extracts.push_back(Ex);

              // Define every width-view clobbered by this call from the same
              // proven return field, so existing exact SSA uses stay linked.
              for (const MedCallClobber &Clobber : MF.CallClobbers) {
                if (Clobber.CallSiteId != Op.CallSiteId ||
                    Clobber.Value.RegOff != F.RegOff || Clobber.Value == Out)
                  continue;
                MedOp Alias;
                Alias.Addr = CallAddr;
                Alias.Output = Clobber.Value;
                if (Alias.Output.Size < Out.Size) {
                  Alias.Opcode = NdOp::SUBBYTES;
                  Alias.addInput(Out);
                  Alias.addInput(MedVar::makeConst(0, TRI.PointerSize));
                } else if (Alias.Output.Size > Out.Size) {
                  Alias.Opcode = NdOp::INT_ZEXT;
                  Alias.addInput(Out);
                } else {
                  Alias.Opcode = NdOp::COPY;
                  Alias.addInput(Out);
                }
                Extracts.push_back(Alias);
              }
              Cum += F.Size;
            }
            MF.CallClobbers.erase(
                std::remove_if(MF.CallClobbers.begin(), MF.CallClobbers.end(),
                               [&](const MedCallClobber &Clobber) {
                                 if (Clobber.CallSiteId != Op.CallSiteId)
                                   return false;
                                 return std::any_of(
                                     Fields.begin(), Fields.end(),
                                     [&](const MedReturnReg &F) {
                                       return F.RegOff == Clobber.Value.RegOff;
                                     });
                               }),
                MF.CallClobbers.end());
            Blk.Ops.insert(Blk.Ops.begin() + OI + 1, Extracts.begin(),
                           Extracts.end());
            OI += Extracts.size();
            MaterializedSites.insert(SiteId);
          }
        }
        MF.StructReturnCandidates.erase(
            std::remove_if(MF.StructReturnCandidates.begin(),
                           MF.StructReturnCandidates.end(),
                           [&](const MedStructReturnCandidate &Candidate) {
                             return MaterializedSites.count(
                                        Candidate.CallSiteId) != 0;
                           }),
            MF.StructReturnCandidates.end());
      }
  }
}

namespace {

/// Whether \p Fields is the integer register pair a double-word integer
/// returns in (RDX:RAX, X1:X0), each field a whole register.
bool isIntegerRegisterPair(const std::vector<MedReturnReg> &Fields,
                           const TargetRegInfo &TRI) {
  auto wholeRegister = [&](const MedReturnReg &Field, uint64_t RegOff) {
    return !Field.IsFP && Field.RegOff == RegOff &&
           (Field.Size == 0 || Field.Size == TRI.PointerSize);
  };
  return TRI.IntReturnReg2 != 0 && Fields.size() == 2 &&
         wholeRegister(Fields[0], TRI.IntReturnReg) &&
         wholeRegister(Fields[1], TRI.IntReturnReg2);
}

} // namespace

// Struct-return tail-call forwarder: `struct S f(args){return g(args);}`
// lowers at -O2 to a lone `b g`, rewritten (CFGBuilder::rewriteAsTailCall) to
// `CALL ret; RETURN ret` carrying only the single integer return register.
// When f returns a small struct across multiple registers -- proven by f's
// own callers, so MF.MultiReturn is already set by the caller-side
// StructRetCallees pass above -- the forwarding call captures only the first
// field register and the RETURN reassembles the trailing fields from
// registers nothing ever wrote (they come back 0: `structfwd` returns {a, 0}
// instead of {a, 2a}).  Two repairs, mirroring how an ordinary struct-return
// call site is modeled (modelCallStructReturn): (1) re-type the in-module
// callee g to the same multi-register struct -- a `return g(...)` returns
// exactly what g returns -- so g actually produces every field register; (2)
// turn the forwarding CALL into a flat aggregate temp with one SUBBYTES per
// field so the RETURN's struct path reads each field register (the emitter
// flattens g's struct result into that temp, the inverse of the callee-side
// aggregate assembly).  Run after the main recoverCallAbi loop so the
// forwarder's IsTailReturn argument recovery (which keys off the CALL still
// writing the integer return register) already ran.  64-bit register struct
// returns only (modelCallStructReturn and the caller-side remodel are gated
// to PointerSize==8; 32-bit aggregates return through sret / the wide-int
// pair).
//
// An import's stub forwards the same way, through the import's slot: to
// this image's own routine, when the loader starts the slot there
// (importOwnRoutine), which is then g; otherwise to another module's
// routine, which keeps the ABI its declaration gives it, so only a compiler
// runtime routine declared to return a double word (`__udivti3`,
// CompilerRuntimeDoubleWord.def) forwards the integer register pair.  A
// forwarder hands its shape to the function it forwards to, which may come
// earlier in the list and forward in turn (`__fixxfti` tail-calls the stub of
// `__fixunsxfti`), so the remodel repeats until no call changes; a remodeled
// call writes a temp and is not remodeled again.
void remodelStructReturnForwarderCalls(const BinaryImage &Img,
                                       PipelineResult &Result) {
  const auto &TRI = getTargetRegInfo(Img.Arch);
  if (TRI.PointerSize == 8) {
    std::map<va_t, MedFunc *> ByEntryMut;
    for (auto &MF : Result.MedFuncs)
      ByEntryMut[MF.Entry] = &MF;
    bool Changed;
    do {
      Changed = false;
      for (auto &MF : Result.MedFuncs) {
        // The function returns a multi-register struct (proven by its
        // callers).  The forwarder is detected structurally below by the
        // tail `CALL ret; RETURN ret` pattern (rewriteAsTailCall's output)
        // -- not by an empty parameter list, since recoverCallAbi has
        // already surfaced the forwarded incoming arguments as this
        // function's parameters.
        if (MF.MultiReturn.size() < 2)
          continue;
        bool Rewrote = false;
        for (auto &Blk : MF.Blocks) {
          for (size_t I = 0; I + 1 < Blk.Ops.size(); ++I) {
            auto &Op = Blk.Ops[I];
            if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
                Op.NumInputs < 1 || !Op.Inputs[0].isConst() ||
                Op.Output.Kind != MedVar::Reg)
              continue;
            auto &Ret = Blk.Ops[I + 1];
            if (Ret.Opcode != NdOp::RETURN || Ret.NumInputs < 1 ||
                Ret.Inputs[0].Kind != MedVar::Reg ||
                Ret.Inputs[0].RegOff != Op.Output.RegOff)
              continue;
            bool ThroughImport = false;
            MedFunc *G =
                forwardingCallee(Img, Result, ByEntryMut, Op, &ThroughImport);
            if (G) {
              // A routine reached through an import's entry took the shape
              // its body confirms before the return contracts settled
              // (propagateStructReturnForwarderShapes), or none.
              if (G->MultiReturn.empty() && !ThroughImport)
                G->MultiReturn = MF.MultiReturn; // g returns what f returns
              // The forwarder's proven shape and the callee's emitted shape
              // must agree (same register in each field) so the SUBBYTES
              // outputs line up with both the call's packed result and the
              // RETURN reassembly.
              if (G->MultiReturn.size() != MF.MultiReturn.size())
                continue;
              bool Match = true;
              for (size_t K = 0; K < MF.MultiReturn.size() && Match; ++K)
                if (G->MultiReturn[K].RegOff != MF.MultiReturn[K].RegOff)
                  Match = false;
              if (!Match)
                continue;
            } else if (!isIntegerRegisterPair(MF.MultiReturn, TRI) ||
                       !importReturnsDoubleWord(Img, Op.Inputs[0].ConstVal,
                                                TRI.PointerSize)) {
              // Another module's routine: only a declared double word.
              continue;
            }
            const auto &Fields = MF.MultiReturn;

            // Fresh Id/SSAVer for the flat result temp and per-field
            // extracts.
            int MaxId = 0, MaxVer = 0;
            auto bump = [&](const MedVar &V) {
              MaxId = std::max(MaxId, V.Id);
              MaxVer = std::max(MaxVer, V.SSAVer);
            };
            for (const auto &B : MF.Blocks) {
              for (const auto &O : B.Ops) {
                bump(O.Output);
                for (uint8_t K = 0; K < O.NumInputs; ++K)
                  bump(O.Inputs[K]);
              }
              for (const auto &Ph : B.Phis) {
                bump(Ph.Output);
                for (const auto &A : Ph.Args)
                  bump(A.second);
              }
            }
            int NextId = MaxId + 1, NextVer = MaxVer + 1;
            uint16_t Total = 0;
            for (const auto &F : Fields)
              Total += F.Size ? F.Size : 8;

            MedVar Tv;
            Tv.Kind = MedVar::Temp;
            Tv.TheArch = Img.Arch;
            Tv.Id = NextId++;
            Tv.SSAVer = 0;
            Tv.Size = Total;
            Op.Output = Tv;

            std::vector<MedOp> Extracts;
            uint16_t Cum = 0;
            for (const auto &F : Fields) {
              uint16_t Sz = F.Size ? F.Size : 8;
              MedVar Out;
              Out.Kind = MedVar::Reg;
              Out.TheArch = Img.Arch;
              Out.RegOff = F.RegOff;
              Out.Id = NextId++;
              Out.SSAVer = NextVer++;
              Out.Size = Sz;
              MedOp Ex;
              Ex.Opcode = NdOp::SUBBYTES;
              Ex.Addr = Op.Addr;
              Ex.Output = Out;
              Ex.addInput(Tv);
              Ex.addInput(MedVar::makeConst(Cum, TRI.PointerSize));
              Extracts.push_back(Ex);
              Cum += Sz;
            }
            // rewriteAsTailCall hard-codes the integer return register
            // (X0/RAX) even for HFA forwarders whose fields live in v0/d0.. —
            // wire the RETURN to the first extracted field so SSA stays valid
            // (the struct return path reassembles every field by register
            // regardless).
            if (!Extracts.empty())
              Ret.Inputs[0] = Extracts.front().Output;
            Blk.Ops.insert(Blk.Ops.begin() + I + 1, Extracts.begin(),
                           Extracts.end());
            Rewrote = true;
            break;
          }
          if (Rewrote)
            break;
        }
        Changed |= Rewrote;
      }
    } while (Changed);
  }
}

} // namespace neverd
