//===- X86Paging.h - x86 linear-address translation -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Ordinary x86 page walks over owned physical RAM. This boundary keeps CPU
/// exceptions separate from missing or unwritable host-owned backing memory.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_X86PAGING_H
#define NEVERD_TRANSLATE_X86_X86PAGING_H

#include "neverd/translate/GuestMemoryRuntime.h"

#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::translate {

/// CPU capabilities are supplied by the embedding environment, never inferred
/// from the host CPU. PhysicalAddressBits must be in the architectural 32..52
/// range. Ordinary RAM, enabled A20, and no concurrent page-table writers are
/// the execution contract. EPT/NPT, HLAT, SGX EPCM and memory types are outside
/// this translation stage; their failures must not be reported as ordinary #PF.
struct X86PagingFeatures {
  uint8_t PhysicalAddressBits = 0;
  bool PSE = false;
  bool PAE = false;
  bool LongMode = false;
  bool PSE36 = false;
  bool PAT = false;
  bool Page1GB = false;
  bool LA57 = false;
  bool NX = false;
  bool SMEP = false;
  bool SMAP = false;
  bool PKU = false;
  bool PKS = false;
};

struct X86PagingState {
  uint64_t CR0 = 0;
  uint64_t CR3 = 0;
  uint64_t CR4 = 0;
  uint64_t EFER = 0;
  uint64_t RFlags = 0;
  uint32_t PKRU = 0;
  uint32_t PKRS = 0;
  /// PAE uses the previously loaded registers, not a fresh CR3 memory read.
  std::array<uint64_t, 4> PDPTE{};
};

struct X86LinearAccess {
  uint64_t Address = 0;
  MemoryAccessKind Kind = MemoryAccessKind::Read;
  uint8_t CPL = 0;
  /// GDT, IDT, TSS and similar implicit accesses remain supervisor accesses
  /// at CPL3; SMAP still applies and AC cannot override it for these accesses.
  bool ImplicitSupervisor = false;
  /// Noncanonical stack addresses raise #SS(0), other addresses #GP(0).
  bool Stack = false;
  /// Legacy explicit operands after whole-segment checking may cross the
  /// 32-bit linear wrap. IA-32e and implicit system references cannot select
  /// this flag. It does not waive segment-offset limit checks.
  bool Wrap32 = false;
};

struct X86MemoryException {
  uint8_t Vector = 0;
  uint32_t ErrorCode = 0;
  /// Present only for #PF. CR2 must retain this value even if delivery faults.
  std::optional<uint64_t> CR2;
};

enum class X86PageWalkStatus : uint8_t {
  Completed,
  Exception,
  BackingFault,
};

struct X86MemoryAccessResult {
  X86PageWalkStatus Status = X86PageWalkStatus::Completed;
  std::optional<X86MemoryException> Exception;
  std::optional<GuestMemoryFault> BackingFault;
  /// A/D writes are visible even when a later level or permission check fails.
  /// The enclosing executor must invalidate each affected executable region.
  std::vector<SelfModificationExit> SelfModifications;
};

struct X86PageWalkResult : X86MemoryAccessResult {
  uint64_t PhysicalAddress = 0;
  uint64_t PageSize = 0;
};

/// Validate the bound paging mode before any RAM or A/D effect. Invalid or
/// unsupported control state is an embedding error, not a guessed CPU fault.
llvm::Error validateX86PagingState(const X86PagingFeatures &Features,
                                   const X86PagingState &State);

/// Check an operand's entire linear extent before any page-table access.
/// Controls have already passed validateX86PagingState. Noncanonical operands
/// report #GP/#SS before any #PF, even when the first byte has no translation.
/// IA-32e operands wrap modulo 2^64; both ends must remain canonical.
/// Legacy linear wrap requires explicit Wrap32 after segment checks.
llvm::Expected<std::optional<X86MemoryException>>
checkX86LinearAccess(const X86PagingState &State, const X86LinearAccess &Access,
                     uint64_t Bytes);

/// Translate one linear address with a fresh walk (no TLB). Sets A/D in owned
/// physical RAM. The caller owns segmentation, whole-operand checks, data
/// access and instruction commit; a successful walk alone executes no operand.
llvm::Expected<X86PageWalkResult>
walkX86PageTables(GuestMemoryRuntime &PhysicalMemory,
                  const X86PagingFeatures &Features,
                  const X86PagingState &State, const X86LinearAccess &Access);

/// Load all four PAE PDPTE registers transactionally. Reserved bits in a
/// present entry produce #GP(0); backing failures stay distinct. The previous
/// register bank is preserved on either failure. CR3 is the requested new CR3.
llvm::Expected<X86PageWalkResult>
loadX86PAEPDPTE(GuestMemoryRuntime &PhysicalMemory,
                const X86PagingFeatures &Features, uint64_t CR3,
                std::array<uint64_t, 4> &PDPTE);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86PAGING_H
