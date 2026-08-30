//===- Intrinsics.h - Intrinsic function definitions --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the Intrinsic enumeration for INTRINSIC operations and
/// provides name/metadata query functions for each intrinsic.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_INTRINSICS_H
#define NEVERD_IR_INTRINSICS_INTRINSICS_H

#include "neverd/Common.h"

namespace neverd {

/// Intrinsic IDs for INTRINSIC operations.
///
/// Each architecture gets a 5000-slot range so entries never collide:
///   Common   :     0 -   999
///   x86/x64  :  1000 -  5999
///   AArch64  :  6000 - 10999
///   ARM32    : 11000 - 15999
enum class Intrinsic : uint16_t {
  None = 0,

  Syscall,

#include "neverd/ir/intrinsics/intrinsics_aarch64.inc"
#include "neverd/ir/intrinsics/intrinsics_arm.inc"
#include "neverd/ir/intrinsics/intrinsics_x86.inc"

  _Count = 16000
};

/// Intrinsics in this list own a memory operand whose address space is carried
/// by MedOp::MemoryAddressSpace.  All other intrinsics must reject a nondefault
/// value so a new or unsupported opcode cannot accidentally drop FS/GS
/// semantics in a backend fallback.
constexpr bool intrinsicSupportsMemoryAddressSpace(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::Movsb:
  case Intrinsic::Movsw:
  case Intrinsic::Movsd:
  case Intrinsic::Movsq:
  case Intrinsic::Lodsb:
  case Intrinsic::Lodsw:
  case Intrinsic::Lodsd:
  case Intrinsic::Lodsq:
  case Intrinsic::Cmpsb:
  case Intrinsic::Cmpsw:
  case Intrinsic::Cmpsd_str:
  case Intrinsic::Cmpsq:
  case Intrinsic::Outsb:
  case Intrinsic::Outsw:
  case Intrinsic::Outsd:
  case Intrinsic::MaskedLoadD:
  case Intrinsic::MaskedLoadQ:
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
  case Intrinsic::Fxsave:
  case Intrinsic::Fxrstor:
  case Intrinsic::Fxsave64Mem:
  case Intrinsic::Fxrstor64Mem:
  case Intrinsic::Xsave:
  case Intrinsic::Xsavec:
  case Intrinsic::Xsaves:
  case Intrinsic::Xsaveopt:
  case Intrinsic::Xrstor:
  case Intrinsic::Xrstors:
  case Intrinsic::Xsave64:
  case Intrinsic::Xsavec64:
  case Intrinsic::Xsaves64:
  case Intrinsic::Xsaveopt64:
  case Intrinsic::Xrstor64:
  case Intrinsic::Xrstors64:
  case Intrinsic::X87Fldenv:
  case Intrinsic::X87Fnstenv:
  case Intrinsic::X87Frstor:
  case Intrinsic::X87Fnsave:
  case Intrinsic::Lgdt:
  case Intrinsic::Lidt:
  case Intrinsic::Sgdt:
  case Intrinsic::Sidt:
  case Intrinsic::Invlpg:
  case Intrinsic::Lldt:
  case Intrinsic::Ltr:
  case Intrinsic::Lmsw:
  case Intrinsic::Sldt:
  case Intrinsic::Str:
  case Intrinsic::Smsw:
    return true;
  default:
    return false;
  }
}

constexpr bool isX86StringIntrinsic(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::Movsb:
  case Intrinsic::Movsw:
  case Intrinsic::Movsd:
  case Intrinsic::Movsq:
  case Intrinsic::Stosb:
  case Intrinsic::Stosw:
  case Intrinsic::Stosd:
  case Intrinsic::Stosq:
  case Intrinsic::Lodsb:
  case Intrinsic::Lodsw:
  case Intrinsic::Lodsd:
  case Intrinsic::Lodsq:
  case Intrinsic::Cmpsb:
  case Intrinsic::Cmpsw:
  case Intrinsic::Cmpsd_str:
  case Intrinsic::Cmpsq:
  case Intrinsic::Scasb:
  case Intrinsic::Scasw:
  case Intrinsic::Scasd:
  case Intrinsic::Scasq:
  case Intrinsic::Outsb:
  case Intrinsic::Outsw:
  case Intrinsic::Outsd:
  case Intrinsic::Insb:
  case Intrinsic::Insw:
  case Intrinsic::Insd:
    return true;
  default:
    return false;
  }
}

/// NumInputs includes the constant intrinsic ID at index 0.  Every string
/// family carries its complete architectural register state, even where the
/// memory segment is fixed to ES/default rather than selectable with FS/GS.
constexpr bool intrinsicStringShapeIsValid(Intrinsic Id, uint8_t NumInputs,
                                           uint16_t OutputSize,
                                           uint16_t AddressSize) {
  const bool ValidAddressSize =
      AddressSize == 2 || AddressSize == 4 || AddressSize == 8;
  switch (Id) {
  case Intrinsic::Movsb:
  case Intrinsic::Movsw:
  case Intrinsic::Movsd:
  case Intrinsic::Movsq:
  case Intrinsic::Stosb:
  case Intrinsic::Stosw:
  case Intrinsic::Stosd:
  case Intrinsic::Stosq:
  case Intrinsic::Lodsb:
  case Intrinsic::Lodsw:
  case Intrinsic::Lodsd:
  case Intrinsic::Lodsq:
    return NumInputs >= 5 && OutputSize != 0 && ValidAddressSize;
  case Intrinsic::Cmpsb:
  case Intrinsic::Cmpsw:
  case Intrinsic::Cmpsd_str:
  case Intrinsic::Cmpsq:
  case Intrinsic::Scasb:
  case Intrinsic::Scasw:
  case Intrinsic::Scasd:
  case Intrinsic::Scasq:
    return NumInputs >= 6 && OutputSize != 0 && ValidAddressSize;
  case Intrinsic::Outsb:
  case Intrinsic::Outsw:
  case Intrinsic::Outsd:
  case Intrinsic::Insb:
  case Intrinsic::Insw:
  case Intrinsic::Insd:
    return NumInputs >= 5 && OutputSize == 0 && ValidAddressSize;
  default:
    return false;
  }
}

/// Validate the operand/output shape required by the address-space-aware
/// implementation.  Keeping this next to the whitelist makes it impossible
/// for malformed segmented memory IR to reach a generic backend fallback.
constexpr bool intrinsicMemoryAddressSpaceShapeIsValid(Intrinsic Id,
                                                       uint8_t NumInputs,
                                                       uint16_t OutputSize,
                                                       uint16_t AddressSize,
                                                       uint16_t MaskSize,
                                                       uint16_t DataSize) {
  switch (Id) {
  case Intrinsic::Movsb:
  case Intrinsic::Movsw:
  case Intrinsic::Movsd:
  case Intrinsic::Movsq:
  case Intrinsic::Lodsb:
  case Intrinsic::Lodsw:
  case Intrinsic::Lodsd:
  case Intrinsic::Lodsq:
  case Intrinsic::Cmpsb:
  case Intrinsic::Cmpsw:
  case Intrinsic::Cmpsd_str:
  case Intrinsic::Cmpsq:
  case Intrinsic::Outsb:
  case Intrinsic::Outsw:
  case Intrinsic::Outsd:
    return intrinsicStringShapeIsValid(Id, NumInputs, OutputSize, AddressSize);
  case Intrinsic::MaskedLoadD:
  case Intrinsic::MaskedLoadQ:
    return NumInputs >= 3 && AddressSize == 8 &&
           (OutputSize == 16 || OutputSize == 32) &&
           MaskSize == OutputSize;
  case Intrinsic::MaskedStoreD:
  case Intrinsic::MaskedStoreQ:
    return NumInputs >= 4 && OutputSize == 0 && AddressSize == 8 &&
           (MaskSize == 16 || MaskSize == 32) && DataSize == MaskSize;
  case Intrinsic::MaskedStoreB:
    return NumInputs >= 4 && OutputSize == 0 && AddressSize == 8 &&
           (MaskSize == 8 || MaskSize == 16) && DataSize == MaskSize;
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
  case Intrinsic::Fxsave:
  case Intrinsic::Fxrstor:
  case Intrinsic::Fxsave64Mem:
  case Intrinsic::Fxrstor64Mem:
  case Intrinsic::X87Fldenv:
  case Intrinsic::X87Fnstenv:
  case Intrinsic::X87Frstor:
  case Intrinsic::X87Fnsave:
  case Intrinsic::Lgdt:
  case Intrinsic::Lidt:
  case Intrinsic::Sgdt:
  case Intrinsic::Sidt:
  case Intrinsic::Invlpg:
  case Intrinsic::Lldt:
  case Intrinsic::Ltr:
  case Intrinsic::Lmsw:
  case Intrinsic::Sldt:
  case Intrinsic::Str:
  case Intrinsic::Smsw:
    return NumInputs >= 2 && OutputSize == 0 && AddressSize == 8;
  case Intrinsic::Xsave:
  case Intrinsic::Xsavec:
  case Intrinsic::Xsaves:
  case Intrinsic::Xsaveopt:
  case Intrinsic::Xrstor:
  case Intrinsic::Xrstors:
  case Intrinsic::Xsave64:
  case Intrinsic::Xsavec64:
  case Intrinsic::Xsaves64:
  case Intrinsic::Xsaveopt64:
  case Intrinsic::Xrstor64:
  case Intrinsic::Xrstors64:
    return NumInputs >= 4 && OutputSize == 0 && AddressSize == 8;
  default:
    return false;
  }
}

/// Some x86 system instructions share an intrinsic ID between r/m16 memory
/// and register encodings.  A default address space can therefore denote a
/// genuine register form; an explicit FS/GS address space never can.
constexpr bool intrinsicDefaultRegisterShapeIsValid(Intrinsic Id,
                                                     uint8_t NumInputs,
                                                     uint16_t OutputSize,
                                                     uint16_t OperandSize) {
  switch (Id) {
  case Intrinsic::Lldt:
  case Intrinsic::Ltr:
  case Intrinsic::Lmsw:
    return NumInputs >= 2 && OutputSize == 0 && OperandSize == 2;
  case Intrinsic::Sldt:
  case Intrinsic::Str:
  case Intrinsic::Smsw:
    return NumInputs == 1 &&
           (OutputSize == 2 || OutputSize == 4 || OutputSize == 8);
  default:
    return false;
  }
}

const char *intrinsicName(Intrinsic Id);
const char *intrinsicCName(Intrinsic Id);
const char *intrinsicAsmMnemonic(Intrinsic Id);
const char *llvmIntrinsicToCName(const char *LLVMName);
Intrinsic intrinsicFromName(const char *Name);
bool isSideeffectIntrinsic(Intrinsic Id);
uint8_t intrinsicOutputCount(Intrinsic Id);

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_INTRINSICS_H
