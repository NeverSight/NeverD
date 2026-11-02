//===- AndroidArguments.h - Bionic AAPCS64 scalar varargs -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ANDROIDARGUMENTS_H
#define NEVERD_EMULATION_ANDROIDARGUMENTS_H

#include "AndroidInternal.h"

#include "llvm/ADT/SmallVector.h"

namespace neverd::emulation::android_model {
/// Scalar variadic arguments for the Android LP64 ABI. The caller's va_list
/// and its save areas remain guest-owned; this cursor never writes them.
class Bionic::ArgumentReader {
public:
  ArgumentReader(Bionic &Model, const NativeCallEvent &Call, unsigned First,
                 uint64_t &Remaining)
      : Model(Model), Call(Call), Remaining(Remaining), NextRegister(First) {}

  llvm::Error initializeVAList(uint64_t Address);
  llvm::Expected<uint64_t> next(unsigned Size = 8);
  // A variadic float is promoted to double and consumes an independent FP
  // register/16-byte save slot, or an eight-byte shared overflow stack slot.
  llvm::Expected<uint64_t> nextFloating();
  bool overlaps(uint64_t Address, unsigned Size) const;

private:
  Bionic &Model;
  const NativeCallEvent &Call;
  uint64_t &Remaining;
  unsigned NextRegister;
  unsigned NextVector = 0;
  bool HasVAList = false, StackLoaded = false;
  uint64_t Stack = 0, GRTop = 0, VRTop = 0;
  int32_t GROffset = 0, VROffset = 0;
  struct ReadRange {
    uint64_t Address;
    unsigned Size;
  };
  llvm::SmallVector<ReadRange, 8> Reads;

  llvm::Error read(uint64_t Address, llvm::MutableArrayRef<uint8_t> Bytes);
  llvm::Expected<uint64_t> stackArgument();
};
} // namespace neverd::emulation::android_model
#endif
