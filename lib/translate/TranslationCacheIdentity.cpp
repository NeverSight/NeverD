//===- TranslationCacheIdentity.cpp - Stable translation hashing --------===//

#include "TranslationCacheIdentity.h"

#include "llvm/ADT/StringExtras.h"

#include <array>
#include <bit>
#include <limits>

namespace neverd::translate::detail {

uint64_t stableSize(std::size_t Value) {
  if constexpr (sizeof(std::size_t) > sizeof(uint64_t))
    if (Value > std::numeric_limits<uint64_t>::max())
      return std::numeric_limits<uint64_t>::max();
  return static_cast<uint64_t>(Value);
}

void StableHashWriter::addByte(uint8_t Value) {
  Hash.update(llvm::ArrayRef<uint8_t>(&Value, 1));
}

void StableHashWriter::addBool(bool Value) { addByte(Value ? 1 : 0); }

void StableHashWriter::addU16(uint16_t Value) {
  uint8_t Bytes[2];
  for (unsigned Index = 0; Index != 2; ++Index)
    Bytes[Index] = static_cast<uint8_t>(Value >> (Index * 8));
  Hash.update(Bytes);
}

void StableHashWriter::addU32(uint32_t Value) {
  uint8_t Bytes[4];
  for (unsigned Index = 0; Index != 4; ++Index)
    Bytes[Index] = static_cast<uint8_t>(Value >> (Index * 8));
  Hash.update(Bytes);
}

void StableHashWriter::addI32(int32_t Value) {
  addU32(std::bit_cast<uint32_t>(Value));
}

void StableHashWriter::addU64(uint64_t Value) {
  uint8_t Bytes[8];
  for (unsigned Index = 0; Index != 8; ++Index)
    Bytes[Index] = static_cast<uint8_t>(Value >> (Index * 8));
  Hash.update(Bytes);
}

void StableHashWriter::addDouble(double Value) {
  addU64(std::bit_cast<uint64_t>(Value));
}

void StableHashWriter::addString(llvm::StringRef Value) {
  addU64(Value.size());
  Hash.update(Value);
}

void StableHashWriter::addBytes(llvm::ArrayRef<uint8_t> Value) {
  addU64(Value.size());
  Hash.update(Value);
}

std::string StableHashWriter::finish(llvm::StringRef Prefix) {
  const std::array<uint8_t, 32> Digest = Hash.final();
  return (Prefix + llvm::toHex(Digest, /*LowerCase=*/true)).str();
}

void hashTranslationOptions(StableHashWriter &Hash,
                            const TranslationOptions &Options,
                            const ResolvedHostTarget &Target) {
  Hash.addByte(static_cast<uint8_t>(Options.Guest));
  Hash.addByte(static_cast<uint8_t>(Options.Mode));
  Hash.addByte(static_cast<uint8_t>(Options.UnsupportedInstructions));
  Hash.addByte(static_cast<uint8_t>(Options.Optimization));
  Hash.addByte(static_cast<uint8_t>(Options.LLVMLevel));
  Hash.addByte(static_cast<uint8_t>(Options.BlockCache));
  Hash.addByte(static_cast<uint8_t>(Options.CodeInvalidation));
  Hash.addByte(static_cast<uint8_t>(Options.DeterministicReplay));
  Hash.addBool(Options.VerifyGeneratedIR);
  Hash.addBool(Options.PreserveExceptionState);
  Hash.addU32(static_cast<uint32_t>(Options.RequiredCapabilities));
  Hash.addU64(Options.InstructionBudget);
  Hash.addU64(Options.BlockBudget);
  Hash.addU64(Options.GeneratedCodeByteBudget);
  Hash.addString(Target.cacheKey());
}

void hashSemanticPolicy(StableHashWriter &Hash,
                        const TranslationSemanticPolicyV1 &Policy) {
  const SymSimplifyOptions &Options = Policy.Simplify;
  Hash.addU64(stableSize(Options.MinMeasuredNodes));
  Hash.addU64(stableSize(Options.MinInstructionsSaved));
  Hash.addU32(Options.MBA.MaxAtoms);
  Hash.addU32(Options.MBA.MaxSynthesisAtoms);
  Hash.addU32(Options.MBA.MaxOptimalSynthesisAtoms);
  Hash.addU32(Options.MBA.VerifySamples);
  Hash.addBool(Options.MBA.AllowGrowth);
  Hash.addU64(stableSize(Options.MBA.MaxWork));
  Hash.addU64(stableSize(Options.MBA.MaxTableBytes));
  Hash.addU64(stableSize(Options.Synthesis.MaxCost));
  Hash.addU64(stableSize(Options.Synthesis.MaxSamples));
  Hash.addU32(Options.Synthesis.VerifySamples);
  Hash.addBool(Options.Synthesis.UseStochasticFallback);
  Hash.addU64(stableSize(Options.Synthesis.MaxWork));
  Hash.addU32(Options.Synthesis.MaxLeaves);
  Hash.addU32(Options.Synthesis.MaxConstants);
  Hash.addU32(Options.Synthesis.StochasticSlots);
  Hash.addU32(Options.Synthesis.StochasticRestarts);
  Hash.addU64(stableSize(Options.Synthesis.StochasticIterations));
  Hash.addBool(Options.Synthesis.AllowVariableShifts);
  Hash.addBool(Options.Synthesis.AllowGrowth);
  Hash.addU64(Options.Synthesis.Seed);
  Hash.addDouble(Options.Solver.Sat.VarDecay);
  Hash.addDouble(Options.Solver.Sat.ClauseDecay);
  Hash.addU64(Options.Solver.Sat.RestartInterval);
  Hash.addDouble(Options.Solver.Sat.LearnedFraction);
  Hash.addDouble(Options.Solver.Sat.LearnedGrowth);
  Hash.addU64(Options.Solver.Sat.MaxConflicts);
  Hash.addU64(Options.Solver.Sat.MaxPropagations);
  Hash.addU64(Options.Solver.Sat.MaxWatchVisits);
  Hash.addBool(Options.Solver.Sat.MinimizeLearned);
  Hash.addBool(Options.Solver.Sat.PhaseSaving);
  Hash.addBool(Options.Solver.Sat.DefaultPhase);
  Hash.addU32(Options.Solver.Blast.MaxWidth);
  Hash.addU64(stableSize(Options.Solver.Blast.MaxGates));
  Hash.addBool(Options.Solver.BuildModel);
  Hash.addBool(Options.EnableSynthesis);
  Hash.addByte(static_cast<uint8_t>(Options.Provider));
  Hash.addU32(Policy.MaxRounds);
}

void hashMemorySlots(StableHashWriter &Hash,
                     llvm::ArrayRef<TranslationIRMemorySlot> Slots) {
  Hash.addU64(Slots.size());
  for (const TranslationIRMemorySlot &Slot : Slots) {
    Hash.addByte(static_cast<uint8_t>(Slot.Region));
    Hash.addU64(Slot.Offset);
    Hash.addU64(Slot.Size);
    Hash.addByte(static_cast<uint8_t>(Slot.Access));
    Hash.addU32(Slot.Alignment);
  }
}

namespace {

void hashVariable(StableHashWriter &Hash, const NdVar &Variable) {
  Hash.addByte(static_cast<uint8_t>(Variable.Space));
  Hash.addU64(Variable.Offset);
  Hash.addU16(Variable.Size);
  Hash.addByte(static_cast<uint8_t>(Variable.Provenance));
  Hash.addU64(Variable.AddressOwnerVA);
}

} // namespace

void hashTranslationBlockDescriptor(
    StableHashWriter &Hash, const TranslationBlockDescriptorV1 &Descriptor) {
  const TranslationBlockDescriptorHeaderV1 &Header = Descriptor.Header;
  Hash.addU32(Header.Magic);
  Hash.addU16(Header.Version);
  Hash.addU16(Header.Size);
  Hash.addU32(static_cast<uint32_t>(Header.Terminator));
  Hash.addU32(static_cast<uint32_t>(Header.Flags));
  Hash.addU64(Header.EntryPC);
  Hash.addU64(Header.FallthroughPC);
  Hash.addU64(Header.StaticTargetPC);
  Hash.addU64(Header.GuestInstructionCount);
  Hash.addU64(Header.GuestByteCount);
  Hash.addU64(Header.ReturnImmediate);
  Hash.addBytes(Descriptor.Bytes);

  Hash.addU64(Descriptor.InstructionBoundaries.size());
  for (const LowInstructionBoundary &Boundary :
       Descriptor.InstructionBoundaries) {
    Hash.addU64(Boundary.Address);
    Hash.addU16(Boundary.Size);
    Hash.addU64(Boundary.FirstOp);
    Hash.addU64(Boundary.OpCount);
    Hash.addByte(static_cast<uint8_t>(Boundary.Mode));
    Hash.addByte(static_cast<uint8_t>(Boundary.Control));
    Hash.addU16(static_cast<uint16_t>(Boundary.ControlFlags));
    Hash.addByte(static_cast<uint8_t>(Boundary.TargetMode));
    Hash.addBool(Boundary.Immediate.has_value());
    if (Boundary.Immediate)
      Hash.addU64(*Boundary.Immediate);
  }

  static_assert(sizeof(int) == sizeof(int32_t));
  Hash.addU64(Descriptor.Ops.size());
  for (const LowOp &Op : Descriptor.Ops) {
    Hash.addByte(static_cast<uint8_t>(Op.Opcode));
    Hash.addByte(static_cast<uint8_t>(Op.MemoryOrdering));
    Hash.addByte(static_cast<uint8_t>(Op.MemoryAddressSpace));
    hashVariable(Hash, Op.Output);
    for (const NdVar &Input : Op.Inputs)
      hashVariable(Hash, Input);
    Hash.addByte(Op.NumInputs);
    Hash.addU64(Op.Addr);
    Hash.addI32(static_cast<int32_t>(Op.Seq));
  }

  Hash.addU64(Descriptor.GenerationBindings.size());
  for (const GuestExecutableRangeBinding &Binding :
       Descriptor.GenerationBindings) {
    Hash.addU64(Binding.Address);
    Hash.addU64(Binding.Size);
    Hash.addU64(Binding.Generation);
  }
}

} // namespace neverd::translate::detail
