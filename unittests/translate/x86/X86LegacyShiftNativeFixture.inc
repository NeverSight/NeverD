//===- X86LegacyShiftNativeFixture.inc - Undefined bit-motion observer ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Separately observe undefined state with the original operand form.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYSHIFTNATIVEFIXTURE_INC
#define NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYSHIFTNATIVEFIXTURE_INC

#include "X86LegacyScalarNativeObserverFixture.inc"

class X86LegacyShiftNativeFixture
    : public X86LegacyScalarNativeObserverFixture {
protected:
  // Both shift fixtures use [BX]/[EBX]. The nonflat scalar fixture stores
  // Value at DS.base+offset, while SS retains its zero base and untouched RAM.
  static uint32_t memoryInputValue(unsigned Case, unsigned Prefix,
                                   uint32_t Value) {
    return Case == 3 && Prefix == 0x36 ? 0 : Value;
  }

  // SysV EDI/ESI/EDX/RCX carry destination/source/count/flags. Capture flags
  // before packing the result and restore the caller's original flag image.
  // The caller must consume only architecturally undefined slices; this does
  // not supply any defined bit, KVM result, RAM or exception to the executor.
  void observe(llvm::ArrayRef<uint8_t> Instruction, uint32_t Destination,
               uint32_t Source, unsigned Count, uint32_t Flags, bool Memory,
               uint64_t &Observed) {
    std::vector<uint8_t> Bytes{0x9c};
    if (Memory)
      Bytes.push_back(0x57); // Independent scratch operand at [RSP].
    else
      Bytes.insert(Bytes.end(), {0x89, 0xf8});
    Bytes.insert(Bytes.end(), {0x51, 0x9d, 0x89, 0xd1});
    Bytes.insert(Bytes.end(), Instruction.begin(), Instruction.end());
    if (Memory)
      // Capture the operand and discard its slot without disturbing flags.
      Bytes.insert(Bytes.end(), {0x8b, 0x04, 0x24, 0x48, 0x8d, 0x64, 0x24, 8});
    Bytes.insert(Bytes.end(), {0x9c, 0x5a, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09,
                               0xd0, 0x9d, 0xc3});
    observeCode(Bytes, Destination, Source, Count, Flags, Observed);
  }
};

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYSHIFTNATIVEFIXTURE_INC
