//===- X86SgxMemory.cpp - Software EPC and EPCM ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checked guest-state import and enclave accesses after ordinary paging.
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/X86SgxMemory.h"

#include "llvm/Support/Errc.h"

#include <bit>

namespace neverd::translate {
namespace {
constexpr uint64_t PageMask = 4095;
llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}
} // namespace

llvm::Expected<std::unique_ptr<X86SgxMemory>>
X86SgxMemory::create(std::vector<X86SgxPage> Pages,
                     std::optional<X86SgxEnclave> Active) {
  auto Memory = std::make_unique<X86SgxMemory>();
  for (auto &Page : Pages) {
    if ((Page.Physical | Page.Linear | Page.OwnerSECS) & PageMask ||
        Page.Type > X86SgxPageType::Trimmed || uint8_t(Page.Permissions) > 7 ||
        (Page.Type != X86SgxPageType::Regular &&
         Page.Permissions != MemoryPermission::None) ||
        (hasPermission(Page.Permissions, MemoryPermission::Write) &&
         !hasPermission(Page.Permissions, MemoryPermission::Read)))
      return invalid("invalid software EPCM page");
    if (!Memory->EPC.emplace(Page.Physical, std::move(Page)).second)
      return invalid("duplicate software EPC physical page");
  }
  if (auto E = Memory->bindActive(Active))
    return std::move(E);
  return Memory;
}

llvm::Error X86SgxMemory::bindX87(std::array<uint8_t, 160> Image,
                                  uint64_t Value) {
  if (Running || Value != 3)
    return invalid("SGX1 x87/SSE import requires an idle CPU and XCR0=3");
  X87 = Image;
  ++Generation;
  return llvm::Error::success();
}

llvm::Error X86SgxMemory::bindActive(std::optional<X86SgxEnclave> Enclave) {
  if (Running)
    return invalid("use EEXIT or AEX to leave a running enclave thread");
  if (Enclave) {
    const auto I = EPC.find(Enclave->SECS);
    if (!std::has_single_bit(Enclave->Size) || Enclave->Size <= PageMask ||
        (Enclave->Base & (Enclave->Size - 1)) || I == EPC.end() ||
        I->second.Type != X86SgxPageType::SECS || !I->second.Valid ||
        I->second.Blocked)
      return invalid(
          "active enclave requires a valid SECS and aligned ELRANGE");
  }
  Active = Enclave;
  ++Generation;
  return llvm::Error::success();
}

llvm::Error
X86SgxMemory::validateBacking(const GuestState &RAM,
                              const X86PagingFeatures &Features) const {
  if (!EPC.empty() && EPC.rbegin()->first >> Features.PhysicalAddressBits)
    return invalid("EPC page exceeds MAXPHYADDR");
  for (const auto &Region : RAM.Memory) {
    if (Region.Bytes.empty())
      continue;
    const auto First = EPC.lower_bound(Region.Address & ~PageMask);
    if (First != EPC.end() &&
        First->first <= Region.Address + (Region.Bytes.size() - 1))
      return invalid("ordinary physical RAM overlaps a private EPC page");
  }
  return llvm::Error::success();
}

X86SgxMemory::Access X86SgxMemory::resolve(const X86LinearAccess &A,
                                           const X86PagingState &Controls,
                                           uint64_t Physical) {
  const auto I = EPC.find(Physical & ~PageMask);
  const bool Direct = Active && !A.ImplicitSupervisor && A.CPL == 3 &&
                      Active->contains(A.Address);
  if (!Direct)
    return {I == EPC.end() ? Target::RAM : Target::AbortPage};
  const bool Write = A.Kind == MemoryAccessKind::Write,
             Fetch = A.Kind == MemoryAccessKind::Execute;
  const auto Required = Write   ? MemoryPermission::Write
                        : Fetch ? MemoryPermission::Execute
                                : MemoryPermission::Read;
  if (I != EPC.end()) {
    auto &P = I->second;
    if (P.Valid && !P.Blocked && !P.Pending && !P.Modified &&
        P.Type == X86SgxPageType::Regular && P.OwnerSECS == Active->SECS &&
        P.Linear == (A.Address & ~PageMask) &&
        hasPermission(P.Permissions, Required))
      return {Target::Private, P.Bytes.data() + (Physical & PageMask)};
  }
  return {Target::RAM, nullptr, x86PageFault(0x8001, Controls, A)};
}

} // namespace neverd::translate
