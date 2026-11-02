//===- PortableX87Function.cpp - Prepare explicit portable x87 calls ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Establish an alternate leaf-function ABI and checked helper success edges.
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/X86/PortableX87Function.h"

#include "PortableX87Validation.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/X86/X87Capabilities.h"
#include "neverd/ir/X86/X87Context.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedReturnValue.h"
#include "neverd/runtime/X87PortableRuntimeC.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace neverd {
namespace {
llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}
using Key = std::tuple<MedVar::VarKind, int, int>;
Key key(const MedVar &Value) { return {Value.Kind, Value.Id, Value.SSAVer}; }
MedVar scalar(uint64_t Value, uint16_t Bytes) {
  return MedVar::makeConst(Value, Bytes, ConstantAddressProvenance::Scalar);
}
MedVar variable(MedVar::VarKind Kind, int Id, uint16_t Bytes) {
  MedVar Value;
  Value.Kind = Kind;
  Value.Id = Id;
  Value.Size = Bytes;
  Value.TheArch = Arch::X64;
  if (Kind == MedVar::Param)
    Value.RegOff = kNoParamReg;
  return Value;
}
MedOp operation(NdOp Opcode, MedVar Output,
                std::initializer_list<MedVar> Inputs, va_t Address = 0) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.Addr = Address;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}
Intrinsic intrinsic(const MedOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && hasValidMedIntrinsicId(Op)
             ? static_cast<Intrinsic>(Op.Inputs[0].ConstVal)
             : Intrinsic::None;
}

/// Remove physical-register and source-address projections from the private
/// alternate ABI. Incoming self-copies are admitted only for actual parameters.
llvm::Expected<MedFunc> normalize(const MedFunc &Source, int &NextId) {
  MedFunc Result;
  Result.Entry = Source.Entry;
  // Private control labels carry no original guest code-range authority.
  Result.OriginalSize = 0;
  Result.Name = llvm::formatv("nvd_portable_x87_{0:x}_v1", Source.Entry).str();
  Result.CC = CallingConv::CDECL;
  Result.ReturnType = NdType::makeInt(4, false);
  Result.ReturnValueEvidence = MedReturnValueEvidence::ReturnsInteger;
  std::map<Key, MedVar> Definitions;
  std::map<Key, MedVar> DefinedSource;
  std::map<Key, MedVar> Parameters;
  std::map<Key, MedVar> SourceParameters;
  std::map<uint64_t, MedVar> IncomingRegisters;
  const auto &Registers = getTargetRegInfo(Arch::X64);
  for (const auto &Param : Source.Params) {
    if ((Param.Kind != MedVar::Param && Param.Kind != MedVar::Reg) ||
        (Param.TheArch != Arch::Unknown && Param.TheArch != Arch::X64) ||
        (Param.Size != 1 && Param.Size != 2 && Param.Size != 4 &&
         Param.Size != 8) ||
        (Param.RegOff != kNoParamReg && !Registers.isGeneralReg(Param.RegOff)))
      return invalid(
          "portable x87 source parameters require integer guest values");
    const auto New = variable(MedVar::Param, Result.Params.size(), Param.Size);
    if (!Parameters.emplace(key(Param), New).second ||
        (Param.RegOff != kNoParamReg &&
         !IncomingRegisters.emplace(Param.RegOff, New).second))
      return invalid("portable x87 source parameter identities are ambiguous");
    SourceParameters.emplace(key(Param), Param);
    Result.Params.push_back(New);
    Result.TypedParams.push_back(
        {New.display(), NdType::makeInt(Param.Size, false)});
  }
  NextId = static_cast<int>(Result.Params.size()) + 2;
  const auto Define = [&](const MedVar &Old) -> bool {
    if (!Old.Size || Old.Size > sizeof(uint64_t) || Old.Id < 0 ||
        (Old.TheArch != Arch::Unknown && Old.TheArch != Arch::X64) ||
        (Old.Kind != MedVar::Reg && Old.Kind != MedVar::Temp &&
         Old.Kind != MedVar::Flag) ||
        NextId == std::numeric_limits<int>::max())
      return false;
    DefinedSource.emplace(key(Old), Old);
    return Definitions
        .emplace(key(Old), variable(MedVar::Temp, NextId++, Old.Size))
        .second;
  };
  for (const auto &Block : Source.Blocks) {
    for (const auto &Phi : Block.Phis)
      if (!Define(Phi.Output))
        return invalid(
            "portable x87 source PHI is not a unique scalar definition");
    for (const auto &Op : Block.Ops)
      if (!Op.Dead && Op.Output.Size && !Define(Op.Output))
        return invalid(
            "portable x87 source output is not a unique scalar definition");
  }
  const auto Input = [&](const MedVar &Old) -> std::optional<MedVar> {
    if (!Old.Size || Old.Size > sizeof(uint64_t) ||
        (Old.TheArch != Arch::Unknown && Old.TheArch != Arch::X64))
      return std::nullopt;
    if (Old.isConst())
      return scalar(Old.ConstVal, Old.Size);
    auto Found = Definitions.find(key(Old));
    if (Found != Definitions.end())
      return Found->second.Size == Old.Size &&
                     (Old.Kind != MedVar::Reg ||
                      DefinedSource.at(key(Old)).RegOff == Old.RegOff)
                 ? std::optional(Found->second)
                 : std::nullopt;
    auto Parameter = Parameters.find(key(Old));
    if (Parameter != Parameters.end())
      return Parameter->second.Size == Old.Size &&
                     SourceParameters.at(key(Old)).RegOff == Old.RegOff
                 ? std::optional(Parameter->second)
                 : std::nullopt;
    if ((Old.Kind == MedVar::Reg || Old.Kind == MedVar::Param) &&
        Old.SSAVer == 0) {
      const auto Register = IncomingRegisters.find(Old.RegOff);
      if (Register != IncomingRegisters.end() &&
          Register->second.Size == Old.Size)
        return Register->second;
    }
    return std::nullopt;
  };
  Result.Blocks = Source.Blocks;
  for (auto &Block : Result.Blocks) {
    for (auto &Phi : Block.Phis) {
      if (Phi.ExceptionalEntry)
        return invalid("portable x87 source has an exceptional PHI input");
      Phi.Output = Definitions.at(key(Phi.Output));
      for (auto &[Predecessor, Value] : Phi.Args) {
        const auto New = Input(Value);
        if (!New)
          return invalid(
              "portable x87 source PHI reads unknown incoming state");
        Value = *New;
      }
    }
    std::vector<MedOp> Operations;
    for (const auto &Old : Block.Ops) {
      if (Old.Dead)
        continue;
      if (Old.NumInputs > Old.Inputs.size() || !Old.IntrinsicOutputs.empty() ||
          Old.MemoryOrdering != NdMemoryOrdering::None ||
          Old.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Old.RegistrationRoot != MedOp::RegistrationRootKind::None ||
          Old.HasSourceErrorResult || Old.SourceCallHint)
        return invalid(
            "portable x87 source has an unsupported operation boundary");
      MedOp Op;
      Op.Opcode = Old.Opcode;
      Op.Addr = Old.Addr;
      if (Old.Output.Size)
        Op.Output = Definitions.at(key(Old.Output));
      for (unsigned I = 0; I != Old.NumInputs; ++I) {
        auto New = Input(Old.Inputs[I]);
        if (Block.Id == 0 && Old.Opcode == NdOp::COPY && Old.NumInputs == 1 &&
            Old.Output == Old.Inputs[I] &&
            Old.Output.RegOff == Old.Inputs[I].RegOff &&
            Old.Output.Size == Old.Inputs[I].Size &&
            (Old.Inputs[I].TheArch == Arch::Unknown ||
             Old.Inputs[I].TheArch == Arch::X64) &&
            Old.Output.Kind == MedVar::Reg && Old.Output.SSAVer == 0) {
          auto Incoming = IncomingRegisters.find(Old.Output.RegOff);
          if (Incoming == IncomingRegisters.end() ||
              Incoming->second.Size != Old.Output.Size)
            return invalid(
                "portable x87 entry reads an unauthenticated register");
          New = Incoming->second;
        }
        if (!New)
          return invalid(
              "portable x87 source reads unknown incoming scalar state");
        Op.addInput(*New);
      }
      Operations.push_back(std::move(Op));
    }
    Block.Ops = std::move(Operations);
  }
  if (auto Error = portable_x87::validateControlAndValues(Result))
    return std::move(Error);
  if (!verifyMedFunc(Result, "portable-x87-input"))
    return invalid("portable x87 source has invalid scalar operation shapes");
  return Result;
}
} // namespace

llvm::Expected<PreparedPortableX87Function>
preparePortableX87Function(const MedFunc &Source, Arch SourceArch,
                           BinaryFormat SourceFormat) {
  if (SourceArch != Arch::X64 || Source.SkippedSSA || Source.Blocks.empty() ||
      Source.ExceptionMetadata || Source.HasUnresolvedSEHFrame ||
      Source.RegistrationStates || Source.RegistrationPhysicalFrame ||
      Source.IsVariadic || Source.ConsumesX87Argument ||
      Source.FPReturnViaX87 || Source.ExplicitX87ReturnValue ||
      Source.ReturnsVectorRegister || Source.DoesNotReturn ||
      Source.ReturnsNoValue || !Source.MultiReturn.empty() ||
      !Source.CallInfos.empty() || !Source.CallClobbers.empty() ||
      std::any_of(Source.ModuleAnalysisRoots.begin(),
                  Source.ModuleAnalysisRoots.end(),
                  [&](va_t Root) { return Root != Source.Entry; }) ||
      !Source.JumpTables.empty() || Source.AnalystStackFrame ||
      Source.SourceTypeHint ||
      (Source.ReturnType && Source.ReturnType->Kind != NdTypeKind::Int &&
       Source.ReturnType->Kind != NdTypeKind::Ptr))
    return invalid("portable x87 requires an ABI-recovered x64 integer leaf");
  // Dense SSA and block IDs are signed ints. Account for the worst-case
  // expansion before casting or adding any IDs; this is representability,
  // not an implementation-selected work budget.
  constexpr uint64_t MaxId = std::numeric_limits<int>::max();
  if (Source.Params.size() > MaxId - 6 || Source.Blocks.size() > MaxId - 2)
    return invalid("portable x87 source exceeds representable interface IDs");
  uint64_t ValueRoom = MaxId - Source.Params.size() - 6;
  uint64_t BlockRoom = MaxId - Source.Blocks.size() - 2;
  for (const auto &Block : Source.Blocks) {
    if (Block.Phis.size() > ValueRoom)
      return invalid("portable x87 source exceeds representable value IDs");
    ValueRoom -= Block.Phis.size();
    if (Block.Ops.size() > ValueRoom / 3 || Block.Ops.size() > BlockRoom / 2)
      return invalid("portable x87 source expansion exceeds representable IDs");
    ValueRoom -= 3 * Block.Ops.size();
    BlockRoom -= 2 * Block.Ops.size();
  }
  for (const auto &Parameter : Source.TypedParams)
    if (Parameter.Type && Parameter.Type->Kind != NdTypeKind::Int &&
        Parameter.Type->Kind != NdTypeKind::Ptr)
      return invalid("portable x87 does not admit a typed floating source ABI");

  int NextId = 0;
  auto Normalized = normalize(Source, NextId);
  if (!Normalized)
    return Normalized.takeError();
  auto &Function = *Normalized;
  std::optional<MedVar> Context;
  unsigned Steps = 0, Returns = 0;
  for (const auto &Block : Function.Blocks) {
    bool Exported = false;
    for (size_t I = 0; I != Block.Ops.size(); ++I) {
      const auto &Op = Block.Ops[I];
      const auto Id = intrinsic(Op);
      if (Op.Opcode == NdOp::INTRINSIC) {
        if (!x87ContextMedShapeIsValid(Op, Arch::X64))
          return invalid(
              "portable x87 source has a malformed context operation");
        if (Id == Intrinsic::X87ContextBegin) {
          if (Context || Block.Id != 0)
            return invalid("portable x87 requires exactly one entry context");
          Context = Op.Output;
        } else if (Id == Intrinsic::X87ContextStep) {
          if (!Context || Exported || Op.Inputs[1] != *Context ||
              !isPortableX87RuntimeInstruction(Op.Inputs[2].ConstVal) ||
              !Op.Inputs[4].isConst() || Op.Inputs[4].ConstVal != 0 ||
              !Op.Inputs[5].isConst())
            return invalid(
                "portable x87 source step is outside the explicit profile");
          ++Steps;
        } else if (Id == Intrinsic::X87ContextExport) {
          if (!Context || Exported || Op.Inputs[1] != *Context ||
              !Block.Succs.empty() || Block.Ops.back().Opcode != NdOp::RETURN)
            return invalid("portable x87 export is not a leaf return boundary");
          // Subregister propagation may assemble the complete RAX value after
          // export. Pure scalar work is allowed; a subsequent x87 step is not.
          Exported = true;
        } else {
          return invalid(
              "portable x87 source needs an unqualified state handoff");
        }
      } else {
        if (!portable_x87::isIntegerOperation(Op.Opcode))
          return invalid("portable x87 source has an unqualified scalar or "
                         "memory operation");
        for (unsigned J = 0; J != Op.NumInputs; ++J)
          if (Context && Op.Inputs[J] == *Context)
            return invalid("portable x87 context escapes its owned operations");
        if (Op.Opcode == NdOp::RETURN) {
          if (!hasExplicitIntegerReturnValue(Op, Arch::X64, sizeof(uint64_t)) ||
              Op.Inputs[0].Size != sizeof(uint64_t) || !Exported)
            return invalid(
                "portable x87 requires an explicit integer return boundary");
          ++Returns;
        }
      }
    }
  }
  if (!Context || !Steps || !Returns)
    return invalid("portable x87 source has no complete owned-context leaf");

  std::vector<uint16_t> ParameterBytes;
  for (const auto &Parameter : Function.Params)
    ParameterBytes.push_back(Parameter.Size);
  const auto Execution = variable(MedVar::Param, Function.Params.size(), 8);
  const auto SourceReturn =
      variable(MedVar::Param, Function.Params.size() + 1, 8);
  Function.Params.push_back(Execution);
  Function.Params.push_back(SourceReturn);
  Function.TypedParams.push_back({Execution.display(), NdType::makePtr()});
  Function.TypedParams.push_back(
      {SourceReturn.display(), NdType::makePtr(NdType::makeInt(8, false))});
  const auto Temporary = [&](uint16_t Bytes) {
    return variable(MedVar::Temp, NextId++, Bytes);
  };

  auto Original = std::move(Function.Blocks);
  Function.Blocks.resize(Original.size() + 1);
  for (unsigned I = 0; I != Function.Blocks.size(); ++I)
    Function.Blocks[I].Id = I;
  std::vector<int> LastChunk(Original.size());
  const auto AppendBlock = [&]() {
    const int Id = Function.Blocks.size();
    Function.Blocks.emplace_back();
    Function.Blocks.back().Id = Id;
    return Id;
  };
  // Null output storage is rejected before any x87 or guest-memory effect.
  const int InvalidEntry = AppendBlock();
  auto NullExecution = Temporary(1), NullReturn = Temporary(1),
       BadEntry = Temporary(1);
  Function.Blocks[0].StartAddr = Source.Entry;
  Function.Blocks[0].Ops = {
      operation(NdOp::INT_EQUAL, NullExecution, {Execution, scalar(0, 8)}),
      operation(NdOp::INT_EQUAL, NullReturn, {SourceReturn, scalar(0, 8)}),
      operation(NdOp::BOOL_OR, BadEntry, {NullExecution, NullReturn}),
      operation(NdOp::COND_BR, {}, {scalar(0, 8), BadEntry})};
  Function.Blocks[0].Succs = {1, InvalidEntry};
  auto InvalidStatus = Temporary(4);
  Function.Blocks[InvalidEntry].Ops = {
      operation(NdOp::COPY, InvalidStatus,
                {scalar(NVD_X87_STATUS_INVALID_STATE, 4)}),
      operation(NdOp::RETURN, {}, {InvalidStatus})};
  for (const auto &Block : Original) {
    int Current = Block.Id + 1;
    Function.Blocks[Current].StartAddr = Block.Id == 0 ? 0 : Block.StartAddr;
    Function.Blocks[Current].EndAddr = Block.EndAddr;
    Function.Blocks[Current].Phis = Block.Phis;
    for (const auto &Op : Block.Ops) {
      const auto Id = intrinsic(Op);
      if (Id == Intrinsic::X87ContextBegin || Id == Intrinsic::X87ContextExport)
        continue;
      if (Id == Intrinsic::X87ContextStep) {
        const auto Status = Temporary(4);
        const int Success = AppendBlock(), Failure = AppendBlock();
        auto &CurrentBlock = Function.Blocks[Current];
        CurrentBlock.Ops.push_back(operation(
            NdOp::INTRINSIC, Status,
            {scalar(static_cast<uint64_t>(Intrinsic::PortableX87Step), 4),
             Execution, Op.Inputs[2], Op.Inputs[5], Op.Inputs[3]},
            Op.Addr));
        CurrentBlock.Ops.push_back(
            operation(NdOp::COND_BR, {}, {scalar(0, 8), Status}, Op.Addr));
        CurrentBlock.Succs = {Success, Failure};
        Function.Blocks[Failure].Ops.push_back(
            operation(NdOp::RETURN, {}, {Status}));
        Current = Success;
        if (Op.Output.Size) {
          const auto Value = Temporary(8);
          Function.Blocks[Current].Ops.push_back(operation(
              NdOp::INTRINSIC, Value,
              {scalar(static_cast<uint64_t>(Intrinsic::PortableX87Result), 4),
               Execution},
              Op.Addr));
          Function.Blocks[Current].Ops.push_back(
              Op.Output.Size == 8
                  ? operation(NdOp::COPY, Op.Output, {Value}, Op.Addr)
                  : operation(NdOp::SUBBYTES, Op.Output, {Value, scalar(0, 4)},
                              Op.Addr));
        }
      } else if (Op.Opcode == NdOp::RETURN) {
        Function.Blocks[Current].Ops.push_back(
            operation(NdOp::STORE, {}, {SourceReturn, Op.Inputs[0]}));
        const auto Status = Temporary(4);
        Function.Blocks[Current].Ops.push_back(
            operation(NdOp::COPY, Status, {scalar(0, 4)}));
        Function.Blocks[Current].Ops.push_back(
            operation(NdOp::RETURN, {}, {Status}));
      } else {
        Function.Blocks[Current].Ops.push_back(Op);
      }
    }
    LastChunk[Block.Id] = Current;
    for (int Successor : Block.Succs)
      Function.Blocks[Current].Succs.push_back(Successor + 1);
    if (Block.Succs.size() == 1 && Block.Ops.back().Opcode == NdOp::BRANCH) {
      const auto &Destination = Original[Block.Succs.front()];
      const auto Target = Block.Ops.back().Inputs[0].ConstVal;
      if (Target != Destination.StartAddr &&
          (Destination.StartAddr || Target != Destination.Ops.front().Addr))
        return invalid(
            "portable x87 direct target disagrees with its successor");
    }
    if (Block.Succs.size() == 2) {
      const auto Target = Block.Ops.back().Inputs[0].ConstVal;
      const auto Matches = [&](int Id) {
        const auto &Destination = Original[Id];
        return Target == Destination.StartAddr ||
               (!Destination.StartAddr && !Destination.Ops.empty() &&
                Target == Destination.Ops.front().Addr);
      };
      if (Target && Matches(Block.Succs[0]) && !Matches(Block.Succs[1]))
        std::swap(Function.Blocks[Current].Succs[0],
                  Function.Blocks[Current].Succs[1]);
      else if (Target && (!Matches(Block.Succs[1]) || Matches(Block.Succs[0])))
        return invalid(
            "portable x87 conditional target does not identify one successor");
    }
  }
  for (auto &Block : Function.Blocks)
    if (Block.Succs.size() == 1 &&
        (Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::BRANCH))
      Block.Ops.push_back(operation(NdOp::BRANCH, {}, {scalar(0, 8)}));
  // HighC and LLVM both consume explicit, unique private control labels.
  // Guest instruction/data addresses remain scalar helper arguments only.
  Function.Entry = 1;
  for (auto &Block : Function.Blocks) {
    Block.StartAddr = static_cast<uint64_t>(Block.Id) + 1;
    Block.EndAddr = Block.StartAddr + 1;
    for (auto &Op : Block.Ops)
      Op.Addr = Block.StartAddr;
    if (Block.Ops.back().Opcode == NdOp::COND_BR)
      Block.Ops.back().Inputs[0] =
          scalar(static_cast<uint64_t>(Block.Succs[1]) + 1, 8);
    else if (Block.Ops.back().Opcode == NdOp::BRANCH)
      Block.Ops.back().Inputs[0] =
          scalar(static_cast<uint64_t>(Block.Succs[0]) + 1, 8);
  }
  for (auto &Block : Function.Blocks) {
    for (auto &Phi : Block.Phis)
      for (auto &[Predecessor, Value] : Phi.Args)
        Predecessor = LastChunk[Predecessor];
    for (int Successor : Block.Succs)
      Function.Blocks[Successor].Preds.push_back(Block.Id);
  }
  PreparedPortableX87Function Prepared(std::move(Function),
                                       std::move(ParameterBytes), SourceFormat,
                                       Source.Entry, Execution, SourceReturn);
  if (auto Error = Prepared.validate())
    return std::move(Error);
  return Prepared;
}

llvm::Error PreparedPortableX87Function::validate() const {
  const auto Same = [](const MedVar &Left, const MedVar &Right) {
    return Left == Right && Left.Size == Right.Size;
  };
  if (Function.Params.size() != ParameterBytes.size() + 2 ||
      Function.TypedParams.size() != Function.Params.size() ||
      !Same(Function.Params[executionParameterIndex()], Execution) ||
      !Same(Function.Params[sourceReturnParameterIndex()], SourceReturn) ||
      Execution.Kind != MedVar::Param || Execution.Size != 8 ||
      SourceReturn.Kind != MedVar::Param || SourceReturn.Size != 8 ||
      Function.CC != CallingConv::CDECL || !Function.ReturnType ||
      Function.ReturnType->Kind != NdTypeKind::Int ||
      Function.ReturnType->Size != 4 || Function.ReturnType->IsSigned ||
      Function.ReturnValueEvidence != MedReturnValueEvidence::ReturnsInteger)
    return invalid("portable x87 prepared ABI is inconsistent");
  const auto &ExecutionType =
      Function.TypedParams[executionParameterIndex()].Type;
  const auto &ReturnPointer =
      Function.TypedParams[sourceReturnParameterIndex()].Type;
  if (!ExecutionType || ExecutionType->Kind != NdTypeKind::Ptr ||
      ExecutionType->Size != 8 || !ExecutionType->Pointee ||
      ExecutionType->Pointee->Kind != NdTypeKind::Void || !ReturnPointer ||
      ReturnPointer->Kind != NdTypeKind::Ptr || ReturnPointer->Size != 8 ||
      !ReturnPointer->Pointee ||
      ReturnPointer->Pointee->Kind != NdTypeKind::Int ||
      ReturnPointer->Pointee->Size != 8 || ReturnPointer->Pointee->IsSigned)
    return invalid("portable x87 prepared pointer roles are inconsistent");
  for (unsigned I = 0; I != ParameterBytes.size(); ++I)
    if (Function.Params[I].Size != ParameterBytes[I] ||
        !Function.TypedParams[I].Type ||
        Function.TypedParams[I].Type->Kind != NdTypeKind::Int ||
        Function.TypedParams[I].Type->Size != ParameterBytes[I] ||
        Function.TypedParams[I].Type->IsSigned)
      return invalid(
          "portable x87 changed a guest parameter into a host value");
  if (auto Error = portable_x87::validateControlAndValues(Function))
    return Error;
  if (!verifyMedFunc(Function, "portable-x87-prepared"))
    return invalid("portable x87 prepared scalar shapes are invalid");
  if (Function.OriginalSize ||
      Function.Entry != Function.Blocks.front().StartAddr)
    return invalid(
        "portable x87 private control labels claim an original code range");
  for (const auto &Block : Function.Blocks) {
    if (Block.StartAddr != static_cast<uint64_t>(Block.Id) + 1 ||
        Block.EndAddr != Block.StartAddr + 1)
      return invalid("portable x87 control labels are not unique");
    const auto &Terminal = Block.Ops.back();
    if (Terminal.Opcode == NdOp::COND_BR &&
        Terminal.Inputs[0] !=
            scalar(Function.Blocks[Block.Succs[1]].StartAddr, 8))
      return invalid(
          "portable x87 branch target disagrees with its taken edge");
    if (Terminal.Opcode == NdOp::BRANCH &&
        Terminal.Inputs[0] !=
            scalar(Function.Blocks[Block.Succs[0]].StartAddr, 8))
      return invalid("portable x87 branch target disagrees with its only edge");
  }
  const auto &Entry = Function.Blocks.front();
  if (Entry.Ops.size() != 4 || Entry.Succs.size() != 2 ||
      Entry.Ops[0].Opcode != NdOp::INT_EQUAL || Entry.Ops[0].NumInputs != 2 ||
      !Same(Entry.Ops[0].Inputs[0], Execution) ||
      Entry.Ops[0].Inputs[1] != scalar(0, 8) ||
      Entry.Ops[1].Opcode != NdOp::INT_EQUAL || Entry.Ops[1].NumInputs != 2 ||
      !Same(Entry.Ops[1].Inputs[0], SourceReturn) ||
      Entry.Ops[1].Inputs[1] != scalar(0, 8) ||
      Entry.Ops[2].Opcode != NdOp::BOOL_OR || Entry.Ops[2].NumInputs != 2 ||
      !Same(Entry.Ops[2].Inputs[0], Entry.Ops[0].Output) ||
      !Same(Entry.Ops[2].Inputs[1], Entry.Ops[1].Output) ||
      Entry.Ops[3].Opcode != NdOp::COND_BR || Entry.Ops[3].NumInputs != 2 ||
      Entry.Ops[3].Inputs[0] !=
          scalar(Function.Blocks[Entry.Succs[1]].StartAddr, 8) ||
      !Same(Entry.Ops[3].Inputs[1], Entry.Ops[2].Output))
    return invalid("portable x87 is missing its entry pointer checks");
  const auto &InvalidEntry = Function.Blocks[Entry.Succs[1]];
  if (InvalidEntry.Preds != std::vector<int>{0} ||
      InvalidEntry.Ops.size() != 2 ||
      InvalidEntry.Ops[0].Opcode != NdOp::COPY ||
      InvalidEntry.Ops[0].NumInputs != 1 ||
      InvalidEntry.Ops[0].Inputs[0] !=
          scalar(NVD_X87_STATUS_INVALID_STATE, 4) ||
      InvalidEntry.Ops[1].Opcode != NdOp::RETURN ||
      InvalidEntry.Ops[1].NumInputs != 1 ||
      !Same(InvalidEntry.Ops[1].Inputs[0], InvalidEntry.Ops[0].Output))
    return invalid("portable x87 entry failure does not return its status");

  std::map<int, MedVar> FailureReturns;
  FailureReturns.emplace(InvalidEntry.Id, InvalidEntry.Ops[0].Output);
  unsigned Steps = 0;
  for (const auto &Block : Function.Blocks) {
    for (size_t I = 0; I != Block.Ops.size(); ++I) {
      const auto &Op = Block.Ops[I];
      if (intrinsic(Op) != Intrinsic::PortableX87Step)
        continue;
      if (!portableX87MedShapeIsValid(Op) || !Same(Op.Inputs[1], Execution) ||
          !isPortableX87RuntimeInstruction(Op.Inputs[2].ConstVal) ||
          I + 2 != Block.Ops.size() || Block.Succs.size() != 2)
        return invalid(
            "portable x87 step has no exact helper or exit boundary");
      const auto &Branch = Block.Ops.back();
      if (Branch.Opcode != NdOp::COND_BR || Branch.NumInputs != 2 ||
          Branch.Inputs[0] !=
              scalar(Function.Blocks[Block.Succs[1]].StartAddr, 8) ||
          !Same(Branch.Inputs[1], Op.Output))
        return invalid("portable x87 step does not test its actual status");
      const auto &Failure = Function.Blocks[Block.Succs[1]];
      const auto &Success = Function.Blocks[Block.Succs[0]];
      if (Failure.Preds != std::vector<int>{Block.Id} ||
          !Failure.Phis.empty() || Failure.Ops.size() != 1 ||
          Failure.Ops[0].Opcode != NdOp::RETURN ||
          Failure.Ops[0].NumInputs != 1 ||
          !Same(Failure.Ops[0].Inputs[0], Op.Output) ||
          !FailureReturns.emplace(Failure.Id, Op.Output).second ||
          Success.Preds != std::vector<int>{Block.Id} || !Success.Phis.empty())
        return invalid(
            "portable x87 helper failure is not an immediate unchanged return");
      const auto Spec = x87ContextInstruction(Op.Inputs[2].ConstVal);
      const bool Produces = Spec->WritesAX || Spec->WritesFlags;
      if ((intrinsic(Success.Ops.front()) == Intrinsic::PortableX87Result) !=
          Produces)
        return invalid("portable x87 scalar result disagrees with its "
                       "successful producer");
      ++Steps;
    }
  }
  if (!Steps)
    return invalid("portable x87 prepared body has no checked steps");
  for (const auto &Block : Function.Blocks) {
    for (size_t I = 0; I != Block.Ops.size(); ++I) {
      const auto &Op = Block.Ops[I];
      const auto Id = intrinsic(Op);
      if (Op.Opcode == NdOp::INTRINSIC) {
        if (!portableX87MedShapeIsValid(Op) || !Same(Op.Inputs[1], Execution))
          return invalid(
              "portable x87 prepared body contains a foreign intrinsic");
        if (Id == Intrinsic::PortableX87Result) {
          if (I != 0 || Block.Preds.size() != 1 || !Block.Phis.empty())
            return invalid("portable x87 result has no exclusive success edge");
          const auto &Producer = Function.Blocks[Block.Preds.front()];
          if (Producer.Ops.size() < 2 || Producer.Succs.size() != 2 ||
              Producer.Succs[0] != Block.Id ||
              intrinsic(Producer.Ops[Producer.Ops.size() - 2]) !=
                  Intrinsic::PortableX87Step)
            return invalid("portable x87 result is stale or has no producer");
        } else if (Id != Intrinsic::PortableX87Step) {
          return invalid(
              "portable x87 prepared body contains a foreign helper");
        }
      } else if (Op.Opcode == NdOp::STORE) {
        if (Op.NumInputs != 2 || !Same(Op.Inputs[0], SourceReturn) ||
            Op.Inputs[1].Size != 8 || I + 3 != Block.Ops.size() ||
            Block.Ops[I + 1].Opcode != NdOp::COPY ||
            Block.Ops[I + 1].NumInputs != 1 ||
            Block.Ops[I + 1].Inputs[0] != scalar(0, 4) ||
            Block.Ops[I + 2].Opcode != NdOp::RETURN ||
            Block.Ops[I + 2].NumInputs != 1 ||
            !Same(Block.Ops[I + 1].Output, Block.Ops[I + 2].Inputs[0]) ||
            FailureReturns.count(Block.Id))
          return invalid(
              "portable x87 publishes a source return before full success");
      } else if (!portable_x87::isIntegerOperation(Op.Opcode)) {
        return invalid(
            "portable x87 prepared body contains an unqualified operation");
      }
      for (unsigned J = 0; J != Op.NumInputs; ++J) {
        if (Same(Op.Inputs[J], Execution) &&
            !((Block.Id == 0 && I == 0 && J == 0) ||
              (Op.Opcode == NdOp::INTRINSIC && J == 1)))
          return invalid(
              "portable x87 execution context escapes its helper boundary");
        if (Same(Op.Inputs[J], SourceReturn) &&
            !((Block.Id == 0 && I == 1 && J == 0) ||
              (Op.Opcode == NdOp::STORE && J == 0)))
          return invalid("portable x87 source-return storage escapes its "
                         "success boundary");
      }
      if (Op.Opcode == NdOp::RETURN) {
        if (Op.NumInputs != 1 || Op.Inputs[0].Kind != MedVar::Temp ||
            Op.Inputs[0].Size != 4)
          return invalid("portable x87 return is not an explicit status");
        auto Failure = FailureReturns.find(Block.Id);
        if (Failure != FailureReturns.end()) {
          if (!Same(Op.Inputs[0], Failure->second))
            return invalid("portable x87 failure return changed its status");
        } else if (I < 2 || Block.Ops[I - 2].Opcode != NdOp::STORE) {
          return invalid(
              "portable x87 success return has no source-result publication");
        }
      }
    }
  }
  return llvm::Error::success();
}

} // namespace neverd
