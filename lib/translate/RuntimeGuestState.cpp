//===- RuntimeGuestState.cpp - Fixed translated guest state ABI ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/translate/RuntimeGuestState.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/Errc.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <utility>

namespace neverd::translate {
namespace {

constexpr TranslationIRMemoryAccess ReadWrite =
    TranslationIRMemoryAccess::Read | TranslationIRMemoryAccess::Write;

constexpr TranslationIRMemorySlot stateSlot(uint64_t Offset, uint64_t Size,
                                            TranslationIRMemoryAccess Access,
                                            uint32_t Alignment) {
  return {TranslationIRMemoryRegion::State, Offset, Size, Access, Alignment};
}

constexpr TranslationIRMemorySlot gprSlot(size_t Index) {
  return stateSlot(offsetof(RuntimeGuestStateX86_64V1, GPR) +
                       Index * sizeof(uint64_t),
                   sizeof(uint64_t), ReadWrite, alignof(uint64_t));
}

constexpr uint32_t stateFieldAlignment(uint64_t Offset) {
  // TranslationIRMemorySlot records the address guarantee at the field, not
  // the C++ field type's natural alignment.  The fixed ABI object's
  // alignas(16) lets byte-sized flags at aligned offsets safely retain
  // stronger optimizer-inferred accesses.
  return static_cast<uint32_t>(
      std::gcd<uint64_t>(alignof(RuntimeGuestStateX86_64V1), Offset));
}

constexpr TranslationIRMemorySlot X86_64MemorySlotsV1[] = {
    gprSlot(0),
    gprSlot(1),
    gprSlot(2),
    gprSlot(3),
    gprSlot(4),
    gprSlot(5),
    gprSlot(6),
    gprSlot(7),
    gprSlot(8),
    gprSlot(9),
    gprSlot(10),
    gprSlot(11),
    gprSlot(12),
    gprSlot(13),
    gprSlot(14),
    gprSlot(15),
    stateSlot(offsetof(RuntimeGuestStateX86_64V1, RIP), sizeof(uint64_t),
              ReadWrite, alignof(uint64_t)),
    stateSlot(offsetof(RuntimeGuestStateX86_64V1, RFlagsBase), sizeof(uint64_t),
              TranslationIRMemoryAccess::Read, alignof(uint64_t)),
    stateSlot(offsetof(RuntimeGuestStateX86_64V1, CF), sizeof(uint8_t),
              ReadWrite,
              stateFieldAlignment(offsetof(RuntimeGuestStateX86_64V1, CF))),
    stateSlot(offsetof(RuntimeGuestStateX86_64V1, PF), sizeof(uint8_t),
              ReadWrite,
              stateFieldAlignment(offsetof(RuntimeGuestStateX86_64V1, PF))),
    stateSlot(offsetof(RuntimeGuestStateX86_64V1, AF), sizeof(uint8_t),
              ReadWrite,
              stateFieldAlignment(offsetof(RuntimeGuestStateX86_64V1, AF))),
    stateSlot(offsetof(RuntimeGuestStateX86_64V1, ZF), sizeof(uint8_t),
              ReadWrite,
              stateFieldAlignment(offsetof(RuntimeGuestStateX86_64V1, ZF))),
    stateSlot(offsetof(RuntimeGuestStateX86_64V1, SF), sizeof(uint8_t),
              ReadWrite,
              stateFieldAlignment(offsetof(RuntimeGuestStateX86_64V1, SF))),
    stateSlot(offsetof(RuntimeGuestStateX86_64V1, DF), sizeof(uint8_t),
              ReadWrite,
              stateFieldAlignment(offsetof(RuntimeGuestStateX86_64V1, DF))),
    stateSlot(offsetof(RuntimeGuestStateX86_64V1, OF), sizeof(uint8_t),
              ReadWrite,
              stateFieldAlignment(offsetof(RuntimeGuestStateX86_64V1, OF))),
};

static_assert(std::size(X86_64MemorySlotsV1) == 25);

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                 Message.str().c_str());
}

uint8_t flagValue(uint64_t Packed, unsigned Bit) {
  return static_cast<uint8_t>((Packed >> Bit) & 1);
}

uint64_t packedRFlags(const RuntimeGuestStateX86_64V1 &State) {
  return State.RFlagsBase | (uint64_t{State.CF} << 0) |
         (uint64_t{State.PF} << 2) | (uint64_t{State.AF} << 4) |
         (uint64_t{State.ZF} << 6) | (uint64_t{State.SF} << 7) |
         (uint64_t{State.DF} << 10) | (uint64_t{State.OF} << 11);
}

} // namespace

llvm::ArrayRef<TranslationIRMemorySlot> runtimeGuestStateX86_64MemorySlotsV1() {
  return X86_64MemorySlotsV1;
}

llvm::Error
validateRuntimeGuestStateX86_64V1(const RuntimeGuestStateX86_64V1 &State) {
  if (State.Magic != kRuntimeGuestStateX86_64MagicV1)
    return invalid("invalid x86-64 runtime-state magic");
  if (State.Version != kRuntimeGuestStateX86_64VersionV1)
    return invalid("unsupported x86-64 runtime-state version");
  if (State.Size != kRuntimeGuestStateX86_64SizeV1)
    return invalid("invalid x86-64 runtime-state size");
  if (State.Reserved0 != 0)
    return invalid("x86-64 runtime-state reserved byte is non-zero");
  if ((State.RFlagsBase & kRuntimeX86_64SplitRFlagsMaskV1) != 0)
    return invalid(
        "x86-64 runtime-state RFLAGS representation is not canonical");
  if (State.CF > 1 || State.PF > 1 || State.AF > 1 || State.ZF > 1 ||
      State.SF > 1 || State.DF > 1 || State.OF > 1)
    return invalid("x86-64 runtime-state flag is not boolean");
  return llvm::Error::success();
}

llvm::Expected<RuntimeGuestStateX86_64V1>
createRuntimeGuestStateX86_64V1(const GuestState &State) {
  if (llvm::Error Error = validateGuestState(State))
    return std::move(Error);
  if (State.Architecture != GuestArchitecture::X86_64)
    return invalid("runtime x86-64 state requires an x86-64 guest");

  RuntimeGuestStateX86_64V1 RuntimeState;
  for (RegisterID ID = 0; ID != kRuntimeX86_64GPRCountV1; ++ID) {
    const GuestRegisterValue *Register = findRegisterValue(State, ID);
    if (Register == nullptr)
      return invalid(llvm::Twine("x86-64 runtime state is missing GPR ") +
                     llvm::Twine(ID));
    RuntimeState.GPR[ID] = Register->Value.getZExtValue();
  }

  const GuestRegisterValue *RIP = findRegisterValue(State, 16);
  const GuestRegisterValue *RFlags = findRegisterValue(State, 17);
  if (RIP == nullptr || RFlags == nullptr)
    return invalid("x86-64 runtime state is missing RIP or RFLAGS");
  RuntimeState.RIP = RIP->Value.getZExtValue();
  const uint64_t PackedFlags = RFlags->Value.getZExtValue();
  RuntimeState.RFlagsBase = PackedFlags & ~kRuntimeX86_64SplitRFlagsMaskV1;
  RuntimeState.CF = flagValue(PackedFlags, 0);
  RuntimeState.PF = flagValue(PackedFlags, 2);
  RuntimeState.AF = flagValue(PackedFlags, 4);
  RuntimeState.ZF = flagValue(PackedFlags, 6);
  RuntimeState.SF = flagValue(PackedFlags, 7);
  RuntimeState.DF = flagValue(PackedFlags, 10);
  RuntimeState.OF = flagValue(PackedFlags, 11);
  return RuntimeState;
}

llvm::Error
applyRuntimeGuestStateX86_64V1(const RuntimeGuestStateX86_64V1 &RuntimeState,
                               GuestState &State) {
  if (llvm::Error Error = validateRuntimeGuestStateX86_64V1(RuntimeState))
    return Error;
  if (llvm::Error Error = validateGuestState(State))
    return Error;
  if (State.Architecture != GuestArchitecture::X86_64)
    return invalid("runtime x86-64 state requires an x86-64 guest");

  std::array<GuestRegisterValue *, 18> Registers{};
  for (RegisterID ID = 0; ID != Registers.size(); ++ID) {
    Registers[ID] = findRegisterValue(State, ID);
    if (Registers[ID] == nullptr)
      return invalid(llvm::Twine("x86-64 logical state is missing register ") +
                     llvm::Twine(ID));
  }

  // Every operation that can return an Error has completed before mutation.
  // The fixed 64-bit APInts do not alter the register vector or any non-integer
  // logical state, so applying a runtime snapshot is O(register-count).
  for (RegisterID ID = 0; ID != kRuntimeX86_64GPRCountV1; ++ID)
    Registers[ID]->Value = llvm::APInt(64, RuntimeState.GPR[ID]);
  Registers[16]->Value = llvm::APInt(64, RuntimeState.RIP);
  Registers[17]->Value = llvm::APInt(64, packedRFlags(RuntimeState));
  return llvm::Error::success();
}

llvm::Error validateBlockExitV1(const BlockExitV1 &Exit) {
  if (Exit.Magic != kBlockExitMagicV1)
    return invalid("invalid block-exit ABI magic");
  if (Exit.Version != kBlockExitVersionV1)
    return invalid("unsupported block-exit ABI version");
  if (Exit.Size != kBlockExitSizeV1)
    return invalid("invalid block-exit ABI size");
  if (Exit.Reserved0 != 0)
    return invalid("block-exit ABI reserved field is non-zero");

  switch (Exit.Kind) {
  case BlockExitKindV1::Continue:
    if (Exit.TargetPC != 0)
      return invalid("continue block exit carries a branch target");
    if (Exit.Detail != 0)
      return invalid("continue block exit carries a detail code");
    return llvm::Error::success();

  case BlockExitKindV1::DirectBranch:
  case BlockExitKindV1::IndirectBranch:
    if (Exit.NextPC != 0)
      return invalid("branch block exit carries a next PC");
    if (Exit.Detail != 0)
      return invalid("branch block exit carries a detail code");
    return llvm::Error::success();

  case BlockExitKindV1::Call:
    if (Exit.Detail != 0)
      return invalid("call block exit carries a detail code");
    return llvm::Error::success();

  case BlockExitKindV1::Return:
    if (Exit.NextPC != 0)
      return invalid("return block exit carries a next PC");
    if (Exit.Detail != 0)
      return invalid("return block exit carries a detail code");
    return llvm::Error::success();

  case BlockExitKindV1::Unsupported:
    if (Exit.TargetPC != 0)
      return invalid("unsupported block exit carries a target PC");
    return llvm::Error::success();

  case BlockExitKindV1::Syscall:
    if (Exit.TargetPC != 0)
      return invalid("syscall block exit carries a target PC");
    return llvm::Error::success();

  case BlockExitKindV1::Trap:
    if (Exit.TargetPC != 0)
      return invalid("trap block exit carries a target PC");
    return llvm::Error::success();
  }
  return invalid("unknown block-exit ABI kind");
}

} // namespace neverd::translate
