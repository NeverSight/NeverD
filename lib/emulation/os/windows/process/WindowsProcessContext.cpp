//===- WindowsProcessContext.cpp - Checked Windows user CONTEXT codec ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessContext.h"

#include "../../../arch/aarch64/AArch64Machine.h"
#include "../../../arch/x86_64/X64Machine.h"
#include "../exception/WindowsX64Context.h"
#include "WindowsProcess.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <climits>

namespace neverd::emulation::windows_process {
namespace {
using namespace value;
struct Field {
  CPURegister Register;
  unsigned Offset, Size;
  uint64_t Mutable;
};
constexpr Field X64Fields[] = {
#define NEVERD_WINDOWS_CONTEXT_X64(Name, Offset, Size, Mask)                   \
  {CPURegister::X64##Name, Offset, Size, Mask},
#include "WindowsProcessExceptions.def"
#undef NEVERD_WINDOWS_CONTEXT_X64
};
constexpr Field ARM64Fields[] = {
#define NEVERD_WINDOWS_CONTEXT_AARCH64(Name, Offset, Size, Mask)               \
  {CPURegister::AArch64##Name, Offset, Size, Mask},
#include "WindowsProcessExceptions.def"
#undef NEVERD_WINDOWS_CONTEXT_AARCH64
};
llvm::ArrayRef<Field> fields(bool X64) {
  return X64 ? llvm::ArrayRef(X64Fields) : llvm::ArrayRef(ARM64Fields);
}
uint64_t read(llvm::ArrayRef<uint8_t> Bytes, const Field &F) {
  uint64_t V = 0;
  for (unsigned I = 0; I < F.Size; ++I)
    V |= uint64_t(Bytes[F.Offset + I]) << (I * CHAR_BIT);
  return V;
}
} // namespace

llvm::Expected<X64SEH::Context>
readUnwindContext(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() != X64ContextSize)
    return failure(text::ExceptionContext);
  X64SEH::Context State;
  for (size_t I = 0; I < State.GPR.size(); ++I)
    State.GPR[I] = llvm::support::endian::read64le(
        Bytes.data() + seh::ContextGPROffset + I * PointerSize);
  State.PC =
      llvm::support::endian::read64le(Bytes.data() + seh::ContextPCOffset);
  State.Flags =
      llvm::support::endian::read32le(Bytes.data() + seh::ContextEFlagsOffset);
  State.CS =
      llvm::support::endian::read16le(Bytes.data() + seh::ContextCSOffset);
  State.SS =
      llvm::support::endian::read16le(Bytes.data() + seh::ContextSSOffset);
  for (size_t I = 0; I < State.Xmm.size(); ++I)
    for (size_t J = 0; J < State.Xmm[I].size(); ++J)
      State.Xmm[I][J] = llvm::support::endian::read64le(
          Bytes.data() + X64ContextXmmOffset +
          (seh::FirstNonvolatileXmm + I) * ContextVectorBytes +
          J * PointerSize);
  return State;
}

llvm::Error writeUnwindContext(const X64SEH::Context &State,
                               llvm::MutableArrayRef<uint8_t> Bytes) {
  if (Bytes.size() != X64ContextSize)
    return failure(text::ExceptionContext);
  for (size_t I = 0; I < State.GPR.size(); ++I)
    llvm::support::endian::write64le(
        Bytes.data() + seh::ContextGPROffset + I * PointerSize, State.GPR[I]);
  llvm::support::endian::write64le(Bytes.data() + seh::ContextPCOffset,
                                   State.PC);
  llvm::support::endian::write32le(Bytes.data() + seh::ContextEFlagsOffset,
                                   State.Flags);
  for (size_t I = 0; I < State.Xmm.size(); ++I)
    for (size_t J = 0; J < State.Xmm[I].size(); ++J)
      llvm::support::endian::write64le(Bytes.data() + X64ContextXmmOffset +
                                           (seh::FirstNonvolatileXmm + I) *
                                               ContextVectorBytes +
                                           J * PointerSize,
                                       State.Xmm[I][J]);
  return llvm::Error::success();
}

llvm::Expected<std::vector<uint8_t>> captureUserContext(ExecutionBackend &CPU,
                                                        ContextOrigin Origin) {
  const bool X64 = CPU.architecture() == GuestArchitecture::X64;
  std::vector<uint8_t> Bytes(X64 ? X64ContextSize : AArch64ContextSize);
  llvm::support::endian::write32le(
      Bytes.data() + (X64 ? X64ContextFlagsOffset : AArch64ContextFlagsOffset),
      X64 ? X64ContextFlags : AArch64ContextFlags);
  if (X64) {
    if (auto E = windows_context::collectX64IntegerContext(
            CPU, Bytes, Origin == ContextOrigin::HardwareFault))
      return E;
  } else
    for (const auto &F : fields(false)) {
      auto V = CPU.readRegister(F.Register);
      if (!V)
        return V.takeError();
      for (unsigned I = 0; I < F.Size; ++I)
        Bytes[F.Offset + I] = (*V)[0] >> (I * CHAR_BIT);
    }
  if (X64) {
    X64MachineState State;
    auto MXCSR = CPU.reg(X64Register::MXCSR);
    if (!MXCSR)
      return MXCSR.takeError();
    State.MXCSR = *MXCSR;
    for (unsigned I = unsigned(CPURegister::X64FPCW);
         I <= unsigned(CPURegister::X64FP7); ++I) {
      auto R = static_cast<CPURegister>(I);
      auto V = CPU.readRegister(R);
      if (!V)
        return V.takeError();
      if (auto E = writeX64FPRegister(State.FP, R, *V))
        return std::move(E);
    }
    for (unsigned I = 0; I < State.Xmm.size(); ++I) {
      auto V = CPU.xmm(I);
      if (!V)
        return V.takeError();
      State.Xmm[I] = *V;
    }
    auto Mask = CPU.supportedControlBits(CPURegister::X64MXCSR);
    if (!Mask)
      return Mask.takeError();
    if (auto E = encodeX64FXState(State,
                                  llvm::MutableArrayRef(Bytes).slice(
                                      X64ContextFPOffset, x64::fp::LegacyBytes),
                                  (*Mask)[0]))
      return std::move(E);
  } else {
    for (unsigned I = 0; I < AArch64ContextVectorCount; ++I) {
      auto V = CPU.vector(I);
      if (!V)
        return V.takeError();
      auto *P =
          Bytes.data() + AArch64ContextVectorsOffset + I * ContextVectorBytes;
      llvm::support::endian::write64le(P, (*V)[0]);
      llvm::support::endian::write64le(P + PointerSize, (*V)[1]);
    }
  }
  return Bytes;
}

llvm::Error captureCallerContext(ExecutionBackend &CPU, uint64_t Destination,
                                 bool UserMode) {
  const bool X64 = CPU.architecture() == GuestArchitecture::X64;
  const uint64_t Size = X64 ? X64ContextSize : AArch64ContextSize;
  if (!Destination || Size > UINT64_MAX - Destination ||
      (UserMode && (Destination < ImageAlignment || Destination >= UserLimit ||
                    Size > UserLimit - Destination)) ||
      Destination % ContextVectorBytes)
    return failure(text::CaptureContextBuffer);
  const unsigned UserAccess = UserMode ? UserAccessible : 0;
  auto Writable = CPU.canAccess(Destination, Size, Write | UserAccess);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure(text::CaptureContextBuffer);
  auto ABI = IntegerABI::get(X64 ? IntegerCallingConvention::Win64
                                 : IntegerCallingConvention::AAPCS64);
  if (!ABI)
    return ABI.takeError();
  auto SP = CPU.readRegister(ABI->info().StackPointer);
  if (!SP)
    return SP.takeError();
  auto CallerSP = ABI->returnStackPointer((*SP)[0]);
  if (!CallerSP)
    return CallerSP.takeError();
  if (X64) {
    auto Readable = CPU.canAccess((*SP)[0], PointerSize, Read | UserAccess);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return failure(text::CaptureContextReturn);
  }
  auto CallerPC = ABI->readReturnAddress(CPU, (*SP)[0]);
  if (!CallerPC)
    return CallerPC.takeError();
  auto Bytes = captureUserContext(CPU);
  if (!Bytes)
    return Bytes.takeError();
  // Capture uses the ABI's caller frame, not the provider gate. ARM64's
  // native routine clears X0 and LR in the record while retaining PC = LR.
  for (const auto &F : fields(X64)) {
    std::optional<uint64_t> Value;
    if (F.Register == ABI->info().StackPointer)
      Value = *CallerSP;
    if (F.Register == CPURegister::X64PC ||
        F.Register == CPURegister::AArch64PC)
      Value = *CallerPC;
    if (F.Register == CPURegister::AArch64X0 ||
        F.Register == CPURegister::AArch64X30)
      Value = 0;
    if (Value)
      for (unsigned I = 0; I < F.Size; ++I)
        (*Bytes)[F.Offset + I] = *Value >> (I * CHAR_BIT);
  }
  if (X64) {
    llvm::support::endian::write32le(Bytes->data() + X64ContextFlagsOffset,
                                     X64CapturedContextFlags);
    auto SS = CPU.readRegister(CPURegister::X64SS);
    if (!SS)
      return SS.takeError();
    windows_context::writeFlatDataSelectors(*Bytes, (*SS)[0]);
#define NEVERD_WINDOWS_CAPTURE_FP_POINTER(Offset)                              \
  llvm::support::endian::write64le(                                            \
      Bytes->data() + Offset,                                                  \
      uint32_t(llvm::support::endian::read64le(Bytes->data() + Offset)));
#include "WindowsContextCapture.def"
#undef NEVERD_WINDOWS_CAPTURE_FP_POINTER
  }
  struct Range {
    GuestArchitecture ISA;
    unsigned Offset, Size;
  };
  constexpr Range Ranges[] = {
#define NEVERD_WINDOWS_CAPTURE_RANGE(ISA, Offset, Size)                        \
  {GuestArchitecture::ISA, Offset, Size},
#include "WindowsContextCapture.def"
#undef NEVERD_WINDOWS_CAPTURE_RANGE
  };
  for (const auto &R : Ranges)
    if (R.ISA == CPU.architecture())
      if (auto E = CPU.write(Destination + R.Offset,
                             llvm::ArrayRef(*Bytes).slice(R.Offset, R.Size)))
        return E;
  return llvm::Error::success();
}

llvm::Error restoreUserContext(ExecutionBackend &CPU,
                               const BackendContext &Snapshot,
                               llvm::ArrayRef<uint8_t> Original,
                               llvm::ArrayRef<uint8_t> Changed,
                               uint64_t StackBase, uint64_t StackTop,
                               ContextOrigin Origin) {
  const bool X64 = CPU.architecture() == GuestArchitecture::X64;
  const uint64_t Size = X64 ? X64ContextSize : AArch64ContextSize;
  if (Original.size() != Size || Changed.size() != Size)
    return failure(text::ExceptionContext);
  std::vector<uint8_t> Mutable(Size);
  std::vector<std::pair<CPURegister, RegisterValue>> Values;
  uint64_t PC = 0, SP = 0, MXCSR = 0;
  for (const auto &F : fields(X64)) {
    const uint64_t V = read(Changed, F);
    for (unsigned I = 0; I < F.Size; ++I)
      Mutable[F.Offset + I] = F.Mutable >> (I * CHAR_BIT);
    uint64_t CPUValue = V;
    if (Origin == ContextOrigin::HardwareFault &&
        F.Register == CPURegister::X64FLAGS) {
      if (!(read(Original, F) & x64::ResumeFlag))
        return failure(text::ExceptionContext);
      // RF is immutable in this bounded user CONTEXT profile. Validate its
      // retained value below, then remove the delivery-only bit on restore.
      // Debug breakpoints and caller changes to RF remain unsupported.
      CPUValue &= ~x64::ResumeFlag;
    }
    Values.push_back({F.Register, {CPUValue, 0}});
    if (F.Register == CPURegister::X64PC ||
        F.Register == CPURegister::AArch64PC)
      PC = V;
    if (F.Register == CPURegister::X64SP ||
        F.Register == CPURegister::AArch64SP)
      SP = V;
    if (F.Register == CPURegister::X64MXCSR)
      MXCSR = V;
  }
  if (X64) {
    const auto FP = Changed.slice(X64ContextFPOffset, x64::fp::LegacyBytes);
    X64MachineState State;
    if (auto E = decodeX64FXState(State, FP))
      return E;
    if (auto E = validateX64FPState(State.FP))
      return E;
    auto Mask = CPU.supportedControlBits(CPURegister::X64MXCSR);
    if (!Mask)
      return Mask.takeError();
    if (State.MXCSR != MXCSR || (MXCSR & ~(*Mask)[0]) ||
        (!CPU.supportsSIMDExceptions() &&
         (MXCSR & x64::InitialMXCSR) != x64::InitialMXCSR))
      return failure(text::ExceptionContext);
    std::vector<uint8_t> Canonical(x64::fp::LegacyBytes);
    if (auto E = encodeX64FXState(State, Canonical, (*Mask)[0]))
      return E;
    if (FP != llvm::ArrayRef(Canonical))
      return failure(text::ExceptionContext);
    std::fill_n(Mutable.begin() + X64ContextFPOffset, x64::fp::LegacyBytes,
                UINT8_MAX);
    for (unsigned I = unsigned(CPURegister::X64FPCW);
         I <= unsigned(CPURegister::X64FP7); ++I) {
      auto R = static_cast<CPURegister>(I);
      Values.push_back({R, readX64FPRegister(State.FP, R)});
    }
    for (unsigned I = 0; I < State.Xmm.size(); ++I)
      Values.push_back(
          {vectorRegister(GuestArchitecture::X64, I), State.Xmm[I]});
  } else {
    std::fill_n(Mutable.begin() + AArch64ContextVectorsOffset,
                AArch64ContextVectorCount * ContextVectorBytes, UINT8_MAX);
    for (unsigned I = 0; I < AArch64ContextVectorCount; ++I) {
      const auto *P =
          Changed.data() + AArch64ContextVectorsOffset + I * ContextVectorBytes;
      Values.push_back({vectorRegister(GuestArchitecture::AArch64, I),
                        {llvm::support::endian::read64le(P),
                         llvm::support::endian::read64le(P + PointerSize)}});
    }
  }
  for (size_t I = 0; I < Size; ++I)
    if ((Original[I] ^ Changed[I]) & ~Mutable[I])
      return failure(text::ExceptionContext);
  if (PC < ImageAlignment || PC >= UserLimit || (!X64 && PC % DWordSize) ||
      SP <= StackBase || SP > StackTop ||
      SP % (X64 ? PointerSize : ContextVectorBytes))
    return failure(text::ExceptionContext);
  auto Code = CPU.canAccess(PC, X64 ? 1 : DWordSize, Execute | UserAccessible);
  if (!Code)
    return Code.takeError();
  auto Stack = CPU.canAccess(SP - 1, 1, Read | Write | UserAccessible);
  if (!Stack)
    return Stack.takeError();
  if (!*Code || !*Stack)
    return failure(text::ExceptionContext);
  if (auto E = CPU.restoreContext(Snapshot))
    return E;
  for (const auto &[R, V] : Values)
    if (auto E = CPU.writeRegister(R, V))
      return E;
  return llvm::Error::success();
}
} // namespace neverd::emulation::windows_process
