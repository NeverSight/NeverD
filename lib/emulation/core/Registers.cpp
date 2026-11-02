//===- Registers.cpp - Guest register identity helpers -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ExecutionDiagnostics.h"

#include "neverd/emulation/CPU.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <limits>

namespace neverd::emulation {
llvm::ArrayRef<CPURegister> architectureRegisters(GuestArchitecture Arch) {
  static constexpr CPURegister All[] = {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  CPURegister::Arch##Name,
#define NEVERD_VECTOR_REGISTER(Arch, Index, Backend)                           \
  CPURegister::Arch##V##Index,
#define NEVERD_EXTENDED_REGISTER NEVERD_SCALAR_REGISTER
#include "neverd/emulation/Registers.def"
#undef NEVERD_EXTENDED_REGISTER
#undef NEVERD_VECTOR_REGISTER
#undef NEVERD_SCALAR_REGISTER
  };
  auto Select = [](GuestArchitecture A) {
    std::vector<CPURegister> Out;
    for (CPURegister R : All)
      if (registerMatches(R, A))
        Out.push_back(R);
    return Out;
  };
  static const auto X64 = Select(GuestArchitecture::X64);
  static const auto AArch64 = Select(GuestArchitecture::AArch64);
  switch (Arch) {
  case GuestArchitecture::X64:
    return X64;
  case GuestArchitecture::AArch64:
    return AArch64;
  }
  return {};
}
const char *registerName(CPURegister R) {
  switch (R) {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  case CPURegister::Arch##Name:                                                \
    return #Name;
#define NEVERD_VECTOR_REGISTER(Arch, Index, Backend)                           \
  case CPURegister::Arch##V##Index:                                            \
    return "V" #Index;
#define NEVERD_EXTENDED_REGISTER NEVERD_SCALAR_REGISTER
#include "neverd/emulation/Registers.def"
#undef NEVERD_EXTENDED_REGISTER
#undef NEVERD_VECTOR_REGISTER
#undef NEVERD_SCALAR_REGISTER
  case CPURegister::Invalid:
    return nullptr;
  }
  return nullptr;
}
namespace {
llvm::Expected<std::vector<RegisterSnapshot>> readRegisterSet(
    GuestArchitecture Architecture,
    llvm::function_ref<llvm::Expected<RegisterValue>(CPURegister)> Read) {
  const auto Registers = architectureRegisters(Architecture);
  if (Registers.empty())
    return diagnostic::error(diagnostic::Register);
  std::vector<RegisterSnapshot> Out;
  Out.reserve(Registers.size());
  for (CPURegister R : Registers) {
    auto Value = Read(R);
    if (!Value)
      return Value.takeError();
    if (!registerValueFits(R, *Value))
      return diagnostic::error(diagnostic::Register);
    Out.push_back({R, *Value});
  }
  return Out;
}
} // namespace
llvm::Expected<std::vector<RegisterSnapshot>>
ExecutionBackend::readRegisters() {
  return readRegisterSet(architecture(),
                         [&](CPURegister R) { return readRegister(R); });
}
llvm::Expected<std::vector<RegisterSnapshot>>
ExecutionBackend::readRegisters(const BackendContext &Context) const {
  return readRegisterSet(architecture(), [&](CPURegister R) {
    return readContextRegister(Context, R);
  });
}
llvm::Expected<RegisterValue>
ExecutionBackend::readContextRegister(const BackendContext &,
                                      CPURegister) const {
  return diagnostic::error(diagnostic::ContextObservationUnsupported);
}
llvm::Expected<RegisterValue>
ExecutionBackend::supportedControlBits(CPURegister) const {
  return diagnostic::error(diagnostic::Register);
}
CPURegister cpuRegister(X64Register R) {
  switch (R) {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  NEVERD_REGISTER_##Arch(Name)
#define NEVERD_REGISTER_X64(Name)                                              \
  case X64Register::Name:                                                      \
    return CPURegister::X64##Name;
#define NEVERD_REGISTER_AArch64(Name)
#define NEVERD_EXTENDED_REGISTER NEVERD_SCALAR_REGISTER
#include "neverd/emulation/Registers.def"
#undef NEVERD_EXTENDED_REGISTER
#undef NEVERD_SCALAR_REGISTER
#undef NEVERD_REGISTER_X64
#undef NEVERD_REGISTER_AArch64
  }
  return CPURegister::Invalid;
}
CPURegister cpuRegister(AArch64Register R) {
  switch (R) {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  NEVERD_REGISTER_##Arch(Name)
#define NEVERD_REGISTER_X64(Name)
#define NEVERD_REGISTER_AArch64(Name)                                          \
  case AArch64Register::Name:                                                  \
    return CPURegister::AArch64##Name;
#include "neverd/emulation/Registers.def"
#undef NEVERD_SCALAR_REGISTER
#undef NEVERD_REGISTER_X64
#undef NEVERD_REGISTER_AArch64
  }
  return CPURegister::Invalid;
}
CPURegister vectorRegister(GuestArchitecture Architecture, unsigned Index) {
#define NEVERD_VECTOR_REGISTER(Arch, Number, Backend)                          \
  if (Architecture == GuestArchitecture::Arch && Index == Number)              \
    return CPURegister::Arch##V##Number;
#include "neverd/emulation/Registers.def"
#undef NEVERD_VECTOR_REGISTER
  return CPURegister::Invalid;
}
bool registerMatches(CPURegister R, GuestArchitecture Architecture) {
  switch (R) {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  case CPURegister::Arch##Name:                                                \
    return Architecture == GuestArchitecture::Arch;
#define NEVERD_VECTOR_REGISTER(Arch, Number, Backend)                          \
  case CPURegister::Arch##V##Number:                                           \
    return Architecture == GuestArchitecture::Arch;
#define NEVERD_EXTENDED_REGISTER NEVERD_SCALAR_REGISTER
#include "neverd/emulation/Registers.def"
#undef NEVERD_EXTENDED_REGISTER
#undef NEVERD_SCALAR_REGISTER
#undef NEVERD_VECTOR_REGISTER
  case CPURegister::Invalid:
    return false;
  }
  return false;
}
unsigned registerWidth(CPURegister R) {
  switch (R) {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  case CPURegister::Arch##Name:                                                \
    return Width;
#define NEVERD_VECTOR_REGISTER(Arch, Number, Backend)                          \
  case CPURegister::Arch##V##Number:                                           \
    return 128;
#define NEVERD_EXTENDED_REGISTER NEVERD_SCALAR_REGISTER
#include "neverd/emulation/Registers.def"
#undef NEVERD_EXTENDED_REGISTER
#undef NEVERD_SCALAR_REGISTER
#undef NEVERD_VECTOR_REGISTER
  case CPURegister::Invalid:
    return 0;
  }
  return 0;
}
bool registerValueFits(CPURegister R, const RegisterValue &V) {
  constexpr unsigned WordBits = std::numeric_limits<uint64_t>::digits;
  const unsigned Width = registerWidth(R);
  if (!Width || Width > WordBits * V.size())
    return false;
  if (Width <= WordBits)
    return !V[1] && (Width == WordBits || !(V[0] >> Width));
  return Width == WordBits * V.size() || !(V[1] >> (Width - WordBits));
}
llvm::Expected<uint64_t> ExecutionBackend::reg(X64Register R) {
  if (registerWidth(cpuRegister(R)) > std::numeric_limits<uint64_t>::digits)
    return diagnostic::error(diagnostic::Register);
  auto V = readRegister(cpuRegister(R));
  if (!V)
    return V.takeError();
  return (*V)[0];
}
llvm::Error ExecutionBackend::setReg(X64Register R, uint64_t V) {
  if (registerWidth(cpuRegister(R)) > std::numeric_limits<uint64_t>::digits)
    return diagnostic::error(diagnostic::Register);
  return writeRegister(cpuRegister(R), {V, 0});
}
llvm::Expected<uint64_t> ExecutionBackend::reg(AArch64Register R) {
  auto V = readRegister(cpuRegister(R));
  if (!V)
    return V.takeError();
  return (*V)[0];
}
llvm::Error ExecutionBackend::setReg(AArch64Register R, uint64_t V) {
  return writeRegister(cpuRegister(R), {V, 0});
}
llvm::Error ExecutionBackend::setGSBase(uint64_t V) {
  return writeRegister(CPURegister::X64GSBase, {V, 0});
}
llvm::Expected<RegisterValue> ExecutionBackend::xmm(unsigned R) {
  return readRegister(vectorRegister(GuestArchitecture::X64, R));
}
llvm::Error ExecutionBackend::setXmm(unsigned R, const XmmValue &V) {
  return writeRegister(vectorRegister(GuestArchitecture::X64, R), V);
}
llvm::Expected<RegisterValue> ExecutionBackend::vector(unsigned R) {
  return readRegister(vectorRegister(architecture(), R));
}
llvm::Error ExecutionBackend::setVector(unsigned R, const RegisterValue &V) {
  return writeRegister(vectorRegister(architecture(), R), V);
}
} // namespace neverd::emulation
