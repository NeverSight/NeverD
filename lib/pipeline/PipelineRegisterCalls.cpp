//===- PipelineRegisterCalls.cpp - Source register call expansion --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve dynamic extra registers from bounded, register-only query leaves.
///
//===----------------------------------------------------------------------===//

#include "PipelineRegisterCalls.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <optional>
#include <set>
#include <tuple>

namespace neverd {
namespace {
using Key = std::tuple<MedVar::VarKind, int, int>;
Key key(const MedVar &Value) { return {Value.Kind, Value.Id, Value.SSAVer}; }

std::optional<std::vector<MedOp>>
expand(llvm::ArrayRef<const MedCallClobber *> Clobbers, const MedOp &Call,
       const BinaryImage &Image, const std::vector<LowOp> &Body, int &NextId,
       size_t &Remaining) {
  if (Body.size() > Remaining || Call.SourceCallHint ||
      Call.ScalarCallSignature || Call.IntegerCallSignature ||
      Call.DoesNotReturn || Call.Output.Kind != MedVar::Reg ||
      Call.Output.RegOff != x86reg::RAX || Call.Output.Size != sizeof(uint64_t))
    return {};
  std::vector<MedVar> Outputs{Call.Output};
  for (const auto *Clobber : Clobbers) {
    const auto Family = gprFamilyOf(Image.Arch, Clobber->Value.RegOff);
    if (!Family || Clobber->PreservedPrefixSize ||
        Clobber->Value.RegOff % sizeof(uint64_t) + Clobber->Value.Size >
            sizeof(uint64_t))
      return {};
    Outputs.push_back(Clobber->Value);
  }
  if (Outputs.size() == 1)
    return {};
  Remaining -= Body.size();

  // Ask the ordinary converter to keep the required physical return words as
  // explicit reads. No new register/partial-write semantics live in this pass.
  // This private capture is never emitted as a callable function.
  LowFunc Capture;
  Capture.Entry = Call.Inputs[0].ConstVal;
  Capture.Name = "register_leaf_capture";
  Capture.Blocks.resize(1);
  auto &Block = Capture.Blocks.front();
  Block.Id = 0;
  Block.StartAddr = Capture.Entry;
  Block.EndAddr = Body.back().Addr + 1;
  Block.Ops = Body;
  LowOp &Return = Block.Ops.back();
  std::vector<uint64_t> Registers{x86reg::RAX};
  auto CaptureRegister = [&](uint64_t Register) {
    if (llvm::is_contained(Registers, Register))
      return true;
    if (Return.NumInputs == std::size(Return.Inputs))
      return false;
    Registers.push_back(Register);
    Return.addInput(NdVar::reg(Register, sizeof(uint64_t)));
    return true;
  };
  for (const auto &Output : Outputs)
    if (!CaptureRegister(Output.RegOff & ~uint64_t(7)))
      return {};
  // Source ABI preservation is a value claim, not merely an absence of
  // caller clobber records. A query that fails to restore RBX is not an
  // ordinary-call leaf even when this caller has no explicit RBX read.
  const auto &Target = getTargetRegInfo(Image.Arch);
  std::set<uint64_t> Preserved;
  for (const auto &Op : Body)
    if (Op.Output.isReg())
      if (const auto Family = gprFamilyOf(Image.Arch, Op.Output.Offset)) {
        const uint64_t Register = *Family * sizeof(uint64_t);
        if (Target.isCallPreserved(Register, sizeof(uint64_t),
                                   Image.abiFormat())) {
          if (!CaptureRegister(Register))
            return {};
          Preserved.insert(Register);
        }
      }
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  MedFunc Captured = Converter.convert(Capture, Image.Arch, Image.abiFormat());
  if (Captured.SkippedSSA || Captured.Blocks.size() != 1 ||
      !Captured.Blocks.front().Phis.empty())
    return {};
  const auto &Ops = Captured.Blocks.front().Ops;
  if (Ops.empty() || Ops.back().Opcode != NdOp::RETURN ||
      Ops.back().NumInputs != Registers.size())
    return {};

  // A live input that still needs an incoming physical register has no
  // self-contained source projection. Keep the original call in that case.
  std::map<Key, size_t> Definitions;
  for (size_t I = 0; I + 1 < Ops.size(); ++I) {
    const auto &Op = Ops[I];
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
        Op.Inputs[0] == Op.Output)
      continue;
    if (Op.Output.Size && Op.Output.Id >= 0)
      Definitions.emplace(key(Op.Output), I);
    for (const auto &Value : Op.IntrinsicOutputs)
      Definitions.emplace(key(Value), I);
  }
  std::vector<MedVar> Pending;
  for (unsigned I = 0; I < Ops.back().NumInputs; ++I) {
    const auto &Value = Ops.back().Inputs[I];
    if (Preserved.count(Registers[I])) {
      if (!isEntryLiveInValue(Captured, Value, Registers[I]))
        return {};
    } else
      Pending.push_back(Value);
  }
  std::set<size_t> Needed;
  auto Include = [&](size_t I) {
    if (Needed.insert(I).second)
      for (unsigned K = 0; K < Ops[I].NumInputs; ++K)
        Pending.push_back(Ops[I].Inputs[K]);
  };
  for (size_t I = 0; I + 1 < Ops.size(); ++I)
    if (Ops[I].Opcode == NdOp::INTRINSIC)
      Include(I); // Preserve every query once, in its original order.
  while (!Pending.empty()) {
    const auto Value = Pending.back();
    Pending.pop_back();
    if (Value.isConst())
      continue;
    const auto Found = Definitions.find(key(Value));
    if (Found == Definitions.end())
      return {};
    Include(Found->second);
  }

  std::map<Key, MedVar> Renamed;
  auto NewValue = [&](const MedVar &Value) {
    MedVar Fresh;
    Fresh.Kind = MedVar::Temp;
    Fresh.Id = NextId++;
    Fresh.Size = Value.Size;
    Fresh.TheArch = Image.Arch;
    Renamed.emplace(key(Value), Fresh);
    return Fresh;
  };
  auto Read = [&](const MedVar &Value) {
    return Value.isConst() ? Value : Renamed.at(key(Value));
  };
  std::vector<MedOp> Expanded;
  for (size_t I : Needed) {
    MedOp Op = Ops[I];
    // The cloned computation belongs to this call occurrence, not to an
    // original instruction in the caller. Never borrow an address witness.
    Op.Addr = Call.Addr;
    Op.OriginSeq = -1;
    for (unsigned K = 0; K < Op.NumInputs; ++K)
      Op.Inputs[K] = Read(Op.Inputs[K]);
    if (Op.Output.Size)
      Op.Output = NewValue(Op.Output);
    for (auto &Value : Op.IntrinsicOutputs)
      Value = NewValue(Value);
    Expanded.push_back(std::move(Op));
  }
  for (const auto &Output : Outputs) {
    const uint64_t Register = Output.RegOff & ~uint64_t(7);
    const size_t Position =
        std::find(Registers.begin(), Registers.end(), Register) -
        Registers.begin();
    MedOp Copy;
    Copy.Addr = Call.Addr;
    Copy.Output = Output;
    Copy.Opcode = Output.Size == sizeof(uint64_t) ? NdOp::COPY : NdOp::SUBBYTES;
    Copy.addInput(Read(Ops.back().Inputs[Position]));
    if (Copy.Opcode == NdOp::SUBBYTES)
      Copy.addInput(MedVar::makeConst(Output.RegOff - Register, 4,
                                      ConstantAddressProvenance::Scalar));
    Expanded.push_back(std::move(Copy));
  }
  return Expanded;
}
} // namespace

void expandSourceRegisterCalls(
    MedFunc &Function, const BinaryImage &Image,
    const std::map<va_t, std::shared_ptr<const std::vector<LowOp>>> &Leaves) {
  if (Image.Arch != Arch::X64 || Leaves.empty() ||
      Function.CallClobbers.empty() || Function.SkippedSSA ||
      Function.AnalystStackFrame ||
      (Function.ExceptionMetadata &&
       Function.ExceptionMetadata->Personality != ExceptionPersonality::None))
    return;
  bool HasCandidate = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      HasCandidate |= Op.Opcode == NdOp::CALL && Op.NumInputs &&
                      Op.Inputs[0].isConst() &&
                      Leaves.count(Op.Inputs[0].ConstVal);
  if (!HasCandidate)
    return;
  int NextId = 0;
  auto Note = [&](const MedVar &Value) {
    NextId = std::max(NextId, Value.Id + 1);
  };
  for (const auto &Block : Function.Blocks) {
    if (!Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty())
      return;
    for (const auto &Phi : Block.Phis) {
      Note(Phi.Output);
      for (const auto &Arg : Phi.Args)
        Note(Arg.second);
    }
    for (const auto &Op : Block.Ops) {
      Note(Op.Output);
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        Note(Op.Inputs[I]);
      for (const auto &Value : Op.IntrinsicOutputs)
        Note(Value);
    }
  }
  for (const auto &Value : Function.Params)
    Note(Value);
  for (const auto &Value : Function.Locals)
    Note(Value);
  std::map<uint32_t, std::vector<const MedCallClobber *>> Clobbers;
  for (const auto &Clobber : Function.CallClobbers) {
    Note(Clobber.Value);
    Clobbers[Clobber.CallSiteId].push_back(&Clobber);
  }
  size_t Remaining = limits::ir::kSourceRegisterCallOps;
  std::set<uint32_t> Removed;
  std::map<std::pair<int, int>, int> Positions;
  for (auto &Block : Function.Blocks) {
    std::vector<MedOp> NewOps;
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const auto &Op = Block.Ops[I];
      if (Op.Opcode == NdOp::CALL && Op.CallSiteId && Op.NumInputs &&
          Op.Inputs[0].isConst()) {
        const auto Body = Leaves.find(Op.Inputs[0].ConstVal);
        if (Body != Leaves.end())
          if (auto Expanded = expand(Clobbers[Op.CallSiteId], Op, Image,
                                     *Body->second, NextId, Remaining)) {
            NewOps.insert(NewOps.end(), Expanded->begin(), Expanded->end());
            Removed.insert(Op.CallSiteId);
            continue;
          }
      }
      Positions[{Block.Id, I}] = NewOps.size();
      NewOps.push_back(Op);
    }
    Block.Ops = std::move(NewOps);
  }
  if (Removed.empty())
    return;
  llvm::erase_if(Function.CallClobbers, [&](const auto &Clobber) {
    return Removed.count(Clobber.CallSiteId);
  });
  llvm::erase_if(Function.StructReturnCandidates, [&](const auto &Candidate) {
    return Removed.count(Candidate.CallSiteId);
  });
  for (uint32_t Site : Removed)
    Function.CallUnreadRegisters.erase(Site);
  llvm::erase_if(Function.CallInfos, [&](auto &Call) {
    const auto Found = Positions.find({Call.BlockId, Call.OpIdx});
    if (Found == Positions.end())
      return true;
    Call.OpIdx = Found->second;
    return false;
  });
}
} // namespace neverd
