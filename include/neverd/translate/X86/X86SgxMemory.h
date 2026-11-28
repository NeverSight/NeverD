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

class X86SgxKeyPlatform;

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
  /// EPCM.PR from a permission restriction. Tracking is the page's imported
  /// epoch observation for EACCEPT in this serialized processor model.
  bool PermissionRestricted = false;
  std::optional<bool> TrackingComplete{};
};

/// Intel ISE 319433-063 gives a 768-byte EREPORT2 store in its operation
/// and a zero reserved tail in its 1024-byte layout. Bind the processor's
/// observed write footprint before publishing a successful report.
enum class X86SgxReport2Write { Payload768, ZeroReservedTail };

/// CPUID.(12H,1):EAX[10] selects the AEX notification attribute.
/// CPUID.(12H,0):EAX[11] separately selects ENCLU[EDECCSSA].
struct X86SgxFeatures {
  bool AEXNotify = false;
  bool DecrementSSA = false;
  /// Additional attributes permitted in initialized SECS, CPUID.(12H,1):EAX.
  bool ProvisionKey = false;
  bool InitTokenKey = false;
  bool KeySeparationAndSharing = false;
  /// CPUID.(12H,0):EAX[1]. Dynamic page operations on the imported EPCM.
  bool SGX2 = false;
  /// CPUID.(12H,0):EAX[12] and CPUID.(12H,1):EAX[11], respectively.
  bool SGX256 = false;
  bool SHA384Measurements = false;
  std::optional<X86SgxReport2Write> Report2Write{};

  static constexpr uint64_t Provision = 1u << 4, InitToken = 1u << 5,
                            KSS = 1u << 7, Notify = 1u << 10, SHA384 = 1u << 11;
  uint64_t allowedAttributes() const {
    return 7 | (ProvisionKey ? Provision : 0) | (InitTokenKey ? InitToken : 0) |
           (KeySeparationAndSharing ? KSS : 0) | (AEXNotify ? Notify : 0) |
           (SHA384Measurements ? SHA384 : 0);
  }
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
  void bindKeyPlatform(std::shared_ptr<const X86SgxKeyPlatform> Platform);
  /// Publish an external tracking observation for the current restriction
  /// or type change. The platform supplies completion of the enclave shootdown.
  llvm::Error bindPageTracking(uint64_t Physical, uint64_t OwnerSECS,
                               bool Complete);
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
  std::shared_ptr<const X86SgxKeyPlatform> Keys;
  bool Enabled = true;
  uint64_t OutsideXCR0 = 0;
  uint64_t Generation = 0;
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86SGXMEMORY_H
