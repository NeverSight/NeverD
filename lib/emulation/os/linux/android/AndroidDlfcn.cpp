//===- AndroidDlfcn.cpp - Bionic dynamic library lifetime ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidDlfcn.h"

#include "AndroidInternal.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/Limits.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <set>

namespace neverd::emulation::android_model {
llvm::Error validateDladdrObservations(
    const std::optional<std::vector<NativeDladdrObservation>> &Observations) {
  if (!Observations)
    return llvm::Error::success();
  if (Observations->size() > dladdr_model::MaximumRecords)
    return failure(diagnostic::DladdrObservationCount);
  auto ValidName = [](const std::string &Name) {
    return !Name.empty() && Name.size() <= dladdr_model::MaximumStringBytes &&
           Name.find('\0') == std::string::npos;
  };
  std::set<uint64_t> Addresses;
  uint64_t Bytes = 0;
  for (const auto &O : *Observations) {
    if (!O.Address || O.ImageBase > O.Address || !ValidName(O.ImageName) ||
        !Addresses.insert(O.Address).second ||
        (O.SymbolName
             ? !ValidName(*O.SymbolName) || !O.SymbolAddress ||
                   O.SymbolAddress < O.ImageBase || O.SymbolAddress > O.Address
             : O.SymbolAddress != 0))
      return failure(diagnostic::DladdrObservationInvalid);
    Bytes += dladdr_model::InfoBytes + O.ImageName.size() + 1;
    if (O.SymbolName)
      Bytes += O.SymbolName->size() + 1;
    if (Bytes > dladdr_model::MaximumSnapshotBytes)
      return failure(diagnostic::DladdrObservationBytes);
  }
  return llvm::Error::success();
}

llvm::Error initializeDladdrObservations(AddressSpace &Space,
                                         const AndroidNativeOptions &Options,
                                         LinkedImage &Linked) {
  if (auto E = validateDladdrObservations(Options.DladdrObservations))
    return E;
  if (!Options.DladdrObservations || Options.DladdrObservations->empty())
    return llvm::Error::success();
  std::vector<uint8_t> Bytes;
  std::map<std::string, uint64_t> Strings;
  auto Intern = [&](const std::string &Text) {
    auto [I, New] =
        Strings.emplace(Text, dladdr_model::StorageBase + Bytes.size());
    if (New) {
      Bytes.insert(Bytes.end(), Text.begin(), Text.end());
      Bytes.push_back(0);
    }
    return I->second;
  };
  for (const auto &O : *Options.DladdrObservations) {
    const uint64_t Name = Intern(O.ImageName);
    const uint64_t Symbol = O.SymbolName ? Intern(*O.SymbolName) : 0;
    Linked.DladdrObservations.emplace(
        O.Address,
        std::array<uint64_t, 4>{Name, O.ImageBase, Symbol, O.SymbolAddress});
  }
  const uint64_t Size = (Bytes.size() + PageSize - 1) & ~(PageSize - 1);
  if (auto E = Space.map(dladdr_model::StorageBase, Size,
                         Read | Write | UserAccessible))
    return E;
  if (auto E = Space.write(dladdr_model::StorageBase, Bytes))
    return E;
  return Space.protect(dladdr_model::StorageBase, Size, Read | UserAccessible);
}

BionicResult Bionic::queryDladdr(const NativeCallEvent &Call) {
  auto I = Linked.DladdrObservations.find(Call.Arguments[0]);
  if (I == Linked.DladdrObservations.end())
    return unsupportedLinking(diagnostic::DladdrObservationUnknown);
  if (auto E = access(Call.Arguments[1], dladdr_model::InfoBytes, Write))
    return std::move(E);
  // Guest writes, unmap or remap must not turn retained metadata into an
  // invisible fallback. Direct guest reads and later queries share these bytes.
  const auto &Observations = *Options.Android->DladdrObservations;
  const auto &O = *std::find_if(
      Observations.begin(), Observations.end(),
      [&](const auto &O) { return O.Address == Call.Arguments[0]; });
  auto CheckString = [&](uint64_t Address,
                         const std::string &Expected) -> llvm::Error {
    if (auto E = access(Address, Expected.size() + 1, Read))
      return E;
    std::vector<uint8_t> Bytes(Expected.size() + 1);
    if (auto E = CPU.read(Address, Bytes))
      return E;
    if (Bytes.back() ||
        !std::equal(Expected.begin(), Expected.end(), Bytes.begin(),
                    [](char A, uint8_t B) { return uint8_t(A) == B; }))
      return failure(diagnostic::DladdrObservationChanged);
    return llvm::Error::success();
  };
  if (auto E = CheckString(I->second[0], O.ImageName))
    return std::move(E);
  if (O.SymbolName)
    if (auto E = CheckString(I->second[2], *O.SymbolName))
      return std::move(E);
  uint8_t Bytes[dladdr_model::InfoBytes];
  for (size_t J = 0; J != I->second.size(); ++J)
    llvm::support::endian::write64le(Bytes + J * sizeof(uint64_t),
                                     I->second[J]);
  if (auto E = CPU.write(Call.Arguments[1], Bytes))
    return std::move(E);
  return value(1);
}

BionicResult Bionic::linkerError(llvm::StringRef Message,
                                 uint64_t ReturnValue) {
  const uint64_t ErrorAddress = tlsAddress() + LinkerErrorAddress - TLSAddress;
  const uint64_t ErrorSlot = tlsAddress() + LinkerErrorSlot - TLSAddress;
  // A fixed guest buffer, separate from errno, TLS ABI slots and constructor
  // argv/envp. A later error may replace its contents; dlerror consumes it
  // once.
  if (Message.size() >= ::neverd::limits::emulation::
                            kAndroidDlfcnBionicLinkerErrorMessageSizeLimit)
    return failure(diagnostic::LinkerErrorLimit);
  std::vector<uint8_t> Bytes(Message.bytes_begin(), Message.bytes_end());
  Bytes.push_back(0);
  if (auto E = access(ErrorAddress, Bytes.size(), Write))
    return std::move(E);
  if (auto E = CPU.write(ErrorAddress, Bytes))
    return std::move(E);
  uint8_t Pointer[8];
  llvm::support::endian::write64le(Pointer, ErrorAddress);
  if (auto E = access(ErrorSlot, sizeof(Pointer), Write))
    return std::move(E);
  if (auto E = CPU.write(ErrorSlot, Pointer))
    return std::move(E);
  return value(ReturnValue);
}
bool Bionic::isResident(const std::string &Library) const {
  const auto &Scope = Options.Android->DefaultScope;
  return Scope &&
         std::find(Scope->begin(), Scope->end(), Library) != Scope->end();
}
BionicResult Bionic::unsupportedLinking(llvm::StringRef Detail) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = diagnostic::DynamicLinkingPrefix + Detail.str();
  return std::optional<BionicValue>();
}

BionicResult Bionic::getLinkerError(NativeCallEvent &) {
  const uint64_t ErrorSlot = tlsAddress() + LinkerErrorSlot - TLSAddress;
  uint8_t Pointer[8], Empty[8]{};
  if (auto E = access(ErrorSlot, sizeof(Pointer), Read | Write))
    return std::move(E);
  if (auto E = CPU.read(ErrorSlot, Pointer))
    return std::move(E);
  uint64_t Address = llvm::support::endian::read64le(Pointer);
  if (auto E = CPU.write(ErrorSlot, Empty))
    return std::move(E);
  return value(Address);
}

BionicResult Bionic::openLibrary(NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  if (!A[0])
    return unsupportedLinking(diagnostic::NullOpenScope);
  auto Library = string(A[0]);
  if (!Library)
    return Library.takeError();
  if (Library->size() >
      ::neverd::limits::emulation::kAndroidDlfcnBionicOpenLibrarySizeLimit)
    return failure(diagnostic::LibraryNameLimit);
  Call.Library = *Library;
  // API 28 LP64 accepts any combination of these bits, including zero
  // and NOLOAD alone. Scope promotion and NODELETE need a fuller loader.
  uint32_t Flags = static_cast<uint32_t>(A[1]);
  if (Flags & ~0x1107u)
    return linkerError(diagnostic::OpenFlags);
  if (Flags & 0x1100u)
    return unsupportedLinking(diagnostic::OpenScopeFlags);
  if (!Linked.Libraries.count(*Library))
    return linkerError(diagnostic::LibraryAbsent);
  auto &State = OpenLibraries[*Library];
  if (!State.References && !isResident(*Library) && (Flags & 4u))
    return linkerError(diagnostic::LibraryNotOpen);
  if (!State.Handle) {
    if (NextHandle > UINT64_MAX - 2)
      return failure(diagnostic::LibraryHandleLimit);
    State.Handle = NextHandle;
    NextHandle += 2;
    Handles.emplace(State.Handle, *Library);
  }
  if (State.References == UINT64_MAX)
    return failure(diagnostic::LibraryReferenceOverflow);
  ++State.References;
  return value(State.Handle);
}

BionicResult Bionic::lookupSymbol(NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  if (!A[1])
    return linkerError(diagnostic::NullSymbol);
  auto Symbol = string(A[1]);
  if (!Symbol)
    return Symbol.takeError();
  if (Symbol->size() >
      ::neverd::limits::emulation::kAndroidDlfcnBionicLookupSymbolSizeLimit)
    return failure(diagnostic::SymbolNameLimit);
  Call.Symbol = *Symbol;
  if (!A[0]) {
    const auto &Scope = Options.Android->DefaultScope;
    if (!Scope)
      return unsupportedLinking(diagnostic::DefaultScope);
    for (const auto &Library : *Scope) {
      const auto &Symbols = Linked.Libraries.at(Library);
      auto I = Symbols.find(*Symbol);
      if (I != Symbols.end()) {
        Call.Library = Library;
        return value(I->second);
      }
    }
    return linkerError(diagnostic::DefaultSymbolAbsent);
  }
  if (A[0] == UINT64_MAX)
    return unsupportedLinking(diagnostic::NextScope);
  auto Handle = Handles.find(A[0]);
  if (Handle == Handles.end())
    return linkerError(diagnostic::LibraryHandle);
  Call.Library = Handle->second;
  const auto &Symbols = Linked.Libraries.at(Handle->second);
  auto I = Symbols.find(*Symbol);
  if (I == Symbols.end())
    return linkerError(diagnostic::LibrarySymbolAbsent);
  return value(I->second);
}

BionicResult Bionic::closeLibrary(NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto Handle = Handles.find(A[0]);
  if (Handle == Handles.end())
    return linkerError(diagnostic::LibraryHandle, UINT64_MAX);
  Call.Library = Handle->second;
  auto &State = OpenLibraries.at(Handle->second);
  if (!State.References)
    return linkerError(diagnostic::LibraryReferences, UINT64_MAX);
  if (!--State.References && !isResident(Handle->second)) {
    Handles.erase(Handle);
    State.Handle = 0;
  }
  return value(0);
}

} // namespace neverd::emulation::android_model
