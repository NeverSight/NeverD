//===- InterpreterMachineState.cpp - Explicit recovery source ABI --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/arch/x86_64/InterpreterMachineState.h"

#include "X64ControlTransitions.h"
#include "X64MachineFlags.h"
#include "X64Recovery.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowRawEnvironment.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/Support/Errc.h"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace neverd::analysis {
namespace {

// Disjoint from every architectural register bank. These are initialized
// scalar identities for SSA construction, never additional source inputs.
constexpr uint64_t GuestBase = uint64_t{1} << 32;
constexpr uint64_t StatePointer = GuestBase + 256;
constexpr uint64_t SystemFlags = GuestBase + 264;
constexpr uint64_t ProfileStatus = GuestBase + 272;
constexpr uint64_t QueryIndex = GuestBase + 280;
constexpr uint64_t QueryAddress = GuestBase + 288;
constexpr uint64_t QueryLeaf = GuestBase + 296;
constexpr uint64_t QuerySubleaf = GuestBase + 304;
constexpr uint64_t GuestVectorBase = GuestBase + 512;
constexpr uint64_t ScratchBase = uint64_t{1} << 61;
constexpr uint64_t ScratchEnd = uint64_t{1} << 62;
using FlagProfile = detail::X64MachineFlags;
constexpr auto &Flags = FlagProfile::Flags;

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                 Message.str().c_str());
}

NdVar scalar(uint64_t Value, uint16_t Size = 8) {
  return NdVar::scalar(Value, Size);
}

bool nativeRegister(const NdVar &Value) {
  if (!Value.isReg() || !Value.Size || Value.Size > 8)
    return false;
  if (Value.Offset < 128 && Value.Offset % 8 + Value.Size <= 8)
    return true;
  if (Value.Size == 1)
    for (const auto &[Offset, Bit] : Flags) {
      (void)Bit;
      if (Value.Offset == Offset)
        return true;
    }
  return false;
}

std::optional<unsigned> xmmIndex(NdVar Value) {
  if (!Value.isReg() || Value.Size != 16 || Value.Offset < x86reg::XMM0 ||
      Value.Offset > x86reg::XMM15 ||
      (Value.Offset - x86reg::VectorBase) % x86reg::VectorRegStride)
    return std::nullopt;
  return (Value.Offset - x86reg::VectorBase) / x86reg::VectorRegStride;
}

llvm::Error validateSourceTemporary(NdVar Value, bool VectorState = false) {
  if (Value.Size > (VectorState ? 16 : 8) || !Value.Size ||
      Value.Offset > InvalidVA - (Value.Size - 1))
    return invalid("machine source has an invalid temporary range");
  if (Value.Offset < ScratchEnd && Value.Offset + Value.Size - 1 >= ScratchBase)
    return invalid("machine source temporary overlaps wrapper scratch");
  return llvm::Error::success();
}

struct InstructionWriter {
  std::vector<LowOp> &Ops;
  va_t Address;
  bool StateRegisters = false;
  bool VectorState = false;
  uint64_t NextTemporary = ScratchBase;

  NdVar temporary(uint16_t Bytes = 8) {
    const auto Value = NdVar::tmp(NextTemporary, Bytes);
    NextTemporary += std::max<uint16_t>(Bytes, 8);
    return Value;
  }

  void emit(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    Op.Addr = Address;
    for (const auto &Input : Inputs)
      Op.addInput(Input);
    Ops.push_back(Op);
  }

  NdVar address(uint64_t Offset) {
    const NdVar Result = temporary();
    emit(NdOp::INT_ADD, Result, {NdVar::reg(StatePointer, 8), scalar(Offset)});
    return Result;
  }

  void load(NdVar Output, uint64_t Offset) {
    if (StateRegisters)
      emit(NdOp::COPY, Output, {NdVar::reg(Offset, Output.Size)});
    else
      emit(NdOp::LOAD, Output, {address(Offset)});
  }

  void store(uint64_t Offset, NdVar Value) {
    if (StateRegisters)
      emit(NdOp::COPY, NdVar::reg(Offset, Value.Size), {Value});
    else
      emit(NdOp::STORE, {}, {address(Offset), Value});
  }

  NdVar packedFlags() {
    NdVar Packed = NdVar::reg(SystemFlags, 8);
    for (const auto &[Offset, Bit] : Flags) {
      const NdVar Extended = temporary();
      emit(NdOp::INT_ZEXT, Extended, {NdVar::reg(GuestBase + Offset, 1)});
      const NdVar Shifted = temporary();
      emit(NdOp::INT_LEFT, Shifted, {Extended, scalar(Bit)});
      const NdVar Next = temporary();
      emit(NdOp::INT_OR, Next, {Packed, Shifted});
      Packed = Next;
    }
    return Packed;
  }

  void entry(uint64_t ParameterRegister) {
    if (!StateRegisters)
      emit(NdOp::COPY, NdVar::reg(StatePointer, 8),
           {NdVar::reg(ParameterRegister, 8)});
    for (uint64_t Index = 0; Index != 16; ++Index)
      load(NdVar::reg(GuestBase + Index * 8, 8), Index * 8);
    if (VectorState)
      for (unsigned Index = 0; Index != 16; ++Index) {
        const auto Low = temporary(), High = temporary();
        const auto Offset =
            offsetof(InterpreterMachineQueryStateX64V2, XMM) + Index * 16;
        load(Low, Offset);
        load(High, Offset + 8);
        emit(NdOp::CONCAT, NdVar::reg(GuestVectorBase + Index * 16, 16),
             {High, Low});
      }
    const NdVar Packed = temporary();
    load(Packed, offsetof(InterpreterMachineStateX64V1, RFlags));
    const NdVar UnsupportedBits = temporary();
    emit(NdOp::INT_AND, UnsupportedBits,
         {Packed, scalar(~FlagProfile::EntryMask)});
    const NdVar Unsupported = temporary(1);
    emit(NdOp::INT_NOTEQUAL, Unsupported, {UnsupportedBits, scalar(0)});
    const NdVar FixedBit = temporary();
    emit(NdOp::INT_AND, FixedBit, {Packed, scalar(2)});
    const NdVar MissingFixed = temporary(1);
    emit(NdOp::INT_EQUAL, MissingFixed, {FixedBit, scalar(0)});
    const NdVar Invalid = temporary(1);
    emit(NdOp::BOOL_OR, Invalid, {Unsupported, MissingFixed});
    emit(NdOp::INT_ZEXT, NdVar::reg(ProfileStatus, 8), {Invalid});
    emit(NdOp::INT_AND, NdVar::reg(SystemFlags, 8),
         {Packed, scalar(~FlagProfile::SplitMask)});
    for (const auto &[Offset, Bit] : Flags) {
      const NdVar Shifted = temporary();
      emit(NdOp::INT_RIGHT, Shifted, {Packed, scalar(Bit)});
      const NdVar Masked = temporary();
      emit(NdOp::INT_AND, Masked, {Shifted, scalar(1)});
      emit(NdOp::INT_NOTEQUAL, NdVar::reg(GuestBase + Offset, 1),
           {Masked, scalar(0)});
    }
  }

  void leave(bool IncludeFlags = true) {
    for (uint64_t Index = 0; Index != 16; ++Index)
      store(Index * 8, NdVar::reg(GuestBase + Index * 8, 8));
    if (VectorState)
      for (unsigned Index = 0; Index != 16; ++Index)
        for (unsigned Half = 0; Half != 2; ++Half) {
          const auto Value = temporary();
          emit(NdOp::SUBBYTES, Value,
               {NdVar::reg(GuestVectorBase + Index * 16, 16),
                scalar(Half * 8, 4)});
          store(offsetof(InterpreterMachineQueryStateX64V2, XMM) + Index * 16 +
                    Half * 8,
                Value);
        }
    if (IncludeFlags)
      store(offsetof(InterpreterMachineStateX64V1, RFlags), packedFlags());
    if (StateRegisters) {
      emit(NdOp::RETURN, {}, {NdVar::reg(ProfileStatus, 8)});
      return;
    }
    // The wrapper's source ABI returns status in the real host RAX. Guest RAX
    // has already been relocated and committed to the state buffer above.
    emit(NdOp::COPY, NdVar::reg(x86reg::RAX, 8),
         {NdVar::reg(ProfileStatus, 8)});
    emit(NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, 8)});
  }

  llvm::Expected<NdVar> readRegister(NdVar Value) {
    if (VectorState)
      if (const auto Index = xmmIndex(Value))
        return NdVar::reg(GuestVectorBase + *Index * 16, 16);
    if (Value.Offset >= 128 || Value.Size == 8) {
      Value.Offset += GuestBase;
      return Value;
    }
    const auto &TRI = getTargetRegInfo(Arch::X64);
    const auto [Base, Bytes] = TRI.findWideReg(Value.Offset, Value.Size);
    const int Offset =
        TRI.subRegByteOffset(Value.Offset, Value.Size, Base, Bytes);
    if (Bytes != 8 || Base >= 128 || Offset < 0)
      return invalid("machine source has an unsupported GPR byte view");
    const NdVar Extracted = temporary(Value.Size);
    emit(NdOp::SUBBYTES, Extracted,
         {NdVar::reg(GuestBase + Base, 8), scalar(Offset, 4)});
    return Extracted;
  }

  llvm::Error writeRegister(NdVar Original, NdVar Value, bool LowSliceView) {
    const auto &TRI = getTargetRegInfo(Arch::X64);
    const auto [Base, Bytes] = TRI.findWideReg(Original.Offset, Original.Size);
    const int Offset =
        TRI.subRegByteOffset(Original.Offset, Original.Size, Base, Bytes);
    if (Bytes != 8 || Base >= 128 || Offset < 0)
      return invalid("machine source has an unsupported GPR byte write");
    const NdVar Full = NdVar::reg(GuestBase + Base, 8);
    if (!LowSliceView && TRI.writeZeroExtends(Original.Offset, Original.Size)) {
      emit(NdOp::INT_ZEXT, Full, {Value});
      return llvm::Error::success();
    }
    const uint64_t Mask = ((uint64_t{1} << (Original.Size * 8)) - 1)
                          << (Offset * 8);
    const NdVar Kept = temporary();
    emit(NdOp::INT_AND, Kept, {Full, scalar(~Mask)});
    const NdVar Extended = temporary();
    emit(NdOp::INT_ZEXT, Extended, {Value});
    const NdVar Shifted = temporary();
    emit(NdOp::INT_LEFT, Shifted, {Extended, scalar(Offset * 8)});
    emit(NdOp::INT_OR, Full, {Kept, Shifted});
    return llvm::Error::success();
  }
};

/// Instruction-local temporary offsets denote byte storage. Capture each
/// producer in fresh scalar storage before spelling differently sized reads;
/// later overlapping writes must not change an earlier producer's value.
struct InstructionTemporaryBytes {
  struct Byte {
    NdVar Value;
    uint16_t Index;
  };
  std::map<uint64_t, Byte> Bytes;

  void write(NdVar Destination, NdVar Value) {
    for (uint16_t I = 0; I != Destination.Size; ++I)
      Bytes[Destination.Offset + I] = {Value, I};
  }

  llvm::Expected<NdVar> read(NdVar Source, InstructionWriter &Writer) const {
    std::vector<Byte> Parts;
    for (uint16_t I = 0; I != Source.Size; ++I) {
      const auto Found = Bytes.find(Source.Offset + I);
      if (Found == Bytes.end())
        return invalid(
            "machine source has an unbound instruction temporary byte");
      Parts.push_back(Found->second);
    }
    NdVar Result;
    for (uint16_t Begin = 0; Begin != Source.Size;) {
      const auto &Part = Parts[Begin];
      uint16_t End = Begin + 1;
      while (End != Source.Size && Parts[End].Value == Part.Value &&
             Parts[End].Index == Part.Index + End - Begin)
        ++End;
      const uint16_t Size = End - Begin;
      auto Value = Part.Value;
      if (Part.Index || Size != Value.Size) {
        Value = Writer.temporary(Size);
        Writer.emit(NdOp::SUBBYTES, Value, {Part.Value, scalar(Part.Index, 4)});
      }
      if (Size != Source.Size) {
        const auto Extended = Writer.temporary(Source.Size);
        Writer.emit(NdOp::INT_ZEXT, Extended, {Value});
        Value = Extended;
      }
      if (Begin) {
        const auto Shifted = Writer.temporary(Source.Size);
        Writer.emit(NdOp::INT_LEFT, Shifted, {Value, scalar(Begin * 8)});
        const auto Joined = Writer.temporary(Source.Size);
        Writer.emit(NdOp::INT_OR, Joined, {Result, Shifted});
        Result = Joined;
      } else
        Result = Value;
      Begin = End;
    }
    return Result;
  }
};

// Optional source compaction only. Reserve the complete layout before changing
// any return, and keep the existing inline form when it cannot fit or the CFG
// is unsuitable. In particular a new block must never bind a dangling target.
std::optional<LowBlock> sharedReturnBlock(const LowFunc &Function,
                                          bool HasAlignmentGuard) {
  std::map<int, const LowBlock *> Blocks;
  std::map<va_t, int> Addresses;
  std::set<std::pair<int, int>> Successors, Predecessors;
  va_t End = 0;
  size_t Returns = 0;
  for (const auto &B : Function.Blocks) {
    if (B.Id < 0 || !Blocks.emplace(B.Id, &B).second ||
        !Addresses.emplace(B.StartAddr, B.Id).second)
      return std::nullopt;
    End = std::max(End, B.EndAddr);
    for (int Id : B.Succs)
      Successors.emplace(B.Id, Id);
    for (int Id : B.Preds)
      Predecessors.emplace(Id, B.Id);
    for (size_t I = 0; I != B.Ops.size(); ++I)
      if (B.Ops[I].Opcode == NdOp::RETURN) {
        if (I + 1 != B.Ops.size() || !B.Succs.empty() ||
            B.InstructionBoundaries.empty())
          return std::nullopt;
        const auto &Last = B.InstructionBoundaries.back();
        if (Last.Control != LowInstructionControl::Return ||
            Last.FirstOp + Last.OpCount != B.Ops.size())
          return std::nullopt;
        ++Returns;
      }
  }
  if (Returns < 2 || End >= InvalidVA - 2 || Successors != Predecessors)
    return std::nullopt;
  for (const auto &B : Function.Blocks) {
    for (int Id : B.Succs)
      if (!Blocks.count(Id))
        return std::nullopt;
    for (int Id : B.Preds)
      if (!Blocks.count(Id))
        return std::nullopt;
    for (const auto &Op : B.Ops)
      if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
        if (!Op.NumInputs || !Op.Inputs[0].isConst())
          return std::nullopt;
        const auto Target = Addresses.find(Op.Inputs[0].Offset);
        if (Target == Addresses.end() ||
            !Successors.count({B.Id, Target->second}))
          return std::nullopt;
      }
  }
  if (HasAlignmentGuard) {
    const auto &Entry = Function.Blocks.front();
    const uint64_t Span = Entry.EndAddr - Entry.StartAddr;
    // addEntryAlignmentGuard relocates the entry beyond the new maximum end
    // and reserves its reject block. Do not consume space that it needs.
    if (!Span || Span > InvalidVA - (End + 2) ||
        InvalidVA - (End + 2) - Span < 3 ||
        Function.Blocks.size() >
            static_cast<size_t>(std::numeric_limits<int>::max() - 3))
      return std::nullopt;
  }
  int Id = 0;
  while (Blocks.count(Id)) {
    if (Id == std::numeric_limits<int>::max())
      return std::nullopt;
    ++Id;
  }
  LowBlock Exit;
  Exit.Id = Id;
  Exit.StartAddr = End + 1;
  Exit.EndAddr = End + 2;
  return Exit;
}

llvm::Error addEntryAlignmentGuard(LowFunc &Function, uint64_t Parameter,
                                   bool StateRegisters,
                                   InterpreterEntryAlignment Alignment) {
  // Move the predecessor-free original anchor out of the entry address. Its
  // operations still execute only on the accepted edge. Do not insert a
  // sticky check in the body: an invalid root must never reach guest memory.
  auto &Body = Function.Blocks.front();
  uint64_t Fresh = 0;
  for (const auto &B : Function.Blocks)
    Fresh = std::max(Fresh, B.EndAddr);
  const uint64_t Span = Body.EndAddr - Body.StartAddr;
  if (!Span || Fresh >= InvalidVA || Span > InvalidVA - Fresh ||
      InvalidVA - Fresh - Span < 3 ||
      Function.Blocks.size() >
          static_cast<size_t>(std::numeric_limits<int>::max() - 2))
    return invalid("machine source alignment guard address space exhausted");
  ++Fresh;
  const uint64_t Delta = Fresh - Body.StartAddr;
  Body.StartAddr += Delta;
  Body.EndAddr += Delta;
  for (auto &Boundary : Body.InstructionBoundaries)
    Boundary.Address += Delta;
  for (auto &Op : Body.Ops)
    Op.Addr += Delta;
  const uint64_t RejectAddress = Body.EndAddr;
  std::map<int, int> IDs;
  for (size_t I = 0; I != Function.Blocks.size(); ++I)
    if (!IDs.emplace(Function.Blocks[I].Id, static_cast<int>(I + 2)).second)
      return invalid("machine source has duplicate block identities");
  for (auto &B : Function.Blocks) {
    B.Id = IDs.at(B.Id);
    for (auto *Edges : {&B.Preds, &B.Succs})
      for (auto &Id : *Edges) {
        const auto At = IDs.find(Id);
        if (At == IDs.end())
          return invalid("machine source has an unknown block identity");
        Id = At->second;
      }
  }
  Body.Preds = {0};
  const auto Finish = [](LowBlock &B, LowInstructionControl Control,
                         LowInstructionControlFlag Flags, uint64_t Target = 0) {
    LowInstructionBoundary Boundary;
    Boundary.Address = B.StartAddr;
    Boundary.Size = 1;
    Boundary.OpCount = B.Ops.size();
    Boundary.Control = Control;
    Boundary.ControlFlags = Flags;
    Boundary.Immediate = Target;
    B.InstructionBoundaries.push_back(Boundary);
    for (size_t I = 0; I != B.Ops.size(); ++I)
      B.Ops[I].Seq = static_cast<int>(I);
  };
  LowBlock Guard;
  Guard.Id = 0;
  Guard.StartAddr = Function.Entry;
  Guard.EndAddr = Function.Entry + 1;
  Guard.Succs = {1, 2};
  InstructionWriter Check{Guard.Ops, Guard.StartAddr, StateRegisters};
  if (!StateRegisters)
    Check.emit(NdOp::COPY, NdVar::reg(StatePointer, 8),
               {NdVar::reg(Parameter, 8)});
  const auto Root = Check.temporary();
  Check.load(Root, x86reg::RSP);
  const auto Low = Check.temporary();
  Check.emit(NdOp::INT_AND, Low, {Root, scalar(Alignment.Alignment - 1)});
  const auto Rejected = Check.temporary(1);
  Check.emit(NdOp::INT_NOTEQUAL, Rejected, {Low, scalar(Alignment.Residue)});
  Check.emit(NdOp::COND_BR, {}, {NdVar::cst(RejectAddress, 8), Rejected});
  Finish(Guard, LowInstructionControl::Branch,
         LowInstructionControlFlag::Branch |
             LowInstructionControlFlag::Conditional,
         RejectAddress);
  LowBlock Reject;
  Reject.Id = 1;
  Reject.StartAddr = RejectAddress;
  Reject.EndAddr = RejectAddress + 1;
  Reject.Preds = {0};
  InstructionWriter Exit{Reject.Ops, Reject.StartAddr, StateRegisters};
  if (!StateRegisters)
    Exit.emit(NdOp::COPY, NdVar::reg(x86reg::RAX, 8), {scalar(2)});
  Exit.emit(NdOp::RETURN, {},
            {StateRegisters ? scalar(2) : NdVar::reg(x86reg::RAX, 8)});
  Finish(Reject, LowInstructionControl::Return,
         LowInstructionControlFlag::Return);
  Function.Blocks.insert(Function.Blocks.begin(), std::move(Reject));
  Function.Blocks.insert(Function.Blocks.begin(), std::move(Guard));
  return llvm::Error::success();
}

llvm::Error rebuildSourcePredecessors(LowFunc &Function) {
  std::map<int, LowBlock *> Blocks;
  for (auto &Block : Function.Blocks) {
    if (!Blocks.emplace(Block.Id, &Block).second)
      return invalid("machine source has duplicate block identities");
    Block.Preds.clear();
  }
  for (const auto &Block : Function.Blocks)
    for (int Successor : Block.Succs) {
      const auto Found = Blocks.find(Successor);
      if (Found == Blocks.end())
        return invalid("machine source has an unknown block identity");
      Found->second->Preds.push_back(Block.Id);
    }
  return llvm::Error::success();
}

struct CPUQuerySourcePlan {
  std::map<int, SpecializationScalarQuery> Queries;
  int NextID = 0;
  va_t NextAddress = 0;

  llvm::Expected<int> identity() {
    if (NextID == std::numeric_limits<int>::max())
      return invalid("CPU-query source block identities exhausted");
    return NextID++;
  }

  llvm::Expected<LowBlock> block() {
    auto ID = identity();
    if (!ID)
      return ID.takeError();
    if (NextAddress >= InvalidVA - 1)
      return invalid("CPU-query source addresses exhausted");
    LowBlock Block;
    Block.Id = *ID;
    Block.StartAddr = NextAddress++;
    Block.EndAddr = NextAddress;
    return Block;
  }
};

/// Isolate complete query instructions before adding their checked source
/// transitions. Existing block entries and branch destinations keep identity.
llvm::Expected<CPUQuerySourcePlan> prepareCPUQuerySource(LowFunc &Function) {
  CPUQuerySourcePlan Plan;
  for (const auto &Block : Function.Blocks) {
    if (!Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty())
      return invalid("machine source cannot wrap exceptional edges");
    if (Block.Id < 0 || Block.Id == std::numeric_limits<int>::max())
      return invalid("CPU-query source has an invalid block identity");
    Plan.NextID = std::max(Plan.NextID, Block.Id + 1);
    Plan.NextAddress = std::max(Plan.NextAddress, Block.EndAddr);
  }
  std::vector<LowBlock> Blocks;
  for (auto &Original : Function.Blocks) {
    std::vector<std::optional<SpecializationScalarQuery>> Queries;
    for (const auto &Boundary : Original.InstructionBoundaries) {
      const auto Ops = llvm::ArrayRef(Original.Ops)
                           .slice(Boundary.FirstOp, Boundary.OpCount);
      Queries.push_back(x64::describeCPUQuery(Ops));
    }
    size_t First = 0;
    while (First != Queries.size()) {
      size_t End = First + 1;
      if (!Queries[First])
        while (End != Queries.size() && !Queries[End])
          ++End;
      LowBlock Part;
      if (!First)
        Part.Id = Original.Id;
      else {
        auto ID = Plan.identity();
        if (!ID)
          return ID.takeError();
        Part.Id = *ID;
        Blocks.back().Succs = {Part.Id};
      }
      const auto &BeginBoundary = Original.InstructionBoundaries[First];
      const auto &EndBoundary = Original.InstructionBoundaries[End - 1];
      Part.StartAddr = BeginBoundary.Address;
      Part.EndAddr = EndBoundary.Address + EndBoundary.Size;
      const size_t BeginOp = BeginBoundary.FirstOp;
      const size_t EndOp = EndBoundary.FirstOp + EndBoundary.OpCount;
      Part.Ops.assign(Original.Ops.begin() + BeginOp,
                      Original.Ops.begin() + EndOp);
      Part.InstructionBoundaries.assign(
          Original.InstructionBoundaries.begin() + First,
          Original.InstructionBoundaries.begin() + End);
      for (auto &Boundary : Part.InstructionBoundaries)
        Boundary.FirstOp -= BeginOp;
      if (End == Queries.size())
        Part.Succs = Original.Succs;
      if (Queries[First]) {
        if (Part.InstructionBoundaries.front().Control !=
                LowInstructionControl::None ||
            (End == Queries.size() && Part.Succs.size() != 1))
          return invalid("CPU-query source requires one ordinary successor");
        Plan.Queries.emplace(Part.Id, std::move(*Queries[First]));
      }
      Blocks.push_back(std::move(Part));
      First = End;
    }
  }
  Function.Blocks = std::move(Blocks);
  if (auto Error = rebuildSourcePredecessors(Function))
    return std::move(Error);
  return Plan;
}

void finishQuerySourceBlock(LowBlock &Block, LowInstructionControl Control,
                            bool Conditional = false, uint64_t Target = 0) {
  LowInstructionBoundary Boundary;
  Boundary.Address = Block.StartAddr;
  Boundary.Size = Block.EndAddr - Block.StartAddr;
  Boundary.OpCount = Block.Ops.size();
  Boundary.Control = Control;
  Boundary.ControlFlags = Control == LowInstructionControl::None
                              ? LowInstructionControlFlag::None
                          : Control == LowInstructionControl::Return
                              ? LowInstructionControlFlag::Return
                              : LowInstructionControlFlag::Branch;
  if (Conditional)
    Boundary.ControlFlags =
        Boundary.ControlFlags | LowInstructionControlFlag::Conditional;
  if (Control == LowInstructionControl::Branch)
    Boundary.Immediate = Target;
  Block.InstructionBoundaries = {Boundary};
  for (size_t I = 0; I != Block.Ops.size(); ++I) {
    Block.Ops[I].Addr = Boundary.Address;
    Block.Ops[I].Seq = static_cast<int>(I);
  }
}

llvm::Error
emitCPUQuerySource(LowBlock &Guard, const SpecializationScalarQuery &Query,
                   CPUQuerySourcePlan &Plan, std::vector<LowBlock> &Additional,
                   uint64_t Parameter, bool &Seeded, bool VectorState) {
  auto Match = Plan.block();
  if (!Match)
    return Match.takeError();
  auto Commit = Plan.block();
  if (!Commit)
    return Commit.takeError();
  auto Exhausted = Plan.block();
  if (!Exhausted)
    return Exhausted.takeError();
  auto Mismatch = Plan.block();
  if (!Mismatch)
    return Mismatch.takeError();
  Commit->Succs = Guard.Succs;
  Guard.Succs = {Exhausted->Id, Match->Id};
  Match->Succs = {Mismatch->Id, Commit->Id};
  Guard.Ops.clear();
  InstructionWriter Check{Guard.Ops, Guard.StartAddr, false, VectorState};
  if (!Seeded) {
    Check.entry(Parameter);
    Seeded = true;
  }
  for (unsigned I = 0; I != Query.Selectors.size(); ++I) {
    auto Value = Query.Selectors[I];
    if (Value.isReg()) {
      if (!nativeRegister(Value))
        return invalid("CPU-query source has an unsupported selector register");
      auto Remapped = Check.readRegister(Value);
      if (!Remapped)
        return Remapped.takeError();
      Value = *Remapped;
    } else if (Value.isTemp()) {
      if (auto Error = validateSourceTemporary(Value))
        return Error;
    } else
      Value = scalar(Value.Offset, Value.Size);
    Check.emit(NdOp::COPY, NdVar::reg(I ? QuerySubleaf : QueryLeaf, 4),
               {Value});
  }
  Check.load(NdVar::reg(QueryIndex, 8),
             offsetof(InterpreterMachineQueryStateX64V1, QueryCursor));
  const auto Count = Check.temporary();
  Check.load(Count, offsetof(InterpreterMachineQueryStateX64V1, QueryCount));
  const auto Unavailable = Check.temporary(1);
  Check.emit(NdOp::INT_LESSEQUAL, Unavailable,
             {Count, NdVar::reg(QueryIndex, 8)});
  Check.emit(NdOp::COND_BR, {},
             {NdVar::cst(Exhausted->StartAddr, 8), Unavailable});
  finishQuerySourceBlock(Guard, LowInstructionControl::Branch, true,
                         Exhausted->StartAddr);

  InstructionWriter Select{Match->Ops, Match->StartAddr};
  const auto Base = Select.temporary();
  Select.load(Base, offsetof(InterpreterMachineQueryStateX64V1, Queries));
  const auto Offset = Select.temporary();
  Select.emit(
      NdOp::INT_MULT, Offset,
      {NdVar::reg(QueryIndex, 8), scalar(sizeof(InterpreterCPUQueryX64V1))});
  Select.emit(NdOp::INT_ADD, NdVar::reg(QueryAddress, 8), {Base, Offset});
  const auto ReadWord = [](InstructionWriter &Writer, uint64_t Offset) {
    const auto Address = Writer.temporary();
    Writer.emit(NdOp::INT_ADD, Address,
                {NdVar::reg(QueryAddress, 8), scalar(Offset)});
    const auto Value = Writer.temporary(4);
    Writer.emit(NdOp::LOAD, Value, {Address});
    return Value;
  };
  const auto Leaf = ReadWord(Select, offsetof(InterpreterCPUQueryX64V1, Leaf));
  const auto Subleaf =
      ReadWord(Select, offsetof(InterpreterCPUQueryX64V1, Subleaf));
  const auto BadLeaf = Select.temporary(1);
  const auto BadSubleaf = Select.temporary(1);
  const auto Rejected = Select.temporary(1);
  Select.emit(NdOp::INT_NOTEQUAL, BadLeaf, {Leaf, NdVar::reg(QueryLeaf, 4)});
  Select.emit(NdOp::INT_NOTEQUAL, BadSubleaf,
              {Subleaf, NdVar::reg(QuerySubleaf, 4)});
  Select.emit(NdOp::BOOL_OR, Rejected, {BadLeaf, BadSubleaf});
  Select.emit(NdOp::COND_BR, {},
              {NdVar::cst(Mismatch->StartAddr, 8), Rejected});
  finishQuerySourceBlock(*Match, LowInstructionControl::Branch, true,
                         Mismatch->StartAddr);

  InstructionWriter Answers{Commit->Ops, Commit->StartAddr};
  const auto Registers = x64::cpuQueryResultRegisters();
  for (unsigned I = 0; I != std::size(Registers); ++I) {
    const auto Answer =
        ReadWord(Answers, offsetof(InterpreterCPUQueryX64V1, Answers) +
                              I * sizeof(uint32_t));
    Answers.emit(NdOp::INT_ZEXT, NdVar::reg(GuestBase + Registers[I], 8),
                 {Answer});
    for (const auto &Binding : Query.Bindings)
      if (Binding.Result == I && Binding.Destination.isTemp()) {
        if (auto Error = validateSourceTemporary(Binding.Destination))
          return Error;
        Answers.emit(NdOp::COPY, Binding.Destination, {Answer});
      }
  }
  const auto Next = Answers.temporary();
  Answers.emit(NdOp::INT_ADD, Next, {NdVar::reg(QueryIndex, 8), scalar(1)});
  Answers.store(offsetof(InterpreterMachineQueryStateX64V1, QueryCursor), Next);
  // A single-successor block needs no new native branch instruction. Its
  // synthetic boundary explicitly describes the ordinary source fallthrough.
  finishQuerySourceBlock(*Commit, LowInstructionControl::None);
  const auto Reject = [VectorState](LowBlock &Block, uint64_t Status) {
    InstructionWriter Writer{Block.Ops, Block.StartAddr, false, VectorState};
    Writer.emit(NdOp::COPY, NdVar::reg(ProfileStatus, 8), {scalar(Status)});
    Writer.leave();
    finishQuerySourceBlock(Block, LowInstructionControl::Return);
  };
  Reject(*Exhausted, 3);
  Reject(*Mismatch, 4);
  Additional.push_back(std::move(*Match));
  Additional.push_back(std::move(*Commit));
  Additional.push_back(std::move(*Exhausted));
  Additional.push_back(std::move(*Mismatch));
  return llvm::Error::success();
}

} // namespace

std::optional<InterpreterMachineSourceABIInfo>
interpreterMachineSourceABIInfo(InterpreterMachineSourceABIKind Kind) {
  switch (Kind) {
  case InterpreterMachineSourceABIKind::ScalarV1:
    return InterpreterMachineSourceABIInfo{
        "x64-machine-state-v1", sizeof(InterpreterMachineStateX64V1)};
  case InterpreterMachineSourceABIKind::QueriesV1:
    return InterpreterMachineSourceABIInfo{
        "x64-machine-query-state-v1",
        sizeof(InterpreterMachineQueryStateX64V1)};
  case InterpreterMachineSourceABIKind::QueriesXMMV2:
    return InterpreterMachineSourceABIInfo{
        "x64-machine-query-state-v2",
        sizeof(InterpreterMachineQueryStateX64V2)};
  }
  return std::nullopt;
}

llvm::Error validateInterpreterMachineStateX64V1(
    const InterpreterMachineStateX64V1 &State) {
  if (!FlagProfile::validEntry(State.RFlags))
    return invalid("machine source requires canonical nonfaulting x64 "
                   "user-mode entry flags");
  return llvm::Error::success();
}

static llvm::Expected<InterpreterMachineSource>
buildMachineSource(const LowFunc &Residual, BinaryFormat SourceFormat,
                   InterpreterMachineStateProfile Profile, bool StateRegisters,
                   std::optional<InterpreterEntryAlignment> EntryAlignment,
                   InterpreterMachineStateLayout Layout,
                   bool CPUQueryTape = false, bool VectorState = false) {
  if (hasLowRawEnvironmentRequirement(Residual))
    return invalid(
        "raw environment requirement has no machine-source contract");
  if (Layout != InterpreterMachineStateLayout::InlineReturns &&
      Layout != InterpreterMachineStateLayout::SharedGPRExit)
    return invalid("unsupported interpreter machine-state layout");
  if (EntryAlignment && !EntryAlignment->valid())
    return invalid("invalid machine source entry alignment");
  if (Profile != InterpreterMachineStateProfile::UserX64NoFaultV1)
    return invalid("unsupported interpreter machine-state profile");
  if (SourceFormat != BinaryFormat::ELF && SourceFormat != BinaryFormat::COFF &&
      SourceFormat != BinaryFormat::MachO)
    return invalid("unsupported interpreter machine-source ABI format");
  if (Residual.Blocks.empty() ||
      Residual.Blocks.front().StartAddr != Residual.Entry ||
      !Residual.Blocks.front().Preds.empty())
    return invalid("machine source requires one predecessor-free entry anchor");
  // Preds is descriptive metadata. Check actual successor edges and direct
  // transfers too: reentering the anchor would reload the source input state.
  for (const auto &B : Residual.Blocks) {
    if (B.hasSucc(Residual.Blocks.front().Id))
      return invalid("machine source entry anchor has a backedge");
    for (const auto &Op : B.Ops)
      if ((Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) &&
          Op.NumInputs && Op.Inputs[0].isConst() &&
          Op.Inputs[0].Offset == Residual.Entry)
        return invalid("machine source entry anchor has a backedge");
  }
  if (Residual.ExceptionMetadata || !Residual.JumpTables.empty())
    return invalid("machine source requires a complete ordinary residual CFG");
  if (auto Error = validateLowInstructionBoundaries(
          Residual, LowInstructionBoundaryRequirement::Required))
    return std::move(Error);
  for (const auto &Range : Residual.FunctionTemporaries)
    if (Range.Offset < ScratchEnd &&
        Range.Offset + Range.Bytes - 1 >= ScratchBase)
      return invalid("function temporary overlaps wrapper scratch");

  const uint64_t ParameterRegister =
      SourceFormat == BinaryFormat::COFF ? x86reg::RCX : x86reg::RDI;
  InterpreterMachineSource Result;
  if (StateRegisters) {
    // The analysis model needs only the executable graph and declared entry
    // roots. Do not copy unbounded diagnostic/provenance trees or strings.
    Result.Function.Entry = Residual.Entry;
    Result.Function.Blocks = Residual.Blocks;
    Result.Function.FunctionTemporaries = Residual.FunctionTemporaries;
    Result.Function.ModuleAnalysisRoots = Residual.ModuleAnalysisRoots;
    Result.Function.OrdinaryModuleAnalysisRoots =
        Residual.OrdinaryModuleAnalysisRoots;
  } else
    Result.Function = Residual;
  CPUQuerySourcePlan Queries;
  if (CPUQueryTape) {
    auto Prepared = prepareCPUQuerySource(Result.Function);
    if (!Prepared)
      return Prepared.takeError();
    Queries = std::move(*Prepared);
  }
  auto SharedReturn =
      Layout == InterpreterMachineStateLayout::SharedGPRExit
          ? sharedReturnBlock(Result.Function, EntryAlignment.has_value())
          : std::nullopt;
  if (CPUQueryTape && SharedReturn) {
    if (SharedReturn->Id == std::numeric_limits<int>::max())
      return invalid("CPU-query source block identities exhausted");
    Queries.NextID = std::max(Queries.NextID, SharedReturn->Id + 1);
    Queries.NextAddress = std::max(Queries.NextAddress, SharedReturn->EndAddr);
  }
  std::vector<LowBlock> QueryBlocks;
  bool HasReturn = false;
  bool Seeded = false;
  for (LowBlock &Block : Result.Function.Blocks) {
    if (!Block.ExceptionalSuccs.empty() || !Block.ExceptionalPreds.empty())
      return invalid("machine source cannot wrap exceptional edges");
    if (const auto Query = Queries.Queries.find(Block.Id);
        Query != Queries.Queries.end()) {
      if (auto Error =
              emitCPUQuerySource(Block, Query->second, Queries, QueryBlocks,
                                 ParameterRegister, Seeded, VectorState))
        return std::move(Error);
      continue;
    }
    const auto Original = std::move(Block.Ops);
    Block.Ops.clear();
    for (LowInstructionBoundary &Boundary : Block.InstructionBoundaries) {
      const size_t First = Block.Ops.size();
      InstructionWriter Writer{Block.Ops, Boundary.Address, StateRegisters,
                               VectorState};
      InstructionTemporaryBytes TemporaryBytes;
      if (!Seeded) {
        Writer.entry(ParameterRegister);
        Seeded = true;
      }
      for (uint64_t Index = Boundary.FirstOp;
           Index != Boundary.FirstOp + Boundary.OpCount; ++Index) {
        LowOp Op = Original[Index];
        if (Op.MemoryOrdering != NdMemoryOrdering::None ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          return invalid(
              "machine source cannot wrap ordered or special memory");
        const NdVar NativeOutput = Op.Output;
        bool CaptureTemporary = false;
        const bool PartialGPROutput =
            NativeOutput.isReg() && NativeOutput.Offset < 128 &&
            NativeOutput.Size && NativeOutput.Size < 8;
        if (PartialGPROutput && Op.Opcode == NdOp::SUBBYTES &&
            (Op.NumInputs != 2 || !Op.Inputs[1].isConst()))
          return invalid("machine source requires an exact GPR slice offset");
        const bool LowSliceView =
            Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
            Op.Inputs[0].isReg() && Op.Inputs[1].isConst() &&
            isSameRegisterLowSlice(NativeOutput.Offset, NativeOutput.Size,
                                   Op.Inputs[0].Offset, Op.Inputs[0].Size,
                                   Op.Inputs[1].Offset);
        const auto Remap = [&](NdVar &Value, bool Input) -> llvm::Error {
          if (!Value.Size)
            return llvm::Error::success();
          if (Value.isReg()) {
            const auto Vector = VectorState ? xmmIndex(Value) : std::nullopt;
            if (!nativeRegister(Value) && !Vector)
              return invalid(
                  "machine source encountered an unsupported register");
            if (Input) {
              auto Read = Writer.readRegister(Value);
              if (!Read)
                return Read.takeError();
              Value = *Read;
            } else if (PartialGPROutput) {
              Value = Writer.temporary(Value.Size);
            } else if (Vector) {
              Value.Offset = GuestVectorBase + *Vector * 16;
            } else {
              Value.Offset += GuestBase;
            }
          } else if (Value.isTemp()) {
            if (auto Error = validateSourceTemporary(Value, VectorState))
              return Error;
            bool FunctionTemporary = false;
            if (!StateRegisters) {
              // Low-to-Med names temporary scalars by (offset, width). Byte
              // aliases have exact semantics in the raw-state proof model,
              // but cannot silently become independent source variables.
              const auto &Ranges = Residual.FunctionTemporaries;
              const auto It = std::lower_bound(
                  Ranges.begin(), Ranges.end(), Value.Offset,
                  [](const LowFunctionTemporary &Range, uint64_t Offset) {
                    return Range.Offset + Range.Bytes - 1 < Offset;
                  });
              if (It != Ranges.end() &&
                  Value.Offset + Value.Size - 1 >= It->Offset &&
                  (Value.Offset != It->Offset || Value.Size != It->Bytes))
                return invalid("machine source requires exact function "
                               "temporary operands, without byte aliases");
              FunctionTemporary =
                  It != Ranges.end() && Value.Offset == It->Offset;
            }
            if (VectorState && !FunctionTemporary) {
              if (Input) {
                auto Read = TemporaryBytes.read(Value, Writer);
                if (!Read)
                  return Read.takeError();
                Value = *Read;
              } else {
                Value = Writer.temporary(Value.Size);
                CaptureTemporary = true;
              }
            }
          } else if (!Value.isConst()) {
            return invalid("machine source requires scalar LowIR operands");
          }
          return llvm::Error::success();
        };
        if (auto Error = Remap(Op.Output, false))
          return std::move(Error);
        if (Op.NumInputs > std::size(Op.Inputs))
          return invalid("machine source has an invalid operation arity");
        for (unsigned Input = 0; Input != Op.NumInputs; ++Input) {
          // Machine-state values name the original guest mapping, not a
          // relocated source object. Drop image provenance at this ABI
          // boundary before either source backend can reinterpret the bits.
          // Direct CFG destinations retain their separate label identity.
          if (Op.Inputs[Input].isConst() &&
              !(Input == 0 &&
                (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR)))
            Op.Inputs[Input] =
                scalar(Op.Inputs[Input].Offset, Op.Inputs[Input].Size);
          if (auto Error = Remap(Op.Inputs[Input], true))
            return std::move(Error);
        }
        switch (Op.Opcode) {
        case NdOp::CALL:
        case NdOp::INDIR_CALL:
        case NdOp::INDIR_BR:
          return invalid(
              "machine source requires resolved call-free control flow");
        case NdOp::RETURN:
          if (Op.Output.Size || Op.NumInputs > 1 ||
              Index + 1 != Boundary.FirstOp + Boundary.OpCount)
            return invalid("machine source has a malformed return boundary");
          if (SharedReturn) {
            // Preserve flag packing at each original return. Only the GPR
            // writeback is shared, so source cleanup need not expand a packed
            // flags expression across the newly introduced join.
            Writer.store(offsetof(InterpreterMachineStateX64V1, RFlags),
                         Writer.packedFlags());
            Writer.emit(NdOp::BRANCH, {},
                        {NdVar::cst(SharedReturn->StartAddr, 8)});
            Boundary.Control = LowInstructionControl::Branch;
            Boundary.ControlFlags = LowInstructionControlFlag::Branch;
            Boundary.TargetMode = LowInstructionTargetMode::Preserve;
            Boundary.Immediate = SharedReturn->StartAddr;
            Block.Succs = {SharedReturn->Id};
            SharedReturn->Preds.push_back(Block.Id);
          } else {
            Writer.leave();
          }
          HasReturn = true;
          break;
        case NdOp::INTRINSIC: {
          if (VectorState) {
            SpecializationOptions Options;
            Options.ExplicitMachineState = true;
            Options.NormalNonfaultingExecution = true;
            if (const auto Valid =
                    x64::alignment::constantValidity(Op, Options)) {
              if (!*Valid)
                return invalid(
                    "machine source constant address violates alignment");
              break;
            }
          }
          auto Transition = detail::lowerX64MachineFlags(
              Op, NdVar::reg(SystemFlags, 8),
              [&](uint16_t Size) { return Writer.temporary(Size); });
          if (!Transition)
            return invalid(
                "machine source supports only canonical x64 flag intrinsics");
          if (Transition->Rejected) {
            if (Op.Inputs[1].isConst() &&
                (Op.Inputs[1].Offset & FlagProfile::UserRejectedWriteMask) != 0)
              return invalid(
                  "POPFQ image violates the nonfaulting source profile");
          }
          Block.Ops.insert(Block.Ops.end(), Transition->Ops.begin(),
                           Transition->Ops.end());
          if (Transition->Rejected) {
            const NdVar Extended = Writer.temporary();
            Writer.emit(NdOp::INT_ZEXT, Extended, {*Transition->Rejected});
            Writer.emit(NdOp::INT_OR, NdVar::reg(ProfileStatus, 8),
                        {NdVar::reg(ProfileStatus, 8), Extended});
          }
          break;
        }
        default:
          Block.Ops.push_back(Op);
          break;
        }
        // Remapped identities are deliberately outside the native register
        // banks. Materialize lane semantics before LowToMed, so its native
        // alias table is never asked to recognize synthetic AH/EAX/etc.
        if (PartialGPROutput)
          if (auto Error =
                  Writer.writeRegister(NativeOutput, Op.Output, LowSliceView))
            return std::move(Error);
        if (CaptureTemporary)
          TemporaryBytes.write(NativeOutput, Op.Output);
      }
      Boundary.FirstOp = First;
      Boundary.OpCount = Block.Ops.size() - First;
      for (size_t Index = First; Index != Block.Ops.size(); ++Index)
        Block.Ops[Index].Seq = static_cast<int>(Index - First);
    }
  }
  if (!HasReturn)
    return invalid("machine source has no ordinary return boundary");
  if (SharedReturn) {
    InstructionWriter Writer{SharedReturn->Ops, SharedReturn->StartAddr,
                             StateRegisters, VectorState};
    Writer.leave(false);
    LowInstructionBoundary Boundary;
    Boundary.Address = SharedReturn->StartAddr;
    Boundary.Size = 1;
    Boundary.OpCount = SharedReturn->Ops.size();
    Boundary.Control = LowInstructionControl::Return;
    Boundary.ControlFlags = LowInstructionControlFlag::Return;
    SharedReturn->InstructionBoundaries.push_back(Boundary);
    for (size_t I = 0; I != SharedReturn->Ops.size(); ++I)
      SharedReturn->Ops[I].Seq = static_cast<int>(I);
    Result.Function.Blocks.push_back(std::move(*SharedReturn));
  }
  if (CPUQueryTape) {
    for (auto &Block : QueryBlocks)
      Result.Function.Blocks.push_back(std::move(Block));
    if (auto Error = rebuildSourcePredecessors(Result.Function))
      return std::move(Error);
  }
  if (EntryAlignment)
    if (auto Error = addEntryAlignmentGuard(Result.Function, ParameterRegister,
                                            StateRegisters, *EntryAlignment))
      return std::move(Error);
  if (auto Error = validateLowInstructionBoundaries(
          Result.Function, LowInstructionBoundaryRequirement::Required))
    return std::move(Error);

  auto &ABI = Result.SourceABI;
  ABI.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  ABI.Architecture = Arch::X64;
  ABI.HasExplicitABI = true;
  ABI.ReturnType = NdType::makeInt(8, false);
  ABI.ReturnLocation = {SourceABICarrierKind::IntegerRegister, x86reg::RAX, 0,
                        8};
  SourceParameterTypeHint Parameter;
  Parameter.Name = VectorState    ? "machine_query_vector_state"
                   : CPUQueryTape ? "machine_query_state"
                                  : "machine_state";
  Parameter.Type = NdType::makePtr(NdType::makeInt(1, false));
  Parameter.Location = {SourceABICarrierKind::IntegerRegister,
                        ParameterRegister, 0, 8};
  ABI.Parameters.push_back(std::move(Parameter));
  std::string Diagnostic;
  if (!validateSourceABI(ABI, Diagnostic))
    return invalid(Diagnostic);
  return Result;
}

llvm::Expected<InterpreterMachineSource>
wrapInterpreterMachineStateX64(const LowFunc &Residual,
                               BinaryFormat SourceFormat,
                               InterpreterMachineStateProfile Profile) {
  return wrapInterpreterMachineStateX64(Residual, SourceFormat, Profile,
                                        std::nullopt);
}

llvm::Expected<InterpreterMachineStateModel>
modelInterpreterMachineStateX64(const LowFunc &Residual,
                                InterpreterMachineStateProfile Profile,
                                uint64_t MaxOperations) {
  return modelInterpreterMachineStateX64(Residual, Profile, MaxOperations,
                                         std::nullopt);
}

llvm::Expected<InterpreterMachineSource> wrapInterpreterMachineStateX64(
    const LowFunc &Residual, BinaryFormat SourceFormat,
    InterpreterMachineStateProfile Profile,
    std::optional<InterpreterEntryAlignment> EntryAlignment) {
  return wrapInterpreterMachineStateX64(
      Residual, SourceFormat, Profile, EntryAlignment,
      InterpreterMachineStateLayout::InlineReturns);
}

llvm::Expected<InterpreterMachineSource> wrapInterpreterMachineStateX64(
    const LowFunc &Residual, BinaryFormat SourceFormat,
    InterpreterMachineStateProfile Profile,
    std::optional<InterpreterEntryAlignment> EntryAlignment,
    InterpreterMachineStateLayout Layout) {
  return buildMachineSource(Residual, SourceFormat, Profile, false,
                            EntryAlignment, Layout);
}

llvm::Expected<InterpreterMachineSource> wrapInterpreterMachineQueryStateX64(
    const LowFunc &Residual, BinaryFormat SourceFormat,
    InterpreterMachineStateProfile Profile,
    std::optional<InterpreterEntryAlignment> EntryAlignment,
    InterpreterMachineStateLayout Layout) {
  return buildMachineSource(Residual, SourceFormat, Profile, false,
                            EntryAlignment, Layout, true);
}

llvm::Expected<InterpreterMachineSource> wrapInterpreterMachineQueryStateX64V2(
    const LowFunc &Residual, BinaryFormat SourceFormat,
    InterpreterMachineStateProfile Profile,
    std::optional<InterpreterEntryAlignment> EntryAlignment,
    InterpreterMachineStateLayout Layout) {
  return buildMachineSource(Residual, SourceFormat, Profile, false,
                            EntryAlignment, Layout, true, true);
}

llvm::Expected<InterpreterMachineSource> wrapInterpreterMachineSourceX64(
    const LowFunc &Residual, InterpreterMachineSourceABIKind Kind,
    BinaryFormat SourceFormat, InterpreterMachineStateProfile Profile,
    std::optional<InterpreterEntryAlignment> EntryAlignment,
    InterpreterMachineStateLayout Layout) {
  switch (Kind) {
  case InterpreterMachineSourceABIKind::ScalarV1:
    return wrapInterpreterMachineStateX64(Residual, SourceFormat, Profile,
                                          EntryAlignment, Layout);
  case InterpreterMachineSourceABIKind::QueriesV1:
    return wrapInterpreterMachineQueryStateX64(Residual, SourceFormat, Profile,
                                               EntryAlignment, Layout);
  case InterpreterMachineSourceABIKind::QueriesXMMV2:
    return wrapInterpreterMachineQueryStateX64V2(
        Residual, SourceFormat, Profile, EntryAlignment, Layout);
  }
  return invalid("unknown interpreter machine source ABI");
}

llvm::Expected<InterpreterMachineStateModel> modelInterpreterMachineStateX64(
    const LowFunc &Residual, InterpreterMachineStateProfile Profile,
    uint64_t MaxOperations,
    std::optional<InterpreterEntryAlignment> EntryAlignment) {
  return modelInterpreterMachineStateX64(
      Residual, Profile, MaxOperations, EntryAlignment,
      InterpreterMachineStateLayout::InlineReturns);
}

llvm::Expected<InterpreterMachineStateModel> modelInterpreterMachineStateX64(
    const LowFunc &Residual, InterpreterMachineStateProfile Profile,
    uint64_t MaxOperations,
    std::optional<InterpreterEntryAlignment> EntryAlignment,
    InterpreterMachineStateLayout Layout) {
  uint64_t Remaining = MaxOperations;
  const auto Charge = [&](uint64_t Count) {
    if (Count > Remaining)
      return false;
    Remaining -= Count;
    return true;
  };
  if (!Charge(Residual.Blocks.size()) ||
      !Charge(Residual.FunctionTemporaries.size()) ||
      !Charge(Residual.ModuleAnalysisRoots.size()) ||
      !Charge(Residual.OrdinaryModuleAnalysisRoots.size()))
    return invalid("machine-state model input budget exhausted");
  for (const auto &B : Residual.Blocks)
    if (!Charge(B.Ops.size()) || !Charge(B.InstructionBoundaries.size()) ||
        !Charge(B.Preds.size()) || !Charge(B.Succs.size()) ||
        !Charge(B.ExceptionalPreds.size()) ||
        !Charge(B.ExceptionalSuccs.size()))
      return invalid("machine-state model input budget exhausted");
  auto Generated = buildMachineSource(Residual, BinaryFormat::ELF, Profile,
                                      true, EntryAlignment, Layout);
  if (!Generated)
    return Generated.takeError();
  InterpreterMachineStateModel Result;
  Result.Function = std::move(Generated->Function);
  Remaining = MaxOperations;
  for (const auto &B : Result.Function.Blocks) {
    if (!Charge(B.Ops.size()))
      return invalid("machine-state model operation budget exhausted");
    for (const auto &Boundary : B.InstructionBoundaries) {
      LowInstructionUndefinedEffects Effects;
      Effects.Coverage = LowUndefinedCoverage::Complete;
      Effects.OpCount = Boundary.OpCount;
      Effects.OperationDigest =
          lowUndefinedOperationDigest(llvm::ArrayRef<LowOp>(B.Ops).slice(
              Boundary.FirstOp, Boundary.OpCount));
      Result.Instructions.push_back({B.Id, Boundary, std::move(Effects)});
    }
  }
  return Result;
}

} // namespace neverd::analysis
