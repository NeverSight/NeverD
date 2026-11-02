//===- CFGBuilderScalarBranch.cpp - Exact scalar control transfers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recovers constant and finite scalar targets before tail-call conversion.
/// Unknown live-ins, mutable loads and partial writes never invent facts.

#include "LowScalarExpression.h"
#include "LowScalarTargets.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {
namespace {
// Registered C++ resumptions may add roots after scalar recovery. Neither
// numeric proof can certify a finite cover before that closure is complete.
bool hasDeferredScalarRoots(const LowFunc &Func, Arch Architecture) {
  return Architecture == Arch::X86 && Func.ExceptionMetadata &&
         Func.ExceptionMetadata->Registration && Func.ExceptionMetadata->Cxx;
}
} // namespace

bool CFGBuilder::recoverComputedScalarBranches(const BinaryImage &Img,
                                               Decoder &Dec, LowFunc &Func,
                                               size_t &RemainingWork) {
  if ((Img.Arch != Arch::X86 && Img.Arch != Arch::X64) ||
      hasDeferredScalarRoots(Func, Img.Arch))
    return false;
  if (std::none_of(Insns.begin(), Insns.end(), [](const auto &Entry) {
        const auto &R = Entry.second;
        return R.ComputedScalarTargets ||
               (R.IsBranch && R.IsIndirect && !R.IsCall && !R.IsCond &&
                !R.IsRet && !R.RejectedComputedTargets &&
                !R.RejectedScalarBranch && !R.ScalarBranchInput &&
                R.JumpTableTargets.empty());
      }))
    return false;
  const auto Targets = finiteScalarBranchTargets(Func, RemainingWork);
  bool Changed = false;
  std::vector<va_t> Explore;
  for (auto &[Address, Rec] : Insns) {
    if (!Rec.IsBranch || !Rec.IsIndirect || Rec.IsCall || Rec.IsCond ||
        Rec.IsRet || Rec.RejectedComputedTargets || Rec.RejectedScalarBranch ||
        Rec.ScalarBranchInput ||
        (!Rec.ComputedScalarTargets && !Rec.JumpTableTargets.empty()))
      continue;
    std::vector<va_t> Values;
    if (Targets)
      if (const auto Found = Targets->find(Address); Found != Targets->end())
        Values = Found->second;
    // Singleton transfers belong to exact-branch recovery. This owner
    // publishes switches with a runtime selector, not constant selectors
    // whose code identity needs a different downstream binding.
    const bool Valid =
        Values.size() > 1 &&
        std::all_of(Values.begin(), Values.end(), [&](va_t T) {
          return T != InvalidVA && !isTailCallTarget(T) &&
                 Img.hasExecutableCodeOwnerRange(T, getInsnAlignment()) &&
                 Img.readVA(T, getInsnAlignment());
        });
    if (!Valid) {
      if (Rec.ComputedScalarTargets) {
        Rec.JumpTableTargets.clear();
        Rec.ComputedScalarTargets = false;
        Rec.RejectedComputedTargets = true;
        Changed = true;
      }
      continue;
    }
    if (Rec.ComputedScalarTargets && Rec.JumpTableTargets == Values)
      continue;
    Rec.ComputedScalarTargets = true;
    Rec.JumpTableTargets = Values;
    ResolvedTableInfo.erase(Address);
    for (va_t Target : Values) {
      BlockStarts.insert(Target);
      Explore.push_back(Target);
    }
    Changed = true;
  }
  if (Changed) {
    for (va_t Target : Explore)
      if (!ExploredAddrs.count(Target))
        explore(Img, Dec, Target);
    exploreAddressTakenRoots(Img, Dec);
    splitBlocks();
    rebuildBlocks(Func);
  }
  return Changed;
}

bool CFGBuilder::recoverScalarConstantBranches(const BinaryImage &Img,
                                               Decoder &Dec, LowFunc &Func,
                                               size_t &RemainingWork,
                                               ScalarImageFacts &ImageScalars) {
  // ARM32 state-changing branches require a separate instruction-mode proof.
  if (Img.Arch != Arch::X86 && Img.Arch != Arch::X64 &&
      Img.Arch != Arch::AArch64)
    return false;
  if (std::none_of(Insns.begin(), Insns.end(), [](const auto &Entry) {
        const auto &Rec = Entry.second;
        return Rec.ScalarBranchInput ||
               (Rec.IsBranch && Rec.IsIndirect && !Rec.IsCond && !Rec.IsCall &&
                !Rec.IsRet && !Rec.RejectedScalarBranch &&
                !Rec.RejectedComputedTargets && Rec.JumpTableTargets.empty());
      }))
    return false;

  bool Changed = false;
  const bool AllowFinite = !hasDeferredScalarRoots(Func, Img.Arch);
  // All image metadata is fixed throughout discovery. Cache failures as well
  // as successes so repeated instructions/rounds do not rescan the image.
  auto Spend = [&](size_t Work) {
    if (Work > RemainingWork) {
      RemainingWork = 0;
      return false;
    }
    RemainingWork -= Work;
    return true;
  };
  // Numeric and symbolic walks borrow only the same authenticated image
  // bytes. The cache is scoped to this unchanged-image CFG construction.
  auto ReadImage = [&](const LowOp &Op,
                       uint64_t Address) -> std::optional<uint64_t> {
    // The image's own headers describe the image that runs, not this file:
    // a read of them stays a read (BinaryImage::imageBaseOffset).
    if (Op.Opcode != NdOp::LOAD || Op.NumInputs != 1 || !Op.Output.Size ||
        Op.Output.Size > sizeof(uint64_t) ||
        Op.Inputs[0].Size < Img.getPointerSize() ||
        Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Img.imageBaseOffset(Address))
      return {};
    const auto Key = std::make_pair(Address, Op.Output.Size);
    auto [It, Inserted] = ImageScalars.try_emplace(Key);
    if (Inserted)
      It->second = readImmutableImageScalar(Img, Address, Op.Output.Size,
                                            &RemainingWork);
    return It->second;
  };
  auto Restore = [&](InsnRecord &Rec) {
    if (!Rec.ScalarBranchInput)
      return false;
    if (Rec.IsIndirect) {
      Rec.JumpTableTargets.clear();
      Rec.ScalarBranchInput.reset();
      Rec.RejectedScalarBranch = true;
      return true;
    }
    for (LowOp &Op : Rec.Ops)
      if (Op.Opcode == NdOp::BRANCH && Op.NumInputs == 1) {
        Op.Opcode = NdOp::INDIR_BR;
        Op.Inputs[0] = *Rec.ScalarBranchInput;
        Rec.IsIndirect = true;
        Rec.BranchTarget = InvalidVA;
        Rec.Immediate.reset();
        Rec.ScalarBranchInput.reset();
        Rec.RejectedScalarBranch = true;
        return true;
      }
    return false;
  };

  using ScalarTargets = std::optional<std::vector<uint64_t>>;
  for (;;) {
    std::map<va_t, ScalarTargets> Targets;
    bool Complete = true;
    for (const LowBlock &Block : Func.Blocks) {
      if (!Spend(1)) {
        Complete = false;
        break;
      }
      if (Block.InstructionBoundaries.empty())
        continue;
      const auto Last = Insns.find(Block.InstructionBoundaries.back().Address);
      if (Last == Insns.end()) {
        Complete = false;
        break;
      }
      const auto &Terminator = Last->second;
      if (!Terminator.ScalarBranchInput &&
          (!Terminator.IsBranch || !Terminator.IsIndirect ||
           Terminator.IsCond || Terminator.IsCall || Terminator.IsRet ||
           Terminator.RejectedScalarBranch ||
           Terminator.RejectedComputedTargets ||
           !Terminator.JumpTableTargets.empty()))
        continue;
      // Direct edges transport register state without a new ABI invocation.
      // Recovered edges cannot justify their own proof. Independent roots and
      // exceptional entries retain arbitrary incoming state.
      struct ScalarPath {
        llvm::SmallVector<const LowBlock *, 8> Blocks;
        llvm::SmallDenseSet<int, 8> Seen;
        llvm::SmallPtrSet<const InsnRecord *, 8> DirectEdges;
      };
      const auto Independent = [&](const LowBlock &Current) {
        return Current.StartAddr == Func.Entry ||
               !Current.ExceptionalPreds.empty() ||
               Func.ModuleAnalysisRoots.count(Current.StartAddr) ||
               Func.OrdinaryModuleAnalysisRoots.count(Current.StartAddr);
      };
      const auto AppendPredecessor = [&](ScalarPath &Path, int P) {
        if (!Spend(1)) {
          Complete = false;
          return false;
        }
        const LowBlock &Current = *Path.Blocks.back();
        if (P < 0 || static_cast<size_t>(P) >= Func.Blocks.size() ||
            Path.Seen.contains(P))
          return false;
        const LowBlock &Previous = Func.Blocks[P];
        if (Previous.InstructionBoundaries.empty())
          return false;
        const auto Edge =
            Insns.find(Previous.InstructionBoundaries.back().Address);
        if (Edge == Insns.end())
          return false;
        const InsnRecord &Transfer = Edge->second;
        if (Transfer.IsCall || Transfer.IsRet || Transfer.IsIndirect ||
            Transfer.ScalarBranchInput || Transfer.IsOpaqueTerminator ||
            Transfer.IsInstructionGuard ||
            (Transfer.IsBranch && Transfer.BranchTarget != Current.StartAddr &&
             (!Transfer.IsCond || Previous.EndAddr != Current.StartAddr)) ||
            (!Transfer.IsBranch && Previous.EndAddr != Current.StartAddr))
          return false;
        Path.Seen.insert(P);
        Path.DirectEdges.insert(&Transfer);
        Path.Blocks.push_back(&Previous);
        return true;
      };
      const auto ExtendPath = [&](ScalarPath &Path) {
        while (Complete) {
          const LowBlock &Current = *Path.Blocks.back();
          if (!Spend(1)) {
            Complete = false;
            break;
          }
          if (Independent(Current) || Current.Preds.size() != 1 ||
              !AppendPredecessor(Path, Current.Preds.front()))
            break;
        }
      };
      const auto EvaluatePath = [&](const ScalarPath &Path) -> ScalarTargets {
        llvm::SmallVector<const InsnRecord *, 16> Instructions;
        for (auto It = Path.Blocks.rbegin(); It != Path.Blocks.rend(); ++It) {
          for (const LowInstructionBoundary &Boundary :
               (*It)->InstructionBoundaries) {
            if (!Spend(1)) {
              Complete = false;
              break;
            }
            const auto At = Insns.find(Boundary.Address);
            if (At == Insns.end()) {
              Complete = false;
              break;
            }
            Instructions.push_back(&At->second);
          }
          if (!Complete)
            break;
        }
        if (!Complete)
          return {};
        std::optional<uint64_t> Target;
        ScalarTargets FiniteTargets;
        // Byte facts preserve architectural subregister overlap exactly. Temps
        // have identities, not byte offsets, and live for one instruction only.
        std::map<uint64_t, uint8_t> Registers;
        std::map<uint64_t, std::pair<uint16_t, uint64_t>> Temps;
        auto ValueOf = [&](const NdVar &Value) -> std::optional<uint64_t> {
          if (!Spend(1 + Value.Size) || !Value.Size || Value.Size > 8)
            return std::nullopt;
          if (Value.isConst())
            return Value.Offset & (UINT64_MAX >> ((8 - Value.Size) * 8));
          if (Value.isTemp()) {
            const auto At = Temps.find(Value.Offset);
            return At != Temps.end() && At->second.first == Value.Size
                       ? std::optional<uint64_t>{At->second.second}
                       : std::nullopt;
          }
          if (!Value.isReg() || Value.Offset > UINT64_MAX - Value.Size)
            return std::nullopt;
          uint64_t Result = 0;
          for (uint16_t I = 0; I < Value.Size; ++I) {
            const auto At = Registers.find(Value.Offset + I);
            if (At == Registers.end())
              return std::nullopt;
            Result |= uint64_t{At->second} << (I * 8);
          }
          return Result;
        };
        for (const InsnRecord *Instruction : Instructions) {
          if (!Spend(1 + Temps.size())) {
            Complete = false;
            break;
          }
          Temps.clear();
          const InsnRecord &Rec = *Instruction;
          for (const LowOp &Op : Rec.Ops) {
            if (!Spend(1)) {
              Complete = false;
              break;
            }
            if (Rec.IsBranch && !Rec.IsCond && !Rec.IsCall && !Rec.IsRet &&
                !Rec.RejectedScalarBranch &&
                (Rec.JumpTableTargets.empty() || Rec.ScalarBranchInput) &&
                Op.NumInputs == 1 &&
                (Op.Opcode == NdOp::INDIR_BR ||
                 (Op.Opcode == NdOp::BRANCH && Rec.ScalarBranchInput))) {
              Target = ValueOf(Rec.ScalarBranchInput.value_or(Op.Inputs[0]));
            }
            if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
              if (!Spend(Registers.size() + Temps.size())) {
                Complete = false;
                break;
              }
              Registers.clear();
              Temps.clear();
            }
            const NdVar &Output = Op.Output;
            if (!Output.isReg() && !Output.isTemp())
              continue;
            auto Result = lowScalarConstantResult(Op, ValueOf);
            if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1)
              if (auto Address = ValueOf(Op.Inputs[0]))
                Result = ReadImage(Op, *Address);
            if (!Spend(1 + Output.Size)) {
              Complete = false;
              break;
            }
            if (Output.isTemp()) {
              if (Result)
                Temps[Output.Offset] = {Output.Size, *Result};
              else
                Temps.erase(Output.Offset);
            } else if (Output.Offset <= UINT64_MAX - Output.Size) {
              for (uint16_t I = 0; I < Output.Size; ++I)
                if (Result)
                  Registers[Output.Offset + I] = uint8_t(*Result >> (I * 8));
                else
                  Registers.erase(Output.Offset + I);
            } else
              Registers.clear();
          }
          if (!Complete)
            break;
          if (Rec.IsOpaqueTerminator || Rec.IsInstructionGuard) {
            if (!Spend(Registers.size() + Temps.size())) {
              Complete = false;
              break;
            }
            Registers.clear();
            Temps.clear();
          }
        }
        if (!Complete || RemainingWork == 0) {
          Complete = false;
          return {};
        }
        if (!Target) {
          std::vector<LowOp> Operations;
          bool Bounded = true;
          for (const InsnRecord *Instruction : Instructions) {
            for (const LowOp &Op : Instruction->Ops) {
              if (Op.Addr == Terminator.Addr && Op.NumInputs == 1 &&
                  (Op.Opcode == NdOp::INDIR_BR ||
                   (Op.Opcode == NdOp::BRANCH &&
                    Terminator.ScalarBranchInput))) {
                Target = constantScalarExpression(
                    Operations,
                    Terminator.ScalarBranchInput.value_or(Op.Inputs[0]),
                    Terminator.Addr, RemainingWork, ReadImage);
                if (!Target && AllowFinite)
                  FiniteTargets = finiteScalarExpression(
                      Operations,
                      Terminator.ScalarBranchInput.value_or(Op.Inputs[0]),
                      Terminator.Addr, RemainingWork, ReadImage);
                Bounded = false;
                break;
              }
              // Prefix edges contribute no path predicate. Every value must
              // hold for arbitrary input on this straight sequence.
              if (Path.DirectEdges.contains(Instruction) &&
                  (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR))
                continue;
              if (Operations.size() ==
                  limits::kMaxScalarBranchSymbolicOperations) {
                Bounded = false;
                break;
              }
              Operations.push_back(Op);
            }
            if (!Bounded)
              break;
          }
        }
        return Target ? ScalarTargets{std::vector<uint64_t>{*Target}}
                      : FiniteTargets;
      };
      ScalarPath Path;
      Path.Blocks.push_back(&Block);
      Path.Seen.insert(Block.Id);
      // Prefer a target proved independent of the block's incoming state.
      // Unrelated predecessors must not spend a local proof's work budget.
      auto Target = EvaluatePath(Path);
      if (Complete && !Target) {
        ExtendPath(Path);
        if (Path.Blocks.size() > 1)
          Target = EvaluatePath(Path);
      }
      const LowBlock &Join = *Path.Blocks.back();
      if (Complete && !Target && Join.Preds.size() > 1 && !Independent(Join)) {
        // Every incoming path must supply a complete cover. Union these
        // covers without recursively enumerating earlier joins, whose inputs
        // remain arbitrary. No uncovered edge may borrow another's mask.
        ScalarTargets Common{std::vector<uint64_t>{}};
        for (int P : Join.Preds) {
          ScalarPath Incoming = Path;
          if (!AppendPredecessor(Incoming, P)) {
            Common.reset();
            break;
          }
          ExtendPath(Incoming);
          const auto Value = EvaluatePath(Incoming);
          if (!Complete || !Value) {
            Common.reset();
            break;
          }
          // Prepay sorted-vector insertion, including shifting the existing
          // cover. A failed union cannot publish only its earlier paths.
          if (!Spend(Value->size() * (1 + Common->size() + Value->size()))) {
            Complete = false;
            Common.reset();
            break;
          }
          for (uint64_t Bits : *Value)
            if (!std::binary_search(Common->begin(), Common->end(), Bits))
              Common->insert(
                  std::lower_bound(Common->begin(), Common->end(), Bits), Bits);
          if (Common->size() > limits::kMaxScalarBranchFiniteValues ||
              (!AllowFinite && Common->size() > 1)) {
            Common.reset();
            break;
          }
        }
        Target = Common;
      }
      Targets[Terminator.Addr] = Target;
      if (!Complete)
        break;
    }
    if (!Complete) {
      // No prefix of an incomplete proof can retain authority after new edges
      // have been explored. Restoring is bounded by the decoded instruction
      // inventory and cannot start more exploration.
      bool Restored = false;
      for (auto &[Addr, Rec] : Insns)
        Restored |= Restore(Rec);
      if (Restored)
        rebuildBlocks(Func);
      return Changed || Restored;
    }

    bool RoundChanged = false;
    std::vector<va_t> Explore;
    for (const auto &[Addr, Values] : Targets) {
      auto &Rec = Insns.at(Addr);
      // An exact transfer has the same reachability authority as a decoded
      // direct branch. A next-entry bound limits speculative address roots,
      // not discontiguous or backward control flow. explore() still keeps
      // independent function entries as tail calls and checks code ownership
      // for every instruction it reaches.
      const bool Finite = Values && Values->size() > 1;
      const bool Valid =
          Values && !Values->empty() &&
          std::all_of(Values->begin(), Values->end(), [&](uint64_t Target) {
            if (Target == InvalidVA || Target % getInsnAlignment() != 0 ||
                !Img.hasExecutableCodeOwnerRange(Target, getInsnAlignment()) ||
                !Img.readVA(Target, getInsnAlignment()))
              return false;
            if (!Finite)
              return true;
            // A finite dispatch is an intra-function CFG, not a set of ABI
            // calls. Do not grant it independent function entries or
            // overlapping decodes.
            if (Target != Func.Entry && isTailCallTarget(Target))
              return false;
            const auto Next = Insns.lower_bound(Target);
            if (Next != Insns.begin()) {
              const auto &Previous = std::prev(Next)->second;
              if (Target - Previous.Addr < Previous.Size)
                return false;
            }
            return true;
          });
      if (Rec.ScalarBranchInput) {
        if (!Valid ||
            (Rec.IsIndirect ? *Values != Rec.JumpTableTargets
                            : Finite || Values->front() != Rec.BranchTarget))
          RoundChanged |= Restore(Rec);
        continue;
      }
      if (!Valid)
        continue;
      for (LowOp &Op : Rec.Ops) {
        if (Op.Opcode != NdOp::INDIR_BR || Op.NumInputs != 1)
          continue;
        Rec.ScalarBranchInput = Op.Inputs[0];
        if (Finite)
          Rec.JumpTableTargets = *Values;
        else {
          Op.Opcode = NdOp::BRANCH;
          Op.Inputs[0] = NdVar::cst(Values->front(), Img.getPointerSize());
          Rec.IsIndirect = false;
          Rec.BranchTarget = Values->front();
          Rec.Immediate = Values->front();
        }
        for (uint64_t Target : *Values) {
          BlockStarts.insert(Target);
          Explore.push_back(Target);
        }
        RoundChanged = true;
        break;
      }
    }
    if (!RoundChanged)
      return Changed;
    Changed = true;
    for (va_t Target : Explore)
      if (!ExploredAddrs.count(Target))
        explore(Img, Dec, Target);
    exploreAddressTakenRoots(Img, Dec);
    splitBlocks();
    rebuildBlocks(Func);
  }
}

} // namespace neverd
