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
#include "X64ContinuationEvents.h"
#include "X64ControlTransitions.h"
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
using Segment = X64ContinuationSegment;
using Exit = X64ContinuationExitV1;
using Flags = detail::X64MachineFlags;
constexpr uint64_t StatePointer = uint64_t{1} << 32;
constexpr uint64_t ScratchBase = uint64_t{1} << 61;

InterpreterMachineStateProfile
flagsProfile(const X64ContinuationSourceOptions &Options) {
  return (Options.Profile == X64ContinuationProfile::KernelSmapNoFaultV3 ||
          Options.Profile ==
              X64ContinuationProfile::KernelFaultAddressNoFaultV4 ||
          Options.Profile == X64ContinuationProfile::KernelReturnNoFaultV5 ||
          Options.Profile == X64ContinuationProfile::KernelEventsNoFaultV6)
             ? InterpreterMachineStateProfile::KernelX64SmapNoFaultV2
             : InterpreterMachineStateProfile::KernelX64NoFaultV1;
}

bool hasControlState(const X64ContinuationSourceOptions &Options) {
  return Options.Profile == X64ContinuationProfile::KernelControlsNoFaultV2 ||
         Options.Profile == X64ContinuationProfile::KernelSmapNoFaultV3 ||
         Options.Profile ==
             X64ContinuationProfile::KernelFaultAddressNoFaultV4 ||
         Options.Profile == X64ContinuationProfile::KernelReturnNoFaultV5 ||
         Options.Profile == X64ContinuationProfile::KernelEventsNoFaultV6;
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
    Controls.X64LegacyGSState = true;
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
  if ((Options.Profile != X64ContinuationProfile::KernelFaultAddressNoFaultV4 &&
       Options.Profile != X64ContinuationProfile::KernelReturnNoFaultV5 &&
       Options.Profile != X64ContinuationProfile::KernelEventsNoFaultV6) ||
      Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
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
  return (Options.Profile == X64ContinuationProfile::KernelReturnNoFaultV5 ||
          Options.Profile == X64ContinuationProfile::KernelEventsNoFaultV6) &&
         Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 3 && !Op.Output.Size &&
         Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
         Op.Inputs[0].Offset == uint64_t(Intrinsic::Sysretq) &&
         Op.Inputs[1] == NdVar::reg(x86reg::RCX, 8) &&
         Op.Inputs[2] == NdVar::reg(x86reg::R11, 8) &&
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
    return Options.SegmentBaseMSRAddressBits && Op.NumInputs == 2 &&
           Op.Output.Size == 8 && Op.Inputs[1].Size == 4;
  return Id == Intrinsic::Wrmsr &&
         (Options.SegmentBaseMSRAddressBits ||
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

  void undefined(const LowUndefinedEffect &Effect, size_t Index) {
    const NdVar Witness =
        load(offsetof(State, Undefined) + Index * 8, Effect.Output.Size);
    const uint64_t Bits = UINT64_MAX >> (64 - Effect.BitCount);
    const uint64_t Mask = Bits << Effect.BitOffset;
    const auto K = [&](uint64_t Value) {
      return constant(Value, Effect.Output.Size);
    };
    const NdVar Limited = temporary(Effect.Output.Size);
    const NdVar Shifted = temporary(Effect.Output.Size);
    const NdVar Kept = temporary(Effect.Output.Size);
    const NdVar Updated = temporary(Effect.Output.Size);
    // A complete unconditional arbitrary production needs no previous value;
    // an instruction-local carrier may not have existed before this point.
    const bool Whole =
        Effect.BitOffset == 0 && Effect.BitCount == Effect.Output.Size * 8;
    const NdVar Old = Whole && !Effect.When ? K(0) : read(Effect.Output);
    emit(NdOp::INT_AND, Limited, {Witness, K(Bits)});
    emit(NdOp::INT_LEFT, Shifted, {Limited, K(Effect.BitOffset)});
    emit(NdOp::INT_AND, Kept, {Old, K(~Mask)});
    emit(NdOp::INT_OR, Updated, {Kept, Shifted});
    NdVar Result = Updated;
    if (Effect.When) {
      Result = temporary(Effect.Output.Size);
      emit(NdOp::SELECT, Result, {read(*Effect.When), Updated, Old});
    }
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
    NdVar Native = constant(0, 1), Allowed = Segment;
    if (Write && Options.NativeSpeculationControlMsrs) {
      const NdVar Control = temporary(1), Command = temporary(1);
      Native = temporary(1);
      Allowed = temporary(1);
      emit(NdOp::INT_EQUAL, Control, {Selector, constant(0x48, 4)});
      emit(NdOp::INT_EQUAL, Command, {Selector, constant(0x49, 4)});
      emit(NdOp::BOOL_OR, Native, {Control, Command});
      emit(NdOp::BOOL_OR, Allowed, {Segment, Native});
    }
    const NdVar Unknown = temporary(1);
    emit(NdOp::BOOL_NOT, Unknown, {Allowed});
    rejectIf(Unknown);
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
    const auto SegmentAccess = [&] {
      const NdVar Address = temporary();
      emit(NdOp::INT_ADD, Address, {NdVar::reg(StatePointer, 8), Offset});
      if (Write)
        emit(NdOp::STORE, {}, {Address, Op.Inputs[2]});
      else
        emit(NdOp::LOAD, Op.Output, {Address});
    };
    if (!Write || !Options.NativeSpeculationControlMsrs) {
      SegmentAccess();
    } else if (!Options.SegmentBaseMSRAddressBits) {
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
          SegmentAccess();
        else
          Block->Ops.push_back(Op);
        Block->Succs = {JoinId};
        emit(NdOp::BRANCH, {},
             {NdVar::cst(Function.Blocks[JoinId].StartAddr, 8)});
      }
      Block = &Function.Blocks[JoinId];
    }
  }

  void sysret() {
    const NdVar PC = read(NdVar::reg(x86reg::RCX, 8));
    const NdVar Shifted = temporary(), Canonical = temporary();
    const NdVar Bad = temporary(1);
    const auto Shift = constant(64 - *Options.PagingAddressBits);
    emit(NdOp::INT_LEFT, Shifted, {PC, Shift});
    emit(NdOp::INT_ASHR, Canonical, {Shifted, Shift});
    emit(NdOp::INT_NOTEQUAL, Bad, {Canonical, PC});
    rejectIf(Bad);
    const NdVar Masked = temporary(), Packed = temporary(),
                Selectors = temporary();
    emit(NdOp::INT_AND, Masked,
         {read(NdVar::reg(x86reg::R11, 8)), constant(0x3c7fd7)});
    emit(NdOp::INT_OR, Packed, {Masked, constant(2)});
    emit(NdOp::INT_RIGHT, Selectors,
         {load(offsetof(StateV5, STAR)), constant(48)});
    for (bool Code : {true, false}) {
      const auto Offset = Code ? offsetof(StateV5, CS) : offsetof(StateV5, SS);
      const NdVar Added = temporary(), User = temporary(),
                  Selector = temporary(2);
      emit(NdOp::INT_ADD, Added, {Selectors, constant(Code ? 16 : 8)});
      emit(NdOp::INT_OR, User, {Added, constant(3)});
      emit(NdOp::SUBBYTES, Selector, {User, constant(0)});
      store(Offset + offsetof(Segment, Base), constant(0));
      store(Offset + offsetof(Segment, Limit), constant(UINT32_MAX, 4));
      store(Offset + offsetof(Segment, Selector), Selector);
      store(Offset + offsetof(Segment, Attributes),
            constant(Code ? 0xa0fb : 0xc0f3, 2));
    }
    if (Options.Profile == X64ContinuationProfile::KernelEventsNoFaultV6) {
      const auto Usable = temporary(1);
      emit(NdOp::INT_AND, Usable,
           {load(offsetof(StateV6, Unusable), 1), constant(0xfc, 1)});
      store(offsetof(StateV6, Unusable), Usable);
    }
    store(offsetof(State, RFlags), Packed);
    leave(Exit::PrivilegeReturn, PC);
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
  const bool FaultAddress =
      Options.Profile == X64ContinuationProfile::KernelFaultAddressNoFaultV4 ||
      Options.Profile == X64ContinuationProfile::KernelReturnNoFaultV5 ||
      Options.Profile == X64ContinuationProfile::KernelEventsNoFaultV6;
  const bool PrivilegeReturn =
      Options.Profile == X64ContinuationProfile::KernelReturnNoFaultV5 ||
      Options.Profile == X64ContinuationProfile::KernelEventsNoFaultV6;
  const bool Events =
      Options.Profile == X64ContinuationProfile::KernelEventsNoFaultV6;
  Result.StateVersion = Events            ? 6
                        : PrivilegeReturn ? 5
                        : FaultAddress    ? 4
                        : Smap            ? 3
                        : Controls        ? 2
                                          : 1;
  Result.StateBytes = Events            ? sizeof(StateV6)
                      : PrivilegeReturn ? sizeof(StateV5)
                      : FaultAddress    ? sizeof(StateV4)
                      : Controls        ? sizeof(StateV2)
                                        : sizeof(State);
  Function.Name = (Events            ? "neverd_step_v6_"
                   : PrivilegeReturn ? "neverd_step_v5_"
                   : FaultAddress    ? "neverd_step_v4_"
                   : Smap            ? "neverd_step_v3_"
                   : Controls        ? "neverd_step_v2_"
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
  const bool Iret =
      Events && Instruction.NativeBytes == std::vector<uint8_t>{0x48, 0xcf} &&
      returnFrameInstruction(Instruction, Intrinsic::Iret, 5);
  const auto FarReturn = Events ? farReturnDiscard(Instruction) : std::nullopt;
  const auto Interrupt =
      Events ? softwareInterruptVector(Instruction) : std::nullopt;
  if (Instruction.ProfileProjection != InterpreterProfileProjection::None ||
      Instruction.UndefinedEffects.Coverage != LowUndefinedCoverage::Complete ||
      (Instruction.Origin.Control == LowInstructionControl::Terminator &&
       !Sysret && !Iret && !FarReturn && !Interrupt))
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
    if (!Timestamp && !Sysret && !Iret && !FarReturn && !Interrupt &&
        !supportedOperation(Op, Options, WideDivision)) {
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
  // At most one control-value guard per original operation. Reserve before
  // taking block references, and keep synthetic block addresses representable.
  const size_t MaxBlocks =
      Instruction.Ops.size() + 3 +
      (Options.SegmentBaseMSRAddressBits && Options.NativeSpeculationControlMsrs
           ? 3
           : 0);
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
      Interrupt ? 0x3d7fd7 : Flags::entryMask(flagsProfile(Options));
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
    Guard.emit(
        NdOp::INT_AND, RPL,
        {Guard.load(offsetof(StateV5, CS) + offsetof(Segment, Selector), 2),
         constant(3, 2)});
    Guard.emit(NdOp::INT_NOTEQUAL, CPLBad, {RPL, constant(0, 2)});
    Guard.emit(
        NdOp::INT_AND, SegmentBits,
        {Guard.load(offsetof(StateV5, CS) + offsetof(Segment, Attributes), 2),
         constant(0x60f8, 2)});
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
  Guard.emit(NdOp::COND_BR, {}, {NdVar::cst(Rejected.StartAddr, 8), Bad});
  Writer Reject{Function, &Rejected, Options};
  Reject.leave(Exit::InvalidState);
  Writer W{Function, &Body, Options};
  if (Result.RequiresNativeExecution) {
    W.leave(Exit::NativeInstruction);
  } else if (Iret || FarReturn || Interrupt) {
    detail::X64ContinuationEvents<Writer> Event(W, *Options.PagingAddressBits);
    if (Iret)
      Event.iret();
    else if (FarReturn)
      Event.farReturn(*FarReturn);
    else
      Event.softwareInterrupt(*Interrupt, Instruction.Fallthrough.Address);
  } else if (Sysret) {
    W.sysret();
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
  const bool Returns =
      Options.Profile == X64ContinuationProfile::KernelReturnNoFaultV5 ||
      Options.Profile == X64ContinuationProfile::KernelEventsNoFaultV6;
  if (Returns != Options.PagingAddressBits.has_value() ||
      (Options.PagingAddressBits && *Options.PagingAddressBits != 48 &&
       *Options.PagingAddressBits != 57))
    return invalid(
        "V5/V6 require the active 48- or 57-bit paging address width");
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
