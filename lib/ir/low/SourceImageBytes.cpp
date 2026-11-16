//===- SourceImageBytes.cpp - Conditional image-byte transforms -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Evaluate byte transforms with explicit, unproved initializer assumptions.

#include "neverd/ir/low/SourceImageBytes.h"

#include "LowScalarTargets.h"

#include "neverd/ir/low/LowArchitecturalValue.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/LowRawEnvironment.h"
#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/symbolic/SymExec.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

namespace neverd {
namespace {
using namespace symbolic;

struct InitialByte {
  SourceImageByteInput Input;
  bool Conflict = false;
  bool ConditionalAddress = false;
};

bool admitted(const LowOp &Op) {
  if (isLowRawEnvironmentRequirement(Op) || isLowArchitecturalValue(Op) ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  return isSourceFrameScalarTransfer(Op.Opcode) || Op.Opcode == NdOp::LOAD ||
         Op.Opcode == NdOp::STORE || Op.Opcode == NdOp::BRANCH ||
         Op.Opcode == NdOp::COND_BR || Op.Opcode == NdOp::RETURN;
}

std::optional<va_t> address(const LowOp &Op, SymExec &Exec,
                            SymContext &Context) {
  const auto Value = Exec.memoryAddressValue(Op);
  const auto Number = Value ? Context.asConst(Value) : std::nullopt;
  return Number && Number->getActiveBits() <= 64
             ? std::optional<va_t>(Number->getZExtValue())
             : std::nullopt;
}

struct Observation {
  const BinaryImage &Image;
  SourceImageByteObservations Result;
  std::map<va_t, InitialByte> Initializers;
  using ObservationKey = std::pair<va_t, std::pair<va_t, va_t>>;
  llvm::DenseSet<ObservationKey> Published{};
  llvm::DenseMap<int, const LowBlock *> Continuations{};
  llvm::DenseMap<uint64_t, uint64_t> FunctionTemporaryMasks{};
  llvm::DenseSet<const LowBlock *> EntryCandidates{};
  llvm::DenseSet<const LowBlock *> ConditionalEntries{};
  LowScalarEntryQueries RegisterQueries;
  std::map<const LowBlock *, std::vector<SourceImageRegisterInput>>
      EntryRegisters;
  // Immutable reads share the loader's full-range/fixup authentication.
  std::map<std::pair<va_t, uint16_t>, std::optional<std::vector<uint8_t>>>
      Reads;

  void seedImmutable(const LowOp &Op, SymContext &Context, SymState &State,
                     SymExec &Exec) {
    if (Op.Opcode != NdOp::LOAD)
      return;
    const auto At = address(Op, Exec, Context);
    if (!At)
      return;
    const auto Key = std::pair{*At, Op.Output.Size};
    auto Found = Reads.find(Key);
    if (Found == Reads.end()) {
      // The loader rejects writable storage before walking fixups and owns
      // complete-range authentication. Cache that result for all prefixes.
      Found =
          Reads
              .emplace(Key, readImmutableImageBytes(Image, *At, Op.Output.Size))
              .first;
    }
    if (!Found->second || Found->second->empty())
      return;
    llvm::APInt Value(Found->second->size() * 8, 0);
    for (size_t I = 0; I < Found->second->size(); ++I)
      Value |= llvm::APInt(Value.getBitWidth(), (*Found->second)[I]).shl(I * 8);
    State.store(Context.mkConst(64, *At), Context.mkConst(Value));
  }

  bool step(const LowOp &Op, SymContext &Context, SymState &State,
            SymExec &Exec) {
    seedImmutable(Op, Context, State, Exec);
    return Exec.step(Op) != StepResult::Unmodelled;
  }

  void indexContinuations(const LowFunc &Function) {
    llvm::DenseMap<int, const LowBlock *> Blocks;
    Blocks.reserve(Function.Blocks.size());
    for (const auto &Block : Function.Blocks)
      Blocks[Block.Id] = &Block;
    for (const auto &Block : Function.Blocks) {
      if (Block.Succs.size() != 1 || !Block.ExceptionalSuccs.empty() ||
          Block.InstructionBoundaries.empty())
        continue;
      const auto Found = Blocks.find(Block.Succs.front());
      if (Found == Blocks.end())
        continue;
      const auto &Edge = Block.InstructionBoundaries.back();
      if (!Edge.Size || Edge.Address < Block.StartAddr ||
          Edge.Address >= Block.EndAddr ||
          Edge.Size != Block.EndAddr - Edge.Address ||
          Edge.FirstOp > Block.Ops.size() ||
          Edge.OpCount != Block.Ops.size() - Edge.FirstOp)
        continue;
      const auto Target = Found->second->StartAddr;
      const bool Direct =
          Edge.Control == LowInstructionControl::Branch && Edge.Immediate &&
          *Edge.Immediate == Target &&
          !hasLowInstructionControlFlag(Edge.ControlFlags,
                                        LowInstructionControlFlag::Indirect) &&
          !hasLowInstructionControlFlag(Edge.ControlFlags,
                                        LowInstructionControlFlag::Conditional);
      const bool Fallthrough = Edge.Control == LowInstructionControl::None &&
                               Block.EndAddr == Target;
      if (Direct || Fallthrough)
        Continuations[Block.Id] = Found->second;
    }
  }

  struct Temporaries {
    va_t Instruction = InvalidVA;
    llvm::DenseMap<uint64_t, uint64_t> Local, Function;

    bool prepare(const LowOp &Op, Observation &Analysis) {
      if (Instruction != Op.Addr) {
        Instruction = Op.Addr;
        Local.clear();
      }
      for (unsigned Index = 0; Index < Op.NumInputs; ++Index) {
        const auto &Input = Op.Inputs[Index];
        if (!Input.isTemp())
          continue;
        if (!Analysis.temporaryWords(
                Input.Offset, Input.Size, [&](uint64_t Word, uint64_t Mask) {
                  const uint64_t LongLived =
                      Analysis.FunctionTemporaryMasks.lookup(Word);
                  const uint64_t Defined = (Function.lookup(Word) & LongLived) |
                                           (Local.lookup(Word) & ~LongLived);
                  return (Defined & Mask) == Mask;
                }))
          return false;
      }
      return true;
    }

    bool define(const NdVar &Output, Observation &Analysis) {
      if (!Output.isTemp())
        return true;
      return Analysis.temporaryWords(
          Output.Offset, Output.Size, [&](uint64_t Word, uint64_t Mask) {
            const uint64_t LongLived =
                Analysis.FunctionTemporaryMasks.lookup(Word);
            if (Mask & LongLived)
              Function[Word] |= Mask & LongLived;
            if (Mask & ~LongLived)
              Local[Word] |= Mask & ~LongLived;
            return true;
          });
    }
  };

  // One mask bit still represents exactly one byte. Packing the definition
  // check does not promote partial writes to whole-word definitions, including
  // unaligned ranges crossing a word.
  template <typename Visit>
  bool temporaryWords(uint64_t Offset, unsigned Bytes, Visit Action) {
    while (Bytes) {
      const unsigned Bit = Offset % 64;
      const unsigned Count = std::min(Bytes, 64 - Bit);
      const uint64_t Mask = (UINT64_MAX >> (64 - Count)) << Bit;
      if (!Action(Offset / 64, Mask))
        return false;
      Bytes -= Count;
      Offset += Count;
    }
    return true;
  }

  void indexTemporaries(const LowFunc &Function) {
    for (const auto &Range : Function.FunctionTemporaries)
      temporaryWords(Range.Offset, Range.Bytes,
                     [&](uint64_t Word, uint64_t Mask) {
                       FunctionTemporaryMasks[Word] |= Mask;
                       return true;
                     });
  }

  void findEntryRegisterQueries(const LowFunc &Function) {
    if (!Function.hasCompleteLiftCoverage() ||
        !Function.UnsafeIndirectBranchAddresses.empty())
      return;
    for (const auto &Start : Function.Blocks) {
      if (!EntryCandidates.contains(&Start) || Start.Preds.empty() ||
          Start.StartAddr == Function.Entry ||
          !Start.ExceptionalPreds.empty() ||
          Function.ModuleAnalysisRoots.contains(Start.StartAddr) ||
          Function.OrdinaryModuleAnalysisRoots.contains(Start.StartAddr))
        continue;
      // Only bytes read before their first write can depend on this entry.
      // Follow the same authenticated straight-line suffix as the observer;
      // a suffix may be the first place it consumes a predecessor's address.
      std::set<uint64_t> Needed, Written;
      llvm::DenseSet<int> Visited;
      const LowBlock *Block = &Start;
      while (Block && Visited.insert(Block->Id).second) {
        bool CompleteBlock = true;
        for (size_t Index = 0; Index < Block->Ops.size(); ++Index) {
          const auto &Op = Block->Ops[Index];
          if (!admitted(Op)) {
            CompleteBlock = false;
            break;
          }
          for (unsigned I = 0; I < Op.NumInputs; ++I) {
            const auto &Input = Op.Inputs[I];
            if (!Input.isReg())
              continue;
            for (unsigned Byte = 0; Byte < Input.Size; ++Byte)
              if (!Written.contains(Input.Offset + Byte))
                Needed.insert(Input.Offset + Byte);
          }
          if (Op.Output.isReg()) {
            for (unsigned Byte = 0; Byte < Op.Output.Size; ++Byte)
              Written.insert(Op.Output.Offset + Byte);
          }
          if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
              Op.Opcode == NdOp::RETURN) {
            CompleteBlock =
                Op.Opcode == NdOp::BRANCH && Index + 1 == Block->Ops.size();
            break;
          }
        }
        if (!CompleteBlock)
          break;
        Block = Continuations.lookup(Block->Id);
      }
      if (!Needed.empty()) {
        auto &Registers = RegisterQueries[&Start];
        Registers.reserve(Needed.size());
        for (const auto Offset : Needed)
          Registers.push_back(NdVar::reg(Offset, 1));
      }
    }
  }

  void seedEntryRegisters(const LowScalarEntryValues &Values,
                          bool Conditional) {
    for (const auto &[Block, Registers] : Values) {
      std::map<uint64_t, uint8_t> Bytes;
      if (const auto Previous = EntryRegisters.find(Block);
          Previous != EntryRegisters.end())
        for (const auto &Input : Previous->second)
          for (unsigned I = 0; I < Input.Bytes; ++I)
            Bytes.emplace(Input.Offset + I, Input.Value >> (I * 8));
      const size_t Before = Bytes.size();
      for (const auto &[Register, Cover] : Registers)
        if (Cover.size() == 1)
          Bytes.try_emplace(Register.Offset, Cover.front());
      if (Bytes.size() == Before)
        continue;
      if (Conditional)
        ConditionalEntries.insert(Block);
      std::vector<SourceImageRegisterInput> Inputs;
      for (const auto &[Offset, Byte] : Bytes) {
        // The query bytes are sorted. Pack adjacent singleton bytes without
        // promoting a non-singleton or missing byte to a known register word.
        if (!Inputs.empty() && Inputs.back().Bytes < 8 &&
            Inputs.back().Offset + Inputs.back().Bytes == Offset) {
          auto &Input = Inputs.back();
          Input.Value |= uint64_t(Byte) << (Input.Bytes * 8);
          ++Input.Bytes;
        } else
          Inputs.push_back({Offset, 1, Byte});
      }
      EntryRegisters[Block] = std::move(Inputs);
    }
  }

  bool hasMissingEntryRegisters() const {
    for (const auto &[Block, Queries] : RegisterQueries) {
      const auto Found = EntryRegisters.find(Block);
      if (Found == EntryRegisters.end())
        return true;
      size_t Bytes = 0;
      for (const auto &Input : Found->second)
        Bytes += Input.Bytes;
      if (Bytes != Queries.size())
        return true;
    }
    return false;
  }

  struct ByteValue {
    uint8_t Value;
    std::map<va_t, SourceImageByteInput> Inputs;
  };

  std::optional<ByteValue> evaluate(SymRef Value, SymContext &Context) {
    llvm::SmallVector<uint32_t> Variables;
    Context.collectVars(Value, Variables);
    std::unordered_map<uint32_t, SymRef> Bindings;
    ByteValue Byte{};
    for (const auto Id : Variables) {
      const auto &Info = Context.varInfo(Id);
      if (!Info.InputOrigin || Info.Fresh ||
          Info.InputOrigin->Kind != SymInputKind::AbsoluteMemory ||
          Info.InputOrigin->Epoch || Info.Width != Info.InputOrigin->Bytes * 8)
        return std::nullopt;
      const auto &Origin = *Info.InputOrigin;
      llvm::APInt Number(Info.Width, 0);
      for (unsigned I = 0; I < Origin.Bytes; ++I) {
        const auto Found = Initializers.find(Origin.Offset + I);
        if (Found == Initializers.end() || Found->second.Conflict)
          return std::nullopt;
        const auto &Input = Found->second.Input;
        Number |= llvm::APInt(Info.Width, Input.Value).shl(I * 8);
        Byte.Inputs.emplace(Input.Address, Input);
      }
      Bindings.emplace(Context.varRef(Id).index(), Context.mkConst(Number));
    }
    const auto Constant = Context.asConst(Context.substitute(Value, Bindings));
    if (!Constant)
      return std::nullopt;
    Byte.Value = Constant->getZExtValue();
    return Byte;
  }

  void observe(const LowBlock &Block, va_t Instruction, SymContext &Context,
               SymState &State, const std::map<va_t, va_t> &Touched,
               bool UseEntryRegisters) {
    std::map<va_t, ByteValue> Values;
    for (const auto &[At, Store] : Touched) {
      const auto Value = State.load(Context.mkConst(64, At), 1);
      if (auto Byte = evaluate(Value, Context))
        Values.emplace(At, std::move(*Byte));
    }
    for (auto It = Values.begin(); It != Values.end();) {
      SourceImageBytes Value;
      Value.Block = Block.StartAddr;
      Value.Instruction = Instruction;
      Value.Address = It->first;
      if (UseEntryRegisters)
        Value.EntryRegisters = EntryRegisters.at(&Block);
      std::map<va_t, SourceImageByteInput> Inputs;
      while (It != Values.end() &&
             It->first - Value.Address == Value.Bytes.size()) {
        Value.Bytes.push_back(It->second.Value);
        Inputs.insert(It->second.Inputs.begin(), It->second.Inputs.end());
        ++It;
        if (!Value.Bytes.back())
          break;
      }
      if (Inputs.empty() || Value.Bytes.size() < 2 || Value.Bytes.back())
        continue;
      const ObservationKey Key{Value.Block, {Value.Instruction, Value.Address}};
      if (Published.contains(Key))
        continue;
      for (const auto &[At, Input] : Inputs)
        Value.Inputs.push_back(Input);
      Published.insert(Key);
      Result.Values.push_back(std::move(Value));
    }
  }

  // Extension can expose a competing initializer after a local observation
  // was published. Its conditions must still pass the same conflict rule.
  void refreshObservations() {
    std::erase_if(Result.Values, [&](SourceImageBytes &Value) {
      for (auto &Input : Value.Inputs) {
        const auto &Initial = Initializers.at(Input.Address);
        if (Initial.Conflict)
          return true;
        Input.Initializer = Initial.Input.Initializer;
      }
      return false;
    });
    Published.clear();
    for (const auto &Value : Result.Values) {
      Published.insert({Value.Block, {Value.Instruction, Value.Address}});
    }
  }

  void collect(const LowFunc &Function, bool Initializing,
               bool FollowContinuations, bool UseEntryRegisters = false) {
    for (const auto &Start : Function.Blocks) {
      const bool Seeded = UseEntryRegisters && EntryRegisters.contains(&Start);
      if (UseEntryRegisters && !Seeded && Initializing)
        continue;
      if (Initializing && FollowContinuations &&
          !Continuations.contains(Start.Id))
        continue;
      SymContext Context;
      SymState State(Context);
      SymExec Exec(Context, State);
      if (Seeded)
        for (const auto &Input : EntryRegisters.at(&Start)) {
          State.write(SymSpace::Register, Input.Offset,
                      Context.mkConst(Input.Bytes * 8, Input.Value));
        }
      Temporaries Defined;
      llvm::DenseSet<int> Visited;
      std::map<va_t, va_t> Touched;
      const LowBlock *Block = &Start;
      while (Block && Visited.insert(Block->Id).second) {
        va_t Last = Block->StartAddr;
        bool Modelled = true, CompleteBlock = true, Stored = false;
        llvm::SmallVector<SourceImageByteInput> InstructionInitializers;
        for (size_t Index = 0; Index < Block->Ops.size(); ++Index) {
          const auto &Op = Block->Ops[Index];
          if (!admitted(Op) || !Defined.prepare(Op, *this)) {
            // The retained state can be reported only after a complete
            // original instruction, never after its admitted LowIR prefix.
            Modelled = Index == 0 || Block->Ops[Index - 1].Addr != Op.Addr;
            CompleteBlock = false;
            break;
          }
          if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2) {
            Stored = true;
            const auto At = address(Op, Exec, Context);
            if (Initializing && !UseEntryRegisters && !At)
              EntryCandidates.insert(&Start);
            if (At && Image.hasObjectDataProvenance(*At) &&
                Op.Inputs[1].Size <= InvalidVA - *At) {
              if (Initializing) {
                if (const auto Value =
                        Context.asConst(Exec.operandValue(Op.Inputs[1]))) {
                  for (unsigned I = 0; I < Op.Inputs[1].Size; ++I) {
                    const uint8_t Byte =
                        Value->extractBitsAsZExtValue(8, I * 8);
                    InstructionInitializers.push_back({*At + I, Op.Addr, Byte});
                  }
                } else if (!UseEntryRegisters)
                  EntryCandidates.insert(&Start);
              } else {
                for (unsigned I = 0; I < Op.Inputs[1].Size; ++I)
                  Touched[*At + I] = Op.Addr;
              }
            }
          }
          if (!step(Op, Context, State, Exec) ||
              !Defined.define(Op.Output, *this)) {
            Modelled = CompleteBlock = false;
            break;
          }
          Last = Op.Addr;
          const bool CompleteInstruction =
              Index + 1 == Block->Ops.size() ||
              Block->Ops[Index + 1].Addr != Op.Addr;
          if (CompleteInstruction) {
            // A later operation in the same instruction may be unsupported
            // or guarded. Such a prefix supplies neither an initializer nor
            // a conflicting proposal against a completed instruction.
            for (const auto &Input : InstructionInitializers) {
              const bool Conditional =
                  Seeded && ConditionalEntries.contains(&Start);
              auto [Found, New] = Initializers.try_emplace(
                  Input.Address, InitialByte{Input, false, Conditional});
              // A hypothesized address must not withdraw an initializer
              // whose address the exact scalar phase already established.
              if (!New && Conditional && !Found->second.ConditionalAddress)
                continue;
              if (!New && Found->second.Input.Value != Input.Value)
                Found->second.Conflict = true;
              else if (!New)
                Found->second.Input.Initializer = std::min(
                    Found->second.Input.Initializer, Input.Initializer);
            }
            InstructionInitializers.clear();
          }
          if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
              Op.Opcode == NdOp::RETURN) {
            Modelled = CompleteInstruction;
            CompleteBlock =
                Op.Opcode == NdOp::BRANCH && Index + 1 == Block->Ops.size();
            break;
          }
        }
        if (!Initializing && Modelled && Stored)
          observe(Start, Last, Context, State, Touched, Seeded);
        if (!CompleteBlock || !FollowContinuations)
          break;
        Block = Continuations.lookup(Block->Id);
      }
    }
  }
};
} // namespace

SourceImageByteObservations
observeSourceImageBytes(const LowFunc &Function, const BinaryImage &Image,
                        const std::map<va_t, uint32_t> *BodyMayWrite) {
  // This observation contract uses ordinary little-endian 64-bit addresses.
  // Other targets retain their source without claiming this extra analysis.
  if (Image.Arch != Arch::X64 && Image.Arch != Arch::AArch64)
    return {};
  Observation Analysis{Image};
  Analysis.indexContinuations(Function);
  Analysis.indexTemporaries(Function);
  Analysis.collect(Function, true, false);
  if (!Analysis.Initializers.empty())
    Analysis.collect(Function, false, false);
  if (!Analysis.Continuations.empty()) {
    Analysis.collect(Function, true, true);
    Analysis.refreshObservations();
    if (!Analysis.Initializers.empty())
      Analysis.collect(Function, false, true);
  }
  // Recursively resolve incoming register dependencies through the shared
  // fixed point. The worklist and visited sets handle loops without recursive
  // host stack growth or a fixed work cutoff. Unknown calls and differing
  // incoming values remain unknown.
  if (!Analysis.EntryCandidates.empty()) {
    const std::optional<LowScalarCallEffects> Calls =
        BodyMayWrite
            ? std::optional<LowScalarCallEffects>{{Image.Arch, *BodyMayWrite}}
            : std::nullopt;
    Analysis.findEntryRegisterQueries(Function);
    if (!Analysis.RegisterQueries.empty())
      if (const auto Values =
              finiteScalarBlockEntryValues(Function, Analysis.RegisterQueries,
                                           nullptr, Calls ? &*Calls : nullptr))
        Analysis.seedEntryRegisters(*Values, false);
  }
  if (!Analysis.EntryRegisters.empty()) {
    Analysis.collect(Function, true, false, true);
    Analysis.collect(Function, true, true, true);
    Analysis.refreshObservations();
    if (!Analysis.Initializers.empty()) {
      Analysis.collect(Function, false, false, true);
      Analysis.collect(Function, false, true, true);
    }
  }
  // A call can prevent an exact address proof without preventing a useful
  // conditional transform. The separate candidate query supplies only
  // explicit entry hypotheses, never callee preservation or memory facts.
  // Reuse one query batch and one fixed point for all missing entry bytes.
  if (Analysis.hasMissingEntryRegisters() &&
      std::any_of(Function.Blocks.begin(), Function.Blocks.end(),
                  [](const LowBlock &Block) {
                    return std::any_of(Block.Ops.begin(), Block.Ops.end(),
                                       [](const LowOp &Op) {
                                         return Op.Opcode == NdOp::CALL ||
                                                Op.Opcode == NdOp::INDIR_CALL;
                                       });
                  })) {
    if (const auto Candidates = conditionalScalarEntryCandidates(
            Function, Analysis.RegisterQueries))
      Analysis.seedEntryRegisters(Candidates->Entries, true);
    if (!Analysis.ConditionalEntries.empty()) {
      Analysis.collect(Function, true, false, true);
      Analysis.collect(Function, true, true, true);
      Analysis.refreshObservations();
      if (!Analysis.Initializers.empty()) {
        Analysis.collect(Function, false, false, true);
        Analysis.collect(Function, false, true, true);
      }
    }
  }
  return std::move(Analysis.Result);
}

} // namespace neverd
