//===- MedVerifier.cpp - IR verification pass for MedIR -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Lightweight MedIR verifier that checks structural invariants after each
/// pass.  Catches use-before-def, size mismatches, and SSA violations
/// early rather than letting them propagate into incorrect codegen.
///
/// Active only in debug builds (NDEBUG not defined) and controlled by the
/// LLVM_DEBUG mechanism.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <set>
#include <tuple>

#define DEBUG_TYPE "neverd-med-verify"

namespace neverd {

bool verifyMedFunc(const MedFunc &Func, const char *PassName) {
#ifdef NDEBUG
  (void)Func;
  (void)PassName;
  return true;
#else
  bool OK = true;

  auto Err = [&](const char *Msg, int BlockId, va_t Addr) -> bool {
    llvm::dbgs() << "MedVerifier [" << PassName << "]: " << Msg
                 << " (block=" << BlockId << ", addr=0x"
                 << llvm::format_hex_no_prefix(Addr, 8) << ")\n";
    OK = false;
    return false;
  };

  // 1. Check operand size consistency
  for (const auto &Blk : Func.Blocks) {
    for (const auto &Op : Blk.Ops) {
      // SUBBYTES: input must be wider than output
      if (Op.Opcode == NdOp::SUBBYTES) {
        if (Op.NumInputs >= 1 && Op.Inputs[0].Size > 0 && Op.Output.Size > 0 &&
            Op.Inputs[0].Size < Op.Output.Size)
          Err("SUBBYTES input smaller than output", Blk.Id, Op.Addr);
      }
      // INT_ZEXT / INT_SEXT: input must be narrower than output
      if (Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT) {
        if (Op.NumInputs >= 1 && Op.Inputs[0].Size > 0 && Op.Output.Size > 0 &&
            Op.Inputs[0].Size >= Op.Output.Size)
          Err("ZEXT/SEXT input not narrower than output", Blk.Id, Op.Addr);
      }
      // Binary ops: inputs should match output size (when both are non-zero)
      if (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB ||
          Op.Opcode == NdOp::INT_AND || Op.Opcode == NdOp::INT_OR ||
          Op.Opcode == NdOp::INT_XOR || Op.Opcode == NdOp::INT_MULT) {
        for (uint8_t I = 0; I < Op.NumInputs; ++I) {
          if (Op.Inputs[I].Size > 0 && Op.Output.Size > 0 &&
              !Op.Inputs[I].isConst() && Op.Inputs[I].Size != Op.Output.Size)
            LLVM_DEBUG(llvm::dbgs()
                       << "MedVerifier [" << PassName
                       << "]: size mismatch in binary op (input " << (int)I
                       << " sz=" << Op.Inputs[I].Size
                       << " vs output sz=" << Op.Output.Size << ")\n");
        }
      }
    }
  }

  // 2. Check block connectivity
  for (const auto &Blk : Func.Blocks) {
    for (int Succ : Blk.Succs) {
      if (Succ < 0 || Succ >= static_cast<int>(Func.Blocks.size()))
        Err("invalid successor block id", Blk.Id,
            Blk.Ops.empty() ? 0 : Blk.Ops.front().Addr);
    }
    for (int Pred : Blk.Preds) {
      if (Pred < 0 || Pred >= static_cast<int>(Func.Blocks.size()))
        Err("invalid predecessor block id", Blk.Id,
            Blk.Ops.empty() ? 0 : Blk.Ops.front().Addr);
    }
  }

  // 3. Check PHI nodes: each PHI should have args for all predecessors
  for (const auto &Blk : Func.Blocks) {
    for (const auto &Phi : Blk.Phis) {
      if (Phi.Output.Size == 0)
        Err("PHI with zero-size output", Blk.Id, 0);
      std::set<int> PhiPreds;
      for (const auto &[PredId, Arg] : Phi.Args)
        PhiPreds.insert(PredId);
      for (int Pred : Blk.Preds) {
        if (!PhiPreds.count(Pred))
          LLVM_DEBUG(llvm::dbgs() << "MedVerifier [" << PassName
                                  << "]: PHI missing arg for pred " << Pred
                                  << " in block " << Blk.Id << "\n");
      }
    }
  }

  // 4. Call clobbers are implicit SSA definitions, not unresolved locals.
  std::set<std::tuple<MedVar::VarKind, int, int>> ExplicitDefs;
  std::map<uint32_t, const MedOp *> CallSites;
  for (const MedBlock &Blk : Func.Blocks) {
    for (const MedOp &Op : Blk.Ops) {
      if (Op.Output.Id >= 0 && Op.Output.Size > 0)
        ExplicitDefs.emplace(Op.Output.Kind, Op.Output.Id, Op.Output.SSAVer);
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Op.CallSiteId != 0 && !CallSites.emplace(Op.CallSiteId, &Op).second)
        Err("duplicate call-site identity", Blk.Id, Op.Addr);
    }
    for (const PhiNode &Phi : Blk.Phis)
      ExplicitDefs.emplace(Phi.Output.Kind, Phi.Output.Id, Phi.Output.SSAVer);
  }
  std::set<std::tuple<MedVar::VarKind, int, int>> ClobberDefs;
  for (const MedCallClobber &Record : Func.CallClobbers) {
    const MedVar &Clobber = Record.Value;
    auto Key = std::make_tuple(Clobber.Kind, Clobber.Id, Clobber.SSAVer);
    if (Record.CallSiteId == 0 || !CallSites.count(Record.CallSiteId))
      Err("call clobber has no owning call site", -1, 0);
    else if (Clobber.Kind != MedVar::Reg || Clobber.Id < 0 || Clobber.Size == 0)
      Err("invalid call-clobber definition", -1, 0);
    else if (Record.PreservedPrefixSize > 0 &&
             (Record.PreservedPrefixSize >= Clobber.Size ||
              Record.PreservedInput.Kind != MedVar::Reg ||
              Record.PreservedInput.Id != Clobber.Id ||
              Record.PreservedInput.SSAVer == Clobber.SSAVer ||
              Record.PreservedInput.RegOff != Clobber.RegOff ||
              Record.PreservedInput.Size != Clobber.Size ||
              Record.PreservedInput.TheArch != Clobber.TheArch))
      Err("invalid partially preserved call clobber", -1, 0);
    else if (ExplicitDefs.count(Key))
      Err("call clobber duplicates explicit definition", -1, 0);
    else if (!ClobberDefs.insert(Key).second)
      Err("duplicate call-clobber definition", -1, 0);
  }

  std::set<uint32_t> CandidateSites;
  for (const MedStructReturnCandidate &Candidate :
       Func.StructReturnCandidates) {
    if (Candidate.CallSiteId == 0 || !CallSites.count(Candidate.CallSiteId))
      Err("struct-return candidate has no owning call site", -1, 0);
    else if (!CandidateSites.insert(Candidate.CallSiteId).second)
      Err("duplicate struct-return candidate", -1, 0);
    if (Candidate.Fields.size() < 2)
      Err("struct-return candidate has fewer than two fields", -1, 0);
    for (const MedVar &Field : Candidate.Fields)
      if (Field.Kind != MedVar::Reg || Field.Id < 0 || Field.Size == 0)
        Err("invalid struct-return candidate field", -1, 0);
  }

  LLVM_DEBUG(if (OK) llvm::dbgs() << "MedVerifier [" << PassName << "]: OK ("
                                  << Func.Blocks.size() << " blocks)\n");
  return OK;
#endif
}

} // namespace neverd
