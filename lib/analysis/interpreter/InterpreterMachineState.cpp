//===- InterpreterMachineState.cpp - Explicit recovery source ABI --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/InterpreterMachineState.h"

#include "X64UserFlags.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Regs.h"

#include "llvm/Support/Errc.h"

#include <initializer_list>
#include <limits>
#include <map>
#include <utility>

namespace neverd::analysis {
namespace {

// Disjoint from every architectural register bank. These are initialized
// scalar identities for SSA construction, never additional source inputs.
constexpr uint64_t GuestBase = uint64_t{1} << 32;
constexpr uint64_t StatePointer = GuestBase + 256;
constexpr uint64_t SystemFlags = GuestBase + 264;
constexpr uint64_t ProfileStatus = GuestBase + 272;
constexpr uint64_t ScratchBase = uint64_t{1} << 61;
constexpr uint64_t ScratchEnd = uint64_t{1} << 62;
using FlagProfile = detail::X64UserFlags;
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

struct InstructionWriter {
  std::vector<LowOp> &Ops;
  va_t Address;
  bool StateRegisters = false;
  uint64_t NextTemporary = ScratchBase;

  NdVar temporary(uint16_t Bytes = 8) {
    const auto Value = NdVar::tmp(NextTemporary, Bytes);
    NextTemporary += 8;
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

  void leave() {
    for (uint64_t Index = 0; Index != 16; ++Index)
      store(Index * 8, NdVar::reg(GuestBase + Index * 8, 8));
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

} // namespace

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
                   std::optional<InterpreterEntryAlignment> EntryAlignment) {
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

  const uint64_t ParameterRegister =
      SourceFormat == BinaryFormat::COFF ? x86reg::RCX : x86reg::RDI;
  InterpreterMachineSource Result;
  if (StateRegisters) {
    // The analysis model needs only the executable graph and declared entry
    // roots. Do not copy unbounded diagnostic/provenance trees or strings.
    Result.Function.Entry = Residual.Entry;
    Result.Function.Blocks = Residual.Blocks;
    Result.Function.ModuleAnalysisRoots = Residual.ModuleAnalysisRoots;
    Result.Function.OrdinaryModuleAnalysisRoots =
        Residual.OrdinaryModuleAnalysisRoots;
  } else
    Result.Function = Residual;
  bool HasReturn = false;
  bool Seeded = false;
  for (LowBlock &Block : Result.Function.Blocks) {
    if (!Block.ExceptionalSuccs.empty() || !Block.ExceptionalPreds.empty())
      return invalid("machine source cannot wrap exceptional edges");
    const auto Original = std::move(Block.Ops);
    Block.Ops.clear();
    for (LowInstructionBoundary &Boundary : Block.InstructionBoundaries) {
      const size_t First = Block.Ops.size();
      InstructionWriter Writer{Block.Ops, Boundary.Address, StateRegisters};
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
            if (!nativeRegister(Value))
              return invalid(
                  "machine source encountered an unsupported register");
            if (Input) {
              auto Read = Writer.readRegister(Value);
              if (!Read)
                return Read.takeError();
              Value = *Read;
            } else if (PartialGPROutput) {
              Value = Writer.temporary(Value.Size);
            } else {
              Value.Offset += GuestBase;
            }
          } else if (Value.isTemp()) {
            if (Value.Size > 8 ||
                Value.Offset >
                    std::numeric_limits<uint64_t>::max() - (Value.Size - 1))
              return invalid("machine source has an invalid temporary range");
            if (Value.Offset < ScratchEnd &&
                Value.Offset + Value.Size - 1 >= ScratchBase)
              return invalid(
                  "machine source temporary overlaps wrapper scratch");
          } else if (!Value.isConst()) {
            return invalid("machine source requires scalar LowIR operands");
          }
          return llvm::Error::success();
        };
        if (auto Error = Remap(Op.Output, false))
          return std::move(Error);
        if (Op.NumInputs > 6)
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
          Writer.leave();
          HasReturn = true;
          break;
        case NdOp::INTRINSIC: {
          auto Transition = detail::lowerX64UserFlags(
              Op, NdVar::reg(SystemFlags, 8),
              [&](uint16_t Size) { return Writer.temporary(Size); });
          if (!Transition)
            return invalid(
                "machine source supports only canonical x64 flag intrinsics");
          if (Transition->Rejected) {
            if (Op.Inputs[1].isConst() &&
                (Op.Inputs[1].Offset & FlagProfile::RejectedWriteMask) != 0)
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
      }
      Boundary.FirstOp = First;
      Boundary.OpCount = Block.Ops.size() - First;
      for (size_t Index = First; Index != Block.Ops.size(); ++Index)
        Block.Ops[Index].Seq = static_cast<int>(Index - First);
    }
  }
  if (!HasReturn)
    return invalid("machine source has no ordinary return boundary");
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
  Parameter.Name = "machine_state";
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
  return buildMachineSource(Residual, SourceFormat, Profile, false,
                            EntryAlignment);
}

llvm::Expected<InterpreterMachineStateModel> modelInterpreterMachineStateX64(
    const LowFunc &Residual, InterpreterMachineStateProfile Profile,
    uint64_t MaxOperations,
    std::optional<InterpreterEntryAlignment> EntryAlignment) {
  uint64_t Remaining = MaxOperations;
  const auto Charge = [&](uint64_t Count) {
    if (Count > Remaining)
      return false;
    Remaining -= Count;
    return true;
  };
  if (!Charge(Residual.Blocks.size()) ||
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
                                      true, EntryAlignment);
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
