//===- AndroidQueries.cpp - Bionic platform queries ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../kernel/LinuxTime.h"
#include "AndroidInternal.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"
#include "neverd/emulation/Limits.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>

namespace neverd::emulation::android_model {
namespace {
using ::neverd::limits::emulation::kAndroidErrorBufferOffset;
using ::neverd::limits::emulation::kAndroidErrorStringsBase;
struct ErrorText {
  uint32_t Code;
  llvm::StringLiteral Text;
};
constexpr ErrorText ErrorTexts[] = {
#define NEVERD_ANDROID_ERROR(Code, Text) {Code, Text},
#include "AndroidErrorStrings.def"
#undef NEVERD_ANDROID_ERROR
};
constexpr uint64_t errorStringsSize() {
  uint64_t Bytes = 0;
  for (const auto &Entry : ErrorTexts)
    Bytes += Entry.Text.size() + 1;
  return Bytes;
}
static_assert(errorStringsSize() <= PageSize);
static_assert(kAndroidErrorBufferOffset >=
              LinkerErrorAddress - TLSAddress +
                  ::neverd::limits::emulation::
                      kAndroidDlfcnBionicLinkerErrorMessageSizeLimit);
static_assert(kAndroidErrorBufferOffset + sizeof("Unknown error -2147483648") <=
              PageSize);
} // namespace

llvm::Error initializeErrorStrings(AddressSpace &Space,
                                   const LinkedImage &Linked) {
  if (!llvm::any_of(Linked.Imports,
                    [](const auto &I) { return I.second == symbol::Strerror; }))
    return llvm::Error::success();
  std::array<uint8_t, PageSize> Bytes{};
  size_t Offset = 0;
  for (const auto &Entry : ErrorTexts) {
    llvm::copy(Entry.Text, Bytes.begin() + Offset);
    Offset += Entry.Text.size() + 1;
  }
  if (auto E = Space.map(kAndroidErrorStringsBase, PageSize,
                         Read | Write | UserAccessible))
    return E;
  if (auto E = Space.write(kAndroidErrorStringsBase, Bytes))
    return E;
  return Space.protect(kAndroidErrorStringsBase, PageSize,
                       Read | UserAccessible);
}

BionicResult Bionic::errorString(const NativeCallEvent &Call) {
  const uint32_t Code = static_cast<uint32_t>(Call.Arguments[0]);
  uint64_t Address = kAndroidErrorStringsBase;
  for (const auto &Entry : ErrorTexts) {
    if (Entry.Code == Code) {
      // Known errors return a borrowed address without dereferencing it.
      // Guest reads remain responsible for faults after protection/unmapping.
      return value(Address);
    }
    Address += Entry.Text.size() + 1;
  }
  // The int ABI discards upper GP bits. Unknown values (including catalog
  // holes) use signed decimal text in reusable thread-owned storage. Known
  // calls leave this buffer intact; neither path changes errno or dlerror.
  std::string Text =
      "Unknown error " + llvm::itostr(std::bit_cast<int32_t>(Code));
  Text.push_back('\0');
  Address = tlsAddress() + kAndroidErrorBufferOffset;
  if (auto E = access(Address, Text.size(), Write))
    return std::move(E);
  if (auto E = CPU.write(
          Address,
          llvm::ArrayRef(reinterpret_cast<const uint8_t *>(Text.data()),
                         Text.size())))
    return std::move(E);
  return value(Address);
}

llvm::Error Bionic::setErrno(uint32_t Value) {
  uint8_t Bytes[4];
  llvm::support::endian::write32le(Bytes, Value);
  return CPU.write(tlsAddress() + ErrnoAddress - TLSAddress, Bytes);
}
llvm::Expected<uint32_t> Bionic::errnoValue() {
  uint8_t Bytes[4];
  if (auto E = CPU.read(tlsAddress() + ErrnoAddress - TLSAddress, Bytes))
    return std::move(E);
  return llvm::support::endian::read32le(Bytes);
}
BionicResult Bionic::getErrno(const NativeCallEvent &) {
  return value(tlsAddress() + ErrnoAddress - TLSAddress);
}

BionicResult Bionic::getPageSize(const NativeCallEvent &) {
  return value(PageSize);
}

BionicResult Bionic::getAPILevel(const NativeCallEvent &) {
  auto Property = propertyNamed("ro.build.version.sdk");
  if (!Property)
    return Property.takeError();
  if (!*Property || (*Property)->Value.empty())
    return value(UINT32_MAX);
  // This query observes the device property, not the selected Bionic ABI.
  // Share atoi's defined prefix, width and overflow decisions. Inputs are
  // immutable, so repeated queries never need a separate cached identity.
  const std::string &Text = (*Property)->Value;
  auto Parsed =
      decimalValue(true, [&](uint64_t Offset) -> llvm::Expected<uint8_t> {
        return Offset < Text.size() ? static_cast<uint8_t>(Text[Offset])
                                    : uint8_t(0);
      });
  if (!Parsed)
    return Parsed.takeError();
  if (!*Parsed)
    return std::optional<BionicValue>();
  return value(**Parsed && **Parsed <= INT32_MAX ? **Parsed : UINT32_MAX);
}

BionicResult Bionic::queryConfiguration(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  // sysconf takes int, independent of unspecified upper GP argument bits.
  switch (static_cast<SysconfName>(static_cast<uint32_t>(A[0]))) {
  case SysconfName::PageSize:
  case SysconfName::PageSizeAlias:
    return value(PageSize);
  }
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = diagnostic::SysconfSelector;
  return std::optional<BionicValue>();
}

BionicResult Bionic::getTime(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto Now = Kernel.clock().read(linux_model::ClockRealtime, Result);
  if (!Now)
    return std::optional<BionicValue>();
  // API 28's fallback obtains a timeval, then stores through the caller's
  // time_t*. This is a user-space store, not the x64 time syscall's EFAULT.
  if (A[0]) {
    if (auto E = access(A[0], 8, Write))
      return std::move(E);
    uint8_t Bytes[8];
    llvm::support::endian::write64le(Bytes,
                                     static_cast<uint64_t>(Now->Seconds));
    if (auto E = CPU.write(A[0], Bytes))
      return std::move(E);
  }
  // A valid negative timestamp is not a Linux errno return.
  return value(static_cast<uint64_t>(Now->Seconds));
}

} // namespace neverd::emulation::android_model
