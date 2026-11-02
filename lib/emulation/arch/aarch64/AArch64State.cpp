//===- AArch64State.cpp - Atomic architectural register capture -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AArch64State.h"

#include "../../core/ExecutionDiagnostics.h"

#include "llvm/Support/MathExtras.h"

#include <iterator>

namespace neverd::emulation {
namespace {
constexpr AArch64Register ScalarRegisters[] = {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  NEVERD_SCALAR_##Arch(Name)
#define NEVERD_SCALAR_X64(Name)
#define NEVERD_SCALAR_AArch64(Name) AArch64Register::Name,
#include "neverd/emulation/Registers.def"
#undef NEVERD_SCALAR_AArch64
#undef NEVERD_SCALAR_X64
#undef NEVERD_SCALAR_REGISTER
};
constexpr unsigned VectorRegisters[] = {
#define NEVERD_VECTOR_REGISTER(Arch, Index, Backend) NEVERD_VECTOR_##Arch(Index)
#define NEVERD_VECTOR_X64(Index)
#define NEVERD_VECTOR_AArch64(Index) Index,
#include "neverd/emulation/Registers.def"
#undef NEVERD_VECTOR_AArch64
#undef NEVERD_VECTOR_X64
#undef NEVERD_VECTOR_REGISTER
};
static_assert([] {
  for (unsigned Index = 0; Index < std::size(ScalarRegisters); ++Index)
    if (unsigned(ScalarRegisters[Index]) != Index)
      return false;
  for (unsigned Index = 0; Index < std::size(VectorRegisters); ++Index)
    if (VectorRegisters[Index] != Index)
      return false;
  return true;
}());
} // namespace
void normalizeAArch64State(AArch64MachineState &State) {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  NEVERD_NORMALIZE_##Arch(Name, Width)
#define NEVERD_NORMALIZE_X64(Name, Width)
#define NEVERD_NORMALIZE_AArch64(Name, Width)                                  \
  State.reg(AArch64Register::Name) &= llvm::maskTrailingOnes<uint64_t>(Width);
#include "neverd/emulation/Registers.def"
#undef NEVERD_NORMALIZE_AArch64
#undef NEVERD_NORMALIZE_X64
#undef NEVERD_SCALAR_REGISTER
  State.reg(AArch64Register::NZCV) &= aarch64::NZCVMask;
}

llvm::Error captureAArch64State(AArch64MachineState &State,
                                AArch64RegisterReader ReadScalar,
                                AArch64VectorReader ReadVector) {
  static_assert(std::size(ScalarRegisters) ==
                std::tuple_size_v<decltype(State.Registers)>);
  static_assert(std::size(VectorRegisters) ==
                std::tuple_size_v<decltype(State.Vectors)>);
  if (!ReadScalar || !ReadVector)
    return diagnostic::error(diagnostic::Register);
  auto Next = State;
  for (auto Register : ScalarRegisters) {
    auto Value = ReadScalar(Register);
    if (!Value)
      return Value.takeError();
    Next.reg(Register) = *Value;
  }
  for (unsigned Index : VectorRegisters) {
    auto Value = ReadVector(Index);
    if (!Value)
      return Value.takeError();
    Next.Vectors[Index] = *Value;
  }
  normalizeAArch64State(Next);
  State = Next;
  return llvm::Error::success();
}
} // namespace neverd::emulation
