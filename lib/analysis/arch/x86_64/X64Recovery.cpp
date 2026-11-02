//===- X64Recovery.cpp - Native x64 recovery contracts -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64Recovery.h"

#include "../../../lift/X86/X86CetEncoding.h"
#include "NativeStackControl.h"
#include "X64MachineFlags.h"

#include "neverd/analysis/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/support/ISAEncoding.h"
#include "neverd/symbolic/SymState.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <array>
#include <set>

namespace neverd::analysis::x64 {
llvm::Error validateRuntimeMemoryOptions(const SpecializationOptions &Options) {
  if ((Options.X64SegmentedReads || Options.X64SegmentedWrites) &&
      (!Options.ExplicitMachineState || !Options.NormalNonfaultingExecution))
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "segmented memory requires explicit nonfaulting machine state");
  return llvm::Error::success();
}

bool supportsRuntimeMemoryOperation(const LowOp &Op,
                                    const SpecializationOptions &Options) {
  if ((Op.MemoryAddressSpace != NdMemoryAddressSpace::X86FS &&
       Op.MemoryAddressSpace != NdMemoryAddressSpace::X86GS) ||
      Op.MemoryOrdering != NdMemoryOrdering::None)
    return false;
  const auto Memory = lowMemoryOperands(Op);
  if (!Memory.Complete)
    return false;
  if (Op.Opcode == NdOp::LOAD)
    return Options.X64SegmentedReads &&
           (Op.NumInputs != 2 || Op.Inputs[0].isConst());
  if (Op.Opcode != NdOp::STORE || !Options.X64SegmentedWrites ||
      Memory.Address->Size != 8 ||
      (Op.NumInputs == 3 && !Op.Inputs[0].isConst()))
    return false;
  return Memory.AccessSize == 1 || Memory.AccessSize == 2 ||
         Memory.AccessSize == 4 || Memory.AccessSize == 8;
}

void seedRuntimeMemoryState(symbolic::SymContext &Ctx,
                            symbolic::SymState &State,
                            const SpecializationOptions &Options) {
  if (!Options.X64SegmentedReads && !Options.X64SegmentedWrites &&
      !Options.X64LegacyGSState)
    return;
  // Projection forgets base correlations across nodes, enlarging the input
  // domain. Distinct variables do not assert distinct linear addresses.
  State.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS,
                                     Ctx.mkFreshVar(64, "fs_base"));
  State.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS,
                                     Ctx.mkFreshVar(64, "gs_base"));
}

llvm::Expected<RawEnvironmentPreparation>
validateRawEnvironmentDeclarations(const SpecializationOptions &Options) {
  if (Options.Architecture != Arch::X64)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "x64 recovery requires the x64 architecture");
  RawEnvironmentPreparation Cost;
  if (!Options.hasRawEnvironment())
    return Cost;
  const auto Invalid = [](const char *Message) {
    return llvm::createStringError(llvm::errc::invalid_argument, "%s", Message);
  };
  if (Options.UndefinedModel != SpecializationUndefinedModel::SelectedLowIR &&
      (Options.UndefinedModel != SpecializationUndefinedModel::FreshScalarV1 ||
       Options.X64MachineExits.empty()))
    return Invalid(
        "architectural choices require a supported raw-region model");
  const auto Exhausted = [] {
    return llvm::createStringError(
        std::make_error_code(std::errc::value_too_large),
        "raw environment preparation budget exhausted");
  };
  // V1 reserves thirty-two bounded preparation passes (validation, owned copies
  // and serialization/hash) before examining any declaration payload. Scalar
  // option fields fit in the 1024-byte header; each record adds 128 bytes.
  // Payload lengths are inspected only after aggregate collection counts fit.
  constexpr uint64_t Passes = 32;
  uint64_t Records = 1;
  const uint64_t RecordLimit = Options.MaxImagePreparationRecords / Passes;
  for (size_t Count :
       {Options.X64CodeRegions.size(), Options.X64ImportBindings.size(),
        Options.X64TerminalImports.size(), Options.X64MachineExits.size(),
        Options.ControlRegisters.size(), Options.ControlFrameSlots.size(),
        Options.EntryConstants.size(), Options.EntryFrameExcludedObjects.size(),
        Options.EntryFrameBounds
            ? Options.EntryFrameBounds->ExcludedAddressRanges.size()
            : size_t{0}}) {
    if (Records > RecordLimit || Count > RecordLimit - Records)
      return Exhausted();
    Records += Count;
  }
  // Pairwise interval and target checks have an explicit worst-case record
  // reservation too. Avoid an uncharged sort or implementation-dependent
  // comparator count; the small raw-domain interface uses bounded lists.
  if (Records > RecordLimit || Records > RecordLimit / Records)
    return Exhausted();
  const uint64_t ByteLimit = Options.MaxImagePreparationBytes / Passes;
  if (ByteLimit < 1024 || Records > (ByteLimit - 1024) / 128)
    return Exhausted();
  uint64_t Bytes = 1024 + Records * 128;
  const auto Add = [&](uint64_t Size) {
    if (Size > ByteLimit - Bytes)
      return false;
    Bytes += Size;
    return true;
  };
  for (const auto &R : Options.X64CodeRegions)
    if (!Add(R.Bytes.size()) || !Add(R.Evidence.size()))
      return Exhausted();
  for (const auto &B : Options.X64ImportBindings)
    if (!Add(B.Module.size()) || !Add(B.Symbol.size()) ||
        !Add(B.Evidence.size()))
      return Exhausted();
  for (const auto &B : Options.X64TerminalImports)
    if (!Add(B.Module.size()) || !Add(B.Symbol.size()))
      return Exhausted();
  Cost.Bytes = Bytes * Passes;
  Cost.Records = Records * Records * Passes;
  // Each primary identity lookup performs two binary searches in size_t-wide
  // indexes (named slots and section mappings): at most 64 comparisons each.
  // This pays the declaration/primary-index boundary independently of either
  // collection's size. Unique slots prevent repeated copying of one identity.
  const uint64_t ImportCount =
      Options.X64ImportBindings.size() + Options.X64TerminalImports.size();
  if (ImportCount > (Options.MaxImagePreparationRecords - Cost.Records) / 128)
    return Exhausted();
  Cost.Records += ImportCount * 128;

  if (!Options.ExplicitMachineState || !Options.NormalNonfaultingExecution ||
      !Options.X64CetDisabled ||
      Options.ByteOrder != llvm::endianness::little ||
      !Options.FrameBaseRegister ||
      Options.FrameBaseRegister->Offset != StackPointer ||
      Options.FrameBaseRegister->Bytes != 8 ||
      (Options.X64MachineExits.empty() &&
       !Options.RequireRestoredFrameAtReturn))
    return Invalid("raw environment requires explicit nonfaulting CET-disabled "
                   "x64 state and an explicit entry RSP contract");
  if (!Options.X64MachineExits.empty() &&
      (Options.RequireRestoredFrameAtReturn ||
       Options.ExternalStoresPreserveEntryReturnSlot ||
       !Options.X64TerminalImports.empty()))
    return Invalid("machine regions exclude ordinary return-slot and terminal "
                   "import contracts");
  if (!Options.EntryFrameExcludedObjects.empty() &&
      (!Options.EntryFrameBounds ||
       Options.EntryFrameBounds->Begin >= Options.EntryFrameBounds->End))
    return Invalid("entry objects require nonempty entry frame bounds");
  for (size_t I = 0; I < Options.EntryFrameExcludedObjects.size(); ++I) {
    const auto &Object = Options.EntryFrameExcludedObjects[I];
    const auto &Register = Object.Register;
    if (Register.Bytes != 8 || !x86reg::isGeneralRegOffset(Register.Offset) ||
        Register.Offset == Options.FrameBaseRegister->Offset ||
        Object.Begin >= Object.End)
      return Invalid("entry object requires a complete non-frame GPR and "
                     "nonempty relative bounds");
    for (size_t J = 0; J < I; ++J)
      if (Options.EntryFrameExcludedObjects[J].Register.Offset ==
          Register.Offset)
        return Invalid("duplicate entry object register");
    for (const auto &Constant : Options.EntryConstants) {
      const auto &Location = Constant.Location;
      if (Location.isReg() && Location.Size &&
          ((Location.Offset <= Register.Offset &&
            Register.Offset - Location.Offset < Location.Size) ||
           (Register.Offset <= Location.Offset &&
            Location.Offset - Register.Offset < Register.Bytes)))
        return Invalid("entry object overlaps an entry constant binding");
    }
  }
  for (size_t I = 0; I < Options.X64MachineExits.size(); ++I) {
    const auto &E = Options.X64MachineExits[I];
    if (E.Version != 1 || E.Address == InvalidVA ||
        (E.Kind != SpecializationX64MachineExitKind::Call &&
         E.Kind != SpecializationX64MachineExitKind::Jump &&
         E.Kind != SpecializationX64MachineExitKind::Return &&
         E.Kind != SpecializationX64MachineExitKind::Trap))
      return Invalid("invalid machine-exit declaration");
    for (size_t J = 0; J < I; ++J)
      if (Options.X64MachineExits[J].Address == E.Address)
        return Invalid("duplicate machine-exit declaration");
  }
  if (auto Error = validateX64TerminalImportDomain(Options))
    return std::move(Error);
  const auto Text = [](llvm::StringRef S, bool AllowSpace = false) {
    return !S.empty() &&
           S.size() <=
               ::neverd::limits::analysis::
                   kX64RecoveryValidateRawEnvironmentDeclarationsTextTextSSizeLimit &&
           llvm::all_of(S, [&](unsigned char C) {
             return C >= (AllowSpace ? 32 : 33) && C <= 126;
           });
  };
  std::vector<std::pair<va_t, va_t>> Ranges;
  for (const auto &R : Options.X64CodeRegions) {
    if (R.Version != 1 || R.Bytes.empty() ||
        R.Bytes.size() > InvalidVA - R.Address || !Text(R.Evidence, true))
      return Invalid("invalid supplemental code region");
    Ranges.emplace_back(R.Address, R.Address + R.Bytes.size());
  }
  for (size_t I = 0; I < Ranges.size(); ++I)
    for (size_t J = 0; J < I; ++J)
      if (Ranges[I].first < Ranges[J].second &&
          Ranges[J].first < Ranges[I].second)
        return Invalid("overlapping supplemental code regions");
  const auto Contains = [&](va_t Target) {
    return llvm::any_of(Ranges, [&](const auto &R) {
      return Target >= R.first && Target < R.second;
    });
  };
  std::vector<std::pair<va_t, va_t>> Slots;
  for (const auto &B : Options.X64TerminalImports)
    Slots.emplace_back(B.Slot, B.Slot + 8);
  for (const auto &B : Options.X64ImportBindings) {
    if (B.Version != 1 || !B.Slot || B.Slot > InvalidVA - 8 ||
        !Text(B.Module) || !Text(B.Symbol) || !Text(B.Evidence, true) ||
        !Contains(B.Target))
      return Invalid("invalid stable named import binding");
    Slots.emplace_back(B.Slot, B.Slot + 8);
  }
  for (size_t I = 0; I < Slots.size(); ++I)
    for (size_t J = 0; J < I; ++J)
      if (Slots[I].first < Slots[J].second && Slots[J].first < Slots[I].second)
        return Invalid("overlapping stable or terminal import bindings");
  return Cost;
}

/// The x64 lifter uses these exact shapes to read and restore the system
/// portion of RFLAGS. The residual keeps each intrinsic. During analysis a
/// PUSHFQ snapshot is an unconstrained runtime value: this overapproximates
/// every architectural flag image without inventing a constant or an alias
/// fact. Writes have no modelled scalar output; later snapshots are fresh
/// again. CLI/STI additionally require the shared supervisor-profile admission.
bool runtimeFlagsIntrinsic(
    const LowOp &Op, std::optional<InterpreterMachineStateProfile> Profile) {
  if (Op.NumInputs == 0 || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2)
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (Id == Intrinsic::Pushf)
    return Op.NumInputs == 1 && Op.Output.isTemp() && Op.Output.Size == 8;
  if (Id == Intrinsic::Popf)
    return Op.NumInputs == 2 && Op.Output.Size == 0 && Op.Inputs[1].Size == 8;
  return Profile && (detail::x64InterruptFlagValue(Op, *Profile).has_value() ||
                     detail::x64AccessCheckFlagValue(Op, *Profile).has_value());
}

namespace {

constexpr uint64_t CPUQueryRegisters[]{x86reg::RAX, x86reg::RBX, x86reg::RCX,
                                       x86reg::RDX};
std::array<NdVar, 2> cpuQuerySelectors() {
  return {NdVar::reg(x86reg::RAX, 4), NdVar::reg(x86reg::RCX, 4)};
}

bool runtimeReadBundle(const SpecializationInstruction &I, NdVar Sample,
                       uint8_t Encoding, size_t Count,
                       std::optional<uint8_t> Third = std::nullopt) {
  const auto &B = I.Origin;
  const unsigned Size = Third ? 3 : 2;
  if (I.NativeBytes.size() != Size || I.NativeBytes[0] != 0x0f ||
      I.NativeBytes[1] != Encoding || (Third && I.NativeBytes[2] != *Third) ||
      B.Size != Size || B.Mode != InstructionMode::Default || B.FirstOp != 0 ||
      B.OpCount != Count || B.Control != LowInstructionControl::None ||
      B.ControlFlags != LowInstructionControlFlag::None || B.Immediate ||
      B.TargetMode != LowInstructionTargetMode::Preserve ||
      B.Address > InvalidVA - Size ||
      I.Fallthrough != SpecializationCursor{B.Address + Size, B.Mode} ||
      I.IsNativeCall || I.TerminalImport ||
      I.NativeStackControl != SpecializationNativeStackControl::None ||
      I.ProfileProjection != InterpreterProfileProjection::None ||
      I.Ops.size() != Count ||
      I.UndefinedEffects.Coverage != LowUndefinedCoverage::Complete ||
      I.UndefinedEffects.OpCount != Count ||
      !I.UndefinedEffects.Effects.empty() ||
      I.UndefinedEffects.OperationDigest !=
          lowUndefinedOperationDigest(I.Ops) ||
      !Sample.isTemp() || (Sample.Size != 8 && Sample.Size != 16) ||
      Sample.Offset > InvalidVA - (Sample.Size - 1))
    return false;
  for (size_t N = 0; N < I.Ops.size(); ++N)
    if (I.Ops[N].Addr != B.Address || I.Ops[N].Seq != static_cast<int>(N) ||
        I.Ops[N].MemoryOrdering != NdMemoryOrdering::None ||
        I.Ops[N].MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
  return true;
}

std::optional<std::vector<NdVar>>
runtimeReadOutputCarriers(llvm::ArrayRef<LowOp> Ops, Intrinsic Id,
                          llvm::ArrayRef<uint64_t> Registers,
                          llvm::ArrayRef<NdVar> Selectors, bool ExactSelectors,
                          std::optional<NdVar> Sample = std::nullopt) {
  if (Ops.size() != 1 + 2 * Registers.size())
    return std::nullopt;
  const auto &Read = Ops[0];
  if (Read.Opcode != NdOp::INTRINSIC ||
      Read.NumInputs != 1 + Selectors.size() || !Read.Inputs[0].isConst() ||
      Read.Inputs[0].Size != 2 ||
      Read.Inputs[0].Offset != static_cast<uint64_t>(Id) ||
      Read.Output != NdVar::reg(x86reg::RAX, 8))
    return std::nullopt;
  for (size_t N = 0; N != Ops.size(); ++N)
    if (Ops[N].Addr != Read.Addr || Ops[N].Seq != static_cast<int>(N) ||
        Ops[N].MemoryOrdering != NdMemoryOrdering::None ||
        Ops[N].MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return std::nullopt;
  for (size_t N = 0; N < Selectors.size(); ++N) {
    const auto &V = Read.Inputs[N + 1];
    if (V.Size != Selectors[N].Size ||
        (!V.isReg() && !V.isTemp() && !V.isConst()) ||
        (!V.isConst() && V.Offset > InvalidVA - (V.Size - 1)) ||
        (ExactSelectors && V != Selectors[N]))
      return std::nullopt;
  }
  const auto Overlap = [](NdVar A, NdVar C) {
    return A.Offset <= C.Offset ? C.Offset - A.Offset < A.Size
                                : A.Offset - C.Offset < C.Size;
  };
  std::vector<NdVar> Outputs;
  for (size_t N = 0; N < Registers.size(); ++N) {
    const auto &Copy = Ops[1 + N];
    const auto &Clear = Ops[1 + Registers.size() + N];
    const auto Value = Copy.Inputs[0];
    if (Copy.Opcode != NdOp::COPY || Copy.NumInputs != 1 ||
        Copy.Output != NdVar::reg(Registers[N], 4) || !Value.isTemp() ||
        Value.Size != 4 || Value.Offset > InvalidVA - 3 ||
        (Sample && Overlap(Value, *Sample)) || Clear.Opcode != NdOp::INT_ZEXT ||
        Clear.NumInputs != 1 || Clear.Output != NdVar::reg(Registers[N], 8) ||
        Clear.Inputs[0] != NdVar::reg(Registers[N], 4) ||
        llvm::any_of(Outputs,
                     [&](NdVar Other) { return Overlap(Value, Other); }))
      return std::nullopt;
    Outputs.push_back(Value);
  }
  return Outputs;
}

std::optional<std::vector<NdVar>>
runtimeReadOutputs(const SpecializationInstruction &I, NdVar Sample,
                   uint8_t Encoding, Intrinsic Id,
                   llvm::ArrayRef<uint64_t> Registers,
                   llvm::ArrayRef<NdVar> Selectors) {
  if (!runtimeReadBundle(I, Sample, Encoding, 1 + 2 * Registers.size()))
    return std::nullopt;
  return runtimeReadOutputCarriers(I.Ops, Id, Registers, Selectors, true,
                                   Sample);
}

std::optional<RuntimeRegisterRead>
lowerRuntimeMsrRead(const SpecializationInstruction &I, NdVar Sample,
                    std::optional<InterpreterMachineStateProfile> Profile) {
  if (!Profile || !isKernelX64MachineStateProfile(*Profile) ||
      !runtimeReadBundle(I, Sample, 0x32, 6))
    return std::nullopt;
  const NdVar Value = I.Ops[0].Output;
  const NdVar Low = I.Ops[1].Output;
  const NdVar Shifted = I.Ops[2].Output;
  const NdVar High = I.Ops[3].Output;
  const auto Match = [&](unsigned Index, NdOp Opcode, NdVar Output,
                         llvm::ArrayRef<NdVar> Inputs) {
    const auto &Op = I.Ops[Index];
    return Op.Opcode == Opcode && Op.Output == Output &&
           Op.NumInputs == Inputs.size() &&
           std::equal(Inputs.begin(), Inputs.end(), Op.Inputs,
                      [](const NdVar &Expected, const NdVar &Actual) {
                        if (Expected.isConst())
                          return Actual.isConst() &&
                                 Actual.Size == Expected.Size &&
                                 Actual.Offset == Expected.Offset;
                        return Actual == Expected;
                      });
  };
  if (!Match(0, NdOp::INTRINSIC, Value,
             {NdVar::scalar(static_cast<uint64_t>(Intrinsic::Rdmsr), 2),
              NdVar::reg(x86reg::RCX, 4)}) ||
      !Match(1, NdOp::SUBBYTES, Low, {Value, NdVar::scalar(0, 4)}) ||
      !Match(2, NdOp::INT_RIGHT, Shifted, {Value, NdVar::scalar(32, 8)}) ||
      !Match(3, NdOp::SUBBYTES, High, {Shifted, NdVar::scalar(0, 4)}) ||
      !Match(4, NdOp::INT_ZEXT, NdVar::reg(x86reg::RAX, 8), {Low}) ||
      !Match(5, NdOp::INT_ZEXT, NdVar::reg(x86reg::RDX, 8), {High}))
    return std::nullopt;
  const NdVar Temporaries[]{Value, Low, Shifted, High};
  const uint16_t Widths[]{8, 4, 8, 4};
  const auto Overlap = [](NdVar A, NdVar B) {
    return A.Offset <= B.Offset ? B.Offset - A.Offset < A.Size
                                : A.Offset - B.Offset < B.Size;
  };
  for (unsigned Index = 0; Index != 4; ++Index) {
    const auto &T = Temporaries[Index];
    if (!T.isTemp() || T.Size != Widths[Index] ||
        T.Offset > InvalidVA - (T.Size - 1) || Overlap(T, Sample))
      return std::nullopt;
    for (unsigned Previous = 0; Previous != Index; ++Previous)
      if (Overlap(T, Temporaries[Previous]))
        return std::nullopt;
  }

  // Intel SDM Vol.2B, RDMSR: ECX selects a 64-bit result, split into EDX:EAX
  // with zero upper halves and unchanged flags. The declared supervisor,
  // nonfaulting domain excludes privilege and inaccessible-MSR faults; it
  // supplies no particular MSR value or repeated-read relation. Unimplemented
  // result bits remain unconstrained too. Keep the original read observable.
  LowOp Copy;
  Copy.Addr = I.Origin.Address;
  Copy.Opcode = NdOp::COPY;
  Copy.Output = Value;
  Copy.addInput(NdVar::tmp(Sample.Offset, 8));
  return RuntimeRegisterRead{8, {Copy}, {Value}};
}

} // namespace

std::optional<llvm::ArrayRef<uint64_t>>
runtimeTimestampRegisters(const SpecializationInstruction &I, NdVar Scratch) {
  static constexpr uint64_t Counter[]{x86reg::RAX, x86reg::RDX};
  static constexpr uint64_t Auxiliary[]{x86reg::RAX, x86reg::RDX, x86reg::RCX};
  const bool Aux = I.NativeBytes.size() == 3;
  const llvm::ArrayRef<uint64_t> Registers =
      Aux ? llvm::ArrayRef<uint64_t>(Auxiliary)
          : llvm::ArrayRef<uint64_t>(Counter);
  if (!runtimeReadBundle(I, Scratch, Aux ? 0x01 : 0x31,
                         1 + 2 * Registers.size(),
                         Aux ? std::optional<uint8_t>(0xf9) : std::nullopt) ||
      !runtimeReadOutputCarriers(I.Ops,
                                 Aux ? Intrinsic::Rdtscp : Intrinsic::Rdtsc,
                                 Registers, {}, true, Scratch))
    return std::nullopt;
  return Registers;
}

// Intel SDM Vol.2B, RDTSC: the low/high words go to EAX/EDX and both
// upper halves clear. The existing multi-output bundle keeps the runtime read;
// these scalar operations only connect its placeholders during raw analysis.
std::optional<RuntimeTimestampRead>
lowerRuntimeTimestampRead(const SpecializationInstruction &I, NdVar Sample) {
  if (Sample.Size != 8)
    return std::nullopt;
  const uint64_t Registers[]{x86reg::RAX, x86reg::RDX};
  const auto Outputs =
      runtimeReadOutputs(I, Sample, 0x31, Intrinsic::Rdtsc, Registers, {});
  if (!Outputs)
    return std::nullopt;
  RuntimeTimestampRead Result;
  std::copy(Outputs->begin(), Outputs->end(),
            Result.PlaceholderOutputs.begin());
  for (unsigned N = 0; N != Result.Ops.size(); ++N) {
    auto &Op = Result.Ops[N];
    Op.Addr = I.Origin.Address;
    Op.Seq = N;
    Op.Opcode = N ? NdOp::SUBBYTES : NdOp::COPY;
    Op.Output = N ? (*Outputs)[N - 1] : I.Ops[0].Output;
    Op.addInput(Sample);
    if (N)
      Op.addInput(NdVar::scalar(4 * (N - 1), 8));
  }
  return Result;
}

std::optional<RuntimeRegisterRead> lowerRuntimeRegisterRead(
    const SpecializationInstruction &I, NdVar Sample,
    std::optional<InterpreterMachineStateProfile> Profile) {
  if (!Sample.isTemp() || Sample.Size != 16 || Sample.Offset > InvalidVA - 15)
    return std::nullopt;
  if (const auto Timestamp =
          lowerRuntimeTimestampRead(I, NdVar::tmp(Sample.Offset, 8)))
    return RuntimeRegisterRead{8,
                               {Timestamp->Ops.begin(), Timestamp->Ops.end()},
                               {Timestamp->PlaceholderOutputs.begin(),
                                Timestamp->PlaceholderOutputs.end()}};

  // CPUID consumes EAX/ECX and writes EAX/EBX/ECX/EDX, clearing their upper
  // halves. The original read remains, including both selectors and its
  // serialization effect. Independent answers overapproximate every CPU;
  // their values cannot authorize host-feature or repeated-query assumptions.
  const auto Outputs =
      runtimeReadOutputs(I, Sample, 0xa2, Intrinsic::Cpuid, CPUQueryRegisters,
                         cpuQuerySelectors());
  if (!Outputs)
    return lowerRuntimeMsrRead(I, Sample, Profile);
  RuntimeRegisterRead Result;
  Result.SampleBytes = 16;
  Result.PlaceholderOutputs = *Outputs;
  for (unsigned N = 0; N != 4; ++N) {
    LowOp Extract;
    Extract.Addr = I.Origin.Address;
    Extract.Seq = Result.Ops.size();
    Extract.Opcode = NdOp::SUBBYTES;
    Extract.Output = (*Outputs)[N];
    Extract.addInput(Sample);
    Extract.addInput(NdVar::scalar(4 * N, 8));
    Result.Ops.push_back(Extract);
  }
  LowOp Primary;
  Primary.Addr = I.Origin.Address;
  Primary.Seq = Result.Ops.size();
  Primary.Opcode = NdOp::INT_ZEXT;
  Primary.Output = I.Ops[0].Output;
  Primary.addInput(Outputs->front());
  Result.Ops.push_back(Primary);
  return Result;
}

std::optional<SpecializationScalarQuery>
describeCPUQuery(llvm::ArrayRef<LowOp> Operations) {
  const auto Outputs =
      runtimeReadOutputCarriers(Operations, Intrinsic::Cpuid, CPUQueryRegisters,
                                cpuQuerySelectors(), false);
  if (!Outputs)
    return std::nullopt;
  SpecializationScalarQuery Query;
  Query.Identity = "x64.cpuid";
  Query.SemanticsVersion = 1;
  Query.Selectors = {Operations[0].Inputs[1], Operations[0].Inputs[2]};
  Query.ResultBits.assign(4, 32);
  for (uint16_t I = 0; I != Outputs->size(); ++I)
    Query.Bindings.push_back({(*Outputs)[I], I});
  Query.Bindings.push_back({Operations[0].Output, 0});
  return Query;
}

llvm::ArrayRef<uint64_t> cpuQueryResultRegisters() { return CPUQueryRegisters; }

// A retained trap keeps its exact lift and undefined-output audit. Neither
// supplies an exception continuation: this nonfaulting proof may collect an
// unreachable trap but cannot execute one or change its coverage grade.
bool isRetainedNativeTrap(const SpecializationInstruction &Insn) {
  if (Insn.Origin.Control != LowInstructionControl::Terminator ||
      Insn.Origin.Immediate || Insn.IsNativeCall ||
      Insn.NativeStackControl != SpecializationNativeStackControl::None ||
      Insn.ProfileProjection != InterpreterProfileProjection::None ||
      Insn.TerminalImport || Insn.NativeBytes.empty() ||
      Insn.NativeBytes.size() > x86::kMaxInstructionLen ||
      Insn.NativeBytes.size() != Insn.Origin.Size || Insn.Ops.size() != 1 ||
      !Insn.UndefinedEffects.Effects.empty() ||
      Insn.UndefinedEffects.OpCount != Insn.Ops.size() ||
      Insn.PreservedState != LowInstructionPreservedState{} ||
      Insn.UndefinedEffects.OperationDigest !=
          lowUndefinedOperationDigest(Insn.Ops))
    return false;
  Decoder Decode;
  Decode.init(Arch::X64);
  Decode.setStrict(true);
  DecodedInsn Native{};
  if (!Decode.decodeOneForLift(Insn.NativeBytes.data(), Insn.NativeBytes.size(),
                               Insn.Origin.Address, Native) ||
      Native.Size != Insn.NativeBytes.size())
    return false;
  auto Flags = LowInstructionControlFlag::Terminator;
  if (Decode.isResumableTrap(Native))
    Flags |= LowInstructionControlFlag::Resumable;
  else if (Native.Id != X86_INS_UD2)
    return false;
  if (Insn.Origin.ControlFlags != Flags)
    return false;
  // Use the decoder's trap classification and exact lift, including INT1 and
  // both INT3 spellings. A self-consistent sidecar cannot authenticate bytes
  // which describe a different operation or exception. The lift owns whether
  // undefined-output coverage is Complete or Missing; neither supplies an
  // exception continuation or permits executing a feasible trap in this proof.
  std::vector<LowOp> Expected;
  LowInstructionUndefinedEffects Effects;
  try {
    Decode.liftToLow(Native, Expected, {}, {}, &Effects);
  } catch (const UnliftedInstruction &) {
    return false;
  }
  return lowUndefinedOperationDigest(Expected) ==
             Insn.UndefinedEffects.OperationDigest &&
         Effects.Coverage == Insn.UndefinedEffects.Coverage &&
         Effects.OpCount == Insn.UndefinedEffects.OpCount &&
         Effects.Effects.empty();
}

// Intel SDM: disabled shadow stacks make RDSSPD/RDSSPQ a NOP, including no
// 32-bit destination zero-extension; INCSSPD/INCSSPQ instead raise #UD. Check
// the provider's classification against canonical bytes and exact operations.
// Missing coverage stays Missing. No other CET instruction is authorized.
bool isCetDisabledProjection(const SpecializationInstruction &Insn) {
  const bool Read = Insn.ProfileProjection ==
                    InterpreterProfileProjection::CetDisabledReadShadowStackV1;
  const bool Trap =
      Insn.ProfileProjection ==
      InterpreterProfileProjection::CetDisabledIncrementShadowStackTrapV1;
  if ((!Read && !Trap) || Insn.IsNativeCall ||
      Insn.Origin.Address == InvalidVA ||
      Insn.Origin.Size > InvalidVA - Insn.Origin.Address ||
      Insn.NativeBytes.size() != Insn.Origin.Size || Insn.Origin.FirstOp ||
      Insn.Origin.OpCount != Insn.Ops.size() ||
      Insn.UndefinedEffects.OpCount != Insn.Ops.size() ||
      Insn.Fallthrough.Address != Insn.Origin.Address + Insn.Origin.Size ||
      Insn.Fallthrough.Mode != Insn.Origin.Mode ||
      Insn.Origin.Mode != InstructionMode::Default ||
      Insn.Origin.TargetMode != LowInstructionTargetMode::Preserve ||
      Insn.NativeStackControl != SpecializationNativeStackControl::None ||
      Insn.Origin.Control != (Trap ? LowInstructionControl::Terminator
                                   : LowInstructionControl::None) ||
      Insn.Origin.ControlFlags != (Trap ? LowInstructionControlFlag::Terminator
                                        : LowInstructionControlFlag::None) ||
      Insn.Origin.Immediate || Insn.Ops.size() != 1 ||
      Insn.UndefinedEffects.Coverage != LowUndefinedCoverage::Missing ||
      !Insn.UndefinedEffects.Effects.empty() ||
      Insn.UndefinedEffects.OperationDigest !=
          lowUndefinedOperationDigest(Insn.Ops))
    return false;
  const auto &Op = Insn.Ops.front();
  if (Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  // Preserve the strict lifter's opaque intrinsic result. It is never
  // executed: this profile faults before any architectural state update.
  if (Read ? (Op.Opcode != NdOp::NOP || Op.Output.Size || Op.NumInputs)
           : (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs != 1 ||
              Op.Output != NdVar::reg(x86reg::RAX, 8) ||
              !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
              Op.Inputs[0].Offset !=
                  static_cast<uint64_t>(Intrinsic::CetIncSsp)))
    return false;
  return x86cet::canonicalEncoding(Insn.NativeBytes,
                                   Read ? x86cet::Operation::Read
                                        : x86cet::Operation::Increment);
}

bool bindCetDisabledPreservedState(SpecializationInstruction &Insn) {
  Insn.PreservedState = {};
  if (Insn.ProfileProjection !=
          InterpreterProfileProjection::CetDisabledReadShadowStackV1 ||
      !isCetDisabledProjection(Insn))
    return false;
  auto &F = Insn.PreservedState;
  F.Audit = LowPreservedStateAudit::CetDisabledReadShadowStackV1;
  F.StateSet = LowPreservedStateSet::LegacyIntegerOpaqueV1;
  F.SemanticsVersion = 1;
  F.Architecture = Arch::X64;
  F.Mode = Insn.Origin.Mode;
  F.Address = Insn.Origin.Address;
  F.Size = Insn.Origin.Size;
  F.OpCount = Insn.Ops.size();
  F.NativeBytesDigest = llvm::toHex(llvm::SHA256::hash(Insn.NativeBytes), true);
  F.OperationDigest = lowUndefinedOperationDigest(Insn.Ops);
  if (!matchesLowPreservedState(F, Insn.Origin, Insn.NativeBytes, Insn.Ops)) {
    F = {};
    return false;
  }
  return true;
}

} // namespace neverd::analysis::x64
