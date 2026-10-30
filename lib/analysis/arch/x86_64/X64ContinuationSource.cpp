//===- X64ContinuationSource.cpp - Explicit native continuation C --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// State transport and physical transfers before ordinary source conversion.

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"

#include "BinaryContinuationInstructions.h"
#include "NativeStackControl.h"
#include "X64ArchitecturalValues.h"
#include "X64ContinuationEvents.h"
#include "X64ContinuationFredReturns.h"
#include "X64ContinuationSystemTransfers.h"
#include "X64ContinuationUserInterrupts.h"
#include "X64ControlTransitions.h"
#include "X64FredReturnEncoding.h"
#include "X64MachineFlags.h"
#include "X64Recovery.h"
#include "X64TaskPriority.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/X86/X86SystemState.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Errc.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace neverd::analysis {
namespace {
using State = X64ContinuationStateV1;
using StateV2 = X64ContinuationStateV2;
using StateV4 = X64ContinuationStateV4;
using StateV5 = X64ContinuationStateV5;
using StateV6 = X64ContinuationStateV6;
using StateV7 = X64ContinuationStateV7;
using StateV8 = X64ContinuationStateV8;
using StateV9 = X64ContinuationStateV9;
using Segment = X64ContinuationSegment;
using Exit = X64ContinuationExitV1;
using Flags = detail::X64MachineFlags;
constexpr uint64_t StatePointer = uint64_t{1} << 32;
constexpr uint64_t ScratchBase = uint64_t{1} << 61;

bool hasFredReturnState(const X64ContinuationSourceOptions &Options) {
  return Options.Profile == X64ContinuationProfile::FredReturnNoFaultV9;
}

bool hasUserInterruptState(const X64ContinuationSourceOptions &Options) {
  return Options.Profile == X64ContinuationProfile::UserInterruptNoFaultV8;
}

bool hasFastSystemState(const X64ContinuationSourceOptions &Options) {
  return Options.Profile == X64ContinuationProfile::KernelFastSystemNoFaultV7 ||
         hasUserInterruptState(Options);
}

bool hasEventState(const X64ContinuationSourceOptions &Options) {
  return Options.Profile == X64ContinuationProfile::KernelEventsNoFaultV6 ||
         hasFastSystemState(Options) || hasFredReturnState(Options);
}

bool hasReturnState(const X64ContinuationSourceOptions &Options) {
  return Options.Profile == X64ContinuationProfile::KernelReturnNoFaultV5 ||
         hasEventState(Options);
}

bool hasFaultAddressState(const X64ContinuationSourceOptions &Options) {
  return Options.Profile ==
             X64ContinuationProfile::KernelFaultAddressNoFaultV4 ||
         hasReturnState(Options);
}

bool hasSmapState(const X64ContinuationSourceOptions &Options) {
  return Options.Profile == X64ContinuationProfile::KernelSmapNoFaultV3 ||
         hasFaultAddressState(Options);
}

bool hasControlState(const X64ContinuationSourceOptions &Options) {
  return Options.Profile == X64ContinuationProfile::KernelControlsNoFaultV2 ||
         hasSmapState(Options);
}

InterpreterMachineStateProfile
flagsProfile(const X64ContinuationSourceOptions &Options) {
  return hasSmapState(Options)
             ? InterpreterMachineStateProfile::KernelX64SmapNoFaultV2
             : InterpreterMachineStateProfile::KernelX64NoFaultV1;
}

static_assert(std::is_standard_layout_v<State>);
static_assert(offsetof(State, RFlags) == 128);
static_assert(offsetof(State, PC) == 136);
static_assert(offsetof(State, FSBase) == 144);
static_assert(offsetof(State, GSBase) == 152);
static_assert(offsetof(State, ZMM) == 160);
static_assert(offsetof(State, Undefined) == 2208);
static_assert(sizeof(State) == 2272);
static_assert(std::is_standard_layout_v<StateV2>);
static_assert(offsetof(StateV2, Core) == 0);
static_assert(offsetof(StateV2, CR8) == 2272);
static_assert(offsetof(StateV2, KernelGSBase) == 2280);
static_assert(offsetof(StateV2, MXCSR) == 2288);
static_assert(sizeof(StateV2) == 2304);
static_assert(std::is_standard_layout_v<StateV4>);
static_assert(offsetof(StateV4, Core) == 0);
static_assert(offsetof(StateV4, CR2) == 2304);
static_assert(sizeof(StateV4) == 2320);
static_assert(std::is_standard_layout_v<StateV5>);
static_assert(sizeof(Segment) == 16);
static_assert(offsetof(StateV5, Core) == 0);
static_assert(offsetof(StateV5, STAR) == 2320);
static_assert(offsetof(StateV5, CS) == 2328);
static_assert(offsetof(StateV5, SS) == 2344);
static_assert(sizeof(StateV5) == 2368);
static_assert(std::is_standard_layout_v<StateV6>);
static_assert(offsetof(StateV6, Core) == 0);
static_assert(offsetof(StateV6, GDTR) == 2368);
static_assert(offsetof(StateV6, IDTR) == 2384);
static_assert(offsetof(StateV6, LDTR) == 2400);
static_assert(offsetof(StateV6, TR) == 2416);
static_assert(offsetof(StateV6, DS) == 2432);
static_assert(offsetof(StateV6, ES) == 2448);
static_assert(offsetof(StateV6, FS) == 2464);
static_assert(offsetof(StateV6, GS) == 2472);
static_assert(offsetof(StateV6, Unusable) == 2480);
static_assert(offsetof(StateV6, NMIBlocked) == 2481);
static_assert(sizeof(StateV6) == 2496);
static_assert(std::is_standard_layout_v<StateV7>);
static_assert(offsetof(StateV7, Core) == 0);
static_assert(offsetof(StateV7, SysenterCS) == 2496);
static_assert(offsetof(StateV7, SysenterESP) == 2504);
static_assert(offsetof(StateV7, SysenterEIP) == 2512);
static_assert(sizeof(StateV7) == 2528);
static_assert(std::is_standard_layout_v<StateV8>);
static_assert(offsetof(StateV8, Core) == 0);
static_assert(offsetof(StateV8, UIF) == 2528);
static_assert(offsetof(StateV8, MonitorActive) == 2529);
static_assert(sizeof(StateV8) == 2544);
static_assert(std::is_standard_layout_v<StateV9>);
static_assert(offsetof(StateV9, Core) == 0);
static_assert(offsetof(StateV9, FredConfig) == 2544);
static_assert(offsetof(StateV9, STIBlocked) == 2552);
static_assert(offsetof(StateV9, PendingDebug) == 2553);
static_assert(sizeof(StateV9) == 2560);

llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                 Text.str().c_str());
}

NdVar constant(uint64_t Value, uint16_t Bytes = 8) {
  return NdVar::scalar(Value, Bytes);
}

std::optional<unsigned> flagBit(NdVar V) {
  if (V.Size == 1)
    for (const auto &[Offset, Bit] : Flags::Flags)
      if (V.Offset == Offset)
        return Bit;
  return std::nullopt;
}

std::optional<uint64_t> stateOffset(NdVar V) {
  if (V.Offset < x86reg::RIP && V.Size <= 8 && V.Offset % 8 + V.Size <= 8)
    return offsetof(State, GPR) + V.Offset;
  if (V.Offset >= x86reg::VectorBase && V.Size <= 16 &&
      V.Offset < x86reg::OpmaskBase &&
      (V.Offset - x86reg::VectorBase) % x86reg::VectorRegStride + V.Size <=
          x86reg::VectorRegStride)
    return offsetof(State, ZMM) + V.Offset - x86reg::VectorBase;
  return std::nullopt;
}

bool supportedValue(NdVar V) {
  if (!V.Size)
    return true;
  if (V.isReg())
    return flagBit(V).has_value() || stateOffset(V).has_value();
  if (V.isTemp())
    return V.Size <= 16 && V.Offset < ScratchBase - V.Size;
  return V.isConst() && V.Size <= 16;
}

bool flagIntrinsic(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs || !Op.Inputs[0].isConst())
    return false;
  const auto I = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  return I == Intrinsic::Pushf || I == Intrinsic::Cli || I == Intrinsic::Sti ||
         I == Intrinsic::Clac || I == Intrinsic::Stac;
}

bool alignmentIntrinsic(LowOp Op) {
  if (Op.Opcode != NdOp::INTRINSIC)
    return false;
  // The address is materialized with the live FS/GS base before emission.
  Op.MemoryAddressSpace = NdMemoryAddressSpace::Default;
  SpecializationOptions Options;
  Options.ExplicitMachineState = true;
  Options.NormalNonfaultingExecution = true;
  return x64::alignment::supportsOperation(Op, Options);
}

SpecializationOptions
controlOptions(const X64ContinuationSourceOptions &Options) {
  SpecializationOptions Controls;
  Controls.Architecture = Arch::X64;
  Controls.ExplicitMachineState = true;
  Controls.NormalNonfaultingExecution = true;
  Controls.X64FlagsProfile = flagsProfile(Options);
  if (hasControlState(Options)) {
    Controls.X64MXCSRMask = Options.MXCSRMask;
    Controls.X64LegacyGSState = !hasFredReturnState(Options);
  }
  return Controls;
}

bool controlIntrinsic(LowOp Op, const X64ContinuationSourceOptions &Options) {
  if (!hasControlState(Options))
    return false;
  Op.MemoryAddressSpace = NdMemoryAddressSpace::Default;
  const auto Controls = controlOptions(Options);
  return detail::taskPriorityOperation(Op).has_value() ||
         x64::mxcsr::supportsScalarOperation(Op, Controls) ||
         x64::legacygs::supportsOperation(Op, Controls);
}

bool faultAddressIntrinsic(const LowOp &Op,
                           const X64ContinuationSourceOptions &Options) {
  if (!hasFaultAddressState(Options) || Op.Opcode != NdOp::INTRINSIC ||
      !Op.NumInputs || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  return (Id == Intrinsic::ReadCr2 && Op.NumInputs == 1 &&
          Op.Output.Size == 8) ||
         (Id == Intrinsic::WriteCr2 && Op.NumInputs == 2 && !Op.Output.Size &&
          Op.Inputs[1].Size == 8);
}

bool sysretIntrinsic(const LowOp &Op,
                     const X64ContinuationSourceOptions &Options) {
  const auto Width = Op.Inputs[1].Size;
  return hasReturnState(Options) && Op.Opcode == NdOp::INTRINSIC &&
         Op.NumInputs == 3 && !Op.Output.Size && Op.Inputs[0].isConst() &&
         Op.Inputs[0].Size == 2 &&
         Op.Inputs[0].Offset == uint64_t(Intrinsic::Sysretq) &&
         (Width == 4 || Width == 8) &&
         Op.Inputs[1] == NdVar::reg(x86reg::RCX, Width) &&
         Op.Inputs[2] == NdVar::reg(x86reg::R11, Width) &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         Op.MemoryOrdering == NdMemoryOrdering::None;
}

bool eventIntrinsic(const LowOp &Op, Intrinsic Id, unsigned Inputs,
                    NdVar Output = {}) {
  return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == Inputs &&
         Op.Output.Size == Output.Size &&
         (!Output.Size || Op.Output == Output) && Op.Inputs[0].isConst() &&
         Op.Inputs[0].Size == 2 && Op.Inputs[0].Offset == uint64_t(Id) &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         Op.MemoryOrdering == NdMemoryOrdering::None;
}

std::optional<uint8_t>
softwareInterruptVector(const SpecializationInstruction &Instruction) {
  const auto &Bytes = Instruction.NativeBytes;
  if (Instruction.Ops.size() != 1)
    return std::nullopt;
  const auto &Op = Instruction.Ops.front();
  if (Bytes == std::vector<uint8_t>{0xcc})
    return eventIntrinsic(Op, Intrinsic::Int3, 1) ? std::optional<uint8_t>(3)
                                                  : std::nullopt;
  if (Bytes.size() != 2 || Bytes[0] != 0xcd)
    return std::nullopt;
  const uint8_t Vector = Bytes[1];
  if (Vector == 3)
    return eventIntrinsic(Op, Intrinsic::Int3, 1) ? std::optional(Vector)
                                                  : std::nullopt;
  // The strict binary provider also supplies ordinary source service shapes.
  // This explicit CPL0 profile owns the original IDT transfer, including those
  // vectors: it neither invokes an OS service nor invents its return in RAX.
  if (Vector == 0x2d) {
    if (!eventIntrinsic(Op, Intrinsic::DebugService, 6,
                        NdVar::reg(x86reg::RAX, 8)))
      return std::nullopt;
    const uint64_t Registers[] = {x86reg::RAX, x86reg::RCX, x86reg::RDX,
                                  x86reg::R8, x86reg::R9};
    for (unsigned I = 0; I != std::size(Registers); ++I)
      if (Op.Inputs[I + 1] != NdVar::reg(Registers[I], 8))
        return std::nullopt;
  } else {
    const bool Void = Vector == 0x29 || Vector == 0x2c;
    if (!eventIntrinsic(Op, Intrinsic::IntN, Vector == 0x29 ? 3 : 2,
                        Void ? NdVar{} : NdVar::reg(x86reg::RAX, 8)) ||
        Op.Inputs[1] != NdVar::cst(Vector, 1) ||
        (Vector == 0x29 && Op.Inputs[2] != NdVar::reg(x86reg::RCX, 4)))
      return std::nullopt;
  }
  return Vector;
}

bool returnFrameInstruction(const SpecializationInstruction &Instruction,
                            Intrinsic Id, unsigned Fields) {
  if (Instruction.Origin.Control != LowInstructionControl::Terminator ||
      Instruction.Ops.size() != Fields * 2 ||
      !eventIntrinsic(Instruction.Ops.back(), Id, Fields + 1))
    return false;
  const auto &Transfer = Instruction.Ops.back();
  for (unsigned I = 0; I != Fields; ++I) {
    const auto &Read = Instruction.Ops[I ? I * 2 : 0];
    NdVar Address = NdVar::reg(x86reg::RSP, 8);
    if (I) {
      const auto &Add = Instruction.Ops[I * 2 - 1];
      if (Add.Opcode != NdOp::INT_ADD || !Add.Output.isTemp() ||
          Add.Output.Size != 8 || Add.NumInputs != 2 ||
          Add.Inputs[0] != Address || Add.Inputs[1] != constant(I * 8) ||
          Add.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Add.MemoryOrdering != NdMemoryOrdering::None)
        return false;
      Address = Add.Output;
    }
    if (Read.Opcode != NdOp::LOAD || !Read.Output.isTemp() ||
        Read.Output.Size != 8 || Read.NumInputs != 1 ||
        Read.Inputs[0] != Address || Transfer.Inputs[I + 1] != Read.Output ||
        Read.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Read.MemoryOrdering != NdMemoryOrdering::None)
      return false;
  }
  return true;
}

std::optional<uint16_t>
farReturnDiscard(const SpecializationInstruction &Instruction) {
  const auto &Bytes = Instruction.NativeBytes;
  if (!returnFrameInstruction(Instruction, Intrinsic::Retf, 2))
    return std::nullopt;
  if (Bytes == std::vector<uint8_t>{0x48, 0xcb})
    return 0;
  if (Bytes.size() == 4 && Bytes[0] == 0x48 && Bytes[1] == 0xca)
    return uint16_t(Bytes[2] | uint16_t(Bytes[3]) << 8);
  return std::nullopt;
}

bool pageTableIntrinsic(const LowOp &Op,
                        const X64ContinuationSourceOptions &Options) {
  if (!Options.NativePageTableAccess || Op.Opcode != NdOp::INTRINSIC ||
      !Op.NumInputs || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  return (Id == Intrinsic::ReadCr3 && Op.NumInputs == 1 &&
          Op.Output.Size == 8) ||
         (Id == Intrinsic::WriteCr3 && Op.NumInputs == 2 && !Op.Output.Size &&
          Op.Inputs[1].Size == 8);
}

bool fenceIntrinsic(const LowOp &Op,
                    const X64ContinuationSourceOptions &Options) {
  return hasControlState(Options) && Op.Opcode == NdOp::INTRINSIC &&
         !Op.Output.Size && Op.NumInputs == 1 && Op.Inputs[0].isConst() &&
         Op.Inputs[0].Size == 2 &&
         Op.Inputs[0].Offset == uint64_t(Intrinsic::Lfence) &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
}

bool writePrefetchIntrinsic(const LowOp &Op,
                            const X64ContinuationSourceOptions &Options) {
  return Options.NativeWritePrefetch && Op.Opcode == NdOp::INTRINSIC &&
         !Op.Output.Size && Op.NumInputs == 2 && Op.Inputs[0].isConst() &&
         Op.Inputs[0].Size == 2 &&
         Op.Inputs[0].Offset == uint64_t(Intrinsic::PrefetchW) &&
         Op.Inputs[1].Size == 8;
}

bool descriptorQueryIntrinsic(const LowOp &Op,
                              const X64ContinuationSourceOptions &Options) {
  if (!Options.NativeDescriptorQueries || Op.Opcode != NdOp::INTRINSIC ||
      Op.NumInputs != 2 || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2)
    return false;
  switch (static_cast<Intrinsic>(Op.Inputs[0].Offset)) {
  case Intrinsic::Sgdt:
    return !Op.Output.Size && Op.Inputs[1].Size == 8;
  case Intrinsic::Verw:
    return Op.Output.Size == 1 && Op.Inputs[1].Size == 8;
  case Intrinsic::X86LoadSegmentLimit:
    return Op.Output.Size == 8 && Op.Inputs[1].Size == 2 &&
           Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
  default:
    return false;
  }
}

bool msrIntrinsic(const LowOp &Op,
                  const X64ContinuationSourceOptions &Options) {
  if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (Id == Intrinsic::Rdmsr)
    return (Options.SegmentBaseMSRAddressBits || hasReturnState(Options)) &&
           Op.NumInputs == 2 && Op.Output.Size == 8 && Op.Inputs[1].Size == 4;
  return Id == Intrinsic::Wrmsr &&
         (Options.SegmentBaseMSRAddressBits || hasReturnState(Options) ||
          Options.NativeSpeculationControlMsrs) &&
         Op.NumInputs == 3 && !Op.Output.Size && Op.Inputs[1].Size == 4 &&
         Op.Inputs[2].Size == 8;
}

bool unsignedWideDivisionIntrinsic(const LowOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
         Op.Inputs[0].isConst() &&
         Op.Inputs[0].Offset ==
             uint64_t(Intrinsic::X86RequireDivPrecondition) &&
         intrinsicX86DivPreconditionShapeIsValid(
             Intrinsic::X86RequireDivPrecondition,
             x86DivPreconditionLowShape(Op, Arch::X64)) &&
         Op.Inputs[1].Size == 16 &&
         Op.Inputs[3].Offset == uint64_t(X86DivKind::Unsigned);
}

bool wideDivisionArithmetic(const LowOp &Op) {
  if (Op.Output.Size != 16)
    return false;
  if (Op.Opcode == NdOp::INT_ZEXT)
    return Op.NumInputs == 1 && Op.Inputs[0].Size == 8;
  if (Op.NumInputs != 2 || Op.Inputs[0].Size != 16 || Op.Inputs[1].Size != 16)
    return false;
  if (Op.Opcode == NdOp::INT_LEFT)
    return Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 64;
  return Op.Opcode == NdOp::INT_OR || Op.Opcode == NdOp::INT_DIV ||
         Op.Opcode == NdOp::INT_REM;
}

// Integer widening is not specific to DIV: scalar vector loads and full-width
// products use the same checked bit-vector operations and existing emitters.
bool wideIntegerOperation(const LowOp &Op) {
  if ((Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT) &&
      Op.Output.Size == 16 && Op.NumInputs == 1)
    return Op.Inputs[0].Size && Op.Inputs[0].Size <= 8;
  if (Op.NumInputs != 2 || Op.Inputs[0].Size != 16 || Op.Inputs[1].Size != 16)
    return false;
  if (Op.Opcode == NdOp::INT_MULT)
    return Op.Output.Size == 16;
  return (Op.Opcode == NdOp::INT_EQUAL || Op.Opcode == NdOp::INT_NOTEQUAL) &&
         Op.Output.Size == 1;
}

bool lockedMemoryIntrinsic(const LowOp &Op,
                           const X64ContinuationSourceOptions &Options) {
  if (!hasControlState(Options) || Op.Opcode != NdOp::INTRINSIC ||
      !Op.NumInputs || !Op.Inputs[0].isConst())
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  const auto Kind = x86SystemStateContract(Id).Kind;
  return (Kind == X86SystemStateKind::LockedLogic ||
          Kind == X86SystemStateKind::LockedBit) &&
         x86SystemStateShapeIsValid(Id, x86SystemStateLowShape(Op, Arch::X64));
}

bool supportedOperation(const LowOp &Op,
                        const X64ContinuationSourceOptions &Options,
                        bool WideDivision) {
  if (Op.NumInputs > std::size(Op.Inputs))
    return false;
  const bool AtomicScalar =
      hasControlState(Options) &&
      (Op.Opcode == NdOp::ATOMIC_ADD || Op.Opcode == NdOp::ATOMIC_XCHG) &&
      Op.NumInputs == 2 && Op.Output.isTemp() &&
      (Op.Output.Size == 1 || Op.Output.Size == 2 || Op.Output.Size == 4 ||
       Op.Output.Size == 8) &&
      Op.Inputs[0].Size == 8 && Op.Inputs[1].Size == Op.Output.Size &&
      Op.MemoryOrdering == NdMemoryOrdering::SequentiallyConsistent;
  const bool Locked = lockedMemoryIntrinsic(Op, Options);
  if (!supportedValue(Op.Output) ||
      (Op.MemoryOrdering != NdMemoryOrdering::None && !AtomicScalar &&
       !Locked) ||
      !isKnownMemoryAddressSpace(Op.MemoryAddressSpace))
    return false;
  for (unsigned I = 0; I < Op.NumInputs; ++I)
    if (!supportedValue(Op.Inputs[I]))
      return false;
  if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default &&
      Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE && !AtomicScalar &&
      !Locked && !alignmentIntrinsic(Op) && !controlIntrinsic(Op, Options) &&
      !writePrefetchIntrinsic(Op, Options) &&
      !descriptorQueryIntrinsic(Op, Options))
    return false;
  if (AtomicScalar || Locked || wideIntegerOperation(Op))
    return true;
  if (WideDivision &&
      (unsignedWideDivisionIntrinsic(Op) || wideDivisionArithmetic(Op)))
    return true;
  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::LOAD:
  case NdOp::STORE:
  case NdOp::SELECT:
  case NdOp::CONCAT:
  case NdOp::SUBBYTES:
  case NdOp::BRANCH:
  case NdOp::COND_BR:
  case NdOp::INDIR_BR:
  case NdOp::NOP:
  case NdOp::POPCOUNT:
  case NdOp::LZCOUNT:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
    return true;
  case NdOp::INT_AND:
    // The lifter masks CL into an operand-width count. The constant-analysis
    // query below intentionally accepts only equal-width operands; mixed-width
    // bitwise operations still use the existing LowToMed/backend semantics.
    return Op.NumInputs == 2 && Op.Output.Size && Op.Output.Size <= 8 &&
           Op.Inputs[0].Size && Op.Inputs[0].Size <= Op.Output.Size &&
           Op.Inputs[1].Size && Op.Inputs[1].Size <= Op.Output.Size;
  case NdOp::INT_XOR:
    // Scalar and legacy whole-vector XOR share the existing integer lowering.
    return Op.NumInputs == 2 && Op.Output.Size && Op.Output.Size <= 16 &&
           Op.Inputs[0].Size == Op.Output.Size &&
           Op.Inputs[1].Size == Op.Output.Size;
  case NdOp::INTRINSIC:
    return flagIntrinsic(Op) || alignmentIntrinsic(Op) ||
           controlIntrinsic(Op, Options) ||
           faultAddressIntrinsic(Op, Options) || fenceIntrinsic(Op, Options) ||
           writePrefetchIntrinsic(Op, Options) || msrIntrinsic(Op, Options) ||
           descriptorQueryIntrinsic(Op, Options) ||
           pageTableIntrinsic(Op, Options);
  default:
    // Admission and arithmetic widths use the scalar owner. The operation
    // itself is still emitted through LowToMed and the existing backends.
    return bool(lowScalarConstantResult(
        Op, [](const NdVar &) { return std::optional<uint64_t>{1}; }));
  }
}

struct Writer {
  LowFunc &Function;
  LowBlock *Block;
  const X64ContinuationSourceOptions &Options;
  uint64_t Next = ScratchBase;

  NdVar temporary(uint16_t Bytes = 8) {
    const NdVar V = NdVar::tmp(Next, Bytes);
    Next += 16;
    return V;
  }

  void emit(NdOp Code, NdVar Output, std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Code;
    Op.Output = Output;
    for (NdVar V : Inputs)
      Op.addInput(V);
    Block->Ops.push_back(Op);
  }

  void append(llvm::ArrayRef<LowOp> Ops) {
    Block->Ops.insert(Block->Ops.end(), Ops.begin(), Ops.end());
  }

  void rejectIf(NdVar Bad) {
    const int NextId = static_cast<int>(Function.Blocks.size());
    LowBlock NextBlock;
    NextBlock.Id = NextId;
    NextBlock.StartAddr = Function.Entry + NextId;
    NextBlock.Preds = {Block->Id};
    Function.Blocks[2].Preds.push_back(Block->Id);
    Block->Succs = {NextId, 2};
    emit(NdOp::COND_BR, {}, {NdVar::cst(Function.Blocks[2].StartAddr, 8), Bad});
    Function.Blocks.push_back(std::move(NextBlock));
    Block = &Function.Blocks.back();
  }

  /// The taken path must leave the instruction. Its reads and writes never
  /// execute on the fallthrough path (unlike an eager SELECT expression).
  template <class Emit> void exitIf(NdVar Condition, Emit EmitExit) {
    const int Parent = Block->Id;
    const int ExitId = static_cast<int>(Function.Blocks.size());
    const int NextId = ExitId + 1;
    for (int Id : {ExitId, NextId}) {
      LowBlock Child;
      Child.Id = Id;
      Child.StartAddr = Function.Entry + Id;
      Child.Preds = {Parent};
      Function.Blocks.push_back(std::move(Child));
    }
    Block = &Function.Blocks[Parent];
    Block->Succs = {NextId, ExitId};
    emit(NdOp::COND_BR, {},
         {NdVar::cst(Function.Blocks[ExitId].StartAddr, 8), Condition});
    Block = &Function.Blocks[ExitId];
    EmitExit();
    Block = &Function.Blocks[NextId];
  }

  NdVar address(uint64_t Offset) {
    const NdVar V = temporary();
    emit(NdOp::INT_ADD, V, {NdVar::reg(StatePointer, 8), constant(Offset)});
    return V;
  }

  NdVar load(uint64_t Offset, uint16_t Bytes = 8) {
    const NdVar V = temporary(Bytes);
    emit(NdOp::LOAD, V, {address(Offset)});
    return V;
  }

  void store(uint64_t Offset, NdVar V) {
    emit(NdOp::STORE, {}, {address(Offset), V});
  }

  void storeIf(NdVar Condition, NdVar Address, NdVar Value) {
    const int Parent = Block->Id;
    const int WriteId = static_cast<int>(Function.Blocks.size());
    const int JoinId = WriteId + 1;
    for (int Id : {WriteId, JoinId}) {
      LowBlock Child;
      Child.Id = Id;
      Child.StartAddr = Function.Entry + Id;
      Child.Preds = Id == WriteId ? std::vector<int>{Parent}
                                  : std::vector<int>{Parent, WriteId};
      Function.Blocks.push_back(std::move(Child));
    }
    // Appending blocks may move the vector. Reacquire the parent before use.
    Block = &Function.Blocks[Parent];
    Block->Succs = {JoinId, WriteId};
    emit(NdOp::COND_BR, {},
         {NdVar::cst(Function.Blocks[WriteId].StartAddr, 8), Condition});
    Block = &Function.Blocks[WriteId];
    emit(NdOp::STORE, {}, {Address, Value});
    Block->Succs = {JoinId};
    emit(NdOp::BRANCH, {}, {NdVar::cst(Function.Blocks[JoinId].StartAddr, 8)});
    Block = &Function.Blocks[JoinId];
  }

  NdVar read(NdVar V) {
    if (V.isConst())
      return constant(V.Offset, V.Size);
    if (!V.isReg())
      return V;
    if (const auto Bit = flagBit(V)) {
      const NdVar Shifted = temporary(), Masked = temporary();
      emit(NdOp::INT_RIGHT, Shifted,
           {load(offsetof(State, RFlags)), constant(*Bit)});
      emit(NdOp::INT_AND, Masked, {Shifted, constant(1)});
      const NdVar Result = temporary(1);
      emit(NdOp::INT_NOTEQUAL, Result, {Masked, constant(0)});
      return Result;
    }
    return load(*stateOffset(V), V.Size);
  }

  void write(NdVar Destination, NdVar Value, bool LowSlice = false) {
    if (!Destination.isReg()) {
      emit(NdOp::COPY, Destination, {Value});
      return;
    }
    if (const auto Bit = flagBit(Destination)) {
      const NdVar Kept = temporary(), Extended = temporary();
      const NdVar Shifted = temporary(), Packed = temporary();
      emit(NdOp::INT_AND, Kept,
           {load(offsetof(State, RFlags)), constant(~(uint64_t{1} << *Bit))});
      emit(NdOp::INT_ZEXT, Extended, {Value});
      emit(NdOp::INT_LEFT, Shifted, {Extended, constant(*Bit)});
      emit(NdOp::INT_OR, Packed, {Kept, Shifted});
      store(offsetof(State, RFlags), Packed);
      return;
    }
    if (Destination.Offset < x86reg::RIP && !LowSlice &&
        getTargetRegInfo(Arch::X64).writeZeroExtends(Destination.Offset,
                                                     Destination.Size)) {
      const NdVar Full = temporary();
      emit(NdOp::INT_ZEXT, Full, {Value});
      store(offsetof(State, GPR) + Destination.Offset, Full);
      return;
    }
    store(*stateOffset(Destination), Value);
  }

  void leave(Exit Kind, std::optional<NdVar> PC = std::nullopt) {
    if (PC)
      store(offsetof(State, PC), *PC);
    emit(NdOp::COPY, NdVar::reg(x86reg::RAX, 8),
         {constant(static_cast<uint64_t>(Kind))});
    emit(NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, 8)});
  }

  void invalidateMonitor(std::optional<NdVar> When = std::nullopt) {
    if (!hasUserInterruptState(Options) && !hasFredReturnState(Options))
      return;
    NdVar Value = constant(0, 1);
    if (When) {
      Value = temporary(1);
      emit(NdOp::SELECT, Value,
           {*When, constant(0, 1), load(offsetof(StateV8, MonitorActive), 1)});
    }
    store(offsetof(StateV8, MonitorActive), Value);
  }

  void undefined(const LowUndefinedEffect &Effect, size_t Index) {
    const NdVar Witness =
        load(offsetof(State, Undefined) + Index * 8, Effect.Output.Size);
    // A complete unconditional arbitrary production needs no previous value;
    // an instruction-local carrier may not have existed before this point.
    const bool Whole =
        Effect.BitOffset == 0 && Effect.BitCount == Effect.Output.Size * 8;
    const NdVar Old = Whole && !Effect.When ? constant(0, Effect.Output.Size)
                                            : read(Effect.Output);
    const NdVar When = Effect.When ? read(*Effect.When) : constant(1, 1);
    const auto Result = detail::lowerX64ArchitecturalBits(
        *this, Old, When, Witness, Effect.BitOffset, Effect.BitCount);
    // Architectural arbitrary bits alter this byte view, not an EAX write.
    write(Effect.Output, Result, true);
  }

  void msr(LowOp Op) {
    const bool Write = Op.Inputs[0].Offset == uint64_t(Intrinsic::Wrmsr);
    const NdVar Selector = Op.Inputs[1];
    NdVar Segment = constant(0, 1), Offset = constant(0);
    if (Options.SegmentBaseMSRAddressBits) {
      const NdVar FS = temporary(1), GS = temporary(1), KernelGS = temporary(1);
      const NdVar EitherGS = temporary(1), GSOffset = temporary();
      Segment = temporary(1);
      Offset = temporary();
      emit(NdOp::INT_EQUAL, FS, {Selector, constant(0xc0000100, 4)});
      emit(NdOp::INT_EQUAL, GS, {Selector, constant(0xc0000101, 4)});
      emit(NdOp::INT_EQUAL, KernelGS, {Selector, constant(0xc0000102, 4)});
      emit(NdOp::BOOL_OR, EitherGS, {GS, KernelGS});
      emit(NdOp::BOOL_OR, Segment, {FS, EitherGS});
      emit(NdOp::SELECT, GSOffset,
           {GS, constant(offsetof(State, GSBase)),
            constant(offsetof(StateV2, KernelGSBase))});
      emit(NdOp::SELECT, Offset,
           {FS, constant(offsetof(State, FSBase)), GSOffset});
    }
    NdVar StateBacked = Segment, Star = constant(0, 1);
    if (hasReturnState(Options)) {
      Star = temporary(1);
      StateBacked = temporary(1);
      const NdVar StateOffset = temporary();
      emit(NdOp::INT_EQUAL, Star, {Selector, constant(0xc0000081, 4)});
      emit(NdOp::BOOL_OR, StateBacked, {Segment, Star});
      emit(NdOp::SELECT, StateOffset,
           {Star, constant(offsetof(StateV5, STAR)), Offset});
      Offset = StateOffset;
    }
    NdVar EntryCS = constant(0, 1), EntryAddress = constant(0, 1);
    if (hasFastSystemState(Options)) {
      const NdVar ESP = temporary(1), EIP = temporary(1);
      const NdVar Entry = temporary(1), Bound = temporary(1);
      const NdVar StackOffset = temporary(), EntryOffset = temporary();
      const NdVar StateOffset = temporary();
      EntryCS = temporary(1);
      EntryAddress = temporary(1);
      emit(NdOp::INT_EQUAL, EntryCS, {Selector, constant(0x174, 4)});
      emit(NdOp::INT_EQUAL, ESP, {Selector, constant(0x175, 4)});
      emit(NdOp::INT_EQUAL, EIP, {Selector, constant(0x176, 4)});
      emit(NdOp::BOOL_OR, EntryAddress, {ESP, EIP});
      emit(NdOp::BOOL_OR, Entry, {EntryCS, EntryAddress});
      emit(NdOp::BOOL_OR, Bound, {StateBacked, Entry});
      emit(NdOp::SELECT, StackOffset,
           {ESP, constant(offsetof(StateV7, SysenterESP)),
            constant(offsetof(StateV7, SysenterEIP))});
      emit(NdOp::SELECT, EntryOffset,
           {EntryCS, constant(offsetof(StateV7, SysenterCS)), StackOffset});
      emit(NdOp::SELECT, StateOffset, {Entry, EntryOffset, Offset});
      Offset = StateOffset;
      StateBacked = Bound;
    }
    NdVar Native = constant(0, 1), Allowed = StateBacked;
    if (Write && Options.NativeSpeculationControlMsrs) {
      const NdVar Control = temporary(1), Command = temporary(1);
      Native = temporary(1);
      Allowed = temporary(1);
      emit(NdOp::INT_EQUAL, Control, {Selector, constant(0x48, 4)});
      emit(NdOp::INT_EQUAL, Command, {Selector, constant(0x49, 4)});
      emit(NdOp::BOOL_OR, Native, {Control, Command});
      emit(NdOp::BOOL_OR, Allowed, {StateBacked, Native});
    }
    const NdVar Unknown = temporary(1);
    emit(NdOp::BOOL_NOT, Unknown, {Allowed});
    rejectIf(Unknown);
    if (Write && hasReturnState(Options)) {
      // The portable STAR write domain keeps Intel's reserved low half zero.
      // This is a profile refusal, not a claim that every CPU faults on a
      // nonzero value. The upper half contains selectors, not a code address.
      const NdVar Low = temporary(), Nonzero = temporary(1), Bad = temporary(1);
      emit(NdOp::INT_AND, Low, {Op.Inputs[2], constant(UINT32_MAX)});
      emit(NdOp::INT_NOTEQUAL, Nonzero, {Low, constant(0)});
      emit(NdOp::BOOL_AND, Bad, {Star, Nonzero});
      rejectIf(Bad);
    }
    if (Write && Options.SegmentBaseMSRAddressBits) {
      const NdVar Shifted = temporary(), Canonical = temporary();
      const NdVar Noncanonical = temporary(1), Bad = temporary(1);
      const auto Shift = constant(64 - *Options.SegmentBaseMSRAddressBits);
      emit(NdOp::INT_LEFT, Shifted, {Op.Inputs[2], Shift});
      emit(NdOp::INT_ASHR, Canonical, {Shifted, Shift});
      emit(NdOp::INT_NOTEQUAL, Noncanonical, {Op.Inputs[2], Canonical});
      emit(NdOp::BOOL_AND, Bad, {Segment, Noncanonical});
      rejectIf(Bad);
    }
    if (Write && Options.SysenterMSRAddressBits) {
      const NdVar Shifted = temporary(), Canonical = temporary();
      const NdVar Noncanonical = temporary(1), Bad = temporary(1);
      const auto Shift = constant(64 - *Options.SysenterMSRAddressBits);
      emit(NdOp::INT_LEFT, Shifted, {Op.Inputs[2], Shift});
      emit(NdOp::INT_ASHR, Canonical, {Shifted, Shift});
      emit(NdOp::INT_NOTEQUAL, Noncanonical, {Op.Inputs[2], Canonical});
      emit(NdOp::BOOL_AND, Bad, {EntryAddress, Noncanonical});
      rejectIf(Bad);
      // Intel SDM Vol. 4: CS[31:16] is readable/writable, CS[63:32]
      // ignores writes and reads as zero. Do not narrow it to the selector.
      const NdVar Low = temporary(), Value = temporary();
      emit(NdOp::INT_AND, Low, {Op.Inputs[2], constant(UINT32_MAX)});
      emit(NdOp::SELECT, Value, {EntryCS, Low, Op.Inputs[2]});
      Op.Inputs[2] = Value;
    }
    const auto StateAccess = [&] {
      const NdVar Address = temporary();
      emit(NdOp::INT_ADD, Address, {NdVar::reg(StatePointer, 8), Offset});
      if (Write)
        emit(NdOp::STORE, {}, {Address, Op.Inputs[2]});
      else
        emit(NdOp::LOAD, Op.Output, {Address});
    };
    if (!Write || !Options.NativeSpeculationControlMsrs) {
      StateAccess();
    } else if (!Options.SegmentBaseMSRAddressBits && !hasReturnState(Options)) {
      Block->Ops.push_back(Op);
    } else {
      const int SegmentId = static_cast<int>(Function.Blocks.size());
      const int NativeId = SegmentId + 1, JoinId = SegmentId + 2;
      const int Parent = Block->Id;
      for (int Id : {SegmentId, NativeId, JoinId}) {
        LowBlock Child;
        Child.Id = Id;
        Child.StartAddr = Function.Entry + Id;
        Child.Preds = Id == JoinId ? std::vector<int>{SegmentId, NativeId}
                                   : std::vector<int>{Parent};
        Function.Blocks.push_back(std::move(Child));
      }
      Block->Succs = {SegmentId, NativeId};
      emit(NdOp::COND_BR, {},
           {NdVar::cst(Function.Blocks[NativeId].StartAddr, 8), Native});
      for (int Id : {SegmentId, NativeId}) {
        Block = &Function.Blocks[Id];
        if (Id == SegmentId)
          StateAccess();
        else
          Block->Ops.push_back(Op);
        Block->Succs = {JoinId};
        emit(NdOp::BRANCH, {},
             {NdVar::cst(Function.Blocks[JoinId].StartAddr, 8)});
      }
      Block = &Function.Blocks[JoinId];
    }
  }

  void operation(LowOp Op) {
    const NdVar NativeOutput = Op.Output;
    const bool LowSlice =
        Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
        Op.Inputs[0].isReg() && Op.Inputs[1].isConst() &&
        isSameRegisterLowSlice(NativeOutput.Offset, NativeOutput.Size,
                               Op.Inputs[0].Offset, Op.Inputs[0].Size,
                               Op.Inputs[1].Offset);
    if (NativeOutput.isReg())
      Op.Output = temporary(NativeOutput.Size);
    for (unsigned I = 0; I < Op.NumInputs; ++I)
      Op.Inputs[I] = read(Op.Inputs[I]);
    if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
      const NdVar Linear = temporary();
      const auto Base = Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS
                            ? offsetof(State, FSBase)
                            : offsetof(State, GSBase);
      const unsigned AddressInput = Op.Opcode == NdOp::INTRINSIC ? 1 : 0;
      emit(NdOp::INT_ADD, Linear, {load(Base), Op.Inputs[AddressInput]});
      Op.Inputs[AddressInput] = Linear;
      Op.MemoryAddressSpace = NdMemoryAddressSpace::Default;
    }
    if (unsignedWideDivisionIntrinsic(Op)) {
      // Reject a faulting invocation before any architectural write. For an
      // unsigned 128/64 division, high < divisor implies both nonzero divisor
      // and a representable quotient. Retain the original precondition below:
      // the native intrinsic remains the authority for division execution.
      const NdVar High = temporary(), Bad = temporary(1);
      emit(NdOp::SUBBYTES, High, {Op.Inputs[1], constant(8)});
      emit(NdOp::INT_LESSEQUAL, Bad, {Op.Inputs[2], High});
      rejectIf(Bad);
      Block->Ops.push_back(Op);
    } else if (flagIntrinsic(Op)) {
      const NdVar System = temporary();
      emit(NdOp::INT_AND, System,
           {load(offsetof(State, RFlags)), constant(~Flags::SplitMask)});
      auto Transition = detail::lowerX64MachineFlags(
          Op, System, [&](uint16_t Bytes) { return temporary(Bytes); },
          flagsProfile(Options));
      // Admission checked this exact shape before any output was published.
      append(Transition->Ops);
      const NdVar Split = temporary(), Packed = temporary();
      emit(NdOp::INT_AND, Split,
           {load(offsetof(State, RFlags)), constant(Flags::SplitMask)});
      emit(NdOp::INT_OR, Packed, {System, Split});
      store(offsetof(State, RFlags), Packed);
    } else if (msrIntrinsic(Op, Options)) {
      msr(Op);
    } else if (faultAddressIntrinsic(Op, Options)) {
      if (Op.Inputs[0].Offset == uint64_t(Intrinsic::ReadCr2))
        emit(NdOp::COPY, Op.Output, {load(offsetof(StateV4, CR2))});
      else
        store(offsetof(StateV4, CR2), Op.Inputs[1]);
    } else if (detail::taskPriorityOperation(Op)) {
      const NdVar Priority = load(offsetof(StateV2, CR8));
      const auto Transition = detail::lowerX64TaskPriority(
          Op, Priority, [&](uint16_t Bytes) { return temporary(Bytes); });
      append(Transition->Ops);
      if (Transition->Rejected) {
        rejectIf(*Transition->Rejected);
        store(offsetof(StateV2, CR8), Priority);
      }
    } else if (x64::mxcsr::supportsScalarOperation(Op,
                                                   controlOptions(Options))) {
      const NdVar Slot = load(offsetof(StateV2, MXCSR), 4);
      const auto Transition = x64::mxcsr::lowerScalarOperation(
          Op, Slot, *Options.MXCSRMask,
          [&](uint16_t Bytes) { return temporary(Bytes); });
      append(Transition->Ops);
      if (Transition->Rejected)
        rejectIf(*Transition->Rejected);
      if (Transition->OutputState)
        store(offsetof(StateV2, MXCSR), Slot);
    } else if (x64::legacygs::supportsOperation(Op, controlOptions(Options))) {
      const NdVar Pair = temporary(16);
      emit(NdOp::CONCAT, Pair,
           {load(offsetof(StateV2, KernelGSBase)),
            load(offsetof(State, GSBase))});
      const auto Transition = x64::legacygs::lowerScalarOperation(
          Op, Pair, [&](uint16_t Bytes) { return temporary(Bytes); });
      append(Transition->Ops);
      const NdVar Active = temporary(), Inactive = temporary();
      emit(NdOp::SUBBYTES, Active, {Pair, constant(0)});
      emit(NdOp::SUBBYTES, Inactive, {Pair, constant(8)});
      store(offsetof(State, GSBase), Active);
      store(offsetof(StateV2, KernelGSBase), Inactive);
    } else {
      Block->Ops.push_back(Op);
    }
    if (NativeOutput.isReg())
      write(NativeOutput, Op.Output, LowSlice);
  }
};

void finishBlock(LowBlock &Block) {
  LowInstructionBoundary B;
  B.Address = Block.StartAddr;
  B.Size = 1;
  B.OpCount = Block.Ops.size();
  B.Control = Block.Succs.empty() ? LowInstructionControl::Return
                                  : LowInstructionControl::Branch;
  B.ControlFlags = Block.Succs.empty()
                       ? LowInstructionControlFlag::Return
                       : LowInstructionControlFlag::Branch |
                             (Block.Succs.size() == 2
                                  ? LowInstructionControlFlag::Conditional
                                  : LowInstructionControlFlag::None);
  if (!Block.Succs.empty())
    B.Immediate = Block.Ops.back().Inputs[0].Offset;
  Block.InstructionBoundaries.push_back(B);
  Block.EndAddr = Block.StartAddr + 1;
  for (size_t I = 0; I < Block.Ops.size(); ++I) {
    Block.Ops[I].Addr = B.Address;
    Block.Ops[I].Seq = static_cast<int>(I);
  }
}

SourceFunctionTypeHint sourceABI() {
  SourceFunctionTypeHint ABI;
  ABI.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  ABI.Architecture = Arch::X64;
  ABI.HasExplicitABI = true;
  ABI.ReturnType = NdType::makeInt(8, false);
  ABI.ReturnLocation = {SourceABICarrierKind::IntegerRegister, x86reg::RAX, 0,
                        8};
  SourceParameterTypeHint P;
  P.Name = "machine_state";
  P.Type = NdType::makePtr(NdType::makeInt(1, false));
  P.Location = {SourceABICarrierKind::IntegerRegister, x86reg::RDI, 0, 8};
  ABI.Parameters.push_back(std::move(P));
  return ABI;
}

llvm::Expected<X64ContinuationInstructionSource>
buildInstruction(SpecializationInstruction Instruction,
                 const X64ContinuationSourceOptions &Options) {
  X64ContinuationInstructionSource Result;
  Result.Address = Instruction.Origin.Address;
  Result.Bytes = Instruction.NativeBytes;
  Result.SourceABI = sourceABI();
  auto &Function = Result.Function;
  Function.Entry = Result.Address;
  const bool Controls = hasControlState(Options);
  const bool Smap =
      Options.Profile == X64ContinuationProfile::KernelSmapNoFaultV3;
  const bool FaultAddress = hasFaultAddressState(Options);
  const bool PrivilegeReturn = hasReturnState(Options);
  const bool Events = hasEventState(Options);
  const bool FastSystem = hasFastSystemState(Options);
  const bool UserInterruptState = hasUserInterruptState(Options);
  const bool FredReturnState = hasFredReturnState(Options);
  Result.StateVersion = FredReturnState      ? 9
                        : UserInterruptState ? 8
                        : FastSystem         ? 7
                        : Events             ? 6
                        : PrivilegeReturn    ? 5
                        : FaultAddress       ? 4
                        : Smap               ? 3
                        : Controls           ? 2
                                             : 1;
  Result.StateBytes = FredReturnState      ? sizeof(StateV9)
                      : UserInterruptState ? sizeof(StateV8)
                      : FastSystem         ? sizeof(StateV7)
                      : Events             ? sizeof(StateV6)
                      : PrivilegeReturn    ? sizeof(StateV5)
                      : FaultAddress       ? sizeof(StateV4)
                      : Controls           ? sizeof(StateV2)
                                           : sizeof(State);
  Function.Name = (FredReturnState      ? "neverd_step_v9_"
                   : UserInterruptState ? "neverd_step_v8_"
                   : FastSystem         ? "neverd_step_v7_"
                   : Events             ? "neverd_step_v6_"
                   : PrivilegeReturn    ? "neverd_step_v5_"
                   : FaultAddress       ? "neverd_step_v4_"
                   : Smap               ? "neverd_step_v3_"
                   : Controls           ? "neverd_step_v2_"
                                        : "neverd_step_") +
                  llvm::utohexstr(Result.Address);
  Exit Kind = Exit::Continue;
  if (Instruction.NativeStackControl !=
      SpecializationNativeStackControl::None) {
    const bool Call = Instruction.NativeStackControl ==
                      SpecializationNativeStackControl::Call;
    auto Expanded =
        expandX64MachineExit(Instruction,
                             {1, Result.Address,
                              Call ? SpecializationX64MachineExitKind::Call
                                   : SpecializationX64MachineExitKind::Return},
                             NdVar::tmp(ScratchBase - 16, 8));
    if (!Expanded)
      return Expanded.takeError();
    Instruction.Ops = std::move(Expanded->Ops);
    Instruction.UndefinedEffects = std::move(Expanded->UndefinedEffects);
    Kind = Call ? Exit::Call : Exit::Return;
  } else if (Instruction.Origin.Control == LowInstructionControl::Branch) {
    Kind = Exit::Jump;
  }
  auto RequireNative = [&](llvm::StringRef Reason) {
    Result.RequiresNativeExecution = true;
    Result.Diagnostic = Reason.str();
  };
  const bool Sysret =
      PrivilegeReturn && Instruction.Ops.size() == 1 &&
      sysretIntrinsic(Instruction.Ops.front(), Options) &&
      Instruction.Origin.Control == LowInstructionControl::Terminator;
  const bool Sysenter =
      FastSystem && Instruction.Ops.size() == 1 &&
      eventIntrinsic(Instruction.Ops.front(), Intrinsic::X86Sysenter, 1) &&
      Instruction.Origin.Control == LowInstructionControl::Terminator;
  const bool Sysexit =
      FastSystem && Instruction.Ops.size() == 1 &&
      eventIntrinsic(Instruction.Ops.front(), Intrinsic::Sysexitq, 3) &&
      (Instruction.Ops.front().Inputs[1].Size == 4 ||
       Instruction.Ops.front().Inputs[1].Size == 8) &&
      Instruction.Ops.front().Inputs[1] ==
          NdVar::reg(x86reg::RDX, Instruction.Ops.front().Inputs[1].Size) &&
      Instruction.Ops.front().Inputs[2] ==
          NdVar::reg(x86reg::RCX, Instruction.Ops.front().Inputs[1].Size) &&
      Instruction.Origin.Control == LowInstructionControl::Terminator;
  const bool Iret =
      Events && Instruction.NativeBytes == std::vector<uint8_t>{0x48, 0xcf} &&
      returnFrameInstruction(Instruction, Intrinsic::Iret, 5);
  const auto FarReturn = Events ? farReturnDiscard(Instruction) : std::nullopt;
  const auto Interrupt =
      Events ? softwareInterruptVector(Instruction) : std::nullopt;
  const auto FarPointer =
      Events && Instruction.ProfileProjection ==
                    InterpreterProfileProjection::X64FarPointerLoadV1
          ? detail::x64FarPointerLoad(Instruction.NativeBytes)
          : std::nullopt;
  const auto UserInterrupt =
      UserInterruptState && Instruction.ProfileProjection ==
                                InterpreterProfileProjection::X64UserInterruptV1
          ? detail::x64UserInterrupt(Instruction.NativeBytes)
          : std::nullopt;
  const auto FredReturn =
      FredReturnState && Instruction.ProfileProjection ==
                             InterpreterProfileProjection::X64FredReturnV1
          ? detail::x64FredReturn(Instruction.NativeBytes)
          : std::nullopt;
  if (FredReturnState && !FredReturn)
    RequireNative("the FRED return profile only owns canonical ERETS/ERETU");
  // These exact frame shapes have no unconditional empty undefined-output
  // audit: their live CS descriptor may select compatibility mode. V6's shared
  // descriptor owner checks CS.L=1 and CS.D=0 before any write, so only this
  // bounded continuation can discharge the target-mode condition.
  const bool OwnedFrameReturn =
      (Iret || FarReturn) &&
      Instruction.UndefinedEffects.Coverage == LowUndefinedCoverage::Missing &&
      Instruction.UndefinedEffects.Effects.empty();
  if ((!FarPointer && !UserInterrupt && !FredReturn &&
       (Instruction.ProfileProjection != InterpreterProfileProjection::None ||
        (!OwnedFrameReturn && Instruction.UndefinedEffects.Coverage !=
                                  LowUndefinedCoverage::Complete))) ||
      (Instruction.Origin.Control == LowInstructionControl::Terminator &&
       !Sysret && !Sysenter && !Sysexit && !Iret && !FarReturn && !Interrupt &&
       !UserInterrupt && !FredReturn))
    RequireNative("instruction requires an explicit native environment");
  if (Instruction.UndefinedEffects.Effects.size() >
      std::size(State{}.Undefined))
    RequireNative("instruction exceeds the v1 arbitrary-value interface");
  for (const auto &E : Instruction.UndefinedEffects.Effects)
    if (!supportedValue(E.Output) || !E.Output.Size || E.Output.Size > 8 ||
        !E.BitCount || E.BitOffset + E.BitCount > E.Output.Size * 8 ||
        E.AfterOp > Instruction.Ops.size() ||
        (E.When && !supportedValue(*E.When)))
      RequireNative("instruction has an unsupported arbitrary-value effect");
  const bool WideDivision =
      Controls && std::any_of(Instruction.Ops.begin(), Instruction.Ops.end(),
                              unsignedWideDivisionIntrinsic);
  const auto Timestamp =
      Options.NativeTimestampReads
          ? x64::runtimeTimestampRegisters(Instruction,
                                           NdVar::tmp(ScratchBase - 16, 16))
          : std::nullopt;
  for (const LowOp &Op : Instruction.Ops) {
    if (!Timestamp && !Sysret && !Sysenter && !Sysexit && !Iret && !FarReturn &&
        !Interrupt && !supportedOperation(Op, Options, WideDivision)) {
      std::string Detail = "unsupported continuation operation: ";
      Detail += ndOpName(Op.Opcode);
      if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
          Op.Inputs[0].isConst()) {
        Detail += " ";
        Detail += intrinsicName(static_cast<Intrinsic>(Op.Inputs[0].Offset));
      }
      RequireNative(Detail);
    }
    if (flagIntrinsic(Op)) {
      const auto Transition = detail::lowerX64MachineFlags(
          Op, NdVar::tmp(ScratchBase, 8),
          [](uint16_t N) { return NdVar::tmp(ScratchBase + 16, N); },
          flagsProfile(Options));
      if (!Transition || Transition->Rejected)
        RequireNative("instruction requires a different flags domain");
    }
  }
  // Reserve before taking block references. Most operations need at most one
  // control-value guard; a modeled MSR write also checks each bound value
  // domain and may split state writes from native speculation effects.
  size_t ExtraMSRBlocks = 0;
  for (const auto &Op : Instruction.Ops)
    if (msrIntrinsic(Op, Options) &&
        Op.Inputs[0].Offset == uint64_t(Intrinsic::Wrmsr)) {
      ExtraMSRBlocks +=
          unsigned(Options.SegmentBaseMSRAddressBits.has_value()) +
          unsigned(Options.SysenterMSRAddressBits.has_value()) +
          unsigned(hasReturnState(Options));
      if ((Options.SegmentBaseMSRAddressBits || hasReturnState(Options)) &&
          Options.NativeSpeculationControlMsrs)
        ExtraMSRBlocks += 3;
    }
  const size_t MaxBlocks =
      Instruction.Ops.size() + 3 + ExtraMSRBlocks +
      (UserInterrupt == detail::X64UserInterrupt::Return ? 2 : 0);
  if (MaxBlocks > Options.MaxOperations ||
      Result.Address >= InvalidVA - MaxBlocks)
    return invalid("continuation source block budget or address exhausted");
  Function.Blocks.reserve(MaxBlocks);
  Function.Blocks.resize(3);
  for (int I = 0; I != 3; ++I) {
    Function.Blocks[I].Id = I;
    Function.Blocks[I].StartAddr = Result.Address + I;
  }
  auto &Entry = Function.Blocks[0];
  auto &Body = Function.Blocks[1];
  auto &Rejected = Function.Blocks[2];
  Entry.Succs = {1, 2};
  Body.Preds = {0};
  Rejected.Preds = {0};
  Writer Guard{Function, &Entry, Options};
  Guard.emit(NdOp::COPY, NdVar::reg(StatePointer, 8),
             {NdVar::reg(x86reg::RDI, 8)});
  const NdVar PCBad = Guard.temporary(1), FlagBits = Guard.temporary();
  const NdVar FlagsBad = Guard.temporary(1), Fixed = Guard.temporary();
  const NdVar FixedBad = Guard.temporary(1), Bad0 = Guard.temporary(1);
  NdVar Bad = Guard.temporary(1);
  Guard.emit(NdOp::INT_NOTEQUAL, PCBad,
             {Guard.load(offsetof(State, PC)), constant(Result.Address)});
  const NdVar Packed = Guard.load(offsetof(State, RFlags));
  const uint64_t EntryFlags =
      (Interrupt || FredReturn ||
       UserInterrupt == detail::X64UserInterrupt::Return)
          ? 0x3d7fd7
      : UserInterrupt ? 0x3d7ed7 // TF requires debug-trap delivery.
                      : Flags::entryMask(flagsProfile(Options));
  Guard.emit(NdOp::INT_AND, FlagBits, {Packed, constant(~EntryFlags)});
  Guard.emit(NdOp::INT_NOTEQUAL, FlagsBad, {FlagBits, constant(0)});
  Guard.emit(NdOp::INT_AND, Fixed, {Packed, constant(2)});
  Guard.emit(NdOp::INT_EQUAL, FixedBad, {Fixed, constant(0)});
  Guard.emit(NdOp::BOOL_OR, Bad0, {PCBad, FlagsBad});
  Guard.emit(NdOp::BOOL_OR, Bad, {Bad0, FixedBad});
  if (Controls) {
    const NdVar PriorityBad = Guard.temporary(1);
    const NdVar Reserved = Guard.temporary(4), MXCSRBad = Guard.temporary(1);
    const NdVar ControlsBad = Guard.temporary(1), AllBad = Guard.temporary(1);
    Guard.emit(NdOp::INT_LESS, PriorityBad,
               {constant(15), Guard.load(offsetof(StateV2, CR8))});
    Guard.emit(NdOp::INT_AND, Reserved,
               {Guard.load(offsetof(StateV2, MXCSR), 4),
                constant(~*Options.MXCSRMask, 4)});
    Guard.emit(NdOp::INT_NOTEQUAL, MXCSRBad, {Reserved, constant(0, 4)});
    Guard.emit(NdOp::BOOL_OR, ControlsBad, {PriorityBad, MXCSRBad});
    Guard.emit(NdOp::BOOL_OR, AllBad, {Bad, ControlsBad});
    Bad = AllBad;
  }
  if (PrivilegeReturn) {
    const NdVar RPL = Guard.temporary(2), SegmentBits = Guard.temporary(2);
    const NdVar CPLBad = Guard.temporary(1), ModeBad = Guard.temporary(1);
    const NdVar SegmentBad = Guard.temporary(1), AllBad = Guard.temporary(1);
    const NdVar Attributes =
        Guard.load(offsetof(StateV5, CS) + offsetof(Segment, Attributes), 2);
    Guard.emit(
        NdOp::INT_AND, RPL,
        {Guard.load(offsetof(StateV5, CS) + offsetof(Segment, Selector), 2),
         constant(3, 2)});
    if (Sysenter || UserInterrupt) {
      // Conforming code retains the caller's CPL even when CS.DPL is lower.
      const NdVar Shifted = Guard.temporary(2), DPL = Guard.temporary(2);
      const NdVar ConformingBits = Guard.temporary(2);
      const NdVar Nonconforming = Guard.temporary(1),
                  Above = Guard.temporary(1);
      const NdVar Different = Guard.temporary(1), WrongDPL = Guard.temporary(1);
      Guard.emit(NdOp::INT_RIGHT, Shifted, {Attributes, constant(5, 2)});
      Guard.emit(NdOp::INT_AND, DPL, {Shifted, constant(3, 2)});
      Guard.emit(NdOp::INT_AND, ConformingBits, {Attributes, constant(4, 2)});
      Guard.emit(NdOp::INT_EQUAL, Nonconforming,
                 {ConformingBits, constant(0, 2)});
      Guard.emit(NdOp::INT_LESS, Above, {RPL, DPL});
      Guard.emit(NdOp::INT_NOTEQUAL, Different, {RPL, DPL});
      Guard.emit(NdOp::BOOL_AND, WrongDPL, {Nonconforming, Different});
      Guard.emit(NdOp::BOOL_OR, CPLBad, {Above, WrongDPL});
    } else {
      Guard.emit(NdOp::INT_NOTEQUAL, CPLBad, {RPL, constant(0, 2)});
    }
    Guard.emit(NdOp::INT_AND, SegmentBits,
               {Attributes,
                constant((Sysenter || UserInterrupt) ? 0x6098 : 0x60f8, 2)});
    Guard.emit(NdOp::INT_NOTEQUAL, ModeBad, {SegmentBits, constant(0x2098, 2)});
    Guard.emit(NdOp::BOOL_OR, SegmentBad, {CPLBad, ModeBad});
    Guard.emit(NdOp::BOOL_OR, AllBad, {Bad, SegmentBad});
    Bad = AllBad;
  }
  if (Events) {
    const NdVar UnusableCS = Guard.temporary(1), BadNMI = Guard.temporary(1);
    const NdVar EventBad = Guard.temporary(1), AllBad = Guard.temporary(1);
    Guard.emit(NdOp::INT_AND, UnusableCS,
               {Guard.load(offsetof(StateV6, Unusable), 1), constant(1, 1)});
    Guard.emit(NdOp::INT_LESS, BadNMI,
               {constant(1, 1), Guard.load(offsetof(StateV6, NMIBlocked), 1)});
    Guard.emit(NdOp::BOOL_OR, EventBad, {UnusableCS, BadNMI});
    Guard.emit(NdOp::BOOL_OR, AllBad, {Bad, EventBad});
    Bad = AllBad;
  }
  if (FastSystem) {
    for (auto Offset :
         {offsetof(StateV7, SysenterCS), offsetof(StateV7, SysenterESP),
          offsetof(StateV7, SysenterEIP)}) {
      const NdVar Value = Guard.load(Offset), Canonical = Guard.temporary();
      const NdVar Invalid = Guard.temporary(1), AllBad = Guard.temporary(1);
      if (Offset == offsetof(StateV7, SysenterCS)) {
        Guard.emit(NdOp::INT_AND, Canonical, {Value, constant(UINT32_MAX)});
      } else {
        const NdVar Shifted = Guard.temporary();
        const auto Shift = constant(64 - *Options.SysenterMSRAddressBits);
        Guard.emit(NdOp::INT_LEFT, Shifted, {Value, Shift});
        Guard.emit(NdOp::INT_ASHR, Canonical, {Shifted, Shift});
      }
      Guard.emit(NdOp::INT_NOTEQUAL, Invalid, {Value, Canonical});
      Guard.emit(NdOp::BOOL_OR, AllBad, {Bad, Invalid});
      Bad = AllBad;
    }
  }
  if (UserInterruptState || FredReturnState) {
    for (auto Offset :
         {offsetof(StateV8, UIF), offsetof(StateV8, MonitorActive)}) {
      const auto Invalid = Guard.temporary(1), AllBad = Guard.temporary(1);
      Guard.emit(NdOp::INT_LESS, Invalid,
                 {constant(1, 1), Guard.load(Offset, 1)});
      Guard.emit(NdOp::BOOL_OR, AllBad, {Bad, Invalid});
      Bad = AllBad;
    }
  }
  if (FredReturnState) {
    const auto BadSTI = Guard.temporary(1), Pending = Guard.temporary(1);
    const auto EventsBad = Guard.temporary(1), AllBad = Guard.temporary(1);
    Guard.emit(NdOp::INT_LESS, BadSTI,
               {constant(1, 1), Guard.load(offsetof(StateV9, STIBlocked), 1)});
    Guard.emit(
        NdOp::INT_NOTEQUAL, Pending,
        {Guard.load(offsetof(StateV9, PendingDebug), 1), constant(0, 1)});
    Guard.emit(NdOp::BOOL_OR, EventsBad, {BadSTI, Pending});
    Guard.emit(NdOp::BOOL_OR, AllBad, {Bad, EventsBad});
    Bad = AllBad;
  }
  Guard.emit(NdOp::COND_BR, {}, {NdVar::cst(Rejected.StartAddr, 8), Bad});
  Writer Reject{Function, &Rejected, Options};
  Reject.leave(Exit::InvalidState);
  Writer W{Function, &Body, Options};
  if (Result.RequiresNativeExecution) {
    W.leave(Exit::NativeInstruction);
  } else if (FredReturn) {
    detail::X64ContinuationFredReturns<Writer>(W, *Options.PagingAddressBits)
        .execute(*FredReturn);
  } else if (UserInterrupt) {
    detail::X64ContinuationUserInterrupts<Writer>(W, Options)
        .execute(*UserInterrupt, Instruction.Fallthrough.Address);
  } else if (Iret || FarReturn || Interrupt || FarPointer) {
    detail::X64ContinuationEvents<Writer> Event(W, *Options.PagingAddressBits);
    if (Iret)
      Event.iret();
    else if (FarReturn)
      Event.farReturn(*FarReturn);
    else if (FarPointer)
      Event.farPointerLoad(*FarPointer, Instruction.Fallthrough.Address,
                           Options.NullSegmentBasePolicy);
    else
      Event.softwareInterrupt(*Interrupt, Instruction.Fallthrough.Address);
  } else if (Sysenter || Sysexit) {
    Result.UndefinedEffects = Instruction.UndefinedEffects.Effects;
    detail::X64ContinuationSystemTransfers<Writer> Transfer(W, Options);
    Transfer.fastSystem(Sysenter,
                        Sysenter || Instruction.Ops.front().Inputs[1].Size == 8,
                        Result.UndefinedEffects);
  } else if (Sysret) {
    Result.UndefinedEffects = Instruction.UndefinedEffects.Effects;
    detail::X64ContinuationSystemTransfers<Writer> Transfer(W, Options);
    Transfer.sysret(Instruction.Ops.front().Inputs[1].Size == 8,
                    Result.UndefinedEffects);
  } else {
    Result.UndefinedEffects = Instruction.UndefinedEffects.Effects;
    NdVar NextPC = constant(Instruction.Fallthrough.Address);
    auto Effects = [&](size_t After) {
      for (size_t I = 0; I != Instruction.UndefinedEffects.Effects.size(); ++I)
        if (Instruction.UndefinedEffects.Effects[I].AfterOp == After)
          W.undefined(Instruction.UndefinedEffects.Effects[I], I);
    };
    Effects(0);
    if (Timestamp) {
      // Keep the native intrinsic and its complete multi-output bundle
      // together. Ordinary operation-by-operation substitution would detach
      // its hidden result carriers, losing EDX and RDTSCP's ECX result.
      W.append(Instruction.Ops);
      for (uint64_t Register : *Timestamp)
        W.write(NdVar::reg(Register, 8), NdVar::reg(Register, 8));
    }
    for (size_t I = 0; !Timestamp && I < Instruction.Ops.size(); ++I) {
      const LowOp &Op = Instruction.Ops[I];
      if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::INDIR_BR) {
        NextPC = W.read(Op.Inputs[0]);
      } else if (Op.Opcode == NdOp::COND_BR) {
        NextPC = W.temporary();
        W.emit(NdOp::SELECT, NextPC,
               {W.read(Op.Inputs[1]), W.read(Op.Inputs[0]),
                constant(Instruction.Fallthrough.Address)});
      } else {
        W.operation(Op);
      }
      Effects(I + 1);
    }
    W.leave(Kind, NextPC);
  }
  if (Function.Blocks.size() > Options.MaxOperations ||
      Result.Address >= InvalidVA - Function.Blocks.size())
    return invalid("continuation source block budget or address exhausted");
  for (auto &Block : Function.Blocks)
    finishBlock(Block);
  if (auto Error = validateLowInstructionBoundaries(
          Function, LowInstructionBoundaryRequirement::Required))
    return std::move(Error);
  return Result;
}
} // namespace

llvm::Expected<std::vector<X64ContinuationInstructionSource>>
buildX64ContinuationSource(const BinaryImage &Image,
                           llvm::ArrayRef<va_t> Addresses,
                           const X64ContinuationSourceOptions &Options) {
  const bool Controls = hasControlState(Options);
  if (Options.Profile != X64ContinuationProfile::KernelNoFaultV1 && !Controls)
    return invalid("continuation source requires its explicit kernel domain");
  const bool Returns = hasReturnState(Options);
  const bool FastSystem = hasFastSystemState(Options);
  if (hasUserInterruptState(Options) != Options.UiretRestoresUIF.has_value())
    return invalid("V8 requires an explicit UIRET_UIF CPU feature binding; "
                   "earlier profiles do not accept it");
  if (FastSystem != Options.SysenterMSRAddressBits.has_value() ||
      (Options.SysenterMSRAddressBits &&
       *Options.SysenterMSRAddressBits != 48 &&
       *Options.SysenterMSRAddressBits != 57))
    return invalid("V7/V8 require the Intel CPU's 48- or 57-bit "
                   "SYSENTER MSR address width");
  if (Options.SysenterMSRAddressBits && Options.SegmentBaseMSRAddressBits &&
      Options.SysenterMSRAddressBits != Options.SegmentBaseMSRAddressBits)
    return invalid(
        "SYSENTER and segment-base MSRs must agree on processor address width");
  if (Returns != Options.PagingAddressBits.has_value() ||
      (Options.PagingAddressBits && *Options.PagingAddressBits != 48 &&
       *Options.PagingAddressBits != 57))
    return invalid(
        "V5 and later require the active 48- or 57-bit paging address width");
  if (FastSystem &&
      *Options.PagingAddressBits > *Options.SysenterMSRAddressBits)
    return invalid("active paging cannot exceed the processor's address width");
  if (Options.NullSegmentBasePolicy &&
      (!hasEventState(Options) ||
       (*Options.NullSegmentBasePolicy != X64NullSegmentBasePolicy::Clear &&
        *Options.NullSegmentBasePolicy != X64NullSegmentBasePolicy::Preserve)))
    return invalid(
        "null segment base policy requires V6 or later and a valid policy");
  if (Options.NativeTimestampReads && !Controls)
    return invalid("native timestamp source requires an x64 host profile");
  if (Options.NativeWritePrefetch && !Controls)
    return invalid("native write prefetch source requires an x64 host profile");
  if (Options.NativePageTableAccess && !Controls)
    return invalid("native paging requires a controls profile");
  if (Options.NativeDescriptorQueries && !Controls)
    return invalid("native descriptor queries require an x64 control profile");
  if ((Options.SegmentBaseMSRAddressBits ||
       Options.NativeSpeculationControlMsrs) &&
      !Controls)
    return invalid("continuation MSRs require the x64 control-state profile");
  if (Options.SegmentBaseMSRAddressBits &&
      *Options.SegmentBaseMSRAddressBits != 48 &&
      *Options.SegmentBaseMSRAddressBits != 57)
    return invalid(
        "segment-base MSRs require 48- or 57-bit canonical addresses");
  if (Controls != Options.MXCSRMask.has_value())
    return invalid(
        "continuation control profiles require an effective MXCSR mask; v1 has "
        "no MXCSR state");
  const auto ControlOptions = controlOptions(Options);
  if (auto Error = x64::mxcsr::validateOptions(ControlOptions))
    return std::move(Error);
  if (auto Error = x64::legacygs::validateOptions(ControlOptions))
    return std::move(Error);
  if (Addresses.empty() || Addresses.size() > Options.MaxInstructions ||
      Addresses.size() > Options.MaxOperations)
    return invalid("continuation source instruction budget exhausted");
  std::set<va_t> Unique;
  for (va_t Address : Addresses)
    if (Address >= InvalidVA - 3 || !Unique.insert(Address).second)
      return invalid("invalid or duplicate continuation instruction address");
  auto Instructions = readBinaryX64ContinuationInstructions(Image, Addresses);
  if (!Instructions)
    return Instructions.takeError();
  uint64_t Work = Addresses.size();
  std::vector<X64ContinuationInstructionSource> Result;
  for (auto &Instruction : *Instructions) {
    auto Unit = buildInstruction(std::move(Instruction), Options);
    if (!Unit)
      return Unit.takeError();
    for (const auto &Block : Unit->Function.Blocks) {
      if (Block.Ops.size() > Options.MaxOperations - Work)
        return invalid("continuation source operation budget exhausted");
      Work += Block.Ops.size();
    }
    Result.push_back(std::move(*Unit));
  }
  return Result;
}

} // namespace neverd::analysis
