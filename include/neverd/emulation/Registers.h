//===- Registers.h - Typed guest register access -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_REGISTERS_H
#define NEVERD_EMULATION_REGISTERS_H
#include "neverd/emulation/ExecutionBackend.h"

#include "llvm/ADT/ArrayRef.h"

#include <array>
#include <cstdint>
namespace neverd::emulation {
using RegisterValue = std::array<uint64_t, 2>;
enum class CPURegister {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend) Arch##Name,
#define NEVERD_VECTOR_REGISTER(Arch, Index, Backend) Arch##V##Index,
#include "neverd/emulation/Registers.def"
#undef NEVERD_SCALAR_REGISTER
#undef NEVERD_VECTOR_REGISTER
  Invalid,
#define NEVERD_EXTENDED_REGISTER(Arch, Name, Width, Backend) Arch##Name,
#include "neverd/emulation/Registers.def"
#undef NEVERD_EXTENDED_REGISTER
};
enum class X64Register {
#define NEVERD_EXTENDED_REGISTER NEVERD_SCALAR_REGISTER
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  NEVERD_REGISTER_##Arch(Name)
#define NEVERD_REGISTER_X64(Name) Name,
#define NEVERD_REGISTER_AArch64(Name)
#include "neverd/emulation/Registers.def"
#undef NEVERD_REGISTER_AArch64
#undef NEVERD_REGISTER_X64
#undef NEVERD_SCALAR_REGISTER
#undef NEVERD_EXTENDED_REGISTER
};
enum class AArch64Register {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  NEVERD_REGISTER_##Arch(Name)
#define NEVERD_REGISTER_X64(Name)
#define NEVERD_REGISTER_AArch64(Name) Name,
#include "neverd/emulation/Registers.def"
#undef NEVERD_REGISTER_AArch64
#undef NEVERD_REGISTER_X64
#undef NEVERD_SCALAR_REGISTER
};
struct RegisterSnapshot {
  CPURegister Register;
  RegisterValue Value;
  bool operator==(const RegisterSnapshot &) const = default;
};
/// The complete modeled register inventory in stable declaration order.
llvm::ArrayRef<CPURegister>
architectureRegisters(GuestArchitecture Architecture);
/// Architecture-local spelling from Registers.def; nullptr for invalid IDs.
const char *registerName(CPURegister Register);
CPURegister cpuRegister(X64Register Register);
CPURegister cpuRegister(AArch64Register Register);
CPURegister vectorRegister(GuestArchitecture Architecture, unsigned Index);
bool registerMatches(CPURegister Register, GuestArchitecture Architecture);
unsigned registerWidth(CPURegister Register);
bool registerValueFits(CPURegister Register, const RegisterValue &Value);
} // namespace neverd::emulation
#endif
