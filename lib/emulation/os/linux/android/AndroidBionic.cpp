//===- AndroidBionic.cpp - Explicit bounded Bionic call models -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::android_model {
llvm::Error Bionic::access(uint64_t Address, uint64_t Size,
                           unsigned Permissions) {
  if (!Budget.remainingMicroseconds()) {
    Expired = true;
    return failure("native call model timed out");
  }
  if (!Size)
    return llvm::Error::success();
  if (Size > Options.MemoryLimit)
    return failure("native memory operation exceeds memory limit");
  auto Allowed = CPU.canAccess(Address, Size, Permissions | UserAccessible);
  if (!Allowed)
    return Allowed.takeError();
  if (!*Allowed)
    return failure("invalid guest pointer in Bionic call");
  return llvm::Error::success();
}
llvm::Expected<uint8_t> Bionic::byte(uint64_t Address) {
  if (auto E = access(Address, 1, Read))
    return std::move(E);
  uint8_t Value;
  if (auto E = CPU.read(Address, llvm::MutableArrayRef<uint8_t>(&Value, 1)))
    return std::move(E);
  return Value;
}
llvm::Expected<std::string> Bionic::string(uint64_t Address) {
  std::string Text;
  for (uint64_t I = 0; I < Options.MemoryLimit && Address <= UINT64_MAX - I;
       ++I) {
    auto V = byte(Address + I);
    if (!V)
      return V.takeError();
    if (!*V)
      return Text;
    Text.push_back(*V);
  }
  return failure("unterminated guest string");
}
llvm::Error Bionic::setErrno(uint32_t Value) {
  uint8_t Bytes[4];
  llvm::support::endian::write32le(Bytes, Value);
  return CPU.write(ErrnoAddress, Bytes);
}
llvm::Expected<uint64_t> Bionic::allocate(uint64_t Size) {
  uint64_t Effective = std::max(uint64_t(1), Size);
  if (Effective > Options.MemoryLimit ||
      Effective > UINT64_MAX - PageSize + 1) {
    if (auto E = setErrno(linux_model::NoMemory))
      return std::move(E);
    return uint64_t(0);
  }
  uint64_t Mapped = (Effective + PageSize - 1) & ~(PageSize - 1);
  ProcessServiceEvent Event{
      0,
      0,
      {0, Mapped, linux_model::ProtRead | linux_model::ProtWrite,
       linux_model::MapPrivate | linux_model::MapAnonymous, UINT64_MAX, 0},
      std::nullopt};
  auto Address = Memory.handle(linux_model::ServiceKind::Mmap, Event, Result);
  if (!Address)
    return Address.takeError();
  if (!*Address)
    return failure("anonymous allocator did not return");
  if (**Address >= uint64_t(0) - 4095) {
    if (auto E = setErrno(uint64_t(0) - **Address))
      return std::move(E);
    return uint64_t(0);
  }
  Allocations.emplace(**Address, Allocation{Size, Mapped});
  return **Address;
}
llvm::Error Bionic::release(uint64_t Address) {
  if (!Address)
    return llvm::Error::success();
  auto I = Allocations.find(Address);
  if (I == Allocations.end())
    return failure("free/realloc does not name a live allocation");
  ProcessServiceEvent Event{
      0, 0, {Address, I->second.MappedSize, 0, 0, 0, 0}, std::nullopt};
  auto Returned =
      Memory.handle(linux_model::ServiceKind::Munmap, Event, Result);
  if (!Returned)
    return Returned.takeError();
  if (!*Returned || **Returned)
    return failure("could not release native allocation");
  Allocations.erase(I);
  return llvm::Error::success();
}
llvm::Expected<uint64_t> Bionic::linkerError(llvm::StringRef Message,
                                             uint64_t ReturnValue) {
  // A fixed guest buffer, separate from errno, TLS ABI slots and constructor
  // argv/envp. A later error may replace its contents; dlerror consumes it
  // once.
  if (Message.size() >= 1024)
    return failure("dynamic linker error exceeds its guest buffer");
  std::vector<uint8_t> Bytes(Message.bytes_begin(), Message.bytes_end());
  Bytes.push_back(0);
  if (auto E = access(LinkerErrorAddress, Bytes.size(), Write))
    return std::move(E);
  if (auto E = CPU.write(LinkerErrorAddress, Bytes))
    return std::move(E);
  uint8_t Pointer[8];
  llvm::support::endian::write64le(Pointer, LinkerErrorAddress);
  if (auto E = access(LinkerErrorSlot, sizeof(Pointer), Write))
    return std::move(E);
  if (auto E = CPU.write(LinkerErrorSlot, Pointer))
    return std::move(E);
  return ReturnValue;
}
llvm::Expected<std::optional<uint64_t>> Bionic::dlfcn(NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto Value = [](uint64_t V) { return std::optional<uint64_t>(V); };
  auto Error = [&](llvm::StringRef Message,
                   uint64_t V = 0) -> llvm::Expected<std::optional<uint64_t>> {
    auto R = linkerError(Message, V);
    if (!R)
      return R.takeError();
    return Value(*R);
  };
  auto Unsupported =
      [&](llvm::StringRef Detail) -> llvm::Expected<std::optional<uint64_t>> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = "unmodeled Android dynamic linking: " + Detail.str();
    return std::optional<uint64_t>();
  };
  if (Call.Name == "dlerror") {
    uint8_t Pointer[8], Empty[8]{};
    if (auto E = access(LinkerErrorSlot, sizeof(Pointer), Read | Write))
      return std::move(E);
    if (auto E = CPU.read(LinkerErrorSlot, Pointer))
      return std::move(E);
    uint64_t Address = llvm::support::endian::read64le(Pointer);
    if (auto E = CPU.write(LinkerErrorSlot, Empty))
      return std::move(E);
    return Value(Address);
  }
  if (Call.Name == "dlopen") {
    if (!A[0])
      return Unsupported("dlopen(NULL) requires a process-wide symbol scope");
    auto Library = string(A[0]);
    if (!Library)
      return Library.takeError();
    if (Library->size() > 1024)
      return failure("dynamic library name exceeds the model limit");
    Call.Library = *Library;
    // API 28 LP64 flags. Scope promotion and NODELETE need a fuller loader.
    uint32_t Flags = static_cast<uint32_t>(A[1]);
    if ((Flags & ~7u) || ((Flags & 3u) != 1 && (Flags & 3u) != 2))
      return Unsupported("only LAZY/NOW with optional NOLOAD are modeled");
    if (!Linked.Libraries.count(*Library))
      return Error("library is absent from the explicit local catalogue");
    auto &State = OpenLibraries[*Library];
    if (!State.References && (Flags & 4u))
      return Error("library is not open (NOLOAD)");
    if (!State.References) {
      if (NextHandle > UINT64_MAX - 2)
        return failure("dynamic library handle space exhausted");
      State.Handle = NextHandle;
      NextHandle += 2;
      Handles.emplace(State.Handle, *Library);
    }
    if (State.References == UINT64_MAX)
      return failure("dynamic library reference count overflow");
    ++State.References;
    return Value(State.Handle);
  }
  if (Call.Name == "dlsym") {
    if (!A[1])
      return Error("dynamic symbol name is null");
    auto Symbol = string(A[1]);
    if (!Symbol)
      return Symbol.takeError();
    if (Symbol->size() > 1024)
      return failure("dynamic symbol name exceeds the model limit");
    Call.Symbol = *Symbol;
    if (!A[0] || A[0] == UINT64_MAX)
      return Unsupported(
          "RTLD_DEFAULT / RTLD_NEXT require ordered process scopes");
    auto Handle = Handles.find(A[0]);
    if (Handle == Handles.end())
      return Error("invalid or closed dynamic library handle");
    Call.Library = Handle->second;
    const auto &Symbols = Linked.Libraries.at(Handle->second);
    auto I = Symbols.find(*Symbol);
    if (I == Symbols.end())
      return Error("symbol is absent from the explicit library catalogue");
    return Value(I->second);
  }
  if (Call.Name == "dlclose") {
    auto Handle = Handles.find(A[0]);
    if (Handle == Handles.end())
      return Error("invalid or closed dynamic library handle", UINT64_MAX);
    Call.Library = Handle->second;
    auto &State = OpenLibraries.at(Handle->second);
    if (!--State.References)
      Handles.erase(Handle);
    return Value(0);
  }
  return failure("invalid dlfcn model dispatch");
}
llvm::Expected<std::optional<uint64_t>> Bionic::invoke(NativeCallEvent &Call) {
  llvm::StringRef Name(Call.Name);
  const auto &A = Call.Arguments;
  auto Value = [](uint64_t V) { return std::optional<uint64_t>(V); };
  if (!Call.Library.empty()) {
    auto I = OpenLibraries.find(Call.Library);
    if (I == OpenLibraries.end() || !I->second.References) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic =
          "call through an inactive dynamic library: " + Call.Library;
      return std::optional<uint64_t>();
    }
  }
  if (Name == "dlopen" || Name == "dlsym" || Name == "dlclose" ||
      Name == "dlerror")
    return dlfcn(Call);
  if (Name == "__errno")
    return Value(ErrnoAddress);
  if (Name == "android_get_device_api_level")
    return Value(28);
  if (Name == "getpid")
    return Value(linux_model::ProcessID);
  if (Name == "gettid")
    return Value(linux_model::ThreadID);
  if (Name == "__stack_chk_fail" || Name == "abort")
    return failure("guest called " + Name);
  if (Name == "memcpy" || Name == "memmove" || Name == "memset" ||
      Name == "memcmp") {
    uint64_t Size = A[2];
    if (auto E = access(A[0], Size, Name == "memcmp" ? Read : Write))
      return std::move(E);
    if (Name != "memset")
      if (auto E = access(A[1], Size, Read))
        return std::move(E);
    if (Name == "memcpy" && Size &&
        (A[0] <= A[1] ? A[1] - A[0] < Size : A[0] - A[1] < Size))
      return failure("overlapping memcpy operands");
    std::vector<uint8_t> Bytes(Size, static_cast<uint8_t>(A[1]));
    if (Name != "memset" && Size)
      if (auto E = CPU.read(A[1], Bytes))
        return std::move(E);
    if (Name == "memcmp") {
      std::vector<uint8_t> Left(Size);
      if (Size)
        if (auto E = CPU.read(A[0], Left))
          return std::move(E);
      for (uint64_t I = 0; I < Size; ++I)
        if (Left[I] != Bytes[I])
          return Value(static_cast<uint32_t>(int(Left[I]) - int(Bytes[I])));
      return Value(0);
    }
    if (Size)
      if (auto E = CPU.write(A[0], Bytes))
        return std::move(E);
    return Value(A[0]);
  }
  if (Name == "strlen" || Name == "strnlen") {
    uint64_t Bound = Name == "strnlen" ? std::min(A[1], Options.MemoryLimit)
                                       : Options.MemoryLimit;
    for (uint64_t I = 0; I < Bound; ++I) {
      if (A[0] > UINT64_MAX - I)
        return failure("string address overflows");
      auto V = byte(A[0] + I);
      if (!V)
        return V.takeError();
      if (!*V)
        return Value(I);
    }
    if (Name == "strnlen" && Bound == A[1])
      return Value(Bound);
    return failure("string scan exceeds memory limit");
  }
  if (Name == "strcmp" || Name == "strncmp") {
    uint64_t Bound = Name == "strncmp" ? std::min(A[2], Options.MemoryLimit)
                                       : Options.MemoryLimit;
    for (uint64_t I = 0; I < Bound; ++I) {
      if (A[0] > UINT64_MAX - I || A[1] > UINT64_MAX - I)
        return failure("string address overflows");
      auto Left = byte(A[0] + I);
      if (!Left)
        return Left.takeError();
      auto Right = byte(A[1] + I);
      if (!Right)
        return Right.takeError();
      if (*Left != *Right)
        return Value(static_cast<uint32_t>(int(*Left) - int(*Right)));
      if (!*Left)
        return Value(0);
    }
    if (Name == "strncmp" && Bound == A[2])
      return Value(0);
    return failure("string comparison exceeds memory limit");
  }
  if (Name == "__system_property_get") {
    auto Key = string(A[0]);
    if (!Key)
      return Key.takeError();
    auto I = Options.Android->Properties.find(*Key);
    std::string Text = I == Options.Android->Properties.end() ? "" : I->second;
    if (auto E = access(A[1], Text.size() + 1, Write))
      return std::move(E);
    std::vector<uint8_t> Bytes(Text.begin(), Text.end());
    Bytes.push_back(0);
    if (auto E = CPU.write(A[1], Bytes))
      return std::move(E);
    return Value(Text.size());
  }
  if (Name == "malloc" || Name == "calloc" || Name == "realloc") {
    uint64_t Size = Name == "realloc" ? A[1] : A[0];
    if (Name == "calloc") {
      if (A[0] && A[1] > UINT64_MAX / A[0]) {
        if (auto E = setErrno(linux_model::NoMemory))
          return std::move(E);
        return Value(0);
      }
      Size = A[0] * A[1];
    }
    std::vector<uint8_t> Saved;
    if (Name == "realloc" && A[0]) {
      auto I = Allocations.find(A[0]);
      if (I == Allocations.end())
        return failure("realloc does not name a live allocation");
      uint64_t Copy = std::min(Size, I->second.Size);
      if (auto E = access(A[0], Copy, Read))
        return std::move(E);
      Saved.resize(Copy);
      if (Copy)
        if (auto E = CPU.read(A[0], Saved))
          return std::move(E);
    }
    auto Address = allocate(Size);
    if (!Address)
      return Address.takeError();
    if (!*Address)
      return Value(0);
    // Anonymous allocations start zeroed, including calloc and realloc growth.
    if (!Saved.empty())
      if (auto E = CPU.write(*Address, Saved))
        return std::move(E);
    if (Name == "realloc")
      if (auto E = release(A[0]))
        return std::move(E);
    return Value(*Address);
  }
  if (Name == "free") {
    if (auto E = release(A[0]))
      return std::move(E);
    return Value(0); // The ABI leaves x0 unspecified for void calls.
  }
  std::optional<linux_model::ServiceKind> Kind;
  if (Name == "mmap" || Name == "mmap64")
    Kind = linux_model::ServiceKind::Mmap;
  if (Name == "mprotect")
    Kind = linux_model::ServiceKind::Mprotect;
  if (Name == "munmap")
    Kind = linux_model::ServiceKind::Munmap;
  if (Name == "write")
    Kind = linux_model::ServiceKind::Write;
  if (Kind) {
    ProcessServiceEvent Event{Call.PC, 0, {}, std::nullopt};
    std::copy_n(A.begin(), Event.Arguments.size(), Event.Arguments.begin());
    // Raw Linux service semantics are shared. Bionic alone owns errno/-1.
    auto Returned = linux_model::handleService(CPU, Memory, *Kind, Event,
                                               Layout, Options, Result);
    if (!Returned)
      return Returned.takeError();
    if (!*Returned)
      return std::optional<uint64_t>();
    if (**Returned >= uint64_t(0) - 4095) {
      if (auto E = setErrno(uint64_t(0) - **Returned))
        return std::move(E);
      return Value(UINT64_MAX);
    }
    return *Returned;
  }
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = "unmodeled Android import: " + Name.str();
  return std::optional<uint64_t>();
}
} // namespace neverd::emulation::android_model
