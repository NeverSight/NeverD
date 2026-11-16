//===- MIPS16EntryAdapter.cpp - Proven classic FP entry transport --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Replay the lifter's pure register transfers, including the actual delay
/// slot, and compare every parameter byte with the shared ABI placements.
//===----------------------------------------------------------------------===//

#include "MIPS16EntryAdapter.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/MIPS/MIPSControlTarget.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/libc/LibCFloatingArguments.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <array>
#include <vector>

namespace neverd::pipeline_detail {
namespace {

// Values 0..255 are constant bytes; higher values identify an incoming
// register byte by significance. These are register views, not guest memory.
using Bytes = llvm::SmallVector<uint64_t, 16>;
constexpr uint64_t RegisterByteBase = 256;

struct Transport {
  llvm::SmallDenseMap<uint64_t, uint64_t, 32> Registers;
  llvm::SmallDenseMap<uint64_t, uint64_t, 32> Temporaries;
  std::array<bool, 4> ArgumentWrites{};
  unsigned TargetWrites = 0;

  std::optional<Bytes> read(const NdVar &Value) const {
    if (!Value.Size || Value.Size > mipsreg::VectorBytes)
      return std::nullopt;
    Bytes Result;
    for (unsigned I = 0; I < Value.Size; ++I) {
      if (Value.isConst()) {
        if (Value.Size > sizeof(Value.Offset))
          return std::nullopt;
        Result.push_back((Value.Offset >> (8 * I)) & 255);
      } else if (Value.isReg()) {
        const auto Found = Registers.find(Value.Offset + I);
        Result.push_back(Found == Registers.end()
                             ? RegisterByteBase + Value.Offset + I
                             : Found->second);
      } else if (Value.isTemp()) {
        const auto Found = Temporaries.find(Value.Offset + I);
        if (Found == Temporaries.end())
          return std::nullopt;
        Result.push_back(Found->second);
      } else {
        return std::nullopt;
      }
    }
    return Result;
  }

  static std::optional<uint64_t> constant(llvm::ArrayRef<uint64_t> Value) {
    if (Value.size() > sizeof(uint64_t))
      return std::nullopt;
    uint64_t Result = 0;
    for (unsigned I = 0; I < Value.size(); ++I) {
      if (Value[I] >= RegisterByteBase)
        return std::nullopt;
      Result |= Value[I] << (8 * I);
    }
    return Result;
  }

  bool write(const NdVar &Output, llvm::ArrayRef<uint64_t> Value,
             bool &WroteRegister) {
    if (Output.Size != Value.size())
      return false;
    if (Output.isReg()) {
      if (Output.Size != 4)
        return false;
      if (Output.Offset == mipsreg::r(25)) {
        if (++TargetWrites > 2 || !constant(Value))
          return false;
      } else if (Output.Offset >= mipsreg::r(4) &&
                 Output.Offset <= mipsreg::r(7) &&
                 Output.Offset % mipsreg::SlotBytes == 0) {
        auto &Written = ArgumentWrites[Output.Offset / mipsreg::SlotBytes - 4];
        if (Written)
          return false;
        Written = true;
      } else {
        return false;
      }
      WroteRegister = true;
    } else if (!Output.isTemp()) {
      return false;
    }
    auto &Bank = Output.isReg() ? Registers : Temporaries;
    for (unsigned I = 0; I < Value.size(); ++I)
      Bank[Output.Offset + I] = Value[I];
    return true;
  }

  bool step(const LowOp &Op, bool &WroteRegister) {
    if (Op.Opcode == NdOp::NOP)
      return Op.NumInputs == 0 && Op.Output.Size == 0;
    if (!Op.Output.Size || Op.NumInputs == 0 || Op.NumInputs > 2)
      return false;
    auto A = read(Op.Inputs[0]);
    auto B = Op.NumInputs == 2 ? read(Op.Inputs[1]) : std::optional<Bytes>{};
    if (!A || (Op.NumInputs == 2 && !B))
      return false;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
      return write(Op.Output, *A, WroteRegister);
    if (Op.Opcode == NdOp::SUBBYTES && B) {
      const auto Offset = constant(*B);
      if (!Offset || *Offset > A->size() ||
          Op.Output.Size > A->size() - *Offset)
        return false;
      return write(Op.Output, llvm::ArrayRef(*A).slice(*Offset, Op.Output.Size),
                   WroteRegister);
    }
    if (Op.Opcode == NdOp::CONCAT && B) {
      B->append(A->begin(), A->end());
      return write(Op.Output, *B, WroteRegister);
    }
    // Only address construction may perform arithmetic. An unknown argument
    // bit cannot acquire a value through arithmetic or an unsupported opcode.
    const auto Left = constant(*A);
    const auto Right = B ? constant(*B) : std::optional<uint64_t>{};
    if (!Left || !Right || Op.Output.Size > sizeof(uint64_t))
      return false;
    uint64_t Result;
    switch (Op.Opcode) {
    case NdOp::INT_ADD:
      Result = *Left + *Right;
      break;
    case NdOp::INT_OR:
      Result = *Left | *Right;
      break;
    case NdOp::INT_LEFT:
      if (*Right >= Op.Output.Size * 8)
        return false;
      Result = *Left << *Right;
      break;
    default:
      return false;
    }
    return write(Op.Output, *read(NdVar::cst(Result, Op.Output.Size)),
                 WroteRegister);
  }
};

} // namespace

std::optional<MIPS16EntryAdapter>
proveMIPS16EntryAdapter(const BinaryImage &Image, va_t Entry,
                        const DebugContext &Debug,
                        const ExecutableCodeOwnerIndex *Owners) {
  if (!isMIPS(Image.Arch) || isMIPS64(Image.Arch) || !Image.MIPS ||
      !Image.MIPS->HasMIPS16 || !Image.MIPS->hasScalarFPABI(false) ||
      Image.instructionModeAt(Entry) != InstructionMode::MIPSClassic)
    return std::nullopt;
  const va_t End = Image.getFunctionMetadataEnd(Entry, Owners);
  if (End == InvalidVA || End <= Entry)
    return std::nullopt;
  // An exact declaration for this entry has precedence over inferred adapter
  // identity. A containing function or name-only record supplies no contract.
  if (const auto Declared = Debug.resolveFunction(Entry);
      Declared && Declared->Addr == Entry &&
      libc::hasFixedPlatformCallDeclaration(*Declared, &Debug))
    return std::nullopt;
  Decoder Decode;
  if (!Decode.init(Image))
    return std::nullopt;
  Transport State;
  std::optional<va_t> Body;
  for (va_t PC = Entry; PC < End;) {
    if (Image.instructionModeAt(PC) != InstructionMode::MIPSClassic ||
        !Decode.selectMode(Image, PC))
      return std::nullopt;
    const size_t Available = std::min<va_t>(End - PC, 8);
    const auto *Code = Image.readVA(PC, Available);
    DecodedExecutionUnit Unit;
    if (!Code || !Decode.decodeUnitForLift(Code, Available, PC, Unit))
      return std::nullopt;
    std::vector<LowOp> Ops;
    try {
      Decode.liftUnitToLow(Unit, Ops);
    } catch (const UnliftedInstruction &) {
      return std::nullopt;
    }
    State.Temporaries.clear();
    bool WroteRegister = false;
    for (const LowOp &Op : Ops) {
      if (Body)
        return std::nullopt;
      if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs == 1) {
        const auto Target = State.read(Op.Inputs[0]);
        const auto Raw =
            Target ? Transport::constant(*Target) : std::optional<uint64_t>{};
        if (!Raw || !Unit.HasDelaySlot)
          return std::nullopt;
        Body = qualifiedMIPSControlTarget(
            Image, InstructionMode::MIPSClassic,
            Decode.controlTargetMode(Unit.Head, InstructionMode::MIPSClassic),
            *Raw);
        if (!Body || Image.instructionModeAt(*Body) != InstructionMode::MIPS16)
          return std::nullopt;
      } else if (!State.step(Op, WroteRegister)) {
        return std::nullopt;
      }
    }
    if (Body)
      break;
    // At most two writes construct t9, and each argument register is written
    // once. Every other nonterminal unit refuses, so metadata size cannot
    // turn adapter discovery into an unbounded straight-line scan.
    if (!WroteRegister)
      return std::nullopt;
    PC += Unit.spanBytes();
  }
  if (!Body ||
      std::none_of(State.ArgumentWrites.begin(), State.ArgumentWrites.end(),
                   [](bool X) { return X; }))
    return std::nullopt;
  const auto Declaration = Debug.resolveFunction(*Body);
  if (!Declaration || Declaration->Addr != *Body)
    return std::nullopt;
  auto Incoming = libc::floatingCallArguments(*Declaration, Image.Arch,
                                              Image.abiFormat(), &Debug);
  auto Outgoing =
      libc::floatingCallArguments(*Declaration, Image.Arch, Image.abiFormat(),
                                  &Debug, InstructionMode::MIPS16);
  if (!Incoming || !Outgoing || Incoming->ReturnKind == NdTypeKind::Float ||
      Incoming->ReturnElements)
    return std::nullopt;
  const auto Types =
      floatingCallTypes(Image.Arch, Image.abiFormat(), *Incoming);
  if (!Types)
    return std::nullopt;
  const auto Before = placeSourceParameters(Image.Arch, Image.abiFormat(),
                                            Types->Result, Types->Parameters);
  const auto After = placeSourceParameters(
      Image.Arch, Image.abiFormat(), Types->Result, Types->Parameters, false,
      std::nullopt, InstructionMode::MIPS16);
  if (!Before || !After || Before->ResultPointer || After->ResultPointer ||
      Before->Parameters.size() != Types->Parameters.size() ||
      After->Parameters.size() != Types->Parameters.size())
    return std::nullopt;
  bool FloatingTransport = false;
  for (size_t P = 0; P < After->Parameters.size(); ++P)
    for (const auto &Destination : After->Parameters[P]) {
      if (Destination.Indirect)
        return std::nullopt;
      for (unsigned I = 0; I < Destination.Location.ValueBytes; ++I) {
        const unsigned Byte = Destination.Offset + I;
        const auto Source = std::find_if(
            Before->Parameters[P].begin(), Before->Parameters[P].end(),
            [&](const auto &Piece) {
              return !Piece.Indirect && Byte >= Piece.Offset &&
                     Byte - Piece.Offset < Piece.Location.ValueBytes;
            });
        if (Source == Before->Parameters[P].end())
          return std::nullopt;
        const auto &From = Source->Location;
        const auto &To = Destination.Location;
        unsigned Part = Byte - Source->Offset;
        if (To.Kind == SourceABICarrierKind::Stack) {
          if (From.Kind != To.Kind ||
              From.EntryStackOffset + Part != To.EntryStackOffset + I)
            return std::nullopt;
          continue;
        }
        if (To.Kind != SourceABICarrierKind::IntegerRegister ||
            (From.Kind != SourceABICarrierKind::IntegerRegister &&
             From.Kind != SourceABICarrierKind::FloatingRegister))
          return std::nullopt;
        uint64_t Register = From.RegisterOffset;
        if (From.Kind == SourceABICarrierKind::FloatingRegister) {
          FloatingTransport = true;
          if (Image.MIPS->hasScalarFP32ABI(false) && Part >= 4) {
            Register += mipsreg::VectorBytes;
            Part -= 4;
          }
        }
        const auto Actual = State.read(NdVar::reg(To.RegisterOffset + I, 1));
        if (!Actual || Actual->front() != RegisterByteBase + Register + Part)
          return std::nullopt;
      }
    }
  if (!FloatingTransport)
    return std::nullopt;
  return MIPS16EntryAdapter{
      *Body, std::move(*Incoming), std::move(*Outgoing),
      Declaration->PrototypeLibrary == UserPrototypeLibrary, Declaration->Name};
}

void projectMIPS16EntryCalls(const BinaryImage &Image,
                             std::vector<MedFunc> &Functions,
                             const DebugContext *Debug) {
  if (!Debug || !isMIPS(Image.Arch) || isMIPS64(Image.Arch) || !Image.MIPS ||
      !Image.MIPS->HasMIPS16)
    return;
  llvm::DenseMap<va_t, std::optional<MIPS16EntryAdapter>> Adapters;
  std::optional<ExecutableCodeOwnerIndex> Owners;
  for (MedFunc &Function : Functions) {
    llvm::DenseMap<std::pair<int, int>, MedCallInfo *> Calls;
    for (auto &Call : Function.CallInfos)
      Calls.try_emplace({Call.BlockId, Call.OpIdx}, &Call);
    for (auto &Block : Function.Blocks)
      for (size_t I = 0; I < Block.Ops.size(); ++I) {
        auto &Op = Block.Ops[I];
        if (Op.Dead || Op.Opcode != NdOp::CALL || Op.SourceCallHint ||
            !Op.ScalarCallSignature ||
            Op.ScalarCallSignature->MIPS16Parameters || Op.NumInputs == 0 ||
            !Op.Inputs[0].isConst())
          continue;
        const auto Found = Calls.find({Block.Id, int(I)});
        if (Found == Calls.end())
          continue;
        auto &Call = *Found->second;
        if (Call.IsIndirect || Call.SourceCallHint ||
            Call.TargetAddr != Op.Inputs[0].ConstVal)
          continue;
        auto [Known, Inserted] = Adapters.try_emplace(Call.TargetAddr);
        if (Inserted) {
          if (!Owners)
            Owners.emplace(Image);
          Known->second =
              proveMIPS16EntryAdapter(Image, Call.TargetAddr, *Debug, &*Owners);
        }
        const auto &Adapter = Known->second;
        if (!Adapter || Adapter->BodyName.empty())
          continue;
        const auto Signature = recoveredFloatingCallArguments(
            Op, Call, Image.Arch, Image.abiFormat());
        if (!Signature || *Signature != Adapter->EntryArguments)
          continue;
        // SSA already captured the classic FP words as logical scalar values.
        // The compiler now supplies the compressed callee's argument transport.
        // No live register read or original instruction target is replaced.
        Op.Inputs[0] = MedVar::makeConst(Adapter->Body, Op.Inputs[0].Size,
                                         ConstantAddressProvenance::CodeAddress,
                                         Adapter->Body);
        Op.Inputs[0].TheArch = Image.Arch;
        Op.ScalarCallSignature =
            std::make_shared<FloatingCallArguments>(Adapter->BodyArguments);
        Call.TargetAddr = Adapter->Body;
        Call.TargetName = Adapter->BodyName;
        Call.SourceCalleeType.reset();
        Function.CallUnreadRegisters.erase(Op.CallSiteId);
      }
  }
}

} // namespace neverd::pipeline_detail
