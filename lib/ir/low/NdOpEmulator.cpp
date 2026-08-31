//===- NdOpEmulator.cpp - Light-weight NdOp emulation -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements NdOpEmulator for computing switch-table targets by
/// executing NdOp sequences along data-flow paths.  This file holds the
/// machine state (registers, write-back memory, load log, call-preserved
/// register set) and the step/run driver; the per-opcode execution handlers
/// live in NdOpEmulatorExec.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/NdOpEmulator.h"

#include "neverd/Limits.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace neverd {

namespace {

bool isInstructionControl(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::BRANCH:
  case NdOp::INDIR_BR:
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
  case NdOp::RETURN:
    return true;
  default:
    return false;
  }
}

const LowInstructionBoundary *
boundaryForOp(llvm::ArrayRef<LowOp> Ops,
              llvm::ArrayRef<LowInstructionBoundary> Boundaries,
              size_t OpIndex) {
  auto It = std::upper_bound(
      Boundaries.begin(), Boundaries.end(), OpIndex,
      [](size_t Index, const LowInstructionBoundary &Boundary) {
        return Index < Boundary.FirstOp;
      });
  if (It == Boundaries.begin())
    return nullptr;
  --It;
  const uint64_t First = It->FirstOp;
  if (First > Ops.size() || It->OpCount > Ops.size() - First)
    return nullptr;
  const uint64_t End = First + It->OpCount;
  return OpIndex >= First && OpIndex < End ? &*It : nullptr;
}

/// End of the instruction containing an ARM predicate guard, or zero when
/// this is an ordinary block-level conditional branch.  A LowBlock's boundary
/// metadata uses InstructionGuard authoritatively after structural validation.
/// The legacy vector API retains the conservative control-only same-address
/// rule.
size_t
predicatedInstructionEnd(llvm::ArrayRef<LowOp> Ops,
                         llvm::ArrayRef<LowInstructionBoundary> Boundaries,
                         size_t Guard) {
  if (Guard >= Ops.size() || Ops[Guard].Opcode != NdOp::COND_BR)
    return 0;

  size_t End = 0;
  if (!Boundaries.empty()) {
    const LowInstructionBoundary *Boundary =
        boundaryForOp(Ops, Boundaries, Guard);
    if (!Boundary || (Boundary->Mode != InstructionMode::ARM &&
                      Boundary->Mode != InstructionMode::Thumb))
      return 0;
    const bool IsTaggedGuard = hasLowInstructionControlFlag(
        Boundary->ControlFlags, LowInstructionControlFlag::InstructionGuard);
    if (!IsTaggedGuard)
      return 0;
    End = static_cast<size_t>(Boundary->FirstOp + Boundary->OpCount);
    return Guard + 1 < End ? End : 0;
  } else {
    if (Ops[Guard].Addr == 0)
      return 0;
    End = Guard + 1;
    while (End < Ops.size() && Ops[End].Addr == Ops[Guard].Addr)
      ++End;
  }

  for (size_t I = Guard + 1; I < End; ++I)
    if (isInstructionControl(Ops[I].Opcode))
      return End;
  return 0;
}

} // namespace

void NdOpEmulator::reset() {
  Registers.clear();
  WideRegisters.clear();
  MemStore.clear();
  MemStoreBytes.clear();
  MXCSR = 0x1f80;
  LoadLog.clear();
  ReachedIndirectBranchTarget.reset();
  ReachedSourceMode.reset();
  ReachedTargetMode.reset();
  resetX87State();
}

void NdOpEmulator::setRegister(uint64_t RegOff, uint64_t Value) {
  Registers[RegOff] = Value;
  WideRegisters.erase(RegOff);
}

std::optional<uint64_t> NdOpEmulator::getRegister(uint64_t RegOff) const {
  auto It = Registers.find(RegOff);
  if (It != Registers.end())
    return It->second;
  return std::nullopt;
}

void NdOpEmulator::setRegisterBytes(uint64_t RegOff,
                                    llvm::ArrayRef<uint8_t> Value) {
  std::vector<uint8_t> Bytes(Value.begin(), Value.end());
  WideRegisters[RegOff] = Bytes;
  uint64_t Low = 0;
  if (!Bytes.empty())
    std::memcpy(&Low, Bytes.data(), std::min(Bytes.size(), sizeof(Low)));
  Registers[RegOff] = Low;
}

std::optional<std::vector<uint8_t>>
NdOpEmulator::getRegisterBytes(uint64_t RegOff) const {
  if (auto It = WideRegisters.find(RegOff); It != WideRegisters.end())
    return It->second;
  auto Scalar = Registers.find(RegOff);
  if (Scalar == Registers.end())
    return std::nullopt;
  std::vector<uint8_t> Bytes(sizeof(Scalar->second));
  std::memcpy(Bytes.data(), &Scalar->second, sizeof(Scalar->second));
  return Bytes;
}

bool NdOpEmulator::setMemoryAddressSpaceBase(NdMemoryAddressSpace AddressSpace,
                                             uint64_t Base) {
  if (Img.Arch != Arch::X86 && Img.Arch != Arch::X64)
    return false;
  switch (AddressSpace) {
  case NdMemoryAddressSpace::X86FS:
  case NdMemoryAddressSpace::X86GS:
    MemoryAddressSpaceBases[AddressSpace] = Base;
    return true;
  case NdMemoryAddressSpace::Default:
    return false;
  }
  return false;
}

bool NdOpEmulator::setX86EnqueueContext(uint8_t CurrentPrivilegeLevel,
                                        uint32_t IA32Pasid,
                                        uint8_t LinearAddressBits) {
  if (Img.Arch != Arch::X64 || CurrentPrivilegeLevel > 3 ||
      (IA32Pasid & UINT32_C(0x7ff00000)) != 0 ||
      (LinearAddressBits != 48 && LinearAddressBits != 57))
    return false;
  X86CurrentPrivilegeLevel = CurrentPrivilegeLevel;
  X86IA32Pasid = IA32Pasid;
  X86LinearAddressBits = LinearAddressBits;
  return true;
}

uint64_t NdOpEmulator::readOperand(const NdVar &Op) const {
  auto Truncate = [&](uint64_t Value) {
    if (Op.Size == 0 || Op.Size >= sizeof(Value))
      return Value;
    const unsigned Bits = Op.Size * 8;
    return Value & ((UINT64_C(1) << Bits) - 1);
  };
  if (Op.isConst())
    return Truncate(Op.Offset);
  if (Op.isReg() || Op.isTemp()) {
    auto It = Registers.find(Op.Offset);
    if (It != Registers.end())
      return Truncate(It->second);
  }
  return 0;
}

std::vector<uint8_t> NdOpEmulator::readOperandBytes(const NdVar &Op) const {
  std::vector<uint8_t> Bytes(Op.Size, 0);
  if (Op.isConst()) {
    if (!Bytes.empty())
      std::memcpy(Bytes.data(), &Op.Offset,
                  std::min(Bytes.size(), sizeof(Op.Offset)));
    return Bytes;
  }
  if (Op.isReg() || Op.isTemp()) {
    if (auto Wide = WideRegisters.find(Op.Offset);
        Wide != WideRegisters.end()) {
      std::copy_n(Wide->second.begin(),
                  std::min(Bytes.size(), Wide->second.size()), Bytes.begin());
      return Bytes;
    }
  }
  const uint64_t Scalar = readOperand(Op);
  if (!Bytes.empty())
    std::memcpy(Bytes.data(), &Scalar, std::min(Bytes.size(), sizeof(Scalar)));
  return Bytes;
}

std::optional<uint64_t>
NdOpEmulator::resolveMemoryAddress(const LowOp &Op, uint64_t Offset) const {
  if (Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
    return Offset;
  auto It = MemoryAddressSpaceBases.find(Op.MemoryAddressSpace);
  if (It == MemoryAddressSpaceBases.end())
    return std::nullopt;
  if (Img.Arch == Arch::X86)
    return (It->second + Offset) & UINT64_C(0xffffffff);
  if (Offset > UINT64_MAX - It->second)
    return std::nullopt;
  return It->second + Offset;
}

void NdOpEmulator::writeOutput(const NdVar &Output, uint64_t Value) {
  if (!Output.isReg() && !Output.isTemp())
    return;
  uint64_t Mask = Output.Size < 8 ? (1ULL << (Output.Size * 8)) - 1 : ~0ULL;
  uint64_t Result = Value & Mask;
  if (Output.isReg() && Output.Size < 8) {
    auto Existing = Registers.find(Output.Offset);
    if (Existing != Registers.end())
      Result = (Existing->second & ~Mask) | Result;
  }
  Registers[Output.Offset] = Result;
  if (Output.Size > sizeof(Value)) {
    std::vector<uint8_t> Bytes(Output.Size, 0);
    std::memcpy(Bytes.data(), &Value, sizeof(Value));
    WideRegisters[Output.Offset] = std::move(Bytes);
  } else if (Output.isReg()) {
    auto Existing = WideRegisters.find(Output.Offset);
    if (Existing != WideRegisters.end() &&
        Existing->second.size() > Output.Size) {
      std::memcpy(Existing->second.data(), &Result, Output.Size);
      uint64_t Low = 0;
      std::memcpy(&Low, Existing->second.data(),
                  std::min(Existing->second.size(), sizeof(Low)));
      Registers[Output.Offset] = Low;
      return;
    }
    WideRegisters.erase(Output.Offset);
  } else {
    WideRegisters.erase(Output.Offset);
  }
}

void NdOpEmulator::writeOutputBytes(const NdVar &Output,
                                    llvm::ArrayRef<uint8_t> Value) {
  if (!Output.isReg() && !Output.isTemp())
    return;
  if (Output.isReg()) {
    auto Existing = WideRegisters.find(Output.Offset);
    if (Existing != WideRegisters.end() &&
        Existing->second.size() > Output.Size) {
      std::copy_n(Value.begin(), std::min<size_t>(Output.Size, Value.size()),
                  Existing->second.begin());
      if (Value.size() < Output.Size)
        std::fill(Existing->second.begin() + Value.size(),
                  Existing->second.begin() + Output.Size, 0);
      uint64_t Low = 0;
      std::memcpy(&Low, Existing->second.data(),
                  std::min(Existing->second.size(), sizeof(Low)));
      Registers[Output.Offset] = Low;
      return;
    }
    if (Output.Size <= sizeof(uint64_t)) {
      uint64_t Low = 0;
      if (!Value.empty())
        std::memcpy(&Low, Value.data(),
                    std::min<size_t>(Output.Size, Value.size()));
      writeOutput(Output, Low);
      return;
    }
  }
  std::vector<uint8_t> Bytes(Output.Size, 0);
  std::copy_n(Value.begin(), std::min(Bytes.size(), Value.size()),
              Bytes.begin());
  uint64_t Low = 0;
  if (!Bytes.empty())
    std::memcpy(&Low, Bytes.data(), std::min(Bytes.size(), sizeof(Low)));
  Registers[Output.Offset] = Low;
  if (Output.Size > sizeof(Low))
    WideRegisters[Output.Offset] = std::move(Bytes);
  else
    WideRegisters.erase(Output.Offset);
}

std::optional<std::vector<uint8_t>>
NdOpEmulator::loadMemoryBytes(uint64_t Addr, uint16_t Size) const {
  if (Size == 0 || Size - 1 > UINT64_MAX - Addr)
    return std::nullopt;
  std::vector<uint8_t> Bytes(Size);
  for (uint16_t I = 0; I < Size; ++I) {
    const uint64_t ByteAddress = Addr + I;
    if (const auto Stored = MemStoreBytes.find(ByteAddress);
        Stored != MemStoreBytes.end()) {
      Bytes[I] = Stored->second;
    } else {
      const Segment *Mapped = Img.getSegmentFor(ByteAddress);
      if (!Mapped || !Mapped->isReadable() || ByteAddress < Mapped->VA)
        return std::nullopt;
      const uint64_t Offset = ByteAddress - Mapped->VA;
      if (Offset >= Mapped->Data.size())
        return std::nullopt;
      Bytes[I] = Mapped->Data[static_cast<size_t>(Offset)];
    }
  }
  return Bytes;
}

std::optional<uint64_t> NdOpEmulator::loadMemory(uint64_t Addr,
                                                 uint16_t Size) const {
  if (Size != 1 && Size != 2 && Size != 4 && Size != 8)
    return std::nullopt;
  const auto Bytes = loadMemoryBytes(Addr, Size);
  if (!Bytes)
    return std::nullopt;
  uint64_t Val = 0;
  std::memcpy(&Val, Bytes->data(), Size);
  return Val;
}

bool NdOpEmulator::canWriteMemoryBytes(uint64_t Addr, uint16_t Size) const {
  if (Size == 0 || Size - 1 > UINT64_MAX - Addr)
    return false;
  if (!StrictMode)
    return true;
  for (uint16_t I = 0; I < Size; ++I) {
    const uint64_t ByteAddress = Addr + I;
    const Segment *Mapped = Img.getSegmentFor(ByteAddress);
    if (!Mapped || !Mapped->isWritable() || ByteAddress < Mapped->VA ||
        ByteAddress - Mapped->VA >= Mapped->Size)
      return false;
  }
  return true;
}

bool NdOpEmulator::storeMemoryBytes(uint64_t Addr,
                                    llvm::ArrayRef<uint8_t> Value) {
  if (Value.empty() || Value.size() > UINT16_MAX)
    return false;
  const uint16_t Size = static_cast<uint16_t>(Value.size());
  if (!canWriteMemoryBytes(Addr, Size))
    return false;
  if (!MemStore.contains(Addr) &&
      static_cast<int>(MemStore.size()) >= limits::kMaxEmulatorStoreEntries) {
    // A dropped store leaves whatever was underneath it visible to the next
    // load of the same address, so it is not a missing write but a wrong read
    // waiting to happen.  The bound stays; what changes is that a caller can
    // find out it was reached.
    ++Skips.DroppedStores;
    return false;
  }
  uint64_t Low = 0;
  std::memcpy(&Low, Value.data(), std::min<size_t>(Value.size(), sizeof(Low)));
  MemStore[Addr] = Low;
  for (uint16_t I = 0; I < Size; ++I)
    MemStoreBytes[Addr + I] = Value[I];
  return true;
}

bool NdOpEmulator::storeMemory(uint64_t Addr, uint16_t Size, uint64_t Value) {
  if (Size != 1 && Size != 2 && Size != 4 && Size != 8)
    return false;
  std::array<uint8_t, sizeof(Value)> Bytes{};
  std::memcpy(Bytes.data(), &Value, Size);
  return storeMemoryBytes(Addr, llvm::ArrayRef(Bytes).take_front(Size));
}

void NdOpEmulator::setCallPreservedRegisters(std::vector<uint64_t> Regs) {
  CallPreservedRegs = std::move(Regs);
  std::sort(CallPreservedRegs.begin(), CallPreservedRegs.end());
  StepOverCalls = true;
}

void NdOpEmulator::clobberVolatileRegisters() {
  for (auto It = Registers.begin(); It != Registers.end();) {
    // The stack pointer, frame pointer, and callee-saved registers (declared by
    // the caller) survive a call by ABI; every other register (caller-saved
    // GPRs, the return-value register, the link register, vector registers) may
    // be overwritten, so its cached value is no longer trustworthy afterward.
    bool Preserved = std::binary_search(CallPreservedRegs.begin(),
                                        CallPreservedRegs.end(), It->first);
    if (Preserved)
      ++It;
    else
      It = Registers.erase(It);
  }
  for (auto It = WideRegisters.begin(); It != WideRegisters.end();) {
    if (!Registers.count(It->first))
      It = WideRegisters.erase(It);
    else
      ++It;
  }
}

bool NdOpEmulator::step(const LowOp &Op) {
  // Consumer-side validation is mandatory because callers may construct a
  // LowOp directly without first running the instruction-boundary verifier.
  // Never approximate an invalid address-space tag as ordinary memory.
  if (!isKnownMemoryAddressSpace(Op.MemoryAddressSpace))
    return false;
  if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs > 0 &&
      Op.Inputs[0].isConst() &&
      (static_cast<Intrinsic>(Op.Inputs[0].Offset) ==
           Intrinsic::X86Invalidate ||
       static_cast<Intrinsic>(Op.Inputs[0].Offset) ==
           Intrinsic::X86MsrAccess)) {
    // A scalar tracer has neither CPL/CR4/CPUID state nor a guest TLB.  Even a
    // structurally valid invalidation or MSR access must therefore stop here
    // rather than pretending the architectural state transition succeeded.
    return false;
  }
  if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
    if (!opcodeSupportsMemoryAddressSpace(Op.Opcode))
      return false;
    if (Op.Opcode == NdOp::INTRINSIC) {
      if (Op.NumInputs == 0 || !Op.Inputs[0].isConst())
        return false;
      const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
      if (!intrinsicSupportsMemoryAddressSpace(Id) ||
          !intrinsicMemoryAddressSpaceShapeIsValid(
              Id, Op.NumInputs, Op.Output.Size,
              Op.NumInputs > 1 ? Op.Inputs[1].Size : 0,
              Op.NumInputs > 2 ? Op.Inputs[2].Size : 0,
              Op.NumInputs > 3 ? Op.Inputs[3].Size : 0))
        return false;
      switch (Id) {
      case Intrinsic::MaskedLoadB:
      case Intrinsic::MaskedLoadW:
      case Intrinsic::MaskedLoadD:
      case Intrinsic::MaskedLoadQ:
      case Intrinsic::MaskedStoreW:
      case Intrinsic::MaskedStoreD:
      case Intrinsic::MaskedStoreQ:
      case Intrinsic::MaskedStoreB:
      case Intrinsic::Clflush:
      case Intrinsic::Clflushopt:
      case Intrinsic::Clwb:
      case Intrinsic::Prefetch:
      case Intrinsic::PrefetchT0:
      case Intrinsic::PrefetchT1:
      case Intrinsic::PrefetchT2:
      case Intrinsic::PrefetchNta:
      case Intrinsic::PrefetchW:
      case Intrinsic::PrefetchWT1:
      case Intrinsic::Ldmxcsr:
      case Intrinsic::Stmxcsr:
      case Intrinsic::AMXLoadConfig:
      case Intrinsic::AMXStoreConfig:
      case Intrinsic::AMXTileLoad:
      case Intrinsic::AMXTileStore:
      case Intrinsic::X86FourFMA:
      case Intrinsic::X86VP4DPWSSD:
      case Intrinsic::X86VP4DPWSSDS:
      case Intrinsic::RequireAligned:
      case Intrinsic::ApxRaoAdd:
      case Intrinsic::ApxRaoAnd:
      case Intrinsic::ApxRaoOr:
      case Intrinsic::ApxRaoXor:
      case Intrinsic::ApxCmpccXadd:
      case Intrinsic::Enqcmd:
      case Intrinsic::Enqcmds:
        return executeIntrinsic(Op);
      default:
        // Segmented strings still require architectural REP/DF/flags state
        // that this constant-tracing engine does not model.  Stop rather than
        // approximate an observable memory effect.
        return false;
      }
    }
  }
  switch (Op.Opcode) {
  case NdOp::BRANCH:
  case NdOp::COND_BR:
  case NdOp::RETURN:
    // These change or end control flow: the single linear execution path the
    // emulator models stops here.
    return false;

  case NdOp::INDIR_BR:
    ReachedIndirectBranchTarget.reset();
    ReachedSourceMode.reset();
    ReachedTargetMode.reset();
    if (Op.NumInputs >= 1)
      ReachedIndirectBranchTarget = Op.Inputs[0];
    return false;

  case NdOp::CALL:
  case NdOp::INDIR_CALL:
    // A call returns to the following instruction, so it does not end the
    // linear path the way a branch does.  When the caller has declared the
    // call-preserved registers, drop the values of every other (caller-saved)
    // register and continue, so a table base materialised *after* an
    // intervening call (a `bl`/`call` inside a switch block before the base
    // `lea`/`adr`) is still folded, without ever reading a stale caller-saved
    // value across the call.  Table/target soundness is still enforced by the
    // segment and validity checks at every use site.  Absent an ABI, fall back
    // to the conservative model: a call ends the emulated path.
    if (!StepOverCalls)
      return false;
    clobberVolatileRegisters();
    return true;

  case NdOp::INTRINSIC:
    if (Op.NumInputs >= 1 && Op.Inputs[0].isConst()) {
      const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
      if (Id == Intrinsic::X87Fprem || Id == Intrinsic::X87Fprem1 ||
          Id == Intrinsic::X87ReadStatus || Id == Intrinsic::X87Fninit ||
          Id == Intrinsic::X87Fnclex)
        return executeX87(Op);
      if (Id == Intrinsic::MaskedLoadB || Id == Intrinsic::MaskedLoadW ||
          Id == Intrinsic::MaskedLoadD || Id == Intrinsic::MaskedLoadQ ||
          Id == Intrinsic::MaskedStoreW || Id == Intrinsic::MaskedStoreD ||
          Id == Intrinsic::MaskedStoreQ || Id == Intrinsic::MaskedStoreB ||
          Id == Intrinsic::Clflush || Id == Intrinsic::Clflushopt ||
          Id == Intrinsic::Clwb || Id == Intrinsic::Prefetch ||
          Id == Intrinsic::PrefetchT0 || Id == Intrinsic::PrefetchT1 ||
          Id == Intrinsic::PrefetchT2 || Id == Intrinsic::PrefetchNta ||
          Id == Intrinsic::PrefetchW || Id == Intrinsic::PrefetchWT1 ||
          Id == Intrinsic::Ldmxcsr || Id == Intrinsic::Stmxcsr ||
          Id == Intrinsic::X86RequireDivPrecondition ||
          Id == Intrinsic::RequireAligned || Id == Intrinsic::AMXLoadConfig ||
          Id == Intrinsic::AMXStoreConfig || Id == Intrinsic::AMXTileLoad ||
          Id == Intrinsic::AMXTileStore || Id == Intrinsic::AMXTileZero ||
          Id == Intrinsic::AMXClearStartRow ||
          Id == Intrinsic::AMXTileCompute || Id == Intrinsic::AMXTileRow ||
          Id == Intrinsic::Pdep || Id == Intrinsic::Pext ||
          Id == Intrinsic::Mpsadbw || Id == Intrinsic::Vdbpsadbw ||
          Id == Intrinsic::X86FourFMA || Id == Intrinsic::F16CConvert ||
          Id == Intrinsic::X86VP4DPWSSD || Id == Intrinsic::X86VP4DPWSSDS ||
          Id == Intrinsic::X86ApproxFloat || Id == Intrinsic::X86FPClass ||
          Id == Intrinsic::X86FPArith || Id == Intrinsic::X86FPConvert ||
          Id == Intrinsic::X86FPRoundTransform ||
          Id == Intrinsic::X86FPExtract ||
          Id == Intrinsic::X86FPRange ||
          Id == Intrinsic::X86FPFixup ||
          Id == Intrinsic::X86FPScale ||
          Id == Intrinsic::X86FPCompare ||
          Id == Intrinsic::EVEXCompressStore ||
          Id == Intrinsic::EVEXExpandLoad || Id == Intrinsic::AesEnc ||
          Id == Intrinsic::AesEncLast || Id == Intrinsic::AesDec ||
          Id == Intrinsic::AesDecLast || Id == Intrinsic::Pclmulqdq ||
          Id == Intrinsic::ApxRaoAdd || Id == Intrinsic::ApxRaoAnd ||
          Id == Intrinsic::ApxRaoOr || Id == Intrinsic::ApxRaoXor ||
          Id == Intrinsic::ApxCmpccXadd || Id == Intrinsic::Enqcmd ||
          Id == Intrinsic::Enqcmds)
        return executeIntrinsic(Op);
    }
    // Opaque intrinsic (SSE/NEON vector op, etc.): it cannot alter control
    // flow, so linear constant-tracing must continue past it.  Its result is
    // unknown — invalidate the output register so dependents do not fold a
    // stale value; registers it does not write (e.g. a loop-invariant table
    // base in a GP register materialised by a later `lea`) are preserved.
    if (StrictMode)
      return false;
    ++Skips.ApproximatedOps;
    if (Op.Output.isReg() || Op.Output.isTemp()) {
      Registers.erase(Op.Output.Offset);
      WideRegisters.erase(Op.Output.Offset);
    }
    return !StrictMode;

  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::SUBBYTES:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NEG2:
  case NdOp::CONCAT:
    return executeCopy(Op);

  case NdOp::LOAD:
    return executeLoad(Op);

  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_MULT:
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
    return executeArith(Op);

  case NdOp::FLOAT_ADD:
  case NdOp::FLOAT_SUB:
  case NdOp::FLOAT_MULT:
  case NdOp::FLOAT_DIV:
  case NdOp::FLOAT_FMA:
  case NdOp::FLOAT_NEG:
  case NdOp::FLOAT_SQRT:
  case NdOp::FLOAT_LESS:
    return executeFloatArith(Op);

  case NdOp::FLOAT_INT2FLOAT:
  case NdOp::FLOAT_UINT2FLOAT:
  case NdOp::FLOAT_FLOAT2INT:
  case NdOp::FLOAT_FLOAT2UINT:
    return executeFloatConvert(Op);

  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
    return executeCompare(Op);

  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::BOOL_NOT:
    return executeBool(Op);

  case NdOp::SELECT:
  case NdOp::INT_NOT:
  case NdOp::POPCOUNT:
  case NdOp::LZCOUNT:
  case NdOp::INSERT:
  case NdOp::EXTRACT:
    return executeMisc(Op);

  case NdOp::STORE:
    return executeStore(Op);

  case NdOp::ATOMIC_XCHG:
    // The constant-tracing emulator stores one host word per value and cannot
    // represent the 128-bit exchanges this opcode exists for.  Stop instead
    // of carrying stale memory or destination state past an observable RMW.
    ++Skips.UnsupportedOps;
    if (Op.Output.isReg() || Op.Output.isTemp()) {
      Registers.erase(Op.Output.Offset);
      WideRegisters.erase(Op.Output.Offset);
    }
    return false;

  case NdOp::ATOMIC_ADD: {
    const LowMemoryOperandView Memory = lowMemoryOperands(Op);
    if (!Memory.Complete || Memory.AccessSize > 8)
      return false;
    auto Addr = resolveMemoryAddress(Op, readOperand(*Memory.Address));
    if (!Addr)
      return false;
    auto Old = loadMemory(*Addr, Memory.AccessSize);
    if (!Old)
      return false;
    if (!storeMemory(*Addr, Memory.AccessSize,
                     *Old + readOperand(*Memory.StoredValue)))
      return false;
    writeOutput(Op.Output, *Old);
    return true;
  }

  case NdOp::ATOMIC_CMPXCHG: {
    const LowMemoryOperandView Memory = lowMemoryOperands(Op);
    if (!Memory.Complete)
      return false;
    auto Addr = resolveMemoryAddress(Op, readOperand(*Memory.Address));
    if (!Addr || !canWriteMemoryBytes(*Addr, Memory.AccessSize))
      return false;
    if (Memory.AccessSize == 16) {
      const auto Old = loadMemoryBytes(*Addr, 16);
      const std::vector<uint8_t> Expected =
          readOperandBytes(*Memory.ExpectedValue);
      const std::vector<uint8_t> Desired =
          readOperandBytes(*Memory.StoredValue);
      if (!Old || Expected.size() != 16 || Desired.size() != 16)
        return false;
      if (*Old == Expected && !storeMemoryBytes(*Addr, Desired))
        return false;
      writeOutputBytes(Op.Output, *Old);
      return true;
    }
    if (Memory.AccessSize > 8) {
      ++Skips.UnsupportedOps;
      return false;
    }
    auto Old = loadMemory(*Addr, Memory.AccessSize);
    if (!Old)
      return false;
    uint64_t Mask =
        Memory.AccessSize < 8 ? (1ULL << (Memory.AccessSize * 8)) - 1 : ~0ULL;
    if (*Old == (readOperand(*Memory.ExpectedValue) & Mask) &&
        !storeMemory(*Addr, Memory.AccessSize,
                     readOperand(*Memory.StoredValue) & Mask))
      return false;
    writeOutput(Op.Output, *Old);
    return true;
  }

  case NdOp::NOP:
    return true;

  default:
    // An opcode with no model here — floating point, a cast the lifters route
    // through no other operation.  The path continues by default because a
    // switch dispatch is recovered from a constant that every use site
    // re-validates against the image, so carrying on past what does not feed
    // the address costs nothing and stopping loses tables.  Counted either
    // way, so a caller can tell a folded constant from one that stepped over
    // something on its way here.
    ++Skips.UnsupportedOps;
    return !StrictMode;
  }
}

size_t NdOpEmulator::run(const std::vector<LowOp> &Ops) {
  return runImpl(Ops, {});
}

size_t NdOpEmulator::run(const LowBlock &Block) {
  if (!Block.InstructionBoundaries.empty()) {
    if (llvm::Error Error = validateLowInstructionBoundaries(
            Block, LowInstructionBoundaryRequirement::Required)) {
      llvm::consumeError(std::move(Error));
      return 0;
    }
  }
  return runImpl(Block.Ops, Block.InstructionBoundaries);
}

size_t
NdOpEmulator::runImpl(llvm::ArrayRef<LowOp> Ops,
                      llvm::ArrayRef<LowInstructionBoundary> Boundaries) {
  size_t Count = 0;
  for (size_t I = 0; I < Ops.size();) {
    const LowOp &Op = Ops[I];
    if (const size_t End = predicatedInstructionEnd(Ops, Boundaries, I)) {
      if (Op.NumInputs < 2)
        break;
      const NdVar &Predicate = Op.Inputs[1];
      std::optional<uint64_t> PredicateValue;
      if (Predicate.isConst())
        PredicateValue = Predicate.Offset;
      else if (Predicate.isReg() || Predicate.isTemp())
        PredicateValue = getRegister(Predicate.Offset);
      if (!PredicateValue)
        break;
      // ARM's guard targets the next instruction when !predicate holds.  A
      // concrete true guard therefore skips every remaining effect in this
      // instruction; a false guard executes them in order.  Neither choice is
      // a block terminator by itself.
      if (*PredicateValue != 0) {
        I = End;
        continue;
      }
      ++I;
      continue;
    }
    if (!step(Op)) {
      if (Op.Opcode == NdOp::INDIR_BR) {
        if (const LowInstructionBoundary *Boundary =
                boundaryForOp(Ops, Boundaries, I)) {
          ReachedSourceMode = Boundary->Mode;
          ReachedTargetMode = Boundary->TargetMode;
        }
      }
      break;
    }
    ++Count;
    ++I;
  }
  return Count;
}

std::optional<uint64_t> NdOpEmulator::reachedIndirectTarget() const {
  if (!ReachedIndirectBranchTarget)
    return std::nullopt;
  if (ReachedIndirectBranchTarget->isConst())
    return ReachedIndirectBranchTarget->Offset;
  if (ReachedIndirectBranchTarget->isReg() ||
      ReachedIndirectBranchTarget->isTemp())
    return getRegister(ReachedIndirectBranchTarget->Offset);
  return std::nullopt;
}

std::optional<uint64_t>
NdOpEmulator::computeTarget(const std::vector<LowOp> &Ops, uint64_t IndexRegOff,
                            uint64_t IndexValue) {
  reset();
  setRegister(IndexRegOff, IndexValue);
  run(Ops);
  return reachedIndirectTarget();
}

std::optional<LowControlTarget>
NdOpEmulator::computeTarget(const LowBlock &Block, uint64_t IndexRegOff,
                            uint64_t IndexValue) {
  reset();
  setRegister(IndexRegOff, IndexValue);
  run(Block);

  const std::optional<uint64_t> RawTarget = reachedIndirectTarget();
  if (!RawTarget || !ReachedSourceMode || !ReachedTargetMode)
    return std::nullopt;
  llvm::Expected<LowControlTarget> Target = canonicalizeLowControlTarget(
      *RawTarget, *ReachedSourceMode, *ReachedTargetMode);
  if (!Target) {
    llvm::consumeError(Target.takeError());
    return std::nullopt;
  }
  return *Target;
}

void NdOpEmulator::collapseLoadRecords(std::vector<LoadRecord> &Records) {
  if (Records.size() <= 1)
    return;

  std::sort(
      Records.begin(), Records.end(),
      [](const LoadRecord &A, const LoadRecord &B) { return A.Addr < B.Addr; });

  std::vector<LoadRecord> Collapsed;
  Collapsed.push_back(Records[0]);
  uint16_t UnitSize = Records[0].Size;
  for (size_t I = 1; I < Records.size(); ++I) {
    auto &Last = Collapsed.back();
    uint64_t LastEnd = Last.Addr + Last.Size;
    if (Records[I].Addr <= LastEnd && Records[I].Size == UnitSize) {
      uint64_t NewEnd = Records[I].Addr + Records[I].Size;
      if (NewEnd > LastEnd)
        Last.Size = static_cast<uint16_t>(NewEnd - Last.Addr);
    } else {
      Collapsed.push_back(Records[I]);
      UnitSize = Records[I].Size;
    }
  }
  Records = std::move(Collapsed);
}

} // namespace neverd
