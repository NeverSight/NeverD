//===- X86SgxMemory.h - Software EPC and EPCM ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Isolated EPC bytes and post-paging access control for an imported enclave.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_X86SGXMEMORY_H
#define NEVERD_TRANSLATE_X86_X86SGXMEMORY_H

#include "neverd/translate/X86/X86Paging.h"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace neverd::analysis::detail {
class X86SgxExecution;
}

namespace neverd::translate {

enum class X86SgxPageType : uint8_t {
  Regular,
  TCS,
  SECS,
  VersionArray,
  Trimmed
};

struct X86SgxPage {
  uint64_t Physical = 0;
  uint64_t Linear = 0;
  uint64_t OwnerSECS = 0;
  X86SgxPageType Type = X86SgxPageType::Regular;
  MemoryPermission Permissions = MemoryPermission::None;
  bool Valid = true;
  bool Blocked = false;
  bool Pending = false;
  bool Modified = false;
  std::array<uint8_t, 4096> Bytes{};
};

/// CPUID.(12H,1):EAX[10] selects the AEX notification attribute.
/// CPUID.(12H,0):EAX[11] separately selects ENCLU[EDECCSSA].
struct X86SgxFeatures {
  bool AEXNotify = false;
  bool DecrementSSA = false;
};

struct X86SgxEnclave {
  uint64_t SECS = 0;
  uint64_t Base = 0;
  uint64_t Size = 0;
  bool contains(uint64_t Address) const {
    return Address >= Base && Address - Base < Size;
  }
};

/// Confined software profile for ordinary SGX data/fetch accesses. EPC pages
/// have private backing, so page walkers can never consume enclave bytes as
/// ordinary physical RAM. For non-enclave EPC accesses this processor profile
/// returns all ones and discards stores (Intel SDM vol. 3D, sections 38.1-5).
class X86SgxMemory final {
public:
  static llvm::Expected<std::unique_ptr<X86SgxMemory>>
  create(std::vector<X86SgxPage> Pages,
         std::optional<X86SgxEnclave> Active = {});
  static llvm::Expected<std::unique_ptr<X86SgxMemory>>
  create(std::vector<X86SgxPage> Pages, std::optional<X86SgxEnclave> Active,
         X86SgxFeatures Features);
  llvm::Error validateBacking(const GuestState &RAM,
                              const X86PagingFeatures &Features) const;
  /// Bind the legacy x87 area of a standard 64-bit XSAVE image. The live CPU
  /// bank owns MXCSR and the vector registers. The outside XCR0 selects
  /// x87/SSE (3) or x87/SSE/AVX (7). Entry can enable a subset for the enclave.
  llvm::Error bindX87(std::array<uint8_t, 160> Image, uint64_t XCR0);
  const std::optional<std::array<uint8_t, 160>> &x87() const { return X87; }
  const std::optional<X86SgxEnclave> &active() const { return Active; }
  uint64_t xcr0() const {
    return Running && Running->SwitchXCR0 ? Running->XFRM : OutsideXCR0;
  }
  llvm::Error bindActive(std::optional<X86SgxEnclave> Enclave);
  bool hasRunningThread() const { return Running.has_value(); }
  uint64_t generation() const { return Generation; }
  bool enabled() const { return Enabled; }
  bool suppressesEnclaveDebug() const {
    return Running && !Running->DebugOptIn;
  }
  bool instructionBreakpointsAt(uint64_t Address) const {
    return !Running || (Running->DebugOptIn && Active->contains(Address));
  }
  /// Disable enclave instructions until the platform installs a reset EPC bank.
  void disableAfterMachineCheck();

private:
  friend class X86PagedMemory;
  friend class neverd::analysis::detail::X86SgxExecution;
  enum class Target { RAM, Private, AbortPage };
  struct Access {
    Target Destination = Target::RAM;
    uint8_t *Bytes = nullptr;
    std::optional<X86MemoryException> Exception;
  };
  Access resolve(const X86LinearAccess &Request, const X86PagingState &Controls,
                 uint64_t Physical);
  std::map<uint64_t, X86SgxPage> EPC;
  std::optional<X86SgxEnclave> Active;
  struct SavedSegment {
    uint64_t Base;
    uint32_t Limit;
    uint16_t Selector, Attributes;
  };
  struct Thread {
    uint64_t TCS, TCSLinear, XSavePage, GprPage, AEP, XFRM;
    SavedSegment FS, GS;
    uint8_t Unusable;
    bool OutsideTF, SwitchXCR0, DebugOptIn;
  };
  std::optional<Thread> Running;
  std::optional<std::array<uint8_t, 160>> X87;
  X86SgxFeatures Features;
  bool Enabled = true;
  uint64_t OutsideXCR0 = 0;
  uint64_t Generation = 0;
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86SGXMEMORY_H
