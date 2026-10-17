//===- DarwinLibSystem.cpp - Authenticated imported libc calls -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Finite libSystem function ABI, errno and shared kernel effects.
#include "DarwinLibSystem.h"

#include "DarwinCallbacks.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::darwin_model {
namespace {
constexpr LibSystemBinding Bindings[] = {
#define NEVERD_DARWIN_LIBSYSTEM_CALL(Name, Symbol, Count, Kernel, Provider)    \
  {Symbol, Count, Kernel, Provider},
#include "DarwinLibSystemCalls.def"
#undef NEVERD_DARWIN_LIBSYSTEM_CALL
};
static_assert(unsigned(LibSystemGateKind::StubBinder) == std::size(Bindings));

bool isGuard(LibSystemGateKind Kind) {
  return Kind == LibSystemGateKind::GuardAcquire ||
         Kind == LibSystemGateKind::GuardRelease ||
         Kind == LibSystemGateKind::GuardAbort;
}

llvm::Expected<std::optional<uint64_t>>
invokeGuard(ExecutionBackend &CPU, LibSystemGateKind Kind, uint64_t Address,
            const ProcessOptions &Options) {
  if (!Address || Address % 8)
    return failure("Darwin C++ guard requires an aligned nonnull pointer");
  const bool Acquire = Kind == LibSystemGateKind::GuardAcquire;
  if (Acquire) {
    auto Readable = CPU.canAccess(Address, 1, Read | UserAccessible);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return failure("Darwin C++ guard storage is not readable");
    auto Initialized = CPU.readInteger(Address, 1);
    if (!Initialized)
      return Initialized.takeError();
    // The completed ABI path consumes only the initialized byte. It neither
    // examines private bookkeeping nor needs mutation or thread observations.
    if (*Initialized)
      return std::optional<uint64_t>(0);
  }
  auto Readable = CPU.canAccess(Address, 8, Read | UserAccessible);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return failure("Darwin C++ guard storage is not readable");
  auto Word = CPU.readInteger(Address, 8);
  if (!Word)
    return Word.takeError();
  if (!Options.DarwinSystem || !Options.DarwinSystem->ThreadSelfPort ||
      !*Options.DarwinSystem->ThreadSelfPort)
    return failure(
        "Darwin C++ guard requires a known nonzero thread self port");
  const uint64_t Owner = uint64_t(*Options.DarwinSystem->ThreadSelfPort) << 32;
  const uint64_t Pending = Owner | 0x200;
  if (Acquire ? *Word != 0 : *Word != Pending) {
    if (Acquire && *Word == Pending)
      return failure("recursive Darwin static initialization is unsupported");
    return failure("unsupported Darwin C++ guard state");
  }
  auto Writable = CPU.canAccess(Address, 8, Write | UserAccessible);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure("Darwin C++ guard storage is not writable");
  // Native macOS x64/ARM64 controls compare all eight bytes, including the
  // Mach owner retained by release. Only this single-main-thread protocol is
  // admitted; waiting/concurrent and arbitrary runtime-private states refuse.
  const uint64_t Value = Acquire ? Pending
                         : Kind == LibSystemGateKind::GuardRelease
                             ? Owner | 0x101
                             : 0;
  if (auto E = CPU.writeInteger(Address, Value, 8))
    return std::move(E);
  return Acquire ? std::optional<uint64_t>(1) : std::nullopt;
}

llvm::Expected<bool> publishReturn(ExecutionBackend &CPU, const IntegerABI &ABI,
                                   uint64_t SP, uint64_t PC,
                                   std::optional<uint64_t> Value,
                                   ProcessResult &Result) {
  if (Value)
    if (auto E = CPU.writeRegister(ABI.info().Result, {*Value, 0}))
      return std::move(E);
  if (auto E = CPU.writeRegister(ABI.info().StackPointer, {SP, 0}))
    return std::move(E);
  if (auto E =
          CPU.writeRegister(ABI.info().Architecture == GuestArchitecture::X64
                                ? CPURegister::X64PC
                                : CPURegister::AArch64PC,
                            {PC, 0}))
    return std::move(E);
  Result.NativeCalls.back().Result = Value;
  Result.PC = PC;
  return true;
}
} // namespace
std::optional<LibSystemGateKind> libSystemImport(llvm::StringRef Symbol,
                                                 llvm::StringRef Provider) {
  if (Provider == value::LibSystemPath && Symbol == "__tlv_bootstrap")
    return LibSystemGateKind::TLVBootstrap;
  if (Provider == value::LibSystemPath && Symbol == "dyld_stub_binder")
    return LibSystemGateKind::StubBinder;
  for (unsigned I = 0; I < std::size(Bindings); ++I)
    if (Bindings[I].Symbol == Symbol && Bindings[I].Provider == Provider)
      return LibSystemGateKind(I);
  return std::nullopt;
}
bool isGlobalCallbackRegistration(LibSystemGateKind Kind) {
  return Kind == LibSystemGateKind::AtExit ||
         Kind == LibSystemGateKind::CxaAtExit;
}
const LibSystemBinding &libSystemBinding(LibSystemGateKind Kind) {
  const auto Index = unsigned(Kind);
  assert(Index < std::size(Bindings));
  return Bindings[Index];
}
llvm::Expected<LibSystemArguments>
readLibSystemArguments(ExecutionBackend &CPU, const IntegerABI &ABI,
                       uint64_t SP, LibSystemGateKind Kind) {
  LibSystemArguments Arguments;
  Arguments.Count = libSystemBinding(Kind).Arguments;
  for (unsigned I = 0; I < Arguments.Count; ++I) {
    auto Value = ABI.readArgument(CPU, SP, I);
    if (!Value)
      return Value.takeError();
    Arguments.Carriers[I] = *Value;
  }
  if (Kind == LibSystemGateKind::Open &&
      (uint32_t(Arguments.Carriers[1]) & value::OpenCreate)) {
    // Apple's ARM64 varargs use stack slots even with unused GP registers.
    // Only the promoted int's four bytes are defined, not the slot padding.
    auto Mode = ABI.info().Architecture == GuestArchitecture::X64
                    ? ABI.readArgument(CPU, SP, 2)
                    : CPU.readInteger(SP, 4);
    if (!Mode)
      return Mode.takeError();
    Arguments.Carriers[2] = *Mode;
    Arguments.Count = 3;
  }
  Arguments.Kernel = Arguments.Carriers;
  if (Kind == LibSystemGateKind::Open)
    Arguments.Kernel[2] = uint16_t(Arguments.Kernel[2]);
  if (Kind == LibSystemGateKind::Mmap || Kind == LibSystemGateKind::Mprotect)
    Arguments.Kernel[2] = uint32_t(Arguments.Kernel[2]);
  if (Kind == LibSystemGateKind::Mmap) {
    const auto Flags = uint32_t(Arguments.Carriers[3]);
    // The C UNIX03 wrapper rejects these before entering the kernel. Keep
    // original carriers in the report and raw BSD mmap semantics unchanged.
    if (!Arguments.Carriers[1] ||
        !(Flags & (value::MapPrivate | value::MapShared)))
      Arguments.Error = value::InvalidArgument;
    Arguments.Kernel[3] = Flags | value::MapUnix03;
    Arguments.Kernel[4] = uint32_t(Arguments.Kernel[4]);
  }
  return Arguments;
}
llvm::Error prepareLibSystem(AddressSpace &Space, const ProcessImage &Image) {
  if (Image.Imports.empty())
    return llvm::Error::success();
  const auto Page = Image.Memory.PageSize;
  for (uint64_t Address : {value::LibSystemGate, value::LibSystemGate + Page})
    if (auto E = Space.map(Address, Page, Read | Write | UserAccessible))
      return E;
  std::vector<uint8_t> Gates(Page, 0);
  for (const auto &[Address, Index] : Image.Imports) {
    const auto Offset = Address - value::LibSystemGate;
    assert(Offset + value::LibSystemGateStride <= Gates.size());
    if (Image.Architecture == GuestArchitecture::X64) {
      Gates[Offset] = 0x0f; // syscall; no implicit successful fallthrough.
      Gates[Offset + 1] = 0x05;
      Gates[Offset + 2] = 0xcc;
    } else {
      llvm::support::endian::write32le(Gates.data() + Offset,
                                       0xd4000001 | (value::LibSystemSVC << 5));
      llvm::support::endian::write32le(Gates.data() + Offset + 4, 0xd4200000);
    }
  }
  if (auto E = Space.write(value::LibSystemGate, Gates))
    return E;
  return Space.protect(value::LibSystemGate, Page,
                       Read | Execute | UserAccessible);
}
llvm::Expected<bool>
invokeLibSystem(ExecutionBackend &CPU, DarwinMemory &Memory, DarwinFiles &Files,
                DarwinEntropy &Entropy, const ProcessImage &Image,
                LibSystemGateKind Binding, const ServiceRequest &Request,
                const ProcessOptions &Options, ProcessResult &Result,
                DarwinCallbacks *Callbacks) {
  if (Binding == LibSystemGateKind::StubBinder ||
      Binding == LibSystemGateKind::TLVBootstrap)
    return failure(diagnostic::LibSystemResolver);
  const auto &Definition = libSystemBinding(Binding);
  const bool X64 = Image.Architecture == GuestArchitecture::X64;
  auto ABI = IntegerABI::get(X64 ? IntegerCallingConvention::SysVAMD64
                                 : IntegerCallingConvention::AAPCS64);
  if (!ABI)
    return ABI.takeError();
  auto SP = CPU.readRegister(ABI->info().StackPointer);
  if (!SP)
    return SP.takeError();
  auto ReturnSP = ABI->returnStackPointer((*SP)[0]);
  if (!ReturnSP)
    return ReturnSP.takeError();
  auto Arguments = readLibSystemArguments(CPU, *ABI, (*SP)[0], Binding);
  if (!Arguments)
    return Arguments.takeError();
  NativeCallEvent Call;
  Call.PC = Request.PC;
  Call.Name = Definition.Symbol.str();
  Call.Module = Definition.Provider.str();
  Call.ArgumentCount = Arguments->Count;
  std::copy_n(Arguments->Carriers.begin(), Arguments->Count,
              Call.Arguments.begin());
  Result.NativeCalls.push_back(std::move(Call));
  uint64_t ReturnPC = 0;
  if (Definition.Kernel != ServiceKind::Exit &&
      Binding != LibSystemGateKind::NormalExit) {
    auto Address = ABI->readReturnAddress(CPU, (*SP)[0]);
    if (!Address)
      return Address.takeError();
    ReturnPC = *Address;
    auto Executable =
        CPU.canAccess(ReturnPC, X64 ? 1 : 4, Execute | UserAccessible);
    if (!Executable)
      return Executable.takeError();
    if (!*Executable || (!X64 && ReturnPC % 4))
      return failure(diagnostic::LibSystemFrame);
    Result.NativeCalls.back().ReturnAddress = ReturnPC;
  }
  const bool GlobalRegistration = isGlobalCallbackRegistration(Binding);
  if (Binding == LibSystemGateKind::TLVAtExit || GlobalRegistration ||
      Binding == LibSystemGateKind::NormalExit) {
    if (!Callbacks)
      return failure("Darwin callback lifecycle is unavailable");
    if (Binding == LibSystemGateKind::NormalExit)
      return Callbacks->exit(CPU, Arguments->Carriers[0], Result);
    if (GlobalRegistration) {
      const auto Object = Binding == LibSystemGateKind::CxaAtExit
                              ? std::optional<uint64_t>(Arguments->Carriers[1])
                              : std::nullopt;
      // The DSO carrier belongs to the event. Process exit does not interpret
      // it as an address or admit selective DSO finalization.
      if (auto E = Callbacks->addGlobal(CPU, Arguments->Carriers[0], Object))
        return std::move(E);
    } else {
      if (auto E = Callbacks->addTLS(CPU, Arguments->Carriers[0],
                                     Arguments->Carriers[1]))
        return std::move(E);
      // The actual runtime entry is void(function, object). Clang can supply
      // an unused third DSO carrier and an unused result; neither is consumed.
      return publishReturn(CPU, *ABI, *ReturnSP, ReturnPC, std::nullopt,
                           Result);
    }
  }
  if (isGuard(Binding)) {
    auto Value = invokeGuard(CPU, Binding, Arguments->Carriers[0], Options);
    if (!Value)
      return Value.takeError();
    return publishReturn(CPU, *ABI, *ReturnSP, ReturnPC, *Value, Result);
  }
  uint64_t Value =
      GlobalRegistration ? 0 : value::LibSystemGate + Image.Memory.PageSize;
  if (Definition.Kernel) {
    ProcessServiceEvent Event{Request.PC, 0, Arguments->Kernel, std::nullopt};
    auto Invoke = [&]() -> llvm::Expected<std::optional<ServiceResult>> {
      if (Arguments->Error)
        return std::optional<ServiceResult>({*Arguments->Error, true});
      return handleService(*Definition.Kernel, CPU, Memory, Files, Entropy,
                           Event, Options, Result);
    };
    auto Returned = Invoke();
    if (!Returned)
      return Returned.takeError();
    if (!*Returned)
      return false;
    Value = (**Returned).Value;
    if ((**Returned).Error) {
      if (auto E = CPU.writeInteger(
              value::LibSystemGate + Image.Memory.PageSize, Value, 4))
        return std::move(E);
      Value = UINT64_MAX;
    }
  }
  return publishReturn(CPU, *ABI, *ReturnSP, ReturnPC, Value, Result);
}
} // namespace neverd::emulation::darwin_model
