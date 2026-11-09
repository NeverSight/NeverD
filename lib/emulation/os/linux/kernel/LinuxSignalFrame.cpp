//===- LinuxSignalFrame.cpp - AArch64 guest signal-frame transfers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxMemory.h"
#include "LinuxSignals.h"

#include "neverd/emulation/Limits.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation::linux_model {
namespace {
// Linux AArch64 UAPI: rt_sigframe { siginfo_t, ucontext }, with the fixed
// FPSIMD context and a separate frame record. See the ABI references in
// docs/process-emulation.md. These are wire offsets, not host C structures.
constexpr unsigned UC = 128, Mask = UC + 40, Context = UC + 176;
constexpr unsigned Registers = Context + 8, SP = Context + 256;
constexpr unsigned PC = Context + 264, PState = Context + 272;
constexpr unsigned FP = Context + 288, FrameSize = 4688, RecordSize = 16;
constexpr uint32_t FPMagic = 0x46508001, FPSize = 528;
constexpr uint64_t Unblockable = (1ULL << 8) | (1ULL << 18);
CPURegister general(unsigned I) {
  return CPURegister(unsigned(CPURegister::AArch64X0) + I);
}
bool refuse(ProcessResult &Result, const char *Text) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Text;
  return false;
}
} // namespace

llvm::Expected<bool>
LinuxSignals::enter(ExecutionBackend &CPU, const MemoryLayout &Layout,
                    LinuxMemory &Memory, uint64_t TID, int32_t Signal,
                    const LinuxSignalAction &Action, bool ThreadDirected,
                    ProcessResult &Result) {
  auto &Thread = Threads.at(TID);
  if (CPU.architecture() != GuestArchitecture::AArch64 ||
      !State.AlternateStackDisabled ||
      Thread.Frames.size() >=
          ::neverd::limits::emulation::
              kLinuxSignalFrameLinuxSignalsEnterThreadFramesSizeLimit ||
      (uint32_t(Action.Flags) & ~uint32_t(0xdc000007)) ||
      (Action.Flags > UINT32_MAX &&
       Action.Flags != uint64_t(int64_t(int32_t(uint32_t(Action.Flags))))))
    return refuse(Result, SignalFrameUnsupported);
  std::array<uint64_t, 34> Scalars;
  for (unsigned I = 0; I < 34; ++I) {
    CPURegister R = I < 31    ? general(I)
                    : I == 31 ? CPURegister::AArch64SP
                    : I == 32 ? CPURegister::AArch64PC
                              : CPURegister::AArch64NZCV;
    auto Value = CPU.readRegister(R);
    if (!Value)
      return Value.takeError();
    Scalars[I] = (*Value)[0];
  }
  std::array<RegisterValue, 32> Vectors;
  for (unsigned I = 0; I < 32; ++I) {
    auto V = CPU.vector(I);
    if (!V)
      return V.takeError();
    Vectors[I] = *V;
  }
  auto FPSR = CPU.readRegister(CPURegister::AArch64FPSR);
  if (!FPSR)
    return FPSR.takeError();
  auto FPCR = CPU.readRegister(CPURegister::AArch64FPCR);
  if (!FPCR)
    return FPCR.takeError();
  if (Scalars[31] < FrameSize + RecordSize || Scalars[31] > Layout.UserLimit)
    return refuse(Result, SignalFrameFault);
  const uint64_t Record = (Scalars[31] - RecordSize) & ~uint64_t(15);
  const uint64_t Base = Record - FrameSize;
  auto Writable =
      CPU.canAccess(Base, Scalars[31] - Base, Write | UserAccessible);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return refuse(Result, SignalFrameFault);
  if (Action.Handler % 4)
    return refuse(Result, SignalFrameUnsupported);
  auto Executable = CPU.canAccess(Action.Handler, 4, Execute | UserAccessible);
  if (!Executable)
    return Executable.takeError();
  if (!*Executable)
    return refuse(Result, SignalFrameFault);

  uint64_t Restorer = Action.Restorer;
  if (!(Action.Flags & 0x04000000)) {
    if (!Trampoline) {
      // The Linux placement owner reserves this workload-local RX mapping.
      // It contains real guest instructions, not a host return callback.
      ProcessServiceEvent Map{};
      Map.Arguments = {0, Layout.PageSize, 3, 0x22, UINT64_MAX, 0};
      auto Allocated = Memory.handle(ServiceKind::Mmap, Map, Result);
      if (!Allocated)
        return Allocated.takeError();
      if (!*Allocated)
        return false;
      if (**Allocated >= uint64_t(-4095))
        return refuse(Result, SignalFrameFault);
      uint8_t Code[8];
      llvm::support::endian::write32le(Code, 0xd2801168);     // mov x8, #139
      llvm::support::endian::write32le(Code + 4, 0xd4000001); // svc #0
      if (auto E = CPU.write(**Allocated, Code))
        return std::move(E);
      Map.Arguments = {**Allocated, Layout.PageSize, 5, 0, 0, 0};
      auto Protected = Memory.handle(ServiceKind::Mprotect, Map, Result);
      if (!Protected)
        return Protected.takeError();
      if (!*Protected)
        return false;
      if (**Protected)
        return refuse(Result, SignalFrameFault);
      Trampoline = **Allocated;
    }
    Restorer = Trampoline;
  }
  auto Put = [&](unsigned Offset, uint64_t Value,
                 unsigned Size = 8) -> llvm::Error {
    uint8_t Bytes[8];
    llvm::support::endian::write64le(Bytes, Value);
    return CPU.write(Base + Offset, llvm::ArrayRef(Bytes).take_front(Size));
  };
  // Write only ABI-defined fields. Padding and unused context storage retain
  // the guest stack bytes; they are not invented zero-filled kernel output.
  if (Action.Flags & 4) {
    std::array<uint8_t, 128> Info{};
    llvm::support::endian::write32le(Info.data(), Signal);
    // SI_TKILL for thread-directed delivery; kill retains SI_USER (zero).
    llvm::support::endian::write32le(Info.data() + 8,
                                     ThreadDirected ? uint32_t(-6) : 0);
    llvm::support::endian::write32le(Info.data() + 16, ProcessID);
    llvm::support::endian::write32le(Info.data() + 20, UserID);
    if (auto E = CPU.write(Base, Info))
      return std::move(E);
  }
  for (auto [Offset, Value] : {std::pair<unsigned, uint64_t>{UC, 0},
                               {UC + 8, 0},
                               {UC + 16, 0},
                               {UC + 32, 0},
                               {Mask, *Thread.Mask},
                               {Context, 0},
                               {SP, Scalars[31]},
                               {PC, Scalars[32]},
                               {PState, Scalars[33]},
                               {FrameSize, Scalars[29]},
                               {FrameSize + 8, Scalars[30]}}) {
    if (auto E = Put(Offset, Value))
      return std::move(E);
  }
  if (auto E = Put(UC + 24, 2, 4)) // SS_DISABLE
    return std::move(E);
  for (unsigned I = 0; I < 31; ++I)
    if (auto E = Put(Registers + 8 * I, Scalars[I]))
      return std::move(E);
  for (auto [Offset, Value] : {std::pair<unsigned, uint64_t>{FP, FPMagic},
                               {FP + 4, FPSize},
                               {FP + 8, (*FPSR)[0]},
                               {FP + 12, (*FPCR)[0]}}) {
    if (auto E = Put(Offset, Value, 4))
      return std::move(E);
  }
  for (unsigned I = 0; I < 32; ++I) {
    if (auto E = Put(FP + 16 + I * 16, Vectors[I][0]))
      return std::move(E);
    if (auto E = Put(FP + 24 + I * 16, Vectors[I][1]))
      return std::move(E);
  }
  if (auto E = Put(FP + FPSize, 0)) // extension-list terminator
    return std::move(E);
  if (auto E = CPU.writeRegister(CPURegister::AArch64X0, {uint64_t(Signal), 0}))
    return std::move(E);
  if (Action.Flags & 4) {
    if (auto E = CPU.writeRegister(CPURegister::AArch64X1, {Base, 0}))
      return std::move(E);
    if (auto E = CPU.writeRegister(CPURegister::AArch64X2, {Base + UC, 0}))
      return std::move(E);
  }
  for (auto [R, Value] : {std::pair{CPURegister::AArch64SP, Base},
                          {CPURegister::AArch64X29, Record},
                          {CPURegister::AArch64X30, Restorer},
                          {CPURegister::AArch64PC, Action.Handler}}) {
    if (auto E = CPU.writeRegister(R, {Value, 0}))
      return std::move(E);
  }
  Thread.Frames.push_back(Base);
  Result.PC = Action.Handler;
  return true;
}

llvm::Expected<bool> LinuxSignals::restore(ExecutionBackend &CPU,
                                           const MemoryLayout &Layout,
                                           uint64_t TID,
                                           ProcessResult &Result) {
  auto I = Threads.find(TID);
  if (CPU.architecture() != GuestArchitecture::AArch64 || I == Threads.end() ||
      I->second.Frames.empty())
    return refuse(Result, SignalReturnUnsupported);
  auto Stack = CPU.readRegister(CPURegister::AArch64SP);
  if (!Stack)
    return Stack.takeError();
  const uint64_t Base = (*Stack)[0];
  if (Base % 16 || Base != I->second.Frames.back() ||
      Base >= Layout.UserLimit || FrameSize > Layout.UserLimit - Base)
    return refuse(Result, SignalReturnUnsupported);
  auto Readable = CPU.canAccess(Base, FrameSize, Read | UserAccessible);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return refuse(Result, SignalFrameFault);
  std::array<uint8_t, FrameSize> Bytes;
  if (auto E = CPU.read(Base, Bytes))
    return std::move(E);
  auto U64 = [&](unsigned Offset) {
    return llvm::support::endian::read64le(Bytes.data() + Offset);
  };
  auto U32 = [&](unsigned Offset) {
    return llvm::support::endian::read32le(Bytes.data() + Offset);
  };
  // Only the admitted fixed FPSIMD layout and disabled alternate stack are
  // supported. Unsupported extension records never become silent NOPs.
  if (U32(UC + 24) != 2 || U64(UC + 16) || U64(UC + 32) || U32(FP) != FPMagic ||
      U32(FP + 4) != FPSize || U64(FP + FPSize) ||
      (U64(PState) & ~uint64_t(0xf0000000)))
    return refuse(Result, SignalReturnUnsupported);
  // Validate backend control values without leaving partially restored CPU
  // registers on a refusal. Guest memory is deliberately not rolled back.
  auto Before = CPU.saveContext();
  if (!Before)
    return Before.takeError();
  auto Rollback = llvm::scope_exit(
      [&] { llvm::consumeError(CPU.restoreContext(**Before)); });
  if (auto E = CPU.writeRegister(CPURegister::AArch64FPCR, {U32(FP + 12), 0}))
    return std::move(E);
  if (auto E = CPU.writeRegister(CPURegister::AArch64FPSR, {U32(FP + 8), 0}))
    return std::move(E);
  for (unsigned J = 0; J < 31; ++J)
    if (auto E = CPU.writeRegister(general(J), {U64(Registers + 8 * J), 0}))
      return std::move(E);
  for (unsigned J = 0; J < 32; ++J)
    if (auto E =
            CPU.setVector(J, {U64(FP + 16 + J * 16), U64(FP + 24 + J * 16)}))
      return std::move(E);
  for (auto [R, Value] : {std::pair{CPURegister::AArch64SP, U64(SP)},
                          {CPURegister::AArch64PC, U64(PC)},
                          {CPURegister::AArch64NZCV, U64(PState)}})
    if (auto E = CPU.writeRegister(R, {Value, 0}))
      return std::move(E);
  Rollback.release();
  I->second.Mask = U64(Mask) & ~Unblockable;
  I->second.Frames.pop_back();
  Result.PC = U64(PC);
  return true;
}
} // namespace neverd::emulation::linux_model
