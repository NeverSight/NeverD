//===- X86AtomicMemory.h - Atomic memory semantic contracts ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared operand, ordering and address-provenance admission for x86 atomics.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86ATOMICMEMORY_H
#define NEVERD_IR_INTRINSICS_X86_X86ATOMICMEMORY_H

#include "neverd/ir/intrinsics/Intrinsics.h"

namespace neverd {

/// An optional final byte preserves long-mode SP/BP provenance or an i386
/// CS/ES/SS offset after effective-address arithmetic. Absence retains the
/// existing DS/FS/GS form. Stack and segment controls are mutually exclusive.
inline constexpr uint8_t X86AtomicStackReference = 1;

constexpr unsigned x86AtomicSegmentPrefix(uint64_t Control) {
  return Control == 0x26 || Control == 0x2e || Control == 0x36 ? Control : 0;
}

constexpr unsigned x86AtomicAddressControlIndex(Intrinsic Id) {
  return isX86CmpccXaddIntrinsic(Id) ? 5 : 3;
}

/// Representation-neutral APX atomic operand facts.  LowIR and MedIR use
/// different value kinds, so each caller classifies its own variables and
/// passes only the semantic facts which the shared contract owns.  Unknown is
/// accepted only where the containing IR has no target field; every known
/// target must be x86-64, except the 32-bit legacy RAO form.
struct ApxAtomicIntrinsicShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  uint8_t NumInputs = 0;
  bool IntrinsicIdIsConst = false;
  uint16_t IntrinsicIdSize = 0;
  bool OutputIsWritableScalar = false;
  uint16_t OutputSize = 0;
  bool AddressIsScalar = false;
  uint16_t AddressSize = 0;
  bool SourceIsScalar = false;
  uint16_t SourceSize = 0;
  bool CompareIsScalar = false;
  uint16_t CompareSize = 0;
  bool ConditionIsConst = false;
  uint64_t Condition = 0;
  uint16_t ConditionSize = 0;
  bool AddressControlIsConst = false;
  uint64_t AddressControl = 0;
  uint16_t AddressControlSize = 0;

  constexpr bool stackReference() const {
    return AddressControl == X86AtomicStackReference;
  }
  constexpr unsigned segmentPrefix() const {
    return x86AtomicSegmentPrefix(AddressControl);
  }
};

constexpr bool
intrinsicApxAtomicShapeIsValid(Intrinsic Id,
                               const ApxAtomicIntrinsicShape &Shape) {
  const bool I386 = isX86LegacyAtomicIntrinsic(Id) &&
                    !isX86CmpccXaddIntrinsic(Id) && Shape.SourceSize == 4;
  if (!isX86AtomicIntrinsic(Id) ||
      (Shape.TargetArch != Arch::Unknown && Shape.TargetArch != Arch::X64 &&
       !(Shape.TargetArch == Arch::X86 && I386)) ||
      !Shape.IntrinsicIdIsConst || Shape.IntrinsicIdSize != 2 ||
      !Shape.AddressIsScalar || Shape.AddressSize != 8 ||
      !isKnownMemoryAddressSpace(Shape.MemoryAddressSpace))
    return false;

  const auto OrdinaryInputs = x86AtomicAddressControlIndex(Id);
  if (Shape.NumInputs != OrdinaryInputs) {
    if (Shape.NumInputs != OrdinaryInputs + 1 || !Shape.AddressControlIsConst ||
        Shape.AddressControlSize != 1 ||
        Shape.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    if (Shape.stackReference()) {
      if (Shape.TargetArch == Arch::X86)
        return false;
    } else if (!Shape.segmentPrefix() || !I386 || Shape.TargetArch == Arch::X64)
      return false;
  }

  if (!isX86CmpccXaddIntrinsic(Id))
    return Shape.OutputSize == 0 && Shape.SourceIsScalar &&
           (Shape.SourceSize == 4 || Shape.SourceSize == 8) &&
           Shape.MemoryOrdering == NdMemoryOrdering::Relaxed;

  return Shape.OutputIsWritableScalar &&
         (Shape.OutputSize == 4 || Shape.OutputSize == 8) &&
         Shape.SourceIsScalar && Shape.SourceSize == Shape.OutputSize &&
         Shape.CompareIsScalar && Shape.CompareSize == Shape.OutputSize &&
         Shape.ConditionIsConst && Shape.ConditionSize == 1 &&
         Shape.Condition < 16 &&
         Shape.MemoryOrdering == NdMemoryOrdering::SequentiallyConsistent;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86ATOMICMEMORY_H
