//===- SourceStackFrame.cpp - Entry-relative stack observations ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded must-analysis for stack observations and private source correction.
//===----------------------------------------------------------------------===//

#include "neverd/ir/SourceStackFrame.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"
#include "neverd/ir/low/LowIntrinsicEffects.h"

#include "llvm/Support/MathExtras.h"

#include <deque>
#include <tuple>

namespace neverd {
namespace {
using Key = std::pair<VnodeSpace, uint64_t>;
using State = std::map<Key, int64_t>;
bool terminal(NdOp Op) {
  return Op == NdOp::BRANCH || Op == NdOp::COND_BR || Op == NdOp::INDIR_BR ||
         Op == NdOp::RETURN;
}
std::optional<int64_t> offset(const State &S, NdVar V, uint16_t Width) {
  if (V.Size != Width || (!V.isReg() && !V.isTemp()))
    return {};
  const auto I = S.find({V.Space, V.Offset});
  return I == S.end() ? std::nullopt : std::optional(I->second);
}
std::optional<int64_t> constant(NdVar V, uint16_t Width) {
  if (!V.isConst() || V.Size != Width)
    return {};
  return Width == 8 ? static_cast<int64_t>(V.Offset)
                    : static_cast<int32_t>(V.Offset);
}
bool bounded(int64_t N) {
  return N >= -::neverd::limits::ir::kSourceStackFrameMaxFrame &&
         N <= ::neverd::limits::ir::kSourceStackFrameMaxFrame;
}
void transfer(State &S, const LowOp &Op, uint16_t Width) {
  std::optional<int64_t> Result;
  if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
    Result = offset(S, Op.Inputs[0], Width);
  if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
      Op.NumInputs == 2) {
    auto Base = offset(S, Op.Inputs[0], Width);
    auto Delta = constant(Op.Inputs[1], Width);
    if (Op.Opcode == NdOp::INT_ADD && !Base) {
      Base = offset(S, Op.Inputs[1], Width);
      Delta = constant(Op.Inputs[0], Width);
    }
    if (Base && Delta && bounded(*Delta)) {
      const int64_t N = *Base + (Op.Opcode == NdOp::INT_SUB ? -*Delta : *Delta);
      if (bounded(N))
        Result = N;
    }
  }
  if (Op.Output.isReg() || Op.Output.isTemp()) {
    std::erase_if(S, [&](const auto &Item) {
      return Item.first.first == Op.Output.Space &&
             Item.first.second < Op.Output.Offset + Op.Output.Size &&
             Op.Output.Offset < Item.first.second + Width;
    });
    if (Result && Op.Output.Size == Width)
      S[{Op.Output.Space, Op.Output.Offset}] = *Result;
  }
}
void expire(State &S, const LowFunc &F, uint16_t Width) {
  std::erase_if(S, [&](const auto &Item) {
    if (Item.first.first != VnodeSpace::TEMP)
      return false;
    if (Item.first.second > UINT64_MAX - (Width - 1))
      return true;
    for (unsigned I = 0; I < Width; ++I)
      if (!F.isFunctionTemporaryByte(Item.first.second + I))
        return true;
    return false;
  });
}
bool meet(State &Left, const State &Right) {
  auto Before = Left;
  std::erase_if(Left, [&](const auto &Item) {
    const auto It = Right.find(Item.first);
    return It == Right.end() || It->second != Item.second;
  });
  return Before != Left;
}
} // namespace

StackFrameAnalysis analyzeSourceStackFrame(const LowFunc &F, Arch Architecture,
                                           const SourceStackFrame &Policy) {
  StackFrameAnalysis Out;
  const auto &TRI = getTargetRegInfo(Architecture);
  const auto Width = TRI.PointerSize;
  if (F.DecodedInstructionCount && !F.hasCompleteInstructionLift()) {
    Out.Error = "Stack inspection requires a complete instruction lift";
    return Out;
  }
  if (Width != 8 ||
      (Architecture != Arch::X64 && Architecture != Arch::AArch64)) {
    Out.Error = "Exact stack correction currently requires x64 or AArch64";
    return Out;
  }
  if (F.Blocks.size() >
          ::neverd::limits::ir::
              kSourceStackFrameAnalyzeSourceStackFrameFBlocksSizeLimit ||
      Policy.Points.size() >
          ::neverd::limits::ir::
              kSourceStackFrameAnalyzeSourceStackFramePolicyPointsSizeLimit ||
      Policy.Variables.size() >
          ::neverd::limits::ir::
              kSourceStackFrameAnalyzeSourceStackFramePolicyVariablesSizeLimit) {
    Out.Error = "Stack-frame analysis budget exceeded";
    return Out;
  }
  std::map<int, const LowBlock *> Blocks;
  std::map<int, State> Inputs;
  std::deque<int> Queue;
  std::set<int> Pending;
  int Entry = -1;
  size_t OperationCount = 0;
  for (const auto &B : F.Blocks) {
    OperationCount += B.Ops.size();
    if (OperationCount >
        ::neverd::limits::ir::
            kSourceStackFrameAnalyzeSourceStackFrameOperationCountLimit) {
      Out.Error = "Stack-frame operation budget exceeded";
      return Out;
    }
    Blocks[B.Id] = &B;
    if (B.StartAddr == F.Entry)
      Entry = B.Id;
    for (size_t I = 0; I < B.Ops.size(); ++I) {
      auto &Row = Out.Instructions[B.Ops[I].Addr];
      Row.Correctable = !terminal(B.Ops[I].Opcode);
    }
  }
  if (Entry < 0) {
    Out.Error = "No ordinary entry block";
    return Out;
  }
  for (const auto &[PC, After] : Policy.Points)
    if (!bounded(After) || !Out.Instructions.count(PC) ||
        !Out.Instructions.at(PC).Correctable) {
      Out.Error = "Select a retained non-branch instruction and a bounded "
                  "entry-SP offset";
      return Out;
    }
  auto Enqueue = [&](int Id) {
    if (Pending.insert(Id).second)
      Queue.push_back(Id);
  };
  Inputs[Entry][{VnodeSpace::REG, TRI.StackPointer}] = 0;
  Enqueue(Entry);
  for (const auto &B : F.Blocks)
    if (((F.ModuleAnalysisRoots.count(B.StartAddr) ||
          F.OrdinaryModuleAnalysisRoots.count(B.StartAddr)) &&
         B.Id != Entry) ||
        !B.ExceptionalPreds.empty()) {
      Inputs[B.Id] = {};
      Enqueue(B.Id);
    }
  size_t Work = 0;
  auto Run = [&](const LowBlock &B, State S, bool Record) {
    for (size_t I = 0; I < B.Ops.size();) {
      const auto PC = B.Ops[I].Addr;
      expire(S, F, Width);
      if (Record)
        Out.Instructions[PC].Before =
            offset(S, NdVar::reg(TRI.StackPointer, Width), Width);
      do {
        const auto &Op = B.Ops[I];
        if (++Work >
            ::neverd::limits::ir::
                kSourceStackFrameAnalyzeSourceStackFrameRunRunWorkLimit) {
          Out.Error = "Stack-frame analysis work budget exceeded";
          return S;
        }
        if (Record && (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
            Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
            Op.NumInputs >= (Op.Opcode == NdOp::LOAD ? 1 : 2)) {
          if (auto At = offset(S, Op.Inputs[0], Width))
            Out.Accesses.push_back(
                {PC, Op.Seq, *At,
                 Op.Opcode == NdOp::LOAD ? Op.Output.Size : Op.Inputs[1].Size,
                 Op.Opcode == NdOp::STORE});
        }
        if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
            Op.Inputs[0].isConst() &&
            isAArch64AddressMemoryIntrinsic(
                static_cast<Intrinsic>(Op.Inputs[0].Offset))) {
          if (Architecture != Arch::AArch64 ||
              !lowAArch64AddressMemoryShapeIsValid(Op)) {
            Out.Error = kAddressMemoryMalformed;
            return S;
          }
          const auto Instruction = *aarch64AddressMemoryInstruction(
              Op.Inputs[kAddressMemoryWordOperand + 1].Offset);
          const auto &Index = Op.Inputs[kAddressMemoryIndexOperand + 1];
          const auto Base =
              offset(S, Op.Inputs[kAddressMemoryBaseOperand + 1], Width);
          // Keep the original instruction opaque to generic memory proofs.
          // A constant index describes its nominal footprint, including SP
          // origin, but proves no checked address, translation or tagging.
          int64_t At, End;
          if (Record && Base && Index.isConst() &&
              !llvm::AddOverflow(
                  *Base,
                  static_cast<int64_t>(Instruction.addressOffset(Index.Offset)),
                  At) &&
              !llvm::AddOverflow(At, int64_t(Instruction.accessBytes()), End) &&
              bounded(At) && bounded(End))
            Out.Accesses.push_back({PC, Op.Seq, At,
                                    uint16_t(Instruction.accessBytes()),
                                    !Instruction.isLoad(), Instruction.Word});
        }
        transfer(S, Op, Width);
        // Calls may overwrite every alias except SP. This is intentionally
        // narrower than the native ABI preservation proof.
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
          std::erase_if(S, [&](const auto &Item) {
            return Item.first != Key{VnodeSpace::REG, TRI.StackPointer};
          });
        ++I;
      } while (I < B.Ops.size() && B.Ops[I].Addr == PC);
      if (const auto P = Policy.Points.find(PC); P != Policy.Points.end())
        S[{VnodeSpace::REG, TRI.StackPointer}] = P->second;
      if (Record)
        Out.Instructions[PC].After =
            offset(S, NdVar::reg(TRI.StackPointer, Width), Width);
    }
    expire(S, F, Width);
    return S;
  };
  while (!Queue.empty() && Out.Error.empty()) {
    const int Id = Queue.front();
    Queue.pop_front();
    Pending.erase(Id);
    const auto &B = *Blocks.at(Id);
    const auto S = Run(B, Inputs.at(Id), false);
    for (int Next : B.Succs) {
      if (!Blocks.count(Next)) {
        Out.Error = "Incomplete stack-frame CFG";
        break;
      }
      const auto [It, New] = Inputs.try_emplace(Next, S);
      if (New || meet(It->second, S))
        Enqueue(Next);
    }
  }
  if (Out.Error.empty())
    for (const auto &[Id, S] : Inputs)
      Run(*Blocks.at(Id), S, true);
  if (!Out.Error.empty()) {
    Out.Accesses.clear();
    Out.Instructions.clear();
  }
  return Out;
}

std::optional<LowFunc> projectSourceStackFrame(const LowFunc &F, Arch A,
                                               const SourceStackFrame &Policy,
                                               std::string &Error) {
  const auto Flow = analyzeSourceStackFrame(F, A, Policy);
  if (!Flow.Error.empty()) {
    Error = Flow.Error;
    return {};
  }
  LowFunc Result = F;
  if (Policy.Points.empty())
    return Result;
  const auto Entry =
      std::find_if(F.Blocks.begin(), F.Blocks.end(),
                   [&](const LowBlock &B) { return B.StartAddr == F.Entry; });
  if (std::any_of(F.Blocks.begin(), F.Blocks.end(),
                  [&](const LowBlock &B) { return B.hasSucc(Entry->Id); })) {
    // The entry-SP carrier below must execute exactly once. A backedge would
    // recapture the current SP and silently change the correction's basis.
    Error = "SP correction requires an entry block without incoming edges";
    return {};
  }
  if (std::any_of(F.Blocks.begin(), F.Blocks.end(), [&](const LowBlock &B) {
        return !B.ExceptionalPreds.empty() || !B.ExceptionalSuccs.empty() ||
               (B.StartAddr != F.Entry &&
                (F.ModuleAnalysisRoots.count(B.StartAddr) ||
                 F.OrdinaryModuleAnalysisRoots.count(B.StartAddr)));
      })) {
    Error = "SP correction requires a single ordinary entry without "
            "exceptional roots";
    return {};
  }
  const auto &TRI = getTargetRegInfo(A);
  uint64_t Temporary = TmpBase;
  for (const auto &B : F.Blocks)
    for (const auto &Op : B.Ops) {
      if (Op.Output.isTemp())
        Temporary = std::max(Temporary, Op.Output.Offset + Op.Output.Size);
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isTemp())
          Temporary =
              std::max(Temporary, Op.Inputs[I].Offset + Op.Inputs[I].Size);
    }
  for (const auto &T : F.FunctionTemporaries)
    Temporary = std::max(Temporary, T.Offset + T.Bytes);
  const auto Carrier = NdVar::tmp(Temporary + 16, TRI.PointerSize);
  const auto SP = NdVar::reg(TRI.StackPointer, TRI.PointerSize);
  Result.FunctionTemporaries.push_back({Carrier.Offset, Carrier.Size});
  for (auto &B : Result.Blocks) {
    std::vector<LowOp> Ops;
    std::vector<size_t> Starts(B.Ops.size() + 1);
    if (B.StartAddr == F.Entry) {
      LowOp Capture;
      Capture.Opcode = NdOp::COPY;
      Capture.Addr = F.Entry;
      Capture.Seq = -2;
      Capture.Output = Carrier;
      Capture.addInput(SP);
      Ops.push_back(Capture);
    }
    for (size_t I = 0; I < B.Ops.size(); ++I) {
      Starts[I] = Ops.size();
      Ops.push_back(B.Ops[I]);
      const auto PC = B.Ops[I].Addr;
      if (I + 1 < B.Ops.size() && B.Ops[I + 1].Addr == PC)
        continue;
      if (const auto P = Policy.Points.find(PC); P != Policy.Points.end()) {
        LowOp Reset;
        Reset.Opcode = NdOp::INT_ADD;
        Reset.Addr = PC;
        Reset.Seq = B.Ops[I].Seq + 1;
        Reset.Output = SP;
        Reset.addInput(Carrier);
        Reset.addInput(
            NdVar::scalar(static_cast<uint64_t>(P->second), TRI.PointerSize));
        Ops.push_back(Reset);
      }
    }
    Starts[B.Ops.size()] = Ops.size();
    for (auto &Boundary : B.InstructionBoundaries) {
      if (Boundary.FirstOp > B.Ops.size() ||
          Boundary.OpCount > B.Ops.size() - Boundary.FirstOp) {
        Error = "Invalid original instruction boundary";
        return {};
      }
      const size_t First = Boundary.FirstOp;
      const size_t End = First + Boundary.OpCount;
      const size_t NewFirst =
          B.StartAddr == F.Entry && First == 0 ? 0 : Starts[First];
      Boundary.FirstOp = NewFirst;
      Boundary.OpCount = Starts[End] - NewFirst;
    }
    B.Ops = std::move(Ops);
  }
  return Result;
}
} // namespace neverd
