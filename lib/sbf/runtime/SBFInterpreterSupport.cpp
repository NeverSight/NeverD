//===- SBFInterpreterSupport.cpp - SBF interpreter setup and decoding -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What the SBF execution loop needs before and around each step: raw
/// instruction decoding, the wide shift and high-multiply primitives the
/// host does not provide, and the program-layout and VM-memory validation
/// that makes execution deterministic.
///
//===----------------------------------------------------------------------===//

#include "SBFInterpreterDetail.h"

#include "llvm/Support/Endian.h"

#include <bit>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace neverd::sbf {

//===----------------------------------------------------------------------===//
// Instruction decoding, validation, and semantic helpers
//===----------------------------------------------------------------------===//

namespace {

bool virtualSpan(const MemoryRegion &Region, uint64_t &Size) {
  const uint64_t BackingSize = Region.Bytes.size();
  if (Region.VMGapSize == 0) {
    Size = BackingSize;
    return true;
  }
  if (BackingSize % Region.VMGapSize != 0 ||
      BackingSize >
          std::numeric_limits<uint64_t>::max() / kStackFrameGapMultiplier)
    return false;
  Size = BackingSize * kStackFrameGapMultiplier;
  return true;
}

bool rangesOverlap(const MemoryRegion &Left, const MemoryRegion &Right) {
  if (Left.Bytes.empty() || Right.Bytes.empty())
    return false;
  uint64_t LeftSize = 0;
  uint64_t RightSize = 0;
  if (!virtualSpan(Left, LeftSize) || !virtualSpan(Right, RightSize))
    return true;
  if (LeftSize > std::numeric_limits<uint64_t>::max() - Left.Address ||
      RightSize > std::numeric_limits<uint64_t>::max() - Right.Address)
    return true;
  return Left.Address < Right.Address + RightSize &&
         Right.Address < Left.Address + LeftSize;
}

} // namespace

namespace interpreter_detail {

int64_t signed64(uint64_t Value) { return std::bit_cast<int64_t>(Value); }
int32_t signed32(uint32_t Value) { return std::bit_cast<int32_t>(Value); }

uint32_t arithmeticShiftRight32(uint32_t Value, uint32_t Shift) {
  Shift &= kWordBitWidth - 1;
  if (Shift == 0)
    return Value;
  uint32_t Result = Value >> Shift;
  if ((Value & (uint32_t{1} << (kWordBitWidth - 1))) != 0)
    Result |= std::numeric_limits<uint32_t>::max() << (kWordBitWidth - Shift);
  return Result;
}

uint64_t arithmeticShiftRight64(uint64_t Value, uint64_t Shift) {
  Shift &= kDoubleWordBitWidth - 1;
  if (Shift == 0)
    return Value;
  uint64_t Result = Value >> Shift;
  if ((Value & (uint64_t{1} << (kDoubleWordBitWidth - 1))) != 0)
    Result |= std::numeric_limits<uint64_t>::max()
              << (kDoubleWordBitWidth - Shift);
  return Result;
}

uint64_t unsignedHighMultiply64(uint64_t Left, uint64_t Right) {
  const uint64_t LeftLow = static_cast<uint32_t>(Left);
  const uint64_t LeftHigh = Left >> kWordBitWidth;
  const uint64_t RightLow = static_cast<uint32_t>(Right);
  const uint64_t RightHigh = Right >> kWordBitWidth;
  const uint64_t LowProduct = LeftLow * RightLow;
  const uint64_t CrossProduct =
      LeftHigh * RightLow + (LowProduct >> kWordBitWidth);
  uint64_t Middle = static_cast<uint32_t>(CrossProduct);
  const uint64_t Carry = CrossProduct >> kWordBitWidth;
  Middle += LeftLow * RightHigh;
  return LeftHigh * RightHigh + Carry + (Middle >> kWordBitWidth);
}

uint64_t signedHighMultiply64(uint64_t Left, uint64_t Right) {
  return unsignedHighMultiply64(Left, Right) -
         ((Left >> (kDoubleWordBitWidth - 1)) != 0 ? Right : 0) -
         ((Right >> (kDoubleWordBitWidth - 1)) != 0 ? Left : 0);
}

RawInstruction decodeRaw(const SBFProgram &Program, size_t Slot) {
  const uint8_t *Bytes = Program.text().data() + Slot * kInstructionSize;
  RawInstruction Instruction;
  Instruction.Slot = Slot;
  Instruction.RawOpcode = Bytes[kOpcodeOffset];
  Instruction.Dst = Bytes[kRegisterByteOffset] & kRegisterEncodingMask;
  Instruction.Src = Bytes[kRegisterByteOffset] >> kRegisterEncodingBits;
  Instruction.Offset = static_cast<int16_t>(
      llvm::support::endian::read16le(Bytes + kBranchOffsetOffset));
  Instruction.Immediate = static_cast<int32_t>(
      llvm::support::endian::read32le(Bytes + kImmediateOffset));
  Instruction.Info =
      getOpcodeInfo(Instruction.RawOpcode, Program.ExecutableImage.version());
  return Instruction;
}

validation_detail::InstructionValidation
validateRawInstruction(const SBFProgram &Program,
                       const RawInstruction &Instruction) {
  const Version TheVersion = Program.ExecutableImage.version();
  return validation_detail::validateInstruction(
      Program.text(), TheVersion,
      {Instruction.Slot, Instruction.RawOpcode, Instruction.Dst,
       Instruction.Src, Instruction.Offset, Instruction.Immediate,
       Instruction.Info});
}

llvm::Error validateProgram(const SBFProgram &Program,
                            const InterpreterOptions &Options) {
  if (llvm::Error Error = validateVMConfig(Program.Config))
    return Error;
  if (!isConcreteVersion(Program.ExecutableImage.version()))
    return llvm::make_error<llvm::StringError>(
        "sbf: raw interpreter requires a concrete SBF version",
        llvm::inconvertibleErrorCode());
  const llvm::ArrayRef<uint8_t> Text = Program.text();
  if (Text.empty() || Text.size() % kInstructionSize != 0)
    return llvm::make_error<llvm::StringError>(
        "sbf: raw interpreter requires non-empty, instruction-aligned text",
        llvm::inconvertibleErrorCode());
  if (Text.size() / kInstructionSize > kMaxInstructions)
    return llvm::make_error<llvm::StringError>(
        "sbf: raw interpreter program exceeds the instruction limit",
        llvm::inconvertibleErrorCode());
  if (Program.ExecutableImage.entrySlot() >= Text.size() / kInstructionSize)
    return llvm::make_error<llvm::StringError>(
        "sbf: raw interpreter entry point is outside program text",
        llvm::inconvertibleErrorCode());
  if (Options.MaxCallDepth && *Options.MaxCallDepth == 0)
    return llvm::make_error<llvm::StringError>(
        "sbf: raw interpreter call-depth limit must be non-zero",
        llvm::inconvertibleErrorCode());
  if (Options.MaxCallDepth &&
      *Options.MaxCallDepth > Program.Config.MaxCallDepth)
    return llvm::make_error<llvm::StringError>(
        "sbf: raw interpreter call-depth limit exceeds the VM configuration",
        llvm::inconvertibleErrorCode());
  return llvm::Error::success();
}

bool rangeContains(const MemoryRegion &Region, uint64_t Address, size_t Size,
                   size_t &Offset) {
  if (Address < Region.Address)
    return false;
  const uint64_t Delta = Address - Region.Address;
  if (Region.VMGapSize == 0) {
    if (Delta > Region.Bytes.size() || Size > Region.Bytes.size() - Delta)
      return false;
    Offset = static_cast<size_t>(Delta);
    return true;
  }

  uint64_t Span = 0;
  if (!virtualSpan(Region, Span) || Delta >= Span)
    return false;
  const uint64_t GapSize = Region.VMGapSize;
  const uint64_t Period = GapSize * kStackFrameGapMultiplier;
  const uint64_t PeriodOffset = Delta % Period;
  if (PeriodOffset >= GapSize || Size > GapSize - PeriodOffset)
    return false;
  const uint64_t Frame = Delta / Period;
  const uint64_t BackingOffset = Frame * GapSize + PeriodOffset;
  if (BackingOffset > Region.Bytes.size() ||
      Size > Region.Bytes.size() - BackingOffset)
    return false;
  Offset = static_cast<size_t>(BackingOffset);
  return true;
}

llvm::Error validateMemory(const std::vector<MemoryRegion> &Memory,
                           Version TheVersion, const SBFVMConfig &Config) {
  const bool RequiresAlignedMapping =
      versionHasFeature(TheVersion, VersionFeature::AlignedMemoryMapping) ||
      Config.AlignedMemoryMapping;
  std::set<uint64_t> AlignedRegionIndices;
  for (size_t I = 0; I < Memory.size(); ++I) {
    if (Memory[I].VMGapSize != 0 && !std::has_single_bit(Memory[I].VMGapSize))
      return llvm::make_error<llvm::StringError>(
          "sbf: VM memory gap size must be a power of two",
          llvm::inconvertibleErrorCode());
    uint64_t Span = 0;
    if (!virtualSpan(Memory[I], Span))
      return llvm::make_error<llvm::StringError>(
          "sbf: gapped VM memory region has an invalid backing size",
          llvm::inconvertibleErrorCode());
    if (Span > std::numeric_limits<uint64_t>::max() - Memory[I].Address)
      return llvm::make_error<llvm::StringError>(
          "sbf: VM memory region address range overflows",
          llvm::inconvertibleErrorCode());
    if (RequiresAlignedMapping) {
      const uint64_t RegionIndex = Memory[I].Address >> kVirtualAddressBits;
      const uint64_t LastAddress =
          Span == 0 ? Memory[I].Address : Memory[I].Address + Span - 1;
      if (LastAddress >> kVirtualAddressBits != RegionIndex)
        return llvm::make_error<llvm::StringError>(
            "sbf: memory region crosses an aligned VM region boundary",
            llvm::inconvertibleErrorCode());
      if (!AlignedRegionIndices.insert(RegionIndex).second)
        return llvm::make_error<llvm::StringError>(
            "sbf: memory mapping has multiple regions at one aligned index",
            llvm::inconvertibleErrorCode());
    }
    for (size_t J = I + 1; J < Memory.size(); ++J)
      if (rangesOverlap(Memory[I], Memory[J]))
        return llvm::make_error<llvm::StringError>(
            "sbf: VM memory regions overlap", llvm::inconvertibleErrorCode());
  }
  return llvm::Error::success();
}

void appendProgramMemory(const SBFProgram &Program,
                         std::vector<MemoryRegion> &Memory) {
  for (const ProgramRegion &Region : Program.ExecutableImage.regions())
    if (Region.DataVisible && !Region.Bytes.empty())
      Memory.push_back({Region.Address, Region.Bytes, false, Region.Name});

  const Version TheVersion = Program.ExecutableImage.version();
  const bool HasStackFrameGaps = usesStackFrameGaps(TheVersion, Program.Config);
  const bool RequiresAlignedMapping =
      versionHasFeature(TheVersion, VersionFeature::AlignedMemoryMapping) ||
      Program.Config.AlignedMemoryMapping;
  if (HasStackFrameGaps && RequiresAlignedMapping) {
    MemoryRegion Stack;
    Stack.Address = kStackStart;
    Stack.Bytes.resize(stackSize(Program.Config));
    Stack.Writable = true;
    Stack.Name = "stack";
    Stack.VMGapSize = Program.Config.StackFrameSize;
    Memory.push_back(std::move(Stack));
  } else if (HasStackFrameGaps) {
    for (size_t Frame = 0; Frame < Program.Config.MaxCallDepth; ++Frame) {
      MemoryRegion Stack;
      Stack.Address = kStackStart + Frame * Program.Config.StackFrameSize *
                                        kStackFrameGapMultiplier;
      Stack.Bytes.resize(Program.Config.StackFrameSize);
      Stack.Writable = true;
      Stack.Name = "stack." + std::to_string(Frame);
      Memory.push_back(std::move(Stack));
    }
  } else {
    MemoryRegion Stack;
    Stack.Address = kStackStart;
    Stack.Bytes.resize(stackSize(Program.Config));
    Stack.Writable = true;
    Stack.Name = "stack";
    Memory.push_back(std::move(Stack));
  }
}

} // namespace interpreter_detail
} // namespace neverd::sbf
