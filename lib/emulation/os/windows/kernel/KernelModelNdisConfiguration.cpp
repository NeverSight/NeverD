///
/// \file
/// KernelModelNdisConfiguration.
///

#include "../WindowsUnicode.h"
#include "KernelModel.h"
#include "KernelModelRuntime.h"
#include "WindowsKernelLayout.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
namespace neverd::emulation {
namespace {
constexpr uint32_t Failure = 0xc0000001, NdisResources = 0xc000009a,
                   HandleTag = 0x6863444e, ParameterTag = 0x6e70444e,
                   NameTag = 0x2020444e;
llvm::Error invalid(const llvm::Twine &T) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "NDIS configuration: " + T);
}
bool overlap(uint64_t A, uint64_t N, uint64_t B, uint64_t M) {
  return N && M && A < B + M && B < A + N;
}
} // namespace
llvm::Error KernelModel::initializeNdisConfiguration() {
  if (!NdisConfiguration)
    return llvm::Error::success();
  for (const auto &[Name, Value] :
       std::initializer_list<std::pair<const char *, uint32_t>>{
           {"processortype", 4},
           {"environment", 1},
           {"ndisversion", NdisConfiguration->Version.value_or(0)}}) {
    auto P = allocate(24, 8);
    if (!P)
      return P.takeError();
    if (auto E = Memory.writeInteger(*P, 0, 4))
      return E;
    if (auto E = Memory.writeInteger(*P + 8, Value, 4))
      return E;
    NdisConfigurationAllocations.emplace(
        *P, NdisConfigurationStorage{24, {{*P, 4}, {*P + 8, 4}}});
    NdisPredefinedParameters.emplace(Name, *P);
  }
  return llvm::Error::success();
}
llvm::Error KernelModel::validateNdisConfigurationAccess(uint64_t A, uint32_t N,
                                                         bool Write) const {
  for (const auto &[Base, S] : NdisConfigurationAllocations)
    if (overlap(A, N, Base, S.Size)) {
      if (!Write)
        for (const auto &[At, Size] : S.Readable)
          if (A >= At && A - At <= Size && N <= Size - (A - At))
            return llvm::Error::success();
      return invalid("access to opaque or read-only configuration storage");
    }
  return llvm::Error::success();
}
llvm::Expected<uint64_t>
KernelModel::callNdisConfiguration(llvm::StringRef API,
                                   llvm::ArrayRef<uint64_t> A) {
  if (API == "NdisCloseConfiguration") {
    auto H = NdisConfigurationHandles.find(A[0]);
    if (H == NdisConfigurationHandles.end())
      return invalid("close requires a live owned configuration handle");
    for (auto Base : H->second.Allocations)
      if (auto E = canReleaseRange(Base, Allocations.at(Base).Size))
        return E;
    ReleasingNdisConfiguration = true;
    llvm::scope_exit Reset([&] { ReleasingNdisConfiguration = false; });
    for (auto Base : H->second.Allocations) {
      auto R = freePool({Base, 0}, true);
      if (!R)
        return R.takeError();
      NdisConfigurationAllocations.erase(Base);
    }
    NdisConfigurationHandles.erase(H);
    return 0;
  }
  if (API == "NdisOpenConfigurationEx") {
    if (auto E = objectBuffer(A[0], 20, false))
      return E;
    auto Type = Memory.readInteger(A[0], 1);
    if (!Type)
      return Type.takeError();
    auto Revision = Memory.readInteger(A[0] + 1, 1);
    if (!Revision)
      return Revision.takeError();
    auto Size = Memory.readInteger(A[0] + 2, 2);
    if (!Size)
      return Size.takeError();
    if (*Type != 0xa9 || *Revision < 1 || *Size < 20)
      return uint64_t(0xc000000d);
    auto Handle = Memory.readInteger(A[0] + 8, 8);
    if (!Handle)
      return Handle.takeError();
    auto Flags = Memory.readInteger(A[0] + 16, 4);
    if (!Flags)
      return Flags.takeError();
    if (*Flags)
      return invalid(
          "nonzero configuration flags require an instance-specific provider");
    if (!NdisConfiguration)
      return invalid("requires explicit ndis_configuration bindings");
    const DriverNdisConfigurationBinding *Binding = nullptr;
    for (const auto &B : NdisConfiguration->Bindings)
      if (B.Handle == *Handle)
        Binding = &B;
    if (!Binding)
      return invalid("unknown external NDIS binding identity");
    if (RegistryCallback || !RegistryNotifications.empty())
      return invalid("registry filter continuations for NDIS configuration are "
                     "unsupported");
    if (auto E = objectBuffer(A[1], 8, true))
      return E;
    if (overlap(A[0], 20, A[1], 8))
      return invalid("aliased configuration object/output is unsupported");
    if (NdisConfigurationHandles.size() >= 256)
      return uint64_t(NdisResources);
    auto P = allocatePool({0x200, 40, HandleTag}, false);
    if (!P)
      return P.takeError();
    if (!*P)
      return uint64_t(NdisResources);
    if (auto E = Memory.writeInteger(A[1], *P, 8)) {
      auto R = freePool({*P, 0}, true);
      if (!R)
        E = llvm::joinErrors(std::move(E), R.takeError());
      return E;
    }
    NdisConfigurationHandles.emplace(
        *P, NdisConfigurationHandle{Binding->RegistryPath, {}, {*P}});
    NdisConfigurationAllocations.emplace(*P, NdisConfigurationStorage{40, {}});
    return 0;
  }
  auto H = NdisConfigurationHandles.find(A[2]);
  if (H == NdisConfigurationHandles.end())
    return invalid("read requires a live owned configuration handle");
  if (auto E = objectBuffer(A[0], 4, true))
    return E;
  if (auto E = objectBuffer(A[1], 8, true))
    return E;
  if (overlap(A[0], 4, A[1], 8))
    return invalid("aliased status/parameter outputs are unsupported");
  auto Keyword = readObjectName(A[3]);
  if (!Keyword)
    return Keyword.takeError();
  if (Keyword->empty() || Keyword->size() > MaxRegistryValueNameLength)
    return invalid("keyword must be a bounded nonempty ASCII registry name");
  auto Status = [&](uint32_t S) -> llvm::Expected<uint64_t> {
    if (auto E = Memory.writeInteger(A[0], S, 4))
      return E;
    return 0;
  };
  const auto Key = llvm::StringRef(*Keyword).lower();
  uint32_t Type = 0, Integer = 0;
  std::vector<uint8_t> Data;
  if (auto Predefined = NdisPredefinedParameters.find(Key);
      Predefined != NdisPredefinedParameters.end()) {
    if (Key == "ndisversion" && !NdisConfiguration->Version)
      return invalid("NdisVersion requires an explicit observed NDIS version");
    if (auto E = Memory.writeInteger(A[1], Predefined->second, 8))
      return E;
    return Status(0);
  } else {
    if (RegistryCallback || !RegistryNotifications.empty())
      return invalid("registry filter continuations for NDIS configuration are "
                     "unsupported");
    if (!H->second.Key) {
      auto K = Registry.providerKey(H->second.RegistryPath);
      if (!K)
        return K.takeError();
      if (!*K)
        return Status(Failure);
      H->second.Key = **K;
    }
    auto V = Registry.providerValue(*H->second.Key, *Keyword);
    if (!V)
      return V.takeError();
    if (V->Status)
      return Status(Failure);
    const auto &Value = *V->Value;
    if (Value.Type == 4) {
      if (Value.Data.size() != 4)
        return invalid("REG_DWORD requires exactly four bytes");
      Integer = llvm::support::endian::read32le(Value.Data.data());
    } else if (Value.Type == 1 || Value.Type == 7) {
      Data = Value.Data;
      Type = Value.Type == 1 ? 2 : 3;
      if (Data.size() > 65534 || (Data.size() & 1) ||
          (Value.Type == 1 && Data.size() < 2) ||
          (Value.Type == 7 &&
           (Data.size() < 4 || Data[Data.size() - 1] || Data[Data.size() - 2] ||
            Data[Data.size() - 3] || Data[Data.size() - 4])))
        return invalid(
            "requires a bounded well-formed REG_SZ/REG_MULTI_SZ payload");
      std::vector<uint16_t> Units;
      for (size_t I = 0; I < Data.size(); I += 2)
        Units.push_back(llvm::support::endian::read16le(Data.data() + I));
      auto UTF8 = windows_unicode::utf8(Units);
      if (!UTF8)
        return invalid(llvm::toString(UTF8.takeError()));
    } else if (Value.Type == 3) {
      Data = Value.Data;
      Type = 4;
      if (Data.size() > 65535)
        return invalid("binary payload exceeds NDIS USHORT length");
    } else
      return invalid(
          "registry type is outside the admitted NDIS parameter contract");
  }
  // The original read allocates a temporary NUL-terminated keyword before
  // querying the registry. All negative query/allocation statuses collapse to
  // NDIS_STATUS_FAILURE in this pinned implementation.
  auto Name = allocatePool({0x200, Keyword->size() * 2 + 2, NameTag}, false);
  if (!Name)
    return Name.takeError();
  if (!*Name)
    return Status(Failure);
  const uint64_t NameBuffer = *Name;
  if (H->second.Allocations.size() >= 257) {
    if (NameBuffer) {
      auto R = freePool({NameBuffer, 0}, true);
      if (!R)
        return R.takeError();
    }
    return Status(Failure);
  }
  const uint64_t Bytes = 32 + Data.size();
  auto P = allocatePool({0x200, Bytes, ParameterTag}, false);
  if (NameBuffer) {
    auto R = freePool({NameBuffer, 0}, true);
    if (!R) {
      auto E = R.takeError();
      if (!P)
        E = llvm::joinErrors(std::move(E), P.takeError());
      else if (*P) {
        auto Cleanup = freePool({*P, 0}, true);
        if (!Cleanup)
          E = llvm::joinErrors(std::move(E), Cleanup.takeError());
      }
      return E;
    }
  }
  if (!P)
    return P.takeError();
  if (!*P)
    return Status(Failure);
  const auto Base = *P, Parameter = Base + 8;
  std::vector<uint8_t> Record(Bytes, windows::UninitializedPoolByte);
  llvm::support::endian::write32le(Record.data() + 8, Type);
  NdisConfigurationStorage Storage{Bytes, {{Parameter, 4}}};
  if (!Type) {
    llvm::support::endian::write32le(Record.data() + 16, Integer);
    Storage.Readable.push_back({Parameter + 8, 4});
  } else {
    auto Length = Data.size();
    if (Type == 2 && !Data[Length - 1] && !Data[Length - 2])
      Length -= 2;
    llvm::support::endian::write16le(Record.data() + 16, Length);
    if (Type != 4)
      llvm::support::endian::write16le(Record.data() + 18, Data.size());
    llvm::support::endian::write64le(Record.data() + 24, Base + 32);
    std::copy(Data.begin(), Data.end(), Record.begin() + 32);
    // A public UNICODE_STRING/BINARY_DATA may be copied or passed to RTL as
    // one 16-byte record. Padding keeps the common deterministic pool policy.
    Storage.Readable.push_back({Parameter + 8, 16});
    if (!Data.empty())
      Storage.Readable.push_back({Base + 32, Data.size()});
  }
  auto Error = Memory.write(Base, Record);
  if (!Error && Type == 2 && uint32_t(A[4]) < 2) {
    auto Converted = runtime::unicodeToInteger(
        *this, Memory, {Parameter + 8, uint32_t(A[4]) == 0 ? 10u : 16u, Base});
    if (!Converted)
      Error = Converted.takeError();
    else {
      // The common RTL parser deliberately rejects aliased caller input/output.
      // Use our inaccessible allocation prefix as private result scratch, then
      // publish the integer union after the parser has consumed the string.
      auto Integer = Memory.readInteger(Base, 4);
      if (!Integer)
        Error = Integer.takeError();
      else
        Error = Memory.writeInteger(Parameter + 8, *Integer, 4);
      Type = uint32_t(A[4]);
      if (!Error)
        Error = Memory.writeInteger(Parameter, Type, 4);
      Storage.Readable = {{Parameter, 4}, {Parameter + 8, 4}};
    }
  }
  if (!Error)
    Error = Memory.writeInteger(A[1], Parameter, 8);
  if (!Error)
    Error = Memory.writeInteger(A[0], 0, 4);
  if (Error) {
    auto R = freePool({Base, 0}, true);
    if (!R)
      Error = llvm::joinErrors(std::move(Error), R.takeError());
    return Error;
  }
  H->second.Allocations.push_back(Base);
  NdisConfigurationAllocations.emplace(Base, std::move(Storage));
  return 0;
}
} // namespace neverd::emulation
