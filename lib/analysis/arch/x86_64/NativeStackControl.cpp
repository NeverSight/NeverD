//===- NativeStackControl.cpp - Physical near-call semantics --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NativeStackControl.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/Errc.h"

#include <iterator>
#include <limits>
#include <set>

namespace neverd::analysis {
namespace {

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s", Message);
}

bool validRange(const NdVar &Value) {
  return Value.Size && Value.Offset <= InvalidVA - (Value.Size - 1);
}

bool validValue(const NdVar &Value) {
  return Value.Size && Value.Size <= 8 &&
         (Value.isConst() ||
          ((Value.isReg() || Value.isTemp()) && validRange(Value)));
}

bool overlaps(const NdVar &Left, const NdVar &Right) {
  return Left.Space == Right.Space && Left.Size && Right.Size &&
         Left.Offset <= Right.Offset + (Right.Size - 1) &&
         Right.Offset <= Left.Offset + (Left.Size - 1);
}

bool ordinary(const LowOp &Op) {
  return Op.MemoryOrdering == NdMemoryOrdering::None &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
}

bool addressOperation(const LowOp &Op, bool FinalLoad) {
  if (!ordinary(Op) || !Op.Output.isTemp() || !validValue(Op.Output))
    return false;
  switch (Op.Opcode) {
  case NdOp::COPY:
    return Op.NumInputs == 1 && Op.Output.Size == Op.Inputs[0].Size;
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_MULT:
    return Op.NumInputs == 2 && Op.Output.Size == Op.Inputs[0].Size &&
           Op.Output.Size == Op.Inputs[1].Size;
  case NdOp::INT_ZEXT:
    return Op.NumInputs == 1 && Op.Output.Size >= Op.Inputs[0].Size;
  case NdOp::SUBBYTES:
    return Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
           Op.Inputs[1].Offset < Op.Inputs[0].Size &&
           Op.Output.Size <= Op.Inputs[0].Size - Op.Inputs[1].Offset;
  case NdOp::LOAD:
    return FinalLoad && Op.NumInputs == 1 && Op.Output.Size == 8 &&
           Op.Inputs[0].Size == 8;
  default:
    return false;
  }
}

llvm::Error validateBoundary(const SpecializationInstruction &Instruction) {
  const auto &B = Instruction.Origin;
  if (B.Mode != InstructionMode::Default ||
      B.TargetMode != LowInstructionTargetMode::Preserve || !B.Size ||
      B.Address == InvalidVA || B.Size > InvalidVA - B.Address ||
      Instruction.Fallthrough.Address != B.Address + B.Size ||
      Instruction.Fallthrough.Mode != B.Mode || Instruction.Ops.empty() ||
      Instruction.Ops.size() >
          static_cast<size_t>(std::numeric_limits<int>::max() - 4) ||
      (!Instruction.NativeBytes.empty() &&
       Instruction.NativeBytes.size() != B.Size))
    return invalid("native stack control requires an exact instruction extent");
  for (size_t I = 0; I < Instruction.Ops.size(); ++I) {
    const auto &Op = Instruction.Ops[I];
    if (Op.NumInputs > std::size(Op.Inputs) || Op.Seq != static_cast<int>(I) ||
        !ordinary(Op))
      return invalid("native stack control has malformed operation metadata");
    if (Op.Output.Size && !validValue(Op.Output))
      return invalid("native stack control has an invalid output operand");
    for (unsigned J = 0; J < Op.NumInputs; ++J)
      if (!validValue(Op.Inputs[J]))
        return invalid("native stack control has an invalid input operand");
  }
  LowBlock Block;
  Block.StartAddr = B.Address;
  Block.EndAddr = Instruction.Fallthrough.Address;
  Block.Ops = Instruction.Ops;
  Block.InstructionBoundaries.push_back(B);
  return validateLowInstructionBoundaries(
      Block, LowInstructionBoundaryRequirement::Required);
}

} // namespace

llvm::Error
validateX64TerminalImportDomain(const SpecializationOptions &Options) {
  if (Options.X64TerminalImports.empty())
    return llvm::Error::success();
  if (!Options.ExplicitMachineState || !Options.NormalNonfaultingExecution ||
      !Options.X64CetDisabled || Options.ByteOrder != llvm::endianness::little)
    return invalid(
        "terminal imports require explicit nonfaulting CET-disabled x64 state");
  const auto Exhausted = [] {
    return llvm::createStringError(
        std::make_error_code(std::errc::value_too_large),
        "terminal import declaration budget exhausted");
  };
  if (Options.X64TerminalImports.size() > Options.MaxImagePreparationRecords)
    return Exhausted();
  std::set<va_t> Slots;
  uint64_t Bytes = 0;
  for (const auto &Binding : Options.X64TerminalImports) {
    for (size_t Size : {Binding.Module.size(), Binding.Symbol.size()}) {
      if (Size > Options.MaxImagePreparationBytes - Bytes)
        return Exhausted();
      Bytes += Size;
    }
    if (Binding.Version != 1 || !Binding.Slot || Binding.Slot > InvalidVA - 8 ||
        Binding.Module.empty() || Binding.Symbol.empty() ||
        Binding.Module.find('\0') != std::string::npos ||
        Binding.Symbol.find('\0') != std::string::npos ||
        !Slots.insert(Binding.Slot).second)
      return invalid("invalid terminal import domain");
  }
  return llvm::Error::success();
}

llvm::Error
validateX64TerminalImportCall(const SpecializationInstruction &Instruction) {
  if (!Instruction.TerminalImport ||
      Instruction.TerminalImport->Binding.Version != 1 ||
      (Instruction.TerminalImport->FixedImageDigest.size() != 64 ||
       !llvm::all_of(Instruction.TerminalImport->FixedImageDigest,
                     [](char C) {
                       return (C >= '0' && C <= '9') || (C >= 'A' && C <= 'F');
                     })) ||
      !Instruction.IsNativeCall ||
      Instruction.NativeStackControl !=
          SpecializationNativeStackControl::Call ||
      Instruction.ProfileProjection != InterpreterProfileProjection::None ||
      Instruction.NativeBytes.size() != 6 || Instruction.Origin.Size != 6 ||
      Instruction.NativeBytes[0] != 0xff || Instruction.NativeBytes[1] != 0x15)
    return invalid(
        "terminal import requires an authenticated canonical memory call");
  if (auto Error = validateBoundary(Instruction))
    return Error;
  const auto Displacement = static_cast<int32_t>(
      llvm::support::endian::read32le(Instruction.NativeBytes.data() + 2));
  const auto Slot = Instruction.Fallthrough.Address +
                    static_cast<uint64_t>(static_cast<int64_t>(Displacement));
  if (Slot != Instruction.TerminalImport->Binding.Slot)
    return invalid("terminal import slot differs from the encoded target");
  Decoder Decode;
  Decode.init(Arch::X64);
  Decode.setStrict(true);
  DecodedInsn Insn{};
  if (!Decode.decodeOneForLift(Instruction.NativeBytes.data(), 6,
                               Instruction.Origin.Address, Insn) ||
      Insn.Size != 6)
    return invalid("terminal import call could not be strictly decoded");
  std::vector<LowOp> Expected;
  if (!Decode.liftX64MemoryCallToLow(Insn, Expected) ||
      lowUndefinedOperationDigest(Expected) !=
          lowUndefinedOperationDigest(Instruction.Ops))
    return invalid(
        "terminal import call bytes differ from the lifted operations");
  return llvm::Error::success();
}

llvm::Expected<NativeStackExpansion>
expandNativeStackControl(const SpecializationInstruction &Instruction,
                         NdVar Stack, NdVar Scratch,
                         NativeReturnExpansion ReturnMode,
                         llvm::ArrayRef<NdVar> ReservedTemporaries) {
  if (!Stack.isReg() || Stack.Size != 8 || !validRange(Stack) ||
      !Scratch.isTemp() || Scratch.Size != 8 || !validRange(Scratch))
    return invalid(
        "native stack control requires an eight-byte stack and scratch");
  if (ReturnMode != NativeReturnExpansion::InternalTransfer &&
      ReturnMode != NativeReturnExpansion::OuterFunctionBoundary)
    return invalid("native stack control has an unknown return expansion");
  if (auto Error = validateBoundary(Instruction))
    return std::move(Error);
  const auto CheckScratch = [&](const NdVar &Value) -> llvm::Error {
    if (!Value.Size || !Value.isTemp())
      return llvm::Error::success();
    if (!validRange(Value) || overlaps(Scratch, Value))
      return invalid("native stack scratch overlaps an existing temporary");
    return llvm::Error::success();
  };
  for (const auto &Op : Instruction.Ops) {
    if (auto Error = CheckScratch(Op.Output))
      return std::move(Error);
    for (unsigned I = 0; I < Op.NumInputs; ++I)
      if (auto Error = CheckScratch(Op.Inputs[I]))
        return std::move(Error);
  }
  for (const auto &Effect : Instruction.UndefinedEffects.Effects) {
    if (auto Error = CheckScratch(Effect.Output))
      return std::move(Error);
    if (Effect.When)
      if (auto Error = CheckScratch(*Effect.When))
        return std::move(Error);
  }
  for (const auto &Reserved : ReservedTemporaries) {
    if (!Reserved.isTemp() || !validRange(Reserved))
      return invalid("native stack control has an invalid reserved temporary");
    if (auto Error = CheckScratch(Reserved))
      return std::move(Error);
  }

  const auto &Effects = Instruction.UndefinedEffects;
  if (!Effects.Effects.empty())
    return invalid(
        "native stack control cannot remap nonempty undefined effects");
  const std::string OriginalDigest =
      lowUndefinedOperationDigest(Instruction.Ops);
  if (Effects.Coverage == LowUndefinedCoverage::Complete &&
      (Effects.OpCount != Instruction.Ops.size() ||
       Effects.OperationDigest != OriginalDigest))
    return invalid("native stack control has stale undefined-effect evidence");
  if (Effects.Coverage != LowUndefinedCoverage::Missing &&
      Effects.Coverage != LowUndefinedCoverage::Complete &&
      Effects.Coverage != LowUndefinedCoverage::Unsupported)
    return invalid(
        "native stack control has unknown undefined-effect coverage");

  NativeStackExpansion Result;
  if (Instruction.PreservedState.Audit != LowPreservedStateAudit::Missing) {
    if (Instruction.PreservedState.Audit !=
            LowPreservedStateAudit::LegacyIntegerV1 ||
        Instruction.ProfileProjection != InterpreterProfileProjection::None ||
        !matchesLowPreservedState(Instruction.PreservedState,
                                  Instruction.Origin, Instruction.NativeBytes,
                                  Instruction.Ops))
      return invalid("native stack control has stale preservation evidence");
    Result.Receipt.Version = 2;
    Result.Receipt.PreservedStateDigest =
        lowPreservedStateDigest(Instruction.PreservedState);
  } else if (Instruction.PreservedState != LowInstructionPreservedState{}) {
    return invalid("native stack control has partial preservation evidence");
  }
  Result.Receipt.ReturnMode = ReturnMode;
  Result.Ops = Instruction.Ops;
  Result.Boundary = Instruction.Origin;
  Result.Receipt.OriginalBoundary = Instruction.Origin;
  Result.Receipt.OriginalOperationDigest = OriginalDigest;
  const auto Make = [&](NdOp Opcode, NdVar Output,
                        std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    Op.Addr = Instruction.Origin.Address;
    Op.Seq = static_cast<int>(Result.Ops.size());
    for (const auto &Input : Inputs)
      Op.addInput(Input);
    Result.Ops.push_back(Op);
  };
  const auto SetBranch = [&](bool Indirect, std::optional<uint64_t> Target) {
    Result.Boundary.Control = LowInstructionControl::Branch;
    Result.Boundary.ControlFlags = LowInstructionControlFlag::Branch;
    if (Indirect)
      Result.Boundary.ControlFlags |= LowInstructionControlFlag::Indirect;
    Result.Boundary.Immediate = Target;
  };
  const LowOp &Control = Instruction.Ops.back();
  switch (Instruction.NativeStackControl) {
  case SpecializationNativeStackControl::Call: {
    const bool Direct = Control.Opcode == NdOp::CALL &&
                        Control.NumInputs == 1 && Control.Inputs[0].isConst();
    const bool Indirect =
        Control.Opcode == NdOp::INDIR_CALL && Control.NumInputs == 1 &&
        (Control.Inputs[0].isReg() || Control.Inputs[0].isTemp());
    const auto Flags = LowInstructionControlFlag::Call |
                       (Indirect ? LowInstructionControlFlag::Indirect
                                 : LowInstructionControlFlag::None);
    if ((!Direct && !Indirect) || Control.Inputs[0].Size != 8 ||
        !Control.Output.isReg() || Control.Output.Size != 8 ||
        Instruction.Origin.Control != LowInstructionControl::Call ||
        Instruction.Origin.ControlFlags != Flags ||
        (Indirect && Instruction.Origin.Immediate))
      return invalid("native near-call certificate does not match its LowIR");
    Result.OriginalPrefixSize = Instruction.Ops.size() - 1;
    if (Control.Inputs[0].isTemp()) {
      if (!Result.OriginalPrefixSize)
        return invalid("memory call has no explicit target load");
      const auto &Load = Instruction.Ops[Result.OriginalPrefixSize - 1];
      if (Load.Opcode != NdOp::LOAD || Load.Output != Control.Inputs[0])
        return invalid("memory call must consume its final eight-byte load");
      for (size_t I = 0; I < Result.OriginalPrefixSize; ++I)
        if (!addressOperation(Instruction.Ops[I],
                              I + 1 == Result.OriginalPrefixSize))
          return invalid("memory call prefix is not a pure address and "
                         "ordinary target load");
    } else if (Result.OriginalPrefixSize) {
      return invalid("direct or register call has an unexpected prefix");
    }
    Result.Ops.pop_back();
    if (Indirect) {
      Result.ExpandedIndirectCall = true;
      Make(NdOp::COPY, Scratch, {Control.Inputs[0]});
    }
    Make(NdOp::INT_SUB, Stack, {Stack, NdVar::scalar(8, 8)});
    Make(NdOp::STORE, {},
         {Stack, NdVar::scalar(Instruction.Fallthrough.Address, 8)});
    if (Indirect)
      Make(NdOp::INDIR_BR, {}, {Scratch});
    else
      Make(NdOp::BRANCH, {}, {Control.Inputs[0]});
    SetBranch(Indirect, Direct
                            ? std::optional<uint64_t>(Control.Inputs[0].Offset)
                            : std::nullopt);
    break;
  }
  case SpecializationNativeStackControl::Return:
    if (Instruction.IsNativeCall || Instruction.Ops.size() != 1 ||
        Control.Opcode != NdOp::RETURN || Control.NumInputs > 1 ||
        Control.Output.Size ||
        Instruction.Origin.Control != LowInstructionControl::Return ||
        Instruction.Origin.ControlFlags != LowInstructionControlFlag::Return ||
        Instruction.Origin.Immediate.value_or(0) > UINT16_MAX)
      return invalid("native near-return certificate does not match its LowIR");
    if (ReturnMode == NativeReturnExpansion::OuterFunctionBoundary &&
        Instruction.Origin.Immediate.value_or(0) != 0)
      return invalid("outer callee-pop return has no source boundary contract");
    if (ReturnMode == NativeReturnExpansion::InternalTransfer) {
      Result.Ops.clear();
      Result.ExpandedReturn = true;
      Make(NdOp::LOAD, Scratch, {Stack});
      Make(NdOp::INT_ADD, Stack,
           {Stack,
            NdVar::scalar(8 + Instruction.Origin.Immediate.value_or(0), 8)});
      Make(NdOp::INDIR_BR, {}, {Scratch});
      SetBranch(true, std::nullopt);
    }
    break;
  case SpecializationNativeStackControl::None: {
    if (!Instruction.IsNativeCall || Instruction.Ops.size() != 2 ||
        Instruction.Origin.Control != LowInstructionControl::None)
      return invalid("native call-to-fallthrough has no exact explicit push");
    const auto &Adjust = Instruction.Ops[0];
    const auto &Store = Instruction.Ops[1];
    if (Adjust.Opcode != NdOp::INT_SUB || Adjust.Output != Stack ||
        Adjust.NumInputs != 2 || Adjust.Inputs[0] != Stack ||
        !Adjust.Inputs[1].isConst() || Adjust.Inputs[1].Size != 8 ||
        Adjust.Inputs[1].Offset != 8 || Store.Opcode != NdOp::STORE ||
        Store.Output.Size || Store.NumInputs != 2 || Store.Inputs[0] != Stack ||
        !Store.Inputs[1].isConst() || Store.Inputs[1].Size != 8 ||
        Store.Inputs[1].Offset != Instruction.Fallthrough.Address)
      return invalid(
          "native call-to-fallthrough push disagrees with its boundary");
    break;
  }
  default:
    return invalid("unknown native stack-control certificate");
  }
  Result.Boundary.OpCount = Result.Ops.size();
  Result.UndefinedEffects = Effects;
  Result.UndefinedEffects.OpCount = Result.Ops.size();
  Result.UndefinedEffects.OperationDigest =
      lowUndefinedOperationDigest(Result.Ops);
  Result.Receipt.ExpandedOperationDigest =
      Result.UndefinedEffects.OperationDigest;
  return Result;
}

llvm::Expected<NativeStackExpansion>
expandX64MachineExit(const SpecializationInstruction &Instruction,
                     const SpecializationX64MachineExit &Declaration,
                     NdVar Scratch, llvm::ArrayRef<NdVar> ReservedTemporaries) {
  if (Declaration.Version != 1 ||
      Declaration.Address != Instruction.Origin.Address ||
      Instruction.TerminalImport ||
      Instruction.ProfileProjection != InterpreterProfileProjection::None ||
      Instruction.NativeBytes.empty() || Instruction.NativeBytes.size() > 15)
    return invalid("machine exit requires its exact declared native transfer");
  if (auto Error = validateBoundary(Instruction))
    return std::move(Error);
  Decoder Decode;
  Decode.init(Arch::X64);
  Decode.setStrict(true);
  DecodedInsn Insn{};
  if (!Decode.decodeOneForLift(Instruction.NativeBytes.data(),
                               Instruction.NativeBytes.size(),
                               Instruction.Origin.Address, Insn) ||
      Insn.Size != Instruction.NativeBytes.size())
    return invalid("machine exit could not be strictly decoded");
  using Kind = SpecializationX64MachineExitKind;
  const auto Native = Instruction.NativeStackControl;
  const auto &B = Instruction.Origin;
  switch (Declaration.Kind) {
  case Kind::Call:
    if (Insn.Id != X86_INS_CALL || !Instruction.IsNativeCall ||
        Native != SpecializationNativeStackControl::Call)
      return invalid("machine CALL exit does not match a physical near call");
    break;
  case Kind::Return:
    if (Insn.Id != X86_INS_RET || Instruction.IsNativeCall ||
        Native != SpecializationNativeStackControl::Return ||
        !((Instruction.NativeBytes.size() == 1 &&
           Instruction.NativeBytes[0] == 0xc3) ||
          (Instruction.NativeBytes.size() == 3 &&
           Instruction.NativeBytes[0] == 0xc2)) ||
        B.Immediate != Decode.returnImmediate(Insn))
      return invalid("machine RET exit requires its canonical near return");
    break;
  case Kind::Jump:
    if (Insn.Id != X86_INS_JMP || Instruction.IsNativeCall ||
        Native != SpecializationNativeStackControl::None ||
        B.Control != LowInstructionControl::Branch ||
        (B.ControlFlags != LowInstructionControlFlag::Branch &&
         B.ControlFlags != (LowInstructionControlFlag::Branch |
                            LowInstructionControlFlag::Indirect)))
      return invalid("machine JMP exit requires an unconditional near jump");
    break;
  case Kind::Trap:
    if (!Decode.isResumableTrap(Insn) || Instruction.IsNativeCall ||
        Native != SpecializationNativeStackControl::None ||
        B.Control != LowInstructionControl::Terminator || B.Immediate ||
        B.ControlFlags != (LowInstructionControlFlag::Terminator |
                           LowInstructionControlFlag::Resumable))
      return invalid("machine trap exit requires a resumable trap terminator");
    break;
  default:
    return invalid("unknown machine-exit kind");
  }
  std::vector<LowOp> Expected;
  LowInstructionUndefinedEffects Effects;
  try {
    if (Declaration.Kind != Kind::Call ||
        !Decode.liftX64MemoryCallToLow(Insn, Expected, &Effects))
      Decode.liftToLow(Insn, Expected, {}, {}, &Effects);
  } catch (const UnliftedInstruction &) {
    return invalid("machine exit has no complete strict lift");
  }
  const auto OriginalDigest = lowUndefinedOperationDigest(Instruction.Ops);
  if (Declaration.Kind == Kind::Trap) {
    // The trap has no exception continuation here. Authenticate the lift's
    // exact undefined-output coverage without changing it or accepting
    // invented arbitrary outputs or opaque-state preservation claims.
    if (lowUndefinedOperationDigest(Expected) != OriginalDigest ||
        Instruction.UndefinedEffects.Coverage != Effects.Coverage ||
        Instruction.UndefinedEffects.OpCount != Effects.OpCount ||
        Instruction.UndefinedEffects.OperationDigest != OriginalDigest ||
        !Effects.Effects.empty() ||
        !Instruction.UndefinedEffects.Effects.empty() ||
        Instruction.PreservedState != LowInstructionPreservedState{})
      return invalid("machine trap exit bytes differ from the complete lifted "
                     "span");
    NativeStackExpansion Result;
    Result.Ops = Instruction.Ops;
    Result.Boundary = B;
    Result.UndefinedEffects = Instruction.UndefinedEffects;
    Result.Receipt.OriginalBoundary = B;
    Result.Receipt.OriginalOperationDigest = OriginalDigest;
    Result.Receipt.ExpandedOperationDigest = OriginalDigest;
    Result.Receipt.ReturnMode = NativeReturnExpansion::OuterFunctionBoundary;
    Result.RetainedTerminator = true;
    return Result;
  }
  if (lowUndefinedOperationDigest(Expected) != OriginalDigest ||
      Effects.Coverage != LowUndefinedCoverage::Complete ||
      !Effects.Effects.empty() ||
      Instruction.UndefinedEffects.Coverage != Effects.Coverage ||
      Instruction.UndefinedEffects.OpCount != Effects.OpCount ||
      Instruction.UndefinedEffects.OperationDigest != OriginalDigest ||
      !Instruction.UndefinedEffects.Effects.empty())
    return invalid("machine-exit bytes differ from the complete lifted span");
  NativeStackExpansion Result;
  if (Declaration.Kind == Kind::Jump) {
    if (Instruction.PreservedState.Audit != LowPreservedStateAudit::Missing ||
        Instruction.PreservedState != LowInstructionPreservedState{})
      return invalid("machine JMP exit has unsupported preservation evidence");
    Result.Ops = Instruction.Ops;
    Result.Boundary = B;
    Result.UndefinedEffects = Instruction.UndefinedEffects;
    Result.Receipt.OriginalBoundary = B;
    Result.Receipt.OriginalOperationDigest = OriginalDigest;
    Result.Receipt.ReturnMode = NativeReturnExpansion::InternalTransfer;
  } else {
    auto Expanded = expandNativeStackControl(
        Instruction, NdVar::reg(x86reg::RSP, 8), Scratch,
        NativeReturnExpansion::InternalTransfer, ReservedTemporaries);
    if (!Expanded)
      return Expanded.takeError();
    Result = std::move(*Expanded);
  }
  if (Result.Ops.empty() || (Result.Ops.back().Opcode != NdOp::BRANCH &&
                             Result.Ops.back().Opcode != NdOp::INDIR_BR))
    return invalid("machine exit lacks its exact final transfer");
  Result.Ops.back().Opcode = NdOp::INDIR_BR;
  Result.Boundary.Control = LowInstructionControl::Branch;
  Result.Boundary.ControlFlags =
      LowInstructionControlFlag::Branch | LowInstructionControlFlag::Indirect;
  Result.Boundary.Immediate.reset();
  Result.UndefinedEffects.OperationDigest =
      lowUndefinedOperationDigest(Result.Ops);
  Result.Receipt.ExpandedOperationDigest =
      Result.UndefinedEffects.OperationDigest;
  return Result;
}

} // namespace neverd::analysis
