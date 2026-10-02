//===- WindowsProcessServices.cpp - Bounded Win32 user API models --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcess.h"

#include "neverd/emulation/CPU.h"

#include <algorithm>

namespace neverd::emulation::windows_process {
namespace {
using namespace value;
constexpr Service Registry[] = {
#define NEVERD_WINDOWS_PROCESS_API(Name, Provider, Count, Returns)             \
  {API::Name, #Name, Count, APIProvider::Provider, Returns},
#include "WindowsProcessServices.def"
#undef NEVERD_WINDOWS_PROCESS_API
};
static_assert((MaxImports + FirstImportGate) * GateStride <= GateSize);
static_assert([] {
  for (auto &S : Registry)
    if (S.Arguments > std::tuple_size_v<decltype(NativeCallEvent::Arguments)>)
      return false;
  return true;
}());
} // namespace
llvm::ArrayRef<Service> services() { return Registry; }
std::optional<APIProvider> findProvider(llvm::StringRef Module) {
  const auto Lower = Module.lower();
  if (Lower == text::NTDLL)
    return APIProvider::Native;
  if (Lower == text::Kernel32 || Lower == text::KernelBase)
    return APIProvider::Kernel;
  return std::nullopt;
}
const Service *findService(llvm::StringRef Module, llvm::StringRef Name) {
  const auto Provider = findProvider(Module);
  for (const auto &S : Registry)
    if (Name == S.Name && Provider == S.Provider)
      return &S;
  return nullptr;
}
std::optional<uint64_t> Services::unsupported(const Service &S) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = std::string(text::ServiceArguments) + S.Name;
  return std::nullopt;
}
llvm::Expected<bool> Services::access(uint64_t Address, uint64_t Size,
                                      unsigned Rights) {
  if (!Size)
    return true;
  if (Address < ImageAlignment || Address >= UserLimit ||
      Size > UserLimit - Address)
    return false;
  return CPU.canAccess(Address, Size, Rights | UserAccessible);
}
llvm::Expected<uint64_t> Services::error(uint32_t Code, uint64_t ReturnValue) {
  if (auto E = CPU.writeInteger(TEB + TebLastError, Code, DWordSize))
    return std::move(E);
  return ReturnValue;
}
llvm::Expected<std::optional<uint64_t>>
Services::heap(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  if (A[0] != HeapHandle)
    return unsupported(S);
  const uint32_t Flags = A[1];
  const uint32_t Allowed =
      HeapNoSerialize | (S.Kind == API::HeapAlloc ? HeapZeroMemory : 0);
  if (Flags & ~Allowed)
    return unsupported(S);
  auto Found = Allocations.find(A[2]);
  if (S.Kind == API::HeapSize)
    return std::optional<uint64_t>(
        Found == Allocations.end() ? UINT64_MAX : Found->second.Size);
  if (S.Kind == API::HeapFree) {
    if (!A[2])
      return std::optional<uint64_t>(1);
    if (Found == Allocations.end())
      return std::optional<uint64_t>(0);
    if (auto E = Memory.unmap(Found->first, Found->second.MappedSize))
      return std::move(E);
    Allocations.erase(Found);
    return std::optional<uint64_t>(1);
  }
  const uint64_t Size = A[2];
  if (Size > Options.MemoryLimit || Size > UINT64_MAX - PageSize)
    return std::optional<uint64_t>(0);
  const uint64_t Mapped =
      (std::max<uint64_t>(Size, 1) + PageSize - 1) & ~(PageSize - 1);
  uint64_t Address = HeapBase;
  for (const auto &[Start, Allocation] : Allocations) {
    if (Mapped <= Start - Address)
      break;
    Address = Start + Allocation.MappedSize;
  }
  if (Address >= HeapLimit || Mapped > HeapLimit - Address ||
      Mapped > Options.MemoryLimit - Memory.mappedBytes())
    return std::optional<uint64_t>(0);
  if (auto E = Memory.map(Address, Mapped, Read | Write | UserAccessible)) {
    bool Exhausted = false;
    E = llvm::handleErrors(
        std::move(E), [&](const GuestMemoryLimitError &) { Exhausted = true; });
    if (E)
      return std::move(E);
    if (Exhausted)
      return std::optional<uint64_t>(0);
  }
  Allocations.emplace(Address, Allocation{Size, Mapped});
  // Fresh guest backing is zero-filled. With no HEAP_ZERO_MEMORY flag its
  // contents are unspecified; the model's deterministic zeroes are permitted.
  return std::optional<uint64_t>(Address);
}
llvm::Expected<std::optional<uint64_t>>
Services::invoke(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto WinError = [&](uint32_t Code,
                      uint64_t ReturnValue =
                          0) -> llvm::Expected<std::optional<uint64_t>> {
    auto V = error(Code, ReturnValue);
    if (!V)
      return V.takeError();
    return std::optional<uint64_t>(*V);
  };
  auto Value = [](uint64_t N) -> llvm::Expected<std::optional<uint64_t>> {
    return std::optional<uint64_t>(N);
  };
  switch (S.Kind) {
  case API::ExitProcess:
  case API::RtlExitUserProcess:
    Result.ExitStatus = uint32_t(A[0]);
    Result.Stop = ProcessStopReason::Exited;
    return std::nullopt;
  case API::GetLastError: {
    auto V = CPU.readInteger(TEB + TebLastError, DWordSize);
    if (!V)
      return V.takeError();
    return Value(*V);
  }
  case API::SetLastError:
    if (auto E =
            CPU.writeInteger(TEB + TebLastError, uint32_t(A[0]), DWordSize))
      return std::move(E);
    return Value(0);
  case API::GetCurrentProcessId:
    return Value(ProcessID);
  case API::GetCurrentThreadId:
    return Value(ThreadID);
  case API::GetCurrentProcess:
    return Value(CurrentProcess);
  case API::GetCurrentThread:
    return Value(CurrentThread);
  case API::GetCommandLineW:
    return Value(Env.CommandLine);
  case API::GetProcessHeap:
    return Value(HeapHandle);
  case API::VirtualAlloc:
  case API::VirtualFree:
  case API::VirtualProtect:
  case API::VirtualQuery:
  case API::FlushInstructionCache:
    return memory(S, Event);
  case API::HeapAlloc:
  case API::HeapFree:
  case API::HeapSize:
    return heap(S, Event);
  case API::GetStdHandle:
    switch (uint32_t(A[0])) {
    case StdInputSelector:
      return Value(StandardInput);
    case StdOutputSelector:
      return Value(StandardOutput);
    case StdErrorSelector:
      return Value(StandardError);
    default:
      return WinError(ErrorInvalidParameter, InvalidHandle);
    }
  case API::WriteFile: {
    // Only synchronous writes to the two explicit byte sinks are modeled.
    // Check every argument before publishing output or a guest completion.
    if (A[4])
      return unsupported(S);
    const uint64_t Count = uint32_t(A[2]);
    auto Written = access(A[3], DWordSize, Write);
    if (!Written)
      return Written.takeError();
    if (!*Written) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = text::UserException;
      return std::nullopt;
    }
    if (A[0] != StandardOutput && A[0] != StandardError) {
      if (auto E = CPU.writeInteger(A[3], 0, DWordSize))
        return std::move(E);
      return WinError(ErrorInvalidHandle);
    }
    auto Bytes = access(A[1], Count, Read);
    if (!Bytes)
      return Bytes.takeError();
    if (!*Bytes) {
      if (auto E = CPU.writeInteger(A[3], 0, DWordSize))
        return std::move(E);
      return WinError(ErrorInvalidUserBuffer);
    }
    if (Count > Options.OutputLimit - Result.StandardOutput.size() -
                    Result.StandardError.size()) {
      Result.Stop = ProcessStopReason::OutputLimit;
      Result.Diagnostic = text::Output;
      return std::nullopt;
    }
    // Win32 clears the completion count before copying the input. In
    // particular, the buffer may overlap this DWORD or a return-address slot.
    if (auto E = CPU.writeInteger(A[3], 0, DWordSize))
      return std::move(E);
    std::vector<uint8_t> Data(Count);
    if (Count)
      if (auto E = CPU.read(A[1], Data))
        return std::move(E);
    if (auto E = CPU.writeInteger(A[3], Count, DWordSize))
      return std::move(E);
    auto &Output =
        A[0] == StandardOutput ? Result.StandardOutput : Result.StandardError;
    Output.append(Data.begin(), Data.end());
    return Value(1);
  }
  case API::TlsAlloc:
    for (uint32_t I = 0; I < DynamicTLSCount; ++I) {
      if (TLSSlots[I])
        continue;
      if (auto E = CPU.writeInteger(TEB + TebTLSSlots + I * PointerSize, 0,
                                    PointerSize))
        return std::move(E);
      TLSSlots.set(I);
      return Value(I);
    }
    return WinError(ErrorNotEnoughMemory, TLSOutOfIndexes);
  case API::TlsFree:
  case API::TlsSetValue:
  case API::TlsGetValue: {
    const uint32_t Index = A[0];
    if (Index >= DynamicTLSCount ||
        (S.Kind == API::TlsFree && !TLSSlots[Index]))
      return WinError(ErrorInvalidParameter);
    const uint64_t Address = TEB + TebTLSSlots + Index * PointerSize;
    if (S.Kind == API::TlsGetValue) {
      auto V = CPU.readInteger(Address, PointerSize);
      if (!V)
        return V.takeError();
      return WinError(ErrorSuccess, *V);
    }
    if (S.Kind == API::TlsFree) {
      if (auto E = CPU.writeInteger(Address, 0, PointerSize))
        return std::move(E);
      TLSSlots.reset(Index);
    } else if (auto E = CPU.writeInteger(Address, A[1], PointerSize))
      return std::move(E);
    return Value(1);
  }
  case API::GetModuleHandleW:
    if (!A[0])
      return Value(Loaded.Base);
    // Module names beyond the sole mapped image require a real module model;
    // never invent handles to the named API provider families.
    return unsupported(S);
  }
  return failure(text::Service);
}
} // namespace neverd::emulation::windows_process
