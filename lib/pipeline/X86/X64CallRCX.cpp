//===- X64CallRCX.cpp - Bind native call register results ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A normal C return convention cannot carry a later-observed RCX. Keep that
/// result at its producing call occurrence. This does not infer that any
/// callee input or pointee is unread, and never replays a call or memory load.
//===----------------------------------------------------------------------===//

#include "X64CallRCX.h"

#include "neverd/ir/intrinsics/X86/X64CallRCX.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86/X64CallRCXProfile.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <array>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {
namespace {
using Key = std::tuple<MedVar::VarKind, int, int>;
Key key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }
bool rcx(const MedCallClobber &C) {
  return C.CallSiteId && !C.PreservedPrefixSize &&
         C.Value.Kind == MedVar::Reg && C.Value.TheArch == Arch::X64 &&
         C.Value.RegOff == x86reg::RCX && C.Value.Size == 8;
}
bool transport(const MedOp &Op) {
  if (Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  return Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::SUBBYTES ||
         Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT ||
         Op.Opcode == NdOp::CONCAT;
}
} // namespace

void captureX64CallRCX(MedFunc &F, const BinaryImage &Image,
                       const std::map<va_t, GPRFamilyMask> &Unread,
                       const std::set<va_t> &Emitted) {
  if (Image.Arch != Arch::X64 || Image.abiFormat() != BinaryFormat::COFF ||
      F.CC != CallingConv::Win64 || F.AnalysisOnly || F.SkippedSSA ||
      F.SourceParametersBound || F.AnalystStackFrame ||
      llvm::none_of(F.CallClobbers, rcx))
    return;
  std::map<uint32_t, std::vector<const MedCallClobber *>> Clobbers;
  std::set<Key> Candidates, Used;
  for (const auto &C : F.CallClobbers) {
    Clobbers[C.CallSiteId].push_back(&C);
    Candidates.insert(key(C.Value));
  }
  int NextId = 0;
  bool Profile = true;
  std::map<Key, const PhiNode *> Phis;
  std::map<Key, const MedOp *> Copies;
  for (const auto &B : F.Blocks) {
    for (const auto &Phi : B.Phis)
      if (!Phis.emplace(key(Phi.Output), &Phi).second)
        return;
    for (const auto &Op : B.Ops)
      if (transport(Op) && !Op.Dead && !Op.PrologueSave && Op.Output.Size)
        if (!Copies.emplace(key(Op.Output), &Op).second)
          return;
  }
  std::set<Key> LivePhis;
  std::vector<const PhiNode *> PendingPhis;
  std::set<Key> LiveCopies;
  std::vector<const MedOp *> PendingCopies;
  std::set<Key> Required;
  std::vector<MedVar> PendingRequired;
  const auto Require = [&](const MedVar &V) {
    if (V.Size == 8 && Required.insert(key(V)).second)
      PendingRequired.push_back(V);
  };
  const auto Note = [&](const MedVar &V) {
    NextId = std::max(NextId, V.Id + 1);
    Profile &= x64CallRCXProfileValue(V);
  };
  const auto Read = [&](const MedVar &V) {
    Note(V);
    if (Candidates.count(key(V)))
      Used.insert(key(V));
    if (const auto At = Phis.find(key(V)); At != Phis.end())
      if (LivePhis.insert(At->first).second)
        PendingPhis.push_back(At->second);
    if (const auto At = Copies.find(key(V)); At != Copies.end())
      if (LiveCopies.insert(At->first).second)
        PendingCopies.push_back(At->second);
  };
  std::map<std::pair<int, int>, const MedCallInfo *> Bound;
  for (const auto &C : F.CallInfos) {
    if (!Bound.emplace(std::pair{C.BlockId, C.OpIdx}, &C).second)
      return;
    for (const auto &V : C.Args) {
      Read(V);
      Require(V);
    }
  }
  // One function walk serves every candidate call. No callee body is scanned.
  for (const auto &B : F.Blocks) {
    for (const auto &Phi : B.Phis) {
      Note(Phi.Output);
      for (const auto &[Pred, V] : Phi.Args)
        Note(V);
    }
    for (size_t O = 0; O < B.Ops.size(); ++O) {
      const auto &Op = B.Ops[O];
      if (!x64CallRCXProfileOperation(Op))
        return;
      Note(Op.Output);
      const bool SourceCall =
          (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Bound.count({B.Id, O});
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        Note(Op.Inputs[I]);
        // CallInfo owns the settled source operands. Old native CALL inputs
        // can still mention a register discarded by source ABI recovery.
        if (!Op.Dead && !Op.PrologueSave && !transport(Op) &&
            (!SourceCall || I == 0))
          Read(Op.Inputs[I]);
        if (!Op.Dead && !Op.PrologueSave &&
            (Op.Opcode == NdOp::RETURN || Op.Opcode == NdOp::STORE ||
             Op.Opcode == NdOp::LOAD))
          Require(Op.Inputs[I]);
      }
      for (const auto &V : Op.IntrinsicOutputs)
        Note(V);
    }
  }
  for (const auto &C : F.CallClobbers) {
    Note(C.Value);
    if (C.PreservedPrefixSize)
      Read(C.PreservedInput);
  }
  for (const auto &V : F.Params)
    Note(V);
  for (const auto &V : F.Locals)
    Note(V);
  // Call recovery can leave dead PHI cycles after its last SSA update. A
  // PHI is not itself an observation: follow only those reaching a real use.
  while (!PendingPhis.empty() || !PendingCopies.empty()) {
    if (!PendingCopies.empty()) {
      const auto *Copy = PendingCopies.back();
      PendingCopies.pop_back();
      for (unsigned I = 0; I < Copy->NumInputs; ++I)
        Read(Copy->Inputs[I]);
      continue;
    }
    const auto *Phi = PendingPhis.back();
    PendingPhis.pop_back();
    for (const auto &[Pred, V] : Phi->Args)
      Read(V);
  }
  // Admit only a full-width source consumer through COPY/PHI transport.
  // A partial register update can retain old upper bits in an AND/OR chain
  // that source demanded-bits folding later removes. Such an intermediate
  // use alone must not add a native bridge to an otherwise ordinary call.
  while (!PendingRequired.empty()) {
    const auto V = PendingRequired.back();
    PendingRequired.pop_back();
    if (const auto At = Phis.find(key(V)); At != Phis.end())
      for (const auto &[Pred, Arg] : At->second->Args)
        Require(Arg);
    if (const auto At = Copies.find(key(V)); At != Copies.end())
      if (At->second->Opcode == NdOp::COPY && At->second->NumInputs == 1)
        Require(At->second->Inputs[0]);
  }
  if (!Profile || llvm::none_of(F.CallClobbers, [&](const auto &C) {
        return rcx(C) && Required.count(key(C.Value));
      }))
    return;
  std::set<uint32_t> Recovered;
  std::map<std::pair<int, int>, int> Positions;
  for (auto &B : F.Blocks) {
    if (!B.ExceptionalSuccs.empty() ||
        llvm::none_of(B.Ops, [&](const MedOp &Op) {
          const auto At = Clobbers.find(Op.CallSiteId);
          return At != Clobbers.end() &&
                 llvm::any_of(At->second, [&](const auto *C) {
                   return rcx(*C) && Required.count(key(C->Value));
                 });
        }))
      continue;
    std::vector<MedOp> Ops;
    for (size_t I = 0; I < B.Ops.size(); ++I) {
      const auto &Call = B.Ops[I];
      const auto At = Bound.find({B.Id, I});
      const auto Cs = Clobbers.find(Call.CallSiteId);
      const auto Admit = [&]() {
        if (Call.Opcode != NdOp::CALL || Call.Dead || !Call.CallSiteId ||
            !Call.NumInputs || !Call.Inputs[0].isConst() ||
            Call.Inputs[0].Size != 8 || Call.SourceCallHint ||
            Call.ScalarCallSignature || Call.IntegerCallSignature ||
            Call.IsTailCall || Call.DoesNotReturn || At == Bound.end() ||
            Cs == Clobbers.end() ||
            (Call.Output.Size &&
             (Call.Output.Size != 8 || Call.Output.Kind != MedVar::Reg ||
              Call.Output.RegOff != x86reg::RAX)))
          return false;
        const auto &C = *At->second;
        if (C.TargetAddr != Call.Inputs[0].ConstVal || C.IsIndirect ||
            C.SourceCallHint || C.ArgumentsInCalleeRegisterOrder ||
            C.VarArgFixedCount != -1 || C.Args.size() > 4 ||
            C.FloatingRegisterArgCount > 0 || !C.StackArgumentOffsets.empty() ||
            Emitted.count(C.TargetAddr))
          return false;
        const auto *Segment = Image.getSegmentFor(C.TargetAddr);
        if (!Segment || !Segment->isExecutable())
          return false;
        for (const auto &V : C.Args)
          if (V.Size != 8 ||
              (V.Kind != MedVar::Const && V.Kind != MedVar::Reg &&
               V.Kind != MedVar::Temp && V.Kind != MedVar::Param &&
               V.Kind != MedVar::Stack && V.Kind != MedVar::RetVal) ||
              (V.Kind == MedVar::Reg &&
               (V.TheArch != Arch::X64 || V.RegOff >= x86reg::XMM0)))
            return false;
        // The source call owns its recovered argument list. Unused register
        // slots are filled only with the independent no-observation proof,
        // never a prototype or a missing entry-read width.
        constexpr unsigned Families[] = {1, 2, 8, 9};
        const auto Proof = Unread.find(C.TargetAddr);
        for (unsigned J = C.Args.size(); J < std::size(Families); ++J)
          if (Proof == Unread.end() ||
              !(Proof->second & (GPRFamilyMask{1} << Families[J])))
            return false;
        bool LiveRCX = false;
        for (const auto *Clobber : Cs->second)
          if (Used.count(key(Clobber->Value))) {
            if (!rcx(*Clobber))
              return false;
            LiveRCX |= Required.count(key(Clobber->Value));
          }
        return LiveRCX;
      };
      if (!Admit()) {
        if (Call.Opcode == NdOp::CALL || Call.Opcode == NdOp::INDIR_CALL)
          Positions[{B.Id, I}] = Ops.size();
        Ops.push_back(Call);
        continue;
      }
      const auto Temp = [&]() {
        MedVar V;
        V.Kind = MedVar::Temp;
        V.Id = NextId++;
        V.Size = 8;
        V.TheArch = Arch::X64;
        return V;
      };
      MedOp Capture;
      Capture.Opcode = NdOp::INTRINSIC;
      Capture.Addr = Call.Addr;
      Capture.OriginSeq = Call.OriginSeq;
      Capture.Output = Call.Output.Size ? Call.Output : Temp();
      Capture.IntrinsicOutputs.push_back(Temp());
      Capture.NumInputs = X64CallRCXArguments + 1;
      Capture.Inputs.resize(Capture.NumInputs);
      Capture.Inputs[0] = MedVar::makeConst(uint64_t(Intrinsic::X64CallRCX), 2);
      Capture.Inputs[1] = Call.Inputs[0];
      // CALL's target was a control operand. It is now a value operand and
      // must retain a relocatable code identity, never the preferred image VA.
      Capture.Inputs[1].Provenance = ConstantAddressProvenance::CodeAddress;
      Capture.Inputs[1].AddressOwnerVA = Call.Inputs[0].ConstVal;
      for (unsigned J = 0; J < 4; ++J) {
        auto V =
            J < At->second->Args.size()
                ? At->second->Args[J]
                : MedVar::makeConst(0, 8, ConstantAddressProvenance::Scalar);
        Capture.Inputs[J + 2] = V;
      }
      Ops.push_back(Capture);
      for (const auto *Clobber : Cs->second)
        if (Used.count(key(Clobber->Value))) {
          MedOp Copy;
          Copy.Opcode = NdOp::COPY;
          Copy.Output = Clobber->Value;
          Copy.NumInputs = 1;
          Copy.Inputs[0] = Capture.IntrinsicOutputs.front();
          Copy.Addr = Call.Addr;
          Ops.push_back(Copy);
        }
      Positions[{B.Id, I}] = -1;
      Recovered.insert(Call.CallSiteId);
    }
    B.Ops = std::move(Ops);
  }
  llvm::erase_if(F.CallInfos, [&](MedCallInfo &C) {
    const auto At = Positions.find({C.BlockId, C.OpIdx});
    if (At == Positions.end())
      return false;
    C.OpIdx = At->second;
    return C.OpIdx < 0;
  });
  llvm::erase_if(F.CallClobbers, [&](const MedCallClobber &C) {
    return Recovered.count(C.CallSiteId);
  });
  llvm::erase_if(F.StructReturnCandidates,
                 [&](const auto &C) { return Recovered.count(C.CallSiteId); });
  for (auto Site : Recovered)
    F.CallUnreadRegisters.erase(Site);
  if (!Recovered.empty())
    for (auto &B : F.Blocks)
      llvm::erase_if(B.Phis, [&](const PhiNode &Phi) {
        return !LivePhis.count(key(Phi.Output));
      });
}
} // namespace neverd
